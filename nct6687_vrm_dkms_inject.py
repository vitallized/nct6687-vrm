#!/usr/bin/env python3
"""Splice eSIO PMBus VRM hwmon attrs into the nct6687d DKMS tree, then rebuild.

Bulk VRM logic lives in dkms/nct6687_vrm.inc.c (copied beside nct6687.c and
#include'd), with raw→millisi math in nct6687_vrm_decode.h, the eSIO
mailbox in nct6687_vrm_mailbox.h, and nct6687_data members in
nct6687_vrm_data.h. patches/vrm-splice.patch adds the small hooks to
nct6687.c, so upstream driver churn rejects a few hunks — not a 500-line
embedded blob.

Safety: --verify-compile; --install loads vrm=0; update_vrm outside update_lock;
GT hidden unless vrm_gt=1; modprobe -r must succeed.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

import nct6687_vrm_overlay_health as overlay_health
import nct6687_vrm_overlay_splice as splice
import nct6687_vrm_overlay_tree as tree
import nct6687_vrm_persist as manifest

INSTALL_REMEDY = "sudo python3 ./nct6687_vrm_dkms_inject.py --install"
PERSIST_REMEDY = "sudo bash pacman-hook/install.sh"


def warn_if_hook_stale() -> None:
    installed = manifest.INSTALLED_LIB / Path(__file__).name
    here = Path(__file__).resolve()
    if not installed.is_file() or here == installed:
        return
    try:
        if installed.read_bytes() == here.read_bytes():
            return
    except OSError:
        return
    print(
        "WARNING: /usr/local inject script differs from this checkout.\n"
        f"  {PERSIST_REMEDY}\n"
        "  (the pacman hook auto-refreshes if source.env still points here)",
        file=sys.stderr,
    )


def _line(prefix: str, f: overlay_health.FileHealth) -> str:
    return f"{prefix} {f.name}: {f.state}" + (f" ({f.detail})" if f.detail else "")


def print_health(h: overlay_health.OverlayHealth) -> int:
    """Print an OverlayHealth. Exit 1 if anything is stale, missing, or unowned."""
    print("DKMS source:", h.src)
    print("Injected:", h.spliced)
    for f in h.tree:
        print(_line("DKMS tree", f))
    if not h.tree_ok:
        print("DKMS tree overlay is stale or incomplete. Re-splice with:")
        print(" ", INSTALL_REMEDY)
    if h.expected_extras:
        print("Expected extras (cleared automatically on the next package upgrade):")
        for p in h.expected_extras:
            print(" ", p)
    if h.unexpected_extras:
        print("Unexpected unowned files (pacman will refuse the next upgrade):")
        for p in h.unexpected_extras:
            print(" ", p)
    elif not h.expected_extras:
        print("Unowned files: none")
    print("This script:", Path(__file__).resolve())
    if h.payload_installed:
        for f in h.payload:
            print(_line("Installed", f))
        if not h.payload_ok:
            print("Installed payload is stale. Refresh with:")
            print(" ", PERSIST_REMEDY)
    else:
        print(f"Installed payload: missing (run {PERSIST_REMEDY})")
    print(h.source_env if h.source_env is not None else "source.env: missing")
    if h.live_module_stock:
        print(
            "Live module: stock (no vrm param) — source is spliced but "
            "this kernel's .ko was not rebuilt"
        )
    elif h.live_vrm is not None:
        print("Live module param vrm=" + h.live_vrm)
    return 0 if h.ok else 1


def check(src: Path | None = None) -> int:
    return print_health(overlay_health.probe(src))


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--verify-compile", action="store_true")
    ap.add_argument("--install", action="store_true")
    ap.add_argument("--restore", action="store_true")
    ap.add_argument("--rebuild", action="store_true")
    ap.add_argument(
        "--reinject",
        action="store_true",
        help="Re-apply patch after package upgrade; skips verify-compile",
    )
    ap.add_argument(
        "--check",
        action="store_true",
        help="Print overlay health; exit 1 if stale, missing, or unowned files",
    )
    ap.add_argument("--no-reload", action="store_true")
    ap.add_argument("--enable-vrm", action="store_true")
    ap.add_argument("--src", type=Path, default=None)
    args = ap.parse_args()
    if args.check:
        return check(args.src)
    if not any(
        [args.verify_compile, args.install, args.restore, args.rebuild, args.reinject]
    ):
        ap.print_help()
        print(
            "\nRefusing bare run. Use --verify-compile first, then --install.",
            file=sys.stderr,
        )
        return 2
    if args.verify_compile:
        warn_if_hook_stale()
        tree.verify_compile(args.src or tree.find_src())
        print(
            "\nCompile OK. Install with: sudo python3", Path(__file__).name, "--install"
        )
        return 0
    if os.geteuid() != 0:
        print("Need root", file=sys.stderr)
        return 1
    src = args.src or tree.find_src()
    reload = not args.no_reload
    load_vrm = tree.want_vrm_enabled(args.enable_vrm)
    if args.restore:
        tree.restore(src)
        if args.rebuild:
            tree.rebuild(src, reload=reload, load_vrm=False)
        return 0
    if args.reinject:
        tree.reinject(src, reload=reload, load_vrm=load_vrm)
        return 0
    if args.install:
        warn_if_hook_stale()
        if not splice.is_spliced(src.read_text()):
            print("Step 1/3: verify-compile...")
            tree.verify_compile(src)
        print("Step 2/3: inject (re-splice hooks from .pre-vrm if already patched)...")
        tree.inject(src)
        print("Step 3/3: dkms install...")
        tree.rebuild(src, reload=reload, load_vrm=load_vrm)
        return 0
    warn_if_hook_stale()
    tree.rebuild(src, reload=reload, load_vrm=load_vrm)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
