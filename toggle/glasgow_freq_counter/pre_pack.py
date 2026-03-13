# nextpnr pre-pack script: constrain freq-counter fast-domain cells
# near the DDR I/O pin to minimize routing delays.
#
# NOTE: This script requires native nextpnr-ice40, NOT yowasp.
# The yowasp (WebAssembly) build does not support --pre-pack scripts.
#
# For yowasp, use --seed sweeping instead (see install.py).

# Create a rectangular region near I/O pin A5 (tile ~10,9)
region = ctx.createRectangularRegion("fast_logic", 6, 5, 15, 14)

count = 0
for cell_name in ctx.cells:
    name = str(cell_name)
    if any(pat in name for pat in [
        ".ec_lo", ".ec_mid", ".ec_hi", ".ec_c1", ".ec_c2",
        ".gc_lo", ".gc_mid", ".gc_hi", ".gc_c1", ".gc_c2",
        ".edge_01", ".edge_12", ".edges_sum", ".prev_i1",
    ]):
        ctx.constrainCellToRegion(cell_name, "fast_logic")
        count += 1

print(f"Pre-pack: constrained {count} cells to fast_logic region")
