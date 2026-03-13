"""Glasgow frequency counter applet — PLL + DDR for high-speed edge counting.

Measures toggle frequency on a single input pin using:
  - iCE40 PLL + DDR I/O for high effective sample rate
  - Free-running edge counters in FPGA, snapshot subtraction over USB

This avoids the USB FIFO overrun issue of the standard analyzer applet.

Protocol:
  Host -> FPGA: 4 bytes (gate_time in 48 MHz cycles, big-endian uint32)
  FPGA -> Host: 12 bytes (edge_count uint32 BE, gate_cycles uint32 BE, sample_freq uint32 BE)
"""

import logging
import struct
import argparse

from amaranth import *
from amaranth.lib import io
from amaranth.lib.cdc import FFSynchronizer

from ... import *


# PLL parameters: 48 MHz * (DIVF+1) / (DIVR+1) / 2^DIVQ
# Target: 200 MHz  =>  48 * 50 / 3 / 4 = 200 MHz
# VCO = 48 * 50 / 3 = 800 MHz (within 533-1066 MHz range)
PLL_DIVR = 2
PLL_DIVF = 49
PLL_DIVQ = 2
PLL_FILTER_RANGE = 1
PLL_OUT_FREQ = 200_000_000
EFFECTIVE_SAMPLE_FREQ = PLL_OUT_FREQ * 2  # 400 MHz with DDR

# Counter segmentation: 8-bit fast stage + 24-bit slow stage = 32 bits.
COUNTER_LO_BITS = 8
COUNTER_HI_BITS = 24


class FreqCounterSubtarget(Elaboratable):
    """FPGA gateware for high-speed frequency counting.

    Architecture: free-running counters with snapshot subtraction.

    The fast domain is pure datapath — no enables, no resets, no
    conditional logic on counter paths. Counters always increment.
    Gate control snapshots start/end values; the sync domain computes
    the difference.

    Fast-domain critical path:
      registered_edge (1-bit FF) -> adder input -> 8-bit carry -> counter FF

    No muxes, no enables, no fan-out from control signals.
    """

    def __init__(self, ports, in_fifo, out_fifo):
        self.ports = ports
        self.in_fifo = in_fifo
        self.out_fifo = out_fifo

    def elaborate(self, platform):
        m = Module()

        # === Fast clock domain via PLL ===
        m.domains += ClockDomain("fast")

        m.submodules.pll = Instance("SB_PLL40_CORE",
            p_FEEDBACK_PATH="SIMPLE",
            p_PLLOUT_SELECT="GENCLK",
            p_DIVR=PLL_DIVR,
            p_DIVF=PLL_DIVF,
            p_DIVQ=PLL_DIVQ,
            p_FILTER_RANGE=PLL_FILTER_RANGE,
            i_REFERENCECLK=ClockSignal("sync"),
            o_PLLOUTCORE=ClockSignal("fast"),
            i_RESETB=Const(1),
            i_BYPASS=Const(0),
        )

        # === DDR Input buffer clocked by PLL ===
        m.submodules.i_buffer = i_buffer = io.DDRBuffer("i", self.ports.i,
                                                         i_domain="fast")

        # === Edge detection (pipelined) ===
        # Stage 1: register previous falling-edge sample
        prev_i1 = Signal()
        m.d.fast += prev_i1.eq(i_buffer.i[1])

        # Stage 2: XOR -> registered edge flags (pure pipeline, always runs)
        edge_01 = Signal()  # transition at rising-edge boundary
        edge_12 = Signal()  # transition at falling-edge boundary
        m.d.fast += [
            edge_01.eq(prev_i1 ^ i_buffer.i[0]),
            edge_12.eq(i_buffer.i[0] ^ i_buffer.i[1]),
        ]

        # === Free-running segmented counters (ALWAYS increment, no enables) ===

        # Single edge counter: increments by (edge_01 + edge_12) = 0, 1, or 2
        # Using one counter instead of two reduces routing pressure.
        # Pipeline the sum too: register (edge_01 + edge_12) before the adder.
        edges_sum = Signal(2)
        m.d.fast += edges_sum.eq(Cat(edge_01 ^ edge_12, edge_01 & edge_12))

        # 3-stage segmented counter: 8 + 12 + 12 = 32 bits
        # Each stage's carry is registered before feeding the next.
        MID_BITS = 12
        HI_BITS = 12

        ec_lo = Signal(COUNTER_LO_BITS)
        ec_mid = Signal(MID_BITS)
        ec_hi = Signal(HI_BITS)
        ec_next = Signal(COUNTER_LO_BITS + 1)
        m.d.comb += ec_next.eq(ec_lo + edges_sum)
        m.d.fast += ec_lo.eq(ec_next[:COUNTER_LO_BITS])
        ec_carry1 = Signal()
        m.d.fast += ec_carry1.eq(ec_next[COUNTER_LO_BITS])
        ec_mid_next = Signal(MID_BITS + 1)
        m.d.comb += ec_mid_next.eq(ec_mid + ec_carry1)
        m.d.fast += ec_mid.eq(ec_mid_next[:MID_BITS])
        ec_carry2 = Signal()
        m.d.fast += ec_carry2.eq(ec_mid_next[MID_BITS])
        with m.If(ec_carry2):
            m.d.fast += ec_hi.eq(ec_hi + 1)

        # Gate counter: same 3-stage segmentation
        gc_lo = Signal(COUNTER_LO_BITS)
        gc_mid = Signal(MID_BITS)
        gc_hi = Signal(HI_BITS)
        gc_next = Signal(COUNTER_LO_BITS + 1)
        m.d.comb += gc_next.eq(gc_lo + 1)
        m.d.fast += gc_lo.eq(gc_next[:COUNTER_LO_BITS])
        gc_carry1 = Signal()
        m.d.fast += gc_carry1.eq(gc_next[COUNTER_LO_BITS])
        gc_mid_next = Signal(MID_BITS + 1)
        m.d.comb += gc_mid_next.eq(gc_mid + gc_carry1)
        m.d.fast += gc_mid.eq(gc_mid_next[:MID_BITS])
        gc_carry2 = Signal()
        m.d.fast += gc_carry2.eq(gc_mid_next[MID_BITS])
        with m.If(gc_carry2):
            m.d.fast += gc_hi.eq(gc_hi + 1)

        # === Gate snapshot logic ===
        # CDC: gate_active from sync to fast
        gate_active_sync = Signal()
        gate_active_fast = Signal()
        m.submodules.cdc_gate = FFSynchronizer(
            gate_active_sync, gate_active_fast, o_domain="fast")

        # Detect rising and falling edges of gate_active in fast domain
        gate_active_prev = Signal()
        m.d.fast += gate_active_prev.eq(gate_active_fast)
        gate_start = Signal()
        gate_end = Signal()
        m.d.comb += [
            gate_start.eq(gate_active_fast & ~gate_active_prev),
            gate_end.eq(~gate_active_fast & gate_active_prev),
        ]

        # Snapshot registers (captured on gate edges, NOT in counter path)
        start_ec_lo = Signal(COUNTER_LO_BITS)
        start_ec_mid = Signal(MID_BITS)
        start_ec_hi = Signal(HI_BITS)
        start_gc_lo = Signal(COUNTER_LO_BITS)
        start_gc_mid = Signal(MID_BITS)
        start_gc_hi = Signal(HI_BITS)

        end_ec_lo = Signal(COUNTER_LO_BITS)
        end_ec_mid = Signal(MID_BITS)
        end_ec_hi = Signal(HI_BITS)
        end_gc_lo = Signal(COUNTER_LO_BITS)
        end_gc_mid = Signal(MID_BITS)
        end_gc_hi = Signal(HI_BITS)

        with m.If(gate_start):
            m.d.fast += [
                start_ec_lo.eq(ec_lo), start_ec_mid.eq(ec_mid), start_ec_hi.eq(ec_hi),
                start_gc_lo.eq(gc_lo), start_gc_mid.eq(gc_mid), start_gc_hi.eq(gc_hi),
            ]

        gate_done_fast = Signal()
        gate_done_sync = Signal()
        m.submodules.cdc_done = FFSynchronizer(
            gate_done_fast, gate_done_sync, o_domain="sync")

        with m.If(gate_end):
            m.d.fast += [
                end_ec_lo.eq(ec_lo), end_ec_mid.eq(ec_mid), end_ec_hi.eq(ec_hi),
                end_gc_lo.eq(gc_lo), end_gc_mid.eq(gc_mid), end_gc_hi.eq(gc_hi),
                gate_done_fast.eq(1),
            ]
        with m.Elif(gate_start):
            m.d.fast += gate_done_fast.eq(0)

        # === Sync domain: subtraction and USB protocol ===
        gate_time = Signal(32)
        gate_timer = Signal(32)
        byte_idx = Signal(range(12))

        result_edges = Signal(32)
        result_gate = Signal(32)
        rx_shift = Signal(32)
        tx_shift = Signal(96)

        with m.FSM(domain="sync"):
            with m.State("IDLE"):
                m.d.sync += byte_idx.eq(0)
                m.d.comb += self.out_fifo.r_en.eq(self.out_fifo.r_rdy)
                with m.If(self.out_fifo.r_rdy):
                    m.d.sync += rx_shift.eq(
                        Cat(self.out_fifo.r_data, rx_shift[:-8]))
                    with m.If(byte_idx == 3):
                        m.next = "START_GATE"
                    with m.Else():
                        m.d.sync += byte_idx.eq(byte_idx + 1)

            with m.State("START_GATE"):
                m.d.sync += [
                    gate_time.eq(rx_shift),
                    gate_timer.eq(rx_shift),
                    gate_active_sync.eq(1),
                ]
                m.next = "COUNTING"

            with m.State("COUNTING"):
                m.d.sync += gate_timer.eq(gate_timer - 1)
                with m.If(gate_timer == 0):
                    m.d.sync += gate_active_sync.eq(0)
                    m.next = "WAIT_DONE"

            with m.State("WAIT_DONE"):
                with m.If(gate_done_sync):
                    # Reassemble and subtract in sync domain
                    ec_start = Cat(start_ec_lo, start_ec_mid, start_ec_hi)
                    ec_end = Cat(end_ec_lo, end_ec_mid, end_ec_hi)
                    gc_start = Cat(start_gc_lo, start_gc_mid, start_gc_hi)
                    gc_end = Cat(end_gc_lo, end_gc_mid, end_gc_hi)

                    total_edges = Signal(32)
                    delta_gc = Signal(32)
                    m.d.comb += [
                        total_edges.eq(ec_end - ec_start),
                        delta_gc.eq(gc_end - gc_start),
                    ]
                    m.d.sync += [
                        result_edges.eq(total_edges),
                        result_gate.eq(delta_gc),
                        byte_idx.eq(0),
                        tx_shift.eq(Cat(
                            Const(PLL_OUT_FREQ, 32),
                            delta_gc,
                            total_edges,
                        )),
                    ]
                    m.next = "SEND_RESULT"

            with m.State("SEND_RESULT"):
                m.d.comb += [
                    self.in_fifo.w_data.eq(tx_shift[-8:]),
                    self.in_fifo.w_en.eq(self.in_fifo.w_rdy),
                ]
                with m.If(self.in_fifo.w_rdy):
                    m.d.sync += tx_shift.eq(tx_shift << 8)
                    with m.If(byte_idx == 11):
                        m.next = "IDLE"
                    with m.Else():
                        m.d.sync += byte_idx.eq(byte_idx + 1)

        return m


class FreqCounterInterface:
    """Host-side interface for the frequency counter."""

    def __init__(self, interface, logger):
        self.lower = interface
        self.logger = logger

    async def measure(self, gate_cycles):
        """Measure frequency over gate_cycles (in 48 MHz system clock cycles).

        Returns (edge_count, gate_cycles_actual, sample_freq_hz).
        """
        await self.lower.write(struct.pack(">I", gate_cycles))
        await self.lower.flush()

        data = await self.lower.read(12)
        if len(data) < 12:
            raise RuntimeError(f"Expected 12 bytes, got {len(data)}")

        edge_count, gate_actual, sample_freq = struct.unpack(">III", bytes(data))
        return edge_count, gate_actual, sample_freq


class FreqCounterApplet(GlasgowApplet):
    logger = logging.getLogger(__name__)
    help = "measure signal frequency using PLL + DDR edge counter"
    description = """
    Measure the frequency of a digital signal using a high-speed edge counter.

    Uses the iCE40 PLL with DDR I/O for high effective sample rate.
    Free-running counters with snapshot subtraction eliminate all
    conditional logic from the critical path. The FPGA counts edges
    internally and reports the count, avoiding the FIFO overrun issues
    of the standard analyzer applet.
    """
    required_revision = "C0"

    @classmethod
    def add_build_arguments(cls, parser, access):
        super().add_build_arguments(parser, access)
        access.add_pins_argument(parser, "i", default="A7")

    def build(self, target, args):
        self.mux_interface = iface = target.multiplexer.claim_interface(self, args)
        iface.add_subtarget(FreqCounterSubtarget(
            ports=iface.get_port_group(i=args.i),
            in_fifo=iface.get_in_fifo(),
            out_fifo=iface.get_out_fifo(),
        ))

    @classmethod
    def add_run_arguments(cls, parser, access):
        super().add_run_arguments(parser, access)

    async def run(self, device, args):
        iface = await device.demultiplexer.claim_interface(
            self, self.mux_interface, args)
        return FreqCounterInterface(iface, self.logger)

    @classmethod
    def add_interact_arguments(cls, parser):
        parser.add_argument(
            "-t", "--gate-ms", metavar="MS", type=float, default=1000,
            help="gate time in milliseconds (default: 1000)")
        parser.add_argument(
            "-n", "--count", metavar="N", type=int, default=1,
            help="number of measurements (default: 1)")
        parser.add_argument(
            "--json", action="store_true",
            help="output results as JSON")

    async def interact(self, device, args, iface):
        import json as json_mod

        gate_cycles = int(args.gate_ms / 1000.0 * 48_000_000)
        results = []

        for i in range(args.count):
            edges, gate_actual, sample_freq = await iface.measure(gate_cycles)

            gate_seconds = gate_actual / sample_freq if sample_freq > 0 else 0
            if edges > 0 and gate_seconds > 0:
                freq_hz = edges / (2.0 * gate_seconds)
            else:
                freq_hz = 0.0

            result = {
                "edges": edges,
                "gate_cycles": gate_actual,
                "sample_freq_hz": sample_freq,
                "gate_seconds": gate_seconds,
                "freq_hz": freq_hz,
            }
            results.append(result)

            if not args.json:
                if freq_hz >= 1e6:
                    freq_str = f"{freq_hz/1e6:.3f} MHz"
                elif freq_hz >= 1e3:
                    freq_str = f"{freq_hz/1e3:.3f} kHz"
                else:
                    freq_str = f"{freq_hz:.1f} Hz"

                self.logger.info(
                    f"[{i+1}/{args.count}] edges={edges}, "
                    f"gate={gate_seconds*1000:.1f}ms, "
                    f"freq={freq_str}")

        if args.json:
            print(json_mod.dumps(results, indent=2))
