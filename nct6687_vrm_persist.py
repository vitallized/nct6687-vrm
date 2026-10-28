#!/usr/bin/env python3
"""Upgrade persist: survive a distro nct6687d package upgrade.

Owns the payload manifest: every overlay file, where it lives in the
checkout, and whether it also lands in the DKMS tree or /etc.

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

# Top-level imports stay stdlib-only. An older installed hook refreshes
# /usr/local with its own (shorter) file list, so sibling overlay modules
# may be missing until this module's refresh() runs.

REPO_ROOT = Path(__file__).resolve().parent
INSTALLED_LIB = Path("/usr/local/lib/nct6687-vrm")


@dataclass(frozen=True)
class PayloadFile:
    rel: str
    dkms: bool = False
    etc: str = ""

    @property
    def name(self) -> str:
        return Path(self.rel).name


# rel: path in the checkout. Every file is copied flat into the lib dir.
# dkms: also copied beside nct6687.c. etc: PersistLayout dir install copies it to.
PAYLOAD: tuple[PayloadFile, ...] = (
    PayloadFile("nct6687_vrm_dkms_inject.py"),
    PayloadFile("nct6687_vrm_persist.py"),
    PayloadFile("nct6687_vrm_overlay_splice.py"),
    PayloadFile("nct6687_vrm_overlay_tree.py"),
    PayloadFile("nct6687_vrm_overlay_health.py"),
    PayloadFile("dkms/nct6687_vrm.inc.c", dkms=True),
    PayloadFile("dkms/nct6687_vrm_decode.h", dkms=True),
    PayloadFile("dkms/nct6687_vrm_data.h", dkms=True),
    PayloadFile("dkms/nct6687_vrm_mailbox.h", dkms=True),
    PayloadFile("patches/vrm-splice.patch"),
    PayloadFile("pacman-hook/nct6687-vrm-preupgrade.hook", etc="hook_dir"),
    PayloadFile("pacman-hook/nct6687-vrm-reinject.hook", etc="hook_dir"),
    PayloadFile("pacman-hook/nct6687-vrm.conf", etc="modprobe_d"),
)

DKMS_FILES: tuple[str, ...] = tuple(f.name for f in PAYLOAD if f.dkms)
PATCH_NAME = "vrm-splice.patch"


def payload_file(name: str) -> PayloadFile:
    for f in PAYLOAD:
        if f.name == name:
            return f
    raise KeyError(name)


def locate(root: Path, name: str) -> Path | None:
    """A payload file under a checkout (rel path) or a flat lib dir (name)."""
    f = payload_file(name)
    for path in (root / f.rel, root / f.name):
        if path.is_file():
            return path
    return None


def resolve(name: str, *, repo: Path | None = None, lib: Path | None = None) -> Path:
    """The copy this process stages: running tree first, then the installed lib."""
    roots = (repo or REPO_ROOT, lib or INSTALLED_LIB)
    for root in roots:
        path = locate(root, name)
        if path is not None:
            return path
    raise SystemExit(f"Missing {name} (tried: {', '.join(str(r) for r in roots)})")


def source_repo(lib: Path | None = None) -> Path | None:
    """Checkout from source.env. None if unset or not a directory — do not invent one."""
    env = (lib or INSTALLED_LIB) / "source.env"
    if not env.is_file():
        return None
    for line in env.read_text().splitlines():
        if line.startswith("SOURCE_REPO="):
            repo = Path(line.split("=", 1)[1].strip())
            return repo if repo.is_dir() else None
    return None


@dataclass
class PersistLayout:
    """Production paths. Tests pass a fake prefix."""

    lib: Path = INSTALLED_LIB
    hook_dir: Path = Path("/etc/pacman.d/hooks")
    modprobe_d: Path = Path("/etc/modprobe.d")
    src_globs: tuple[str, ...] = field(default_factory=lambda: ("/usr/src/nct6687d*",))


def _tree():
    import nct6687_vrm_overlay_tree as tree

    return tree


def payload_complete(repo: Path) -> bool:
    return all((repo / f.rel).is_file() for f in PAYLOAD)


def payload_installed(layout: PersistLayout) -> bool:
    return all((layout.lib / f.name).is_file() for f in PAYLOAD)


def refresh(layout: PersistLayout, repo: Path | None = None) -> bool:
    """Copy the full payload from SOURCE_REPO. False if there is nothing to copy.

    Updates /etc hooks only where install already wrote them; never the
    modprobe.d conf (the user may have set vrm=0 there).
    """
    repo = repo or source_repo(layout.lib)
    if repo is None or not payload_complete(repo):
        return False
    layout.lib.mkdir(parents=True, exist_ok=True)
    changed = False
    for f in PAYLOAD:
        src = repo / f.rel
        dests = [layout.lib / f.name]
        etc_dst = layout.hook_dir / f.name
        if f.etc == "hook_dir" and etc_dst.is_file():
            dests.append(etc_dst)
        for dst in dests:
            if dst.is_file() and src.read_bytes() == dst.read_bytes():
                continue
            shutil.copy2(src, dst)
            changed = True
    if changed:
        print("Refreshed", layout.lib, "from", repo, file=sys.stderr)
    return True


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
    tree = _tree()
    removed = tree.clear_unowned(
        pkg_dirs if pkg_dirs is not None else tree.src_dirs(layout.src_globs)
    )
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
    tree = _tree()
    if src is None:
        try:
            src = tree.find_src()
        except SystemExit as exc:
            print("persist: no nct6687d sources — skip splice:", exc, file=sys.stderr)
            return 0
    print("Re-applying VRM splice to", src)
    try:
        tree.inject(src)
    except SystemExit as exc:
        print("persist: splice failed (driver layout changed?):", exc, file=sys.stderr)
        return 1
    try:
        if rebuild is None:
            tree.rebuild(src, reload=False, load_vrm=tree.want_vrm_enabled(False))
        else:
            rebuild(src)
    except SystemExit as exc:
        print("persist: rebuild failed (tree left spliced):", exc, file=sys.stderr)
        return 1
    return 0


def install(repo: Path | None = None, layout: PersistLayout | None = None) -> None:
    layout = layout or PersistLayout()
    repo = repo or REPO_ROOT
    if not payload_complete(repo):
        raise SystemExit(f"Incomplete persist payload in {repo}")
    layout.lib.mkdir(parents=True, exist_ok=True)
    layout.hook_dir.mkdir(parents=True, exist_ok=True)
    layout.modprobe_d.mkdir(parents=True, exist_ok=True)
    for f in PAYLOAD:
        shutil.copy2(repo / f.rel, layout.lib / f.name)
    (layout.lib / "source.env").write_text(f"SOURCE_REPO={repo}\n")
    for f in PAYLOAD:
        if f.etc:
            shutil.copy2(layout.lib / f.name, getattr(layout, f.etc) / f.name)
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
    if os.geteuid() != 0:
        print("Need root", file=sys.stderr)
        return 1
    if args.command == "install":
        install()
        return 0
    if args.command == "pre_transaction":
        return pre_transaction()
    return post_transaction()


if __name__ == "__main__":
    raise SystemExit(main())
