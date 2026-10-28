"""Overlay health: is the live DKMS splice and installed payload current?

Read-only. probe() returns an OverlayHealth; the CLI prints it.

References:
- DKMS tree overlay files and nct6687.c are compared to what this process
  would stage (manifest.resolve: running tree, then /usr/local). From the
  checkout that is the checkout; from /usr/local it is the install.
- The /usr/local payload is compared to SOURCE_REPO, or to the running tree
  when source.env has none.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import nct6687_vrm_overlay_splice as splice
import nct6687_vrm_overlay_tree as tree
import nct6687_vrm_persist as manifest

OK, STALE, MISSING = "ok", "STALE", "missing"


@dataclass(frozen=True)
class FileHealth:
    name: str
    path: Path
    state: str
    detail: str = ""


@dataclass
class OverlayHealth:
    src: Path
    spliced: bool
    expected_extras: list[Path] = field(default_factory=list)
    unexpected_extras: list[Path] = field(default_factory=list)
    tree: list[FileHealth] = field(default_factory=list)
    payload_installed: bool = False
    payload: list[FileHealth] = field(default_factory=list)
    source_env: str | None = None
    module_loaded: bool = False
    live_vrm: str | None = None

    @property
    def tree_ok(self) -> bool:
        return all(f.state == OK for f in self.tree)

    @property
    def payload_ok(self) -> bool:
        return self.payload_installed and all(f.state == OK for f in self.payload)

    @property
    def live_module_stock(self) -> bool:
        """Source is spliced but the loaded .ko has no vrm param."""
        return self.spliced and self.module_loaded and self.live_vrm is None

    @property
    def ok(self) -> bool:
        return (
            not self.unexpected_extras
            and self.tree_ok
            and self.payload_ok
            and not self.live_module_stock
        )


def _compare(name: str, path: Path, reference: Path | None) -> FileHealth:
    if not path.is_file():
        return FileHealth(name, path, MISSING)
    if reference is None:
        return FileHealth(name, path, STALE, "no reference copy")
    if path.read_bytes() != reference.read_bytes():
        return FileHealth(name, path, STALE, f"differs from {reference}")
    return FileHealth(name, path, OK)


def _reference(name: str, repo: Path, lib: Path) -> Path | None:
    try:
        return manifest.resolve(name, repo=repo, lib=lib)
    except SystemExit:
        return None


def _probe_tree(src: Path, repo: Path, lib: Path) -> list[FileHealth]:
    out = [
        _compare(name, src.parent / name, _reference(name, repo, lib))
        for name in manifest.DKMS_FILES
    ]
    bak = tree.backup_path(src)
    if not bak.is_file():
        return out
    patch = _reference(manifest.PATCH_NAME, repo, lib)
    try:
        if patch is None:
            raise SystemExit(f"Missing {manifest.PATCH_NAME}")
        want = splice.apply(bak.read_text(), patch)
    except SystemExit as exc:
        out.append(FileHealth(src.name, src, STALE, f"{bak.name}: {exc}"))
        return out
    if src.read_text() != want:
        out.append(
            FileHealth(src.name, src, STALE, f"not the splice patch applied to {bak.name}")
        )
    else:
        out.append(FileHealth(src.name, src, OK))
    return out


def _probe_payload(lib: Path, repo: Path) -> list[FileHealth]:
    reference_root = manifest.source_repo(lib) or repo
    return [
        _compare(f.name, lib / f.name, manifest.locate(reference_root, f.name))
        for f in manifest.PAYLOAD
    ]


def probe(
    src: Path | None = None,
    *,
    repo: Path | None = None,
    lib: Path | None = None,
    sys_module: Path = Path("/sys/module/nct6687"),
) -> OverlayHealth:
    repo = repo or manifest.REPO_ROOT
    lib = lib or manifest.INSTALLED_LIB
    src = src or tree.find_src()
    health = OverlayHealth(src=src, spliced=splice.is_spliced(src.read_text()))
    for p in tree.unowned_toplevel(src.parent):
        (health.expected_extras if tree.is_overlay_extra(p) else health.unexpected_extras).append(p)
    if health.spliced:
        health.tree = _probe_tree(src, repo, lib)
    health.payload_installed = (lib / "nct6687_vrm_dkms_inject.py").is_file()
    if health.payload_installed:
        health.payload = _probe_payload(lib, repo)
    env = lib / "source.env"
    health.source_env = env.read_text().rstrip() if env.is_file() else None
    health.module_loaded = sys_module.is_dir()
    vrm = sys_module / "parameters" / "vrm"
    if vrm.is_file():
        health.live_vrm = vrm.read_text().strip()
    return health
