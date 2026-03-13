# nextpnr pre-pack script: constrain freq-counter fast-domain cells
# near the DDR I/O pin to minimize routing delays.
#
# Cell naming: Glasgow's multiplexer prefixes signals with "U$NN."
# After synthesis, Yosys renames to e.g. "U$22.ec_lo[0]".
# The ctx.cells iterator yields KeyValue objects with .first = name string.

import sys, os

log_path = "/home/tim/rpi5-rp1-pio-bench/toggle/tmp/pre_pack.log"
os.makedirs(os.path.dirname(log_path), exist_ok=True)

# Create a rectangular region near I/O pin A5 (tile ~10,9)
region = ctx.createRectangularRegion("fast_logic", 7, 6, 14, 13)

patterns = [
    "ec_lo", "ec_mid", "ec_hi", "ec_c1", "ec_c2",
    "gc_lo", "gc_mid", "gc_hi", "gc_c1", "gc_c2",
    "edge_01", "edge_12", "edges_sum",
    "prev_i1",
]

count = 0
constrained = []
for kv in ctx.cells:
    name = kv.first  # .first is the cell name string
    if any(pat in name for pat in patterns):
        ctx.constrainCellToRegion(name, "fast_logic")
        constrained.append(name)
        count += 1

msg = f"Pre-pack: constrained {count} cells to fast_logic region (7,6)-(14,13)\n"
for c in sorted(constrained):
    msg += f"  {c}\n"

with open(log_path, "w") as f:
    f.write(msg)
print(msg, file=sys.stderr)
