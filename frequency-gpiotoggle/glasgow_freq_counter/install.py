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

    # Patch assembly.py nextpnr_opts for manual placement + timing override.
    # Uses native nextpnr-ice40 --pre-pack for placement constraints and
    # --timing-allow-fail since we overclock beyond the timing model.
    assembly_file = glasgow_pkg / "hardware" / "assembly.py"
    assembly_src = assembly_file.read_text()

    pre_pack_path = str(applet_dir / "pre_pack.py")
    desired_opts = (
        f'nextpnr_opts="--placer heap '
        f'--pre-pack {pre_pack_path} '
        f'--timing-allow-fail",'
    )

    # Find and replace any existing nextpnr_opts line
    import re
    match = re.search(r'nextpnr_opts="[^"]*",', assembly_src)
    if match:
        old_full = match.group(0)
        if old_full != desired_opts:
            assembly_src = assembly_src.replace(old_full, desired_opts)
            assembly_file.write_text(assembly_src)
            print(f"Patched assembly.py nextpnr_opts with --pre-pack + --timing-allow-fail")
        else:
            print("assembly.py already patched")
    else:
        print("WARNING: Could not find nextpnr_opts to patch")

    # Also set AMARANTH_USE_YOWASP=0 to force native toolchain
    # Glasgow's toolchain finder checks for this
    toolchain_file = glasgow_pkg / "hardware" / "toolchain.py"
    if toolchain_file.exists():
        tc_src = toolchain_file.read_text()
        if "AMARANTH_USE_YOWASP" not in tc_src:
            print("Note: Set AMARANTH_USE_YOWASP=0 in env to use native nextpnr")

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
