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
# Target: 128 MHz  =>  48 * 8 / 1 / 3 ~= 128 MHz
#   Actually: 48 * (DIVF+1) / (DIVR+1) / 2^DIVQ
#   48 * 8 / 1 / 3 = 128 (but DIVQ must be integer, 2^DIVQ)
#   48 * 16 / 1 / 8 = 96 MHz (DIVF=15, DIVQ=3) - too low
#   Try: VCO = 48 * (10+1) = 528 MHz, / 2^2 = 132 MHz
#   VCO range 533-1066 MHz: 528 is below minimum!
#   Try: VCO = 48 * (12+1) = 624 MHz, / 2^2 = 156 MHz - too high for fabric
#   Try: VCO = 48 * (11+1) = 576 MHz, / 2^2 = 144 MHz - fails timing w/ DDR
#   Try: VCO = 48 * (22+1) = 1104 MHz, / 2^3 = 138 MHz
#        VCO 1104 > 1066 MHz maximum!
#   Try: VCO = 48 * (21+1) = 1056 MHz, / 2^3 = 132 MHz - VCO in range!
#   nextpnr reports 132-142 MHz achievable with DDR logic
# Target: 132 MHz => 48 * 22 / 1 / 8 = 132 MHz
# VCO = 48 * 22 = 1056 MHz (within 533-1066 MHz range)
PLL_DIVR = 0
PLL_DIVF = 21
PLL_DIVQ = 3
PLL_FILTER_RANGE = 1
PLL_OUT_FREQ = 132_000_000  # 132 MHz fabric clock
EFFECTIVE_SAMPLE_FREQ = PLL_OUT_FREQ * 2  # 264 MHz with DDR


class FreqCounterSubtarget(Elaboratable):
    """FPGA gateware for high-speed frequency counting.

    Runs a PLL at 144 MHz with DDR input sampling (288 MHz effective).
    Counts edges during a host-specified gate period, then sends
    the count back over the USB FIFO.

    DDR sampling: The SB_IO primitive in DDR mode captures data on both
    edges of the INPUT_CLK. With i_domain="fast" (144 MHz PLL), the
    SB_IO samples at both rising and falling edges = 288 MHz effective.
    Each fast clock cycle produces two samples: i[0] (rising) and i[1]
    (falling). Edge detection checks transitions between all consecutive
    sample pairs: prev_i1->i0 and i0->i1.
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
        # To detect edges at 288 MHz, check transitions between:
        #   1. prev_i1 -> cur_i0  (across clock cycle boundary)
        #   2. cur_i0  -> cur_i1  (within same clock cycle)
        prev_i1 = Signal()
        m.d.fast += prev_i1.eq(i_buffer.i[1])

        # Count edges: 0, 1, or 2 edges per fast clock cycle
        edge_01 = Signal()  # edge between prev falling and current rising
        edge_12 = Signal()  # edge between current rising and current falling
        m.d.comb += [
            edge_01.eq(prev_i1 ^ i_buffer.i[0]),
            edge_12.eq(i_buffer.i[0] ^ i_buffer.i[1]),
        ]

        # === Edge counters (two independent 32-bit counters) ===
        # Split into two counters to halve critical path depth.
        # Each counter only needs a 1-bit increment (edge detected or not).
        # Sum the two counters when latching results.
        edge_count_01 = Signal(32)  # counts edges at rising-edge boundaries
        edge_count_12 = Signal(32)  # counts edges at falling-edge boundaries
        gate_count = Signal(32)     # actual gate cycles in fast domain
        counting = Signal()

        with m.If(counting):
            m.d.fast += gate_count.eq(gate_count + 1)
            with m.If(edge_01):
                m.d.fast += edge_count_01.eq(edge_count_01 + 1)
            with m.If(edge_12):
                m.d.fast += edge_count_12.eq(edge_count_12 + 1)

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
        latched_edges = Signal(32)
        latched_gate = Signal(32)

        m.submodules.cdc_done = FFSynchronizer(
            gate_done_fast, gate_done_sync, o_domain="sync")

        # Fast domain: counting logic
        with m.If(gate_active_fast & ~counting):
            m.d.fast += [
                counting.eq(1),
                edge_count_01.eq(0),
                edge_count_12.eq(0),
                gate_count.eq(0),
                gate_done_fast.eq(0),
            ]
        with m.Elif(~gate_active_fast & counting):
            m.d.fast += [
                counting.eq(0),
                latched_edges.eq(edge_count_01 + edge_count_12),
                latched_gate.eq(gate_count),
                gate_done_fast.eq(1),
            ]

        # Sync domain: state machine
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
                    m.d.sync += [
                        result_edges.eq(latched_edges),
                        result_gate.eq(latched_gate),
                        byte_idx.eq(0),
                        # Pack: [edges_32][gate_32][sample_freq_32] = 96 bits
                        # Cat builds LSB-first, tx_shift[-8:] sends MSB first
                        tx_shift.eq(Cat(
                            Const(PLL_OUT_FREQ, 32),
                            latched_gate,
                            latched_edges,
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
