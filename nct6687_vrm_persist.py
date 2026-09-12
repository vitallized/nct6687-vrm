#!/usr/bin/env python3
"""Upgrade persist: survive a distro nct6687d package upgrade.

pre_transaction / post_transaction are the pacman hook Exec lines.
install registers those hooks and copies the full payload.
--install (first splice + reload) stays on nct6687_vrm_dkms_inject.py.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path

import nct6687_vrm_dkms_inject as inject

REPO_ROOT = Path(__file__).resolve().parent

# (name in lib, path relative to repo)
_PAYLOAD: tuple[tuple[str, str], ...] = (
    ("nct6687_vrm_dkms_inject.py", "nct6687_vrm_dkms_inject.py"),
    ("nct6687_vrm_persist.py", "nct6687_vrm_persist.py"),
    ("nct6687_vrm.inc.c", "dkms/nct6687_vrm.inc.c"),
    ("nct6687_vrm_decode.h", "dkms/nct6687_vrm_decode.h"),
    ("nct6687_vrm_data.h", "dkms/nct6687_vrm_data.h"),
    ("nct6687_vrm_mailbox.h", "dkms/nct6687_vrm_mailbox.h"),
    ("vrm-splice.patch", "patches/vrm-splice.patch"),
    ("nct6687-vrm-preupgrade.hook", "pacman-hook/nct6687-vrm-preupgrade.hook"),
    ("nct6687-vrm-reinject.hook", "pacman-hook/nct6687-vrm-reinject.hook"),
    ("nct6687-vrm.conf", "pacman-hook/nct6687-vrm.conf"),
)


@dataclass
class PersistLayout:
    """Production paths. Tests pass a fake prefix."""

    lib: Path = Path("/usr/local/lib/nct6687-vrm")
    hook_dir: Path = Path("/etc/pacman.d/hooks")
    modprobe_d: Path = Path("/etc/modprobe.d")
    src_globs: tuple[str, ...] = field(default_factory=lambda: ("/usr/src/nct6687d*/nct6687.c",))


def source_repo(layout: PersistLayout) -> Path | None:
    env = layout.lib / "source.env"
    if not env.is_file():
        return None
    for line in env.read_text().splitlines():
        if line.startswith("SOURCE_REPO="):
            repo = Path(line.split("=", 1)[1].strip())
            return repo if repo.is_dir() else None
    return None


def payload_complete(repo: Path) -> bool:
    return all((repo / rel).is_file() for _name, rel in _PAYLOAD)


def refresh(layout: PersistLayout, repo: Path | None = None) -> bool:
    """Copy the full payload from SOURCE_REPO. False if there is nothing to copy."""
    repo = repo or source_repo(layout)
    if repo is None or not payload_complete(repo):
        return False
    layout.lib.mkdir(parents=True, exist_ok=True)
    changed = False
    for name, rel in _PAYLOAD:
        src = repo / rel
        dst = layout.lib / name
        if dst.is_file() and src.read_bytes() == dst.read_bytes():
            continue
        shutil.copy2(src, dst)
        changed = True
    if changed:
        print("Refreshed", layout.lib, "from", repo, file=sys.stderr)
    return True


def payload_installed(layout: PersistLayout) -> bool:
    need = (
        layout.lib / "nct6687_vrm_persist.py",
        layout.lib / "nct6687_vrm_dkms_inject.py",
        layout.lib / inject.PATCH_NAME,
        layout.lib / inject.INC_NAME,
    )
    return all(p.is_file() for p in need)


def pre_transaction(
    layout: PersistLayout | None = None,
    repo: Path | None = None,
    pkg_dirs: list[Path] | None = None,
) -> int:
    layout = layout or PersistLayout()
    refresh(layout, repo)
    if not payload_installed(layout):
        print("persist: missing payload — skip pre-upgrade clear", file=sys.stderr)
        return 0
    removed = inject.clear_unowned(pkg_dirs)
    if not removed:
        print("No unowned files to remove")
    return 0


def post_transaction(
    layout: PersistLayout | None = None,
    repo: Path | None = None,
    *,
    src: Path | None = None,
    rebuild=None,
) -> int:
    layout = layout or PersistLayout()
    refresh(layout, repo)
    if not payload_installed(layout):
        print("persist: missing payload — cannot re-splice", file=sys.stderr)
        return 1
    if src is None:
        try:
            src = inject.find_src()
        except SystemExit as exc:
            print("persist: no nct6687d sources — skip splice:", exc, file=sys.stderr)
            return 0
    print("Re-applying VRM splice to", src)
    try:
        inject.inject(src)
    except SystemExit as exc:
        print("persist: splice failed (driver layout changed?):", exc, file=sys.stderr)
        return 1
    if rebuild is None:
        inject.rebuild(
            src,
            reload=False,
            load_vrm=inject.want_vrm_enabled(False),
        )
    else:
        rebuild(src)
    return 0


def install(repo: Path | None = None, layout: PersistLayout | None = None) -> None:
    layout = layout or PersistLayout()
    repo = repo or REPO_ROOT
    if not payload_complete(repo):
        raise SystemExit(f"Incomplete persist payload in {repo}")
    layout.lib.mkdir(parents=True, exist_ok=True)
    layout.hook_dir.mkdir(parents=True, exist_ok=True)
    layout.modprobe_d.mkdir(parents=True, exist_ok=True)
    for name, rel in _PAYLOAD:
        shutil.copy2(repo / rel, layout.lib / name)
    (layout.lib / "source.env").write_text(f"SOURCE_REPO={repo}\n")
    shutil.copy2(layout.lib / "nct6687-vrm-preupgrade.hook", layout.hook_dir / "nct6687-vrm-preupgrade.hook")
    shutil.copy2(layout.lib / "nct6687-vrm-reinject.hook", layout.hook_dir / "nct6687-vrm-reinject.hook")
    shutil.copy2(layout.lib / "nct6687-vrm.conf", layout.modprobe_d / "nct6687-vrm.conf")
    print("Installed persist payload in", layout.lib)
    print("Hooks:", layout.hook_dir / "nct6687-vrm-preupgrade.hook")
    print("       ", layout.hook_dir / "nct6687-vrm-reinject.hook")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "command",
        choices=("pre_transaction", "post_transaction", "install"),
    )
    args = ap.parse_args()
    if args.command == "install":
        if os.geteuid() != 0:
            print("Need root", file=sys.stderr)
            return 1
        install()
        return 0
    if args.command in ("pre_transaction", "post_transaction") and os.geteuid() != 0:
        print("Need root", file=sys.stderr)
        return 1
    if args.command == "pre_transaction":
        return pre_transaction()
    return post_transaction()


if __name__ == "__main__":
    raise SystemExit(main())
