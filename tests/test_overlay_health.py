"""Overlay health against tmp trees: checkout, /usr/local lib, DKMS tree, sysfs."""

from __future__ import annotations

import shutil
from dataclasses import dataclass
from pathlib import Path

import pytest

import nct6687_vrm_dkms_inject as cli
import nct6687_vrm_overlay_health as health
import nct6687_vrm_overlay_tree as tree
import nct6687_vrm_persist as manifest

ROOT = Path(__file__).resolve().parents[1]
STOCK_R257 = Path(__file__).resolve().parent / "fixtures" / "nct6687_r257.c"


@dataclass
class World:
    checkout: Path
    layout: manifest.PersistLayout
    src: Path
    sys_module: Path

    def probe(self, **kw) -> health.OverlayHealth:
        kw.setdefault("repo", self.checkout)
        kw.setdefault("lib", self.layout.lib)
        return health.probe(self.src, sys_module=self.sys_module, **kw)


@pytest.fixture
def world(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> World:
    """Persist installed from a frozen checkout; live tree spliced; vrm=Y loaded."""
    checkout = tmp_path / "checkout"
    for f in manifest.PAYLOAD:
        (checkout / f.rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / f.rel, checkout / f.rel)
    layout = manifest.PersistLayout(
        lib=tmp_path / "lib", hook_dir=tmp_path / "hooks", modprobe_d=tmp_path / "modprobe.d"
    )
    manifest.install(checkout, layout)
    monkeypatch.setattr(manifest, "REPO_ROOT", checkout)
    monkeypatch.setattr(manifest, "INSTALLED_LIB", layout.lib)
    monkeypatch.setattr(tree, "_pacman_owned_files", lambda: set())
    pkg = tmp_path / "usr" / "src" / "nct6687d-r257"
    pkg.mkdir(parents=True)
    src = pkg / "nct6687.c"
    shutil.copy2(STOCK_R257, src)
    tree.inject(src)
    sys_module = tmp_path / "sys" / "module" / "nct6687"
    (sys_module / "parameters").mkdir(parents=True)
    (sys_module / "parameters" / "vrm").write_text("Y\n")
    return World(checkout, layout, src, sys_module)


def _state(entries: list[health.FileHealth], name: str) -> str:
    return next(f.state for f in entries if f.name == name)


def test_fresh_splice_is_ok(world: World) -> None:
    h = world.probe()
    assert h.ok
    assert h.spliced
    assert {f.name for f in h.tree} == {*manifest.DKMS_FILES, "nct6687.c"}
    assert {f.name for f in h.payload} == {f.name for f in manifest.PAYLOAD}
    assert h.live_vrm == "Y"
    assert len(h.expected_extras) == len(manifest.DKMS_FILES) + 1
    assert not h.unexpected_extras


def test_stale_tree_file_after_checkout_and_lib_update(world: World) -> None:
    """The bug: checkout and /usr/local moved on, /usr/src kept the old include."""
    (world.checkout / "dkms" / tree.INC_NAME).write_text("/* newer VRM */\n")
    assert manifest.refresh(world.layout, world.checkout)
    h = world.probe()
    assert h.payload_ok
    assert _state(h.tree, tree.INC_NAME) == health.STALE
    assert not h.tree_ok
    assert not h.ok


def test_install_remedy_clears_stale_tree(world: World) -> None:
    (world.checkout / "dkms" / tree.INC_NAME).write_text("/* newer VRM */\n")
    manifest.refresh(world.layout, world.checkout)
    tree.inject(world.src)
    assert world.probe().ok


def test_live_c_not_equal_to_patch_on_backup_is_stale(world: World) -> None:
    text = world.src.read_text().replace('#include "nct6687_vrm_data.h"\n', "", 1)
    world.src.write_text(text)
    h = world.probe()
    assert _state(h.tree, "nct6687.c") == health.STALE
    assert not h.ok


def test_backup_the_patch_rejects_is_stale(world: World) -> None:
    tree.backup_path(world.src).write_text("not a driver\n")
    h = world.probe()
    entry = next(f for f in h.tree if f.name == "nct6687.c")
    assert entry.state == health.STALE
    assert "rejected" in entry.detail


def test_missing_tree_file_after_solo_pre_transaction(world: World) -> None:
    (world.src.parent / tree.INC_NAME).unlink()
    h = world.probe()
    assert _state(h.tree, tree.INC_NAME) == health.MISSING
    assert not h.ok


def test_stock_tree_skips_tree_checks(world: World) -> None:
    tree.restore(world.src)
    tree.backup_path(world.src).unlink()
    h = world.probe()
    assert not h.spliced
    assert h.tree == []
    assert h.ok


def test_payload_stale_against_source_repo(world: World) -> None:
    (world.checkout / "nct6687_vrm_persist.py").write_text("# newer\n")
    h = world.probe()
    assert _state(h.payload, "nct6687_vrm_persist.py") == health.STALE
    assert not h.payload_ok
    assert not h.ok


def test_payload_missing_new_module(world: World) -> None:
    """An older hook refreshed /usr/local with its shorter file list."""
    (world.layout.lib / "nct6687_vrm_overlay_health.py").unlink()
    h = world.probe()
    assert _state(h.payload, "nct6687_vrm_overlay_health.py") == health.MISSING
    assert not h.ok


def test_payload_not_installed(world: World, tmp_path: Path) -> None:
    h = world.probe(lib=tmp_path / "empty-lib")
    assert not h.payload_installed
    assert not h.ok


def test_without_source_repo_payload_compares_running_tree(world: World) -> None:
    (world.layout.lib / "source.env").unlink()
    assert world.probe().ok
    (world.layout.lib / "nct6687_vrm_persist.py").write_text("# stale\n")
    h = world.probe()
    assert _state(h.payload, "nct6687_vrm_persist.py") == health.STALE


def test_unexpected_unowned_file_fails(
    world: World, monkeypatch: pytest.MonkeyPatch
) -> None:
    surprise = world.src.parent / "Kbuild"
    surprise.write_text("obj-m += nct6687.o\n")
    owned = {world.src.as_posix().lstrip("/")}
    monkeypatch.setattr(tree, "_pacman_owned_files", lambda: owned)
    h = world.probe()
    assert h.unexpected_extras == [surprise]
    assert tree.backup_path(world.src) in h.expected_extras
    assert not h.ok


def test_spliced_source_but_stock_module_loaded(world: World) -> None:
    (world.sys_module / "parameters" / "vrm").unlink()
    h = world.probe()
    assert h.live_module_stock
    assert not h.ok


def test_print_health_adapter(world: World, capsys: pytest.CaptureFixture[str]) -> None:
    assert cli.print_health(world.probe()) == 0
    assert "STALE" not in capsys.readouterr().out
    (world.src.parent / tree.INC_NAME).write_text("old\n")
    assert cli.print_health(world.probe()) == 1
    out = capsys.readouterr().out
    assert f"DKMS tree {tree.INC_NAME}: STALE" in out
    assert cli.INSTALL_REMEDY in out
