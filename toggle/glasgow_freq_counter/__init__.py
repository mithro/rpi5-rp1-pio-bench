"""Glasgow frequency counter applet — PLL + DDR for high-speed edge counting.

Measures toggle frequency on a single input pin using:
  - iCE40 PLL: 48 MHz x 3 = 144 MHz fabric clock
  - DDR I/O: SB_IO samples on both PLL clock edges = 288 MHz effective
  - Edge counter: counts transitions in FPGA, sends count over USB

This avoids the USB FIFO overrun issue of the standard analyzer applet
and provides accurate frequency measurement up to ~144 MHz (Nyquist at 288 MHz).

Protocol:
  Host -> FPGA: 4 bytes (gate_time in 48 MHz cycles, big-endian uint32)
  FPGA -> Host: 12 bytes (edge_count uint32 BE, gate_cycles uint32 BE, sample_freq uint32 BE)

The sample_freq reported is the effective DDR rate (2 x PLL freq = 288 MHz).
"""

import logging
import struct
import argparse

from amaranth import *
from amaranth.lib import io
from amaranth.lib.cdc import FFSynchronizer

from ... import *


# PLL parameters: 48 MHz * (DIVF+1) / (DIVR+1) / 2^DIVQ
# Target: 168 MHz  =>  48 * 14 / 1 / 4 = 168 MHz
# VCO = 48 * 14 = 672 MHz (within 533-1066 MHz range)
# With pipelined edge detection, 8-bit segmented counters, and
# sync-domain addition. Achievable max ~170 MHz; 168 MHz is reliable.
PLL_DIVR = 0
PLL_DIVF = 13
PLL_DIVQ = 2
PLL_FILTER_RANGE = 1
PLL_OUT_FREQ = 168_000_000  # 168 MHz fabric clock
EFFECTIVE_SAMPLE_FREQ = PLL_OUT_FREQ * 2  # 336 MHz with DDR

# Counter segmentation: 8-bit fast stage + 24-bit slow stage = 32 bits total.
# The fast stage runs the full carry chain in the critical path.
# The slow stage only increments on registered carry-out from the fast stage,
# so its carry chain is decoupled from the critical path.
# 8-bit carry chain: ~1 ns on iCE40 (vs ~2 ns for 12-bit).
COUNTER_LO_BITS = 8
COUNTER_HI_BITS = 24


class FreqCounterSubtarget(Elaboratable):
    """FPGA gateware for high-speed frequency counting.

    Runs a PLL at 144 MHz with DDR input sampling (288 MHz effective).
    Counts edges during a host-specified gate period, then sends
    the count back over the USB FIFO.

    DDR sampling: The SB_IO primitive in DDR mode captures data on both
    edges of the 144 MHz PLL clock = 288 MHz effective sample rate.
    Each fast clock cycle produces two samples: i[0] (rising edge) and
    i[1] (falling edge). Edge detection checks all consecutive sample
    pairs: prev_i1->i0 and i0->i1, detecting 0-2 edges per cycle.

    Counter architecture: Each 32-bit counter is segmented into a 12-bit
    fast stage and a 20-bit slow stage. Only the 12-bit carry chain is
    in the critical path. The slow stage increments on a registered
    carry-out, fully decoupled from timing.
    """

    def __init__(self, ports, in_fifo, out_fifo):
        self.ports = ports
        self.in_fifo = in_fifo
        self.out_fifo = out_fifo

    def elaborate(self, platform):
        m = Module()

        # === Fast clock domain via PLL (144 MHz) ===
        m.domains += ClockDomain("fast")
        pll_locked = Signal()

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
            o_LOCK=pll_locked,
        )

        # === DDR Input buffer clocked by PLL (288 MHz effective) ===
        # DDRBuffer with i_domain="fast" makes the SB_IO use the 144 MHz
        # PLL clock as INPUT_CLK. The SB_IO captures on both clock edges:
        #   i[0] = rising edge sample  (D_IN_0)
        #   i[1] = falling edge sample (D_IN_1)
        # Both outputs are already registered in the fast domain by the
        # SB_IO + Amaranth's re-registration FFs. No CDC needed.
        m.submodules.i_buffer = i_buffer = io.DDRBuffer("i", self.ports.i,
                                                         i_domain="fast")

        # Each fast clock cycle we get two samples: i[0] and i[1]
        # To detect edges at DDR rate, check transitions between:
        #   1. prev_i1 -> cur_i0  (across clock cycle boundary)
        #   2. cur_i0  -> cur_i1  (within same clock cycle)
        prev_i1 = Signal()
        m.d.fast += prev_i1.eq(i_buffer.i[1])

        # Edge detection with pipeline register to break timing path.
        # The XOR is combinational, then registered before use as enable.
        # Adds 1 cycle latency (7 ns at 144 MHz) — negligible for 1s gate.
        edge_01 = Signal()  # registered edge between prev falling and current rising
        edge_12 = Signal()  # registered edge between current rising and current falling
        m.d.fast += [
            edge_01.eq(prev_i1 ^ i_buffer.i[0]),
            edge_12.eq(i_buffer.i[0] ^ i_buffer.i[1]),
        ]

        # === Segmented edge counters ===
        # Each 32-bit counter is split into a 12-bit fast stage and a
        # 20-bit slow stage. The fast stage carry-out is registered,
        # so only 12 bits of carry chain are in the critical path.
        # The slow stage increments one cycle later on the registered carry.
        ec01_lo = Signal(COUNTER_LO_BITS)
        ec01_hi = Signal(COUNTER_HI_BITS)
        ec01_carry = Signal()

        ec12_lo = Signal(COUNTER_LO_BITS)
        ec12_hi = Signal(COUNTER_HI_BITS)
        ec12_carry = Signal()

        gc_lo = Signal(COUNTER_LO_BITS)
        gc_hi = Signal(COUNTER_HI_BITS)
        gc_carry = Signal()

        counting = Signal()

        # Fast stage: 12-bit increment (critical path)
        with m.If(counting):
            # Gate counter: always increments
            gc_next = Signal(COUNTER_LO_BITS + 1)
            m.d.comb += gc_next.eq(gc_lo + 1)
            m.d.fast += [
                gc_lo.eq(gc_next[:COUNTER_LO_BITS]),
                gc_carry.eq(gc_next[COUNTER_LO_BITS]),
            ]
            # Edge counter 01: increment on edge
            with m.If(edge_01):
                ec01_next = Signal(COUNTER_LO_BITS + 1)
                m.d.comb += ec01_next.eq(ec01_lo + 1)
                m.d.fast += [
                    ec01_lo.eq(ec01_next[:COUNTER_LO_BITS]),
                    ec01_carry.eq(ec01_next[COUNTER_LO_BITS]),
                ]
            with m.Else():
                m.d.fast += ec01_carry.eq(0)
            # Edge counter 12: increment on edge
            with m.If(edge_12):
                ec12_next = Signal(COUNTER_LO_BITS + 1)
                m.d.comb += ec12_next.eq(ec12_lo + 1)
                m.d.fast += [
                    ec12_lo.eq(ec12_next[:COUNTER_LO_BITS]),
                    ec12_carry.eq(ec12_next[COUNTER_LO_BITS]),
                ]
            with m.Else():
                m.d.fast += ec12_carry.eq(0)

        # Slow stage: 20-bit increment on registered carry (not in critical path)
        with m.If(gc_carry):
            m.d.fast += gc_hi.eq(gc_hi + 1)
        with m.If(ec01_carry):
            m.d.fast += ec01_hi.eq(ec01_hi + 1)
        with m.If(ec12_carry):
            m.d.fast += ec12_hi.eq(ec12_hi + 1)

        # === Gate control (sync domain) ===
        gate_time = Signal(32)
        gate_timer = Signal(32)
        byte_idx = Signal(range(12))

        # CDC: gate signals from sync to fast domain
        gate_active_sync = Signal()
        gate_active_fast = Signal()
        m.submodules.cdc_gate = FFSynchronizer(
            gate_active_sync, gate_active_fast, o_domain="fast")

        # CDC: latch counter when gate ends (fast → sync)
        gate_done_fast = Signal()
        gate_done_sync = Signal()
        # Latch each counter half separately — no 32-bit add in fast domain
        latched_ec01_lo = Signal(COUNTER_LO_BITS)
        latched_ec01_hi = Signal(COUNTER_HI_BITS)
        latched_ec12_lo = Signal(COUNTER_LO_BITS)
        latched_ec12_hi = Signal(COUNTER_HI_BITS)
        latched_gc_lo = Signal(COUNTER_LO_BITS)
        latched_gc_hi = Signal(COUNTER_HI_BITS)

        m.submodules.cdc_done = FFSynchronizer(
            gate_done_fast, gate_done_sync, o_domain="sync")

        # Fast domain: counting logic
        with m.If(gate_active_fast & ~counting):
            m.d.fast += [
                counting.eq(1),
                ec01_lo.eq(0), ec01_hi.eq(0), ec01_carry.eq(0),
                ec12_lo.eq(0), ec12_hi.eq(0), ec12_carry.eq(0),
                gc_lo.eq(0), gc_hi.eq(0), gc_carry.eq(0),
                gate_done_fast.eq(0),
            ]
        with m.Elif(~gate_active_fast & counting):
            # Latch raw counter halves — NO addition in fast domain
            m.d.fast += [
                counting.eq(0),
                latched_ec01_lo.eq(ec01_lo),
                latched_ec01_hi.eq(ec01_hi),
                latched_ec12_lo.eq(ec12_lo),
                latched_ec12_hi.eq(ec12_hi),
                latched_gc_lo.eq(gc_lo),
                latched_gc_hi.eq(gc_hi),
                gate_done_fast.eq(1),
            ]

        # Sync domain: reassemble and sum (48 MHz — plenty of timing margin)
        result_edges = Signal(32)
        result_gate = Signal(32)

        # Shift registers for USB protocol
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
                    # Reassemble and sum in sync domain (48 MHz, no timing pressure)
                    edges_01 = Cat(latched_ec01_lo, latched_ec01_hi)
                    edges_12 = Cat(latched_ec12_lo, latched_ec12_hi)
                    gate_full = Cat(latched_gc_lo, latched_gc_hi)
                    total_edges = Signal(32)
                    m.d.comb += total_edges.eq(edges_01 + edges_12)
                    m.d.sync += [
                        result_edges.eq(total_edges),
                        result_gate.eq(gate_full),
                        byte_idx.eq(0),
                        tx_shift.eq(Cat(
                            Const(PLL_OUT_FREQ, 32),
                            gate_full,
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
    help = "measure signal frequency using PLL + edge counter"
    description = """
    Measure the frequency of a digital signal using a high-speed edge counter.

    Uses the iCE40 PLL at 144 MHz with DDR I/O (288 MHz effective sample
    rate), providing accurate frequency measurement up to ~144 MHz
    (Nyquist limit at 288 MHz). The FPGA counts edges internally and
    reports the count, avoiding the FIFO overrun issues of the standard
    analyzer applet.
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

            # Frequency = edges / 2 / (gate_actual / sample_freq)
            # gate_actual is in PLL cycles (144 MHz), sample_freq = 144 MHz
            # DDR gives 2 samples per PLL cycle (288 MHz effective), but
            # edges are already counted at full DDR rate in the FPGA
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
