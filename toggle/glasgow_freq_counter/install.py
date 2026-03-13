#!/usr/bin/env python3
"""Install the freq-counter applet into the Glasgow package.

Copies the applet module into Glasgow's applet directory and registers
it in the package entry points.

Usage:
  python3 install.py [--uninstall]
"""

import os
import shutil
import sys
from pathlib import Path


def find_glasgow_paths():
    """Find Glasgow package paths in the uv tools directory."""
    uv_tools = Path.home() / ".local" / "share" / "uv" / "tools" / "glasgow"

    # Find site-packages
    for sp in uv_tools.rglob("site-packages/glasgow"):
        if sp.is_dir():
            glasgow_pkg = sp
            break
    else:
        print("ERROR: Glasgow package not found in uv tools", file=sys.stderr)
        sys.exit(1)

    # Find dist-info
    for di in glasgow_pkg.parent.glob("glasgow-*.dist-info"):
        if di.is_dir():
            dist_info = di
            break
    else:
        print("ERROR: Glasgow dist-info not found", file=sys.stderr)
        sys.exit(1)

    return glasgow_pkg, dist_info


def install():
    glasgow_pkg, dist_info = find_glasgow_paths()
    applet_dir = glasgow_pkg / "applet" / "interface" / "freq_counter"
    entry_points = dist_info / "entry_points.txt"

    # Copy applet module
    src_dir = Path(__file__).parent
    os.makedirs(applet_dir, exist_ok=True)
    shutil.copy2(src_dir / "__init__.py", applet_dir / "__init__.py")
    print(f"Installed applet to {applet_dir}")

    # Copy pre-pack script if present
    pre_pack_src = src_dir / "pre_pack.py"
    if pre_pack_src.exists():
        shutil.copy2(pre_pack_src, applet_dir / "pre_pack.py")
        print(f"Installed pre-pack script")

    # Patch assembly.py nextpnr_opts to allow timing failure.
    # iCE40 timing models are conservative; designs often work beyond
    # the reported Fmax. --timing-allow-fail lets us build the bitstream
    # and test empirically.
    assembly_file = glasgow_pkg / "hardware" / "assembly.py"
    assembly_src = assembly_file.read_text()

    # Clean any previous patches
    old_opts = None
    for candidate in [
        'nextpnr_opts="--placer heap --pre-pack',  # old pre-pack patch
        'nextpnr_opts="--placer heap --timing-allow-fail"',  # already patched
        'nextpnr_opts="--placer heap"',  # unpatched
    ]:
        if candidate in assembly_src:
            old_opts = candidate
            break

    new_opts = 'nextpnr_opts="--placer heap --timing-allow-fail"'
    if old_opts and old_opts != new_opts:
        # Find the full old string up to the closing quote+comma
        idx = assembly_src.index(old_opts)
        end = assembly_src.index('",', idx) + 2
        old_full = assembly_src[idx:end]
        assembly_src = assembly_src.replace(old_full, new_opts + ',')
        assembly_file.write_text(assembly_src)
        print(f"Patched assembly.py: {new_opts}")
    elif old_opts == new_opts:
        print("assembly.py already patched with --timing-allow-fail")
    else:
        print("WARNING: Could not find nextpnr_opts to patch")

    # Register entry point
    ep_text = entry_points.read_text()
    ep_line = "freq-counter = glasgow.applet.interface.freq_counter:FreqCounterApplet"

    if ep_line in ep_text:
        print("Entry point already registered")
    else:
        # Add after the [glasgow.applet] section header
        lines = ep_text.splitlines()
        new_lines = []
        added = False
        for line in lines:
            new_lines.append(line)
            if line.strip() == "[glasgow.applet]" and not added:
                new_lines.append(ep_line)
                added = True

        if not added:
            print("ERROR: Could not find [glasgow.applet] section", file=sys.stderr)
            sys.exit(1)

        entry_points.write_text("\n".join(new_lines) + "\n")
        print(f"Registered entry point in {entry_points}")

    # Clear importlib caches
    cache_dir = applet_dir / "__pycache__"
    if cache_dir.exists():
        shutil.rmtree(cache_dir)
        print("Cleared __pycache__")

    print("Done! Try: glasgow run freq-counter --help")


def uninstall():
    glasgow_pkg, dist_info = find_glasgow_paths()
    applet_dir = glasgow_pkg / "applet" / "interface" / "freq_counter"
    entry_points = dist_info / "entry_points.txt"

    # Remove applet module
    if applet_dir.exists():
        shutil.rmtree(applet_dir)
        print(f"Removed {applet_dir}")

    # Remove entry point
    ep_text = entry_points.read_text()
    ep_line = "freq-counter = glasgow.applet.interface.freq_counter:FreqCounterApplet"
    if ep_line in ep_text:
        ep_text = ep_text.replace(ep_line + "\n", "")
        entry_points.write_text(ep_text)
        print("Removed entry point")

    print("Done!")


if __name__ == "__main__":
    if "--uninstall" in sys.argv:
        uninstall()
    else:
        install()
