"""Hardware-free tests for DKMS tree staging and the inject CLI (tmp trees only)."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

import nct6687_vrm_dkms_inject as cli
import nct6687_vrm_overlay_splice as splice
import nct6687_vrm_overlay_tree as tree
import nct6687_vrm_persist as manifest

STOCK_R257 = Path(__file__).resolve().parent / "fixtures" / "nct6687_r257.c"


@pytest.fixture
def stock() -> str:
    return STOCK_R257.read_text()


@pytest.fixture
def pkg(tmp_path: Path, stock: str) -> Path:
    """A fake distro DKMS tree holding stock r257."""
    d = tmp_path / "nct6687d-r257"
    d.mkdir()
    (d / "nct6687.c").write_text(stock)
    (d / "Kbuild").write_text(tree.MINIMAL_KBUILD)
    return d


def test_kernels_for_rebuild_puts_running_first() -> None:
    assert tree.kernels_for_rebuild(
        ["6.1.0-lts", "6.12.0-current", "6.13.0-rc"],
        "6.12.0-current",
    ) == ["6.12.0-current", "6.1.0-lts", "6.13.0-rc"]


def test_kernels_for_rebuild_unions_header_only_kernels() -> None:
    assert tree.kernels_for_rebuild(
        ["6.12.0-current"],
        "6.12.0-current",
        extra=["6.13.0-new"],
    ) == ["6.12.0-current", "6.13.0-new"]


def test_src_dirs_finds_leftover_tree_without_nct6687_c(tmp_path: Path) -> None:
    src_root = tmp_path / "usr" / "src"
    leftover = src_root / "nct6687d-dkms-git-r88"
    leftover.mkdir(parents=True)
    (leftover / tree.INC_NAME).write_text("stale\n")
    live = src_root / "nct6687d-dkms-git-r99"
    live.mkdir()
    (live / "nct6687.c").write_text("stock\n")
    found = tree.src_dirs((str(src_root / "nct6687d*"),))
    assert found == [leftover, live]


def test_modules_with_headers_lists_build_dirs(tmp_path: Path) -> None:
    (tmp_path / "6.12.0" / "build").mkdir(parents=True)
    (tmp_path / "6.13.0" / "not-build").mkdir(parents=True)
    assert tree.modules_with_headers(tmp_path) == ["6.12.0"]


def test_rebuild_skips_running_kernel_without_headers(tmp_path: Path) -> None:
    missing = tmp_path / "no-build"
    reason = tree.rebuild_skip_reason("7.2.3-old", "7.2.3-old", missing)
    assert reason is not None
    assert "Skipping running kernel" in reason
    have = tmp_path / "build"
    have.mkdir()
    assert tree.rebuild_skip_reason("7.2.4-new", "7.2.3-old", have) is None


def test_kbuild_modules_argv_is_m_tree(tmp_path: Path) -> None:
    headers = tmp_path / "hdr"
    src = tmp_path / "src"
    argv = tree.kbuild_modules_argv(headers, src)
    assert argv[:3] == ["make", "-C", str(headers)]
    assert argv[3] == f"M={src}"
    assert argv[-1] == "modules"
    assert not any(a.startswith("TARGET=") for a in argv)


def test_kbuild_extra_argv_clang(tmp_path: Path) -> None:
    headers = tmp_path / "hdr"
    headers.mkdir()
    (headers / ".config").write_text("CONFIG_CC_IS_CLANG=y\n")
    assert tree.kbuild_extra_argv(headers) == ["LLVM=1"]
    assert "LLVM=1" in tree.kbuild_modules_argv(headers, tmp_path / "src")


def test_kbuild_extra_argv_not_clang(tmp_path: Path) -> None:
    headers = tmp_path / "hdr"
    headers.mkdir()
    (headers / ".config").write_text("CONFIG_CC_IS_GCC=y\n")
    assert tree.kbuild_extra_argv(headers) == []


def test_stage_m_tree_writes_overlay_files(stock: str, pkg: Path, tmp_path: Path) -> None:
    dest = tmp_path / "scratch"
    tree.stage_m_tree(dest, stock, pkg_dir=pkg)
    assert (dest / "Kbuild").read_text() == tree.MINIMAL_KBUILD
    assert (dest / "nct6687.c").read_text() == splice.apply(stock)
    for name in manifest.DKMS_FILES:
        assert (dest / name).read_bytes() == manifest.resolve(name).read_bytes()
    assert not (dest / "Makefile").exists()


def test_stage_m_tree_synthesizes_kbuild_in_scratch(stock: str, tmp_path: Path) -> None:
    bare = tmp_path / "bare"
    bare.mkdir()
    dest = tmp_path / "scratch"
    tree.stage_m_tree(dest, stock, pkg_dir=bare, synthesize_kbuild=True)
    assert (dest / "Kbuild").read_text() == tree.MINIMAL_KBUILD


def test_stage_m_tree_does_not_invent_kbuild_on_pkg(stock: str, tmp_path: Path) -> None:
    bare = tmp_path / "bare"
    bare.mkdir()
    tree.stage_m_tree(bare, stock, pkg_dir=bare)
    assert not (bare / "Kbuild").exists()
    assert splice.is_spliced((bare / "nct6687.c").read_text())


def test_stage_m_tree_reject_leaves_tree_untouched(pkg: Path) -> None:
    with pytest.raises(SystemExit, match="rejected"):
        tree.stage_m_tree(pkg, "not a driver\n", pkg_dir=pkg)
    assert sorted(p.name for p in pkg.iterdir()) == ["Kbuild", "nct6687.c"]


def test_inject_fresh_backs_up_stock_and_splices(stock: str, pkg: Path) -> None:
    src = pkg / "nct6687.c"
    tree.inject(src)
    assert tree.backup_path(src).read_text() == stock
    assert src.read_text() == splice.apply(stock)
    for name in manifest.DKMS_FILES:
        assert (pkg / name).is_file()


def test_inject_replaces_stale_prevrm_when_stock_has_no_marker(
    stock: str, pkg: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    src = pkg / "nct6687.c"
    tree.backup_path(src).write_text("old package stock\n")
    tree.inject(src)
    assert tree.backup_path(src).read_text() == stock
    assert src.read_text() == splice.apply(stock)
    assert "Replaced stale backup" in capsys.readouterr().out


def test_inject_spliced_with_backup_resplices_from_backup(stock: str, pkg: Path) -> None:
    """--install on an old splice rebuilds nct6687.c from .pre-vrm with today's patch."""
    src = pkg / "nct6687.c"
    tree.backup_path(src).write_text(stock)
    old_splice = splice.apply(stock).replace('#include "nct6687_vrm_data.h"\n', "")
    src.write_text(old_splice)
    tree.inject(src)
    assert tree.backup_path(src).read_text() == stock
    assert src.read_text() == splice.apply(stock)


def test_inject_spliced_without_backup_only_refreshes_files(stock: str, pkg: Path) -> None:
    src = pkg / "nct6687.c"
    spliced = splice.apply(stock)
    src.write_text(spliced)
    (pkg / tree.INC_NAME).write_text("stale\n")
    tree.inject(src)
    assert src.read_text() == spliced
    assert not tree.backup_path(src).exists()
    assert (pkg / tree.INC_NAME).read_bytes() == manifest.resolve(tree.INC_NAME).read_bytes()


def test_restore_puts_stock_back_and_drops_overlay_files(stock: str, pkg: Path) -> None:
    src = pkg / "nct6687.c"
    tree.inject(src)
    tree.restore(src)
    assert src.read_text() == stock
    for name in manifest.DKMS_FILES:
        assert not (pkg / name).exists()


def test_pre_upgrade_flag_is_gone(monkeypatch: pytest.MonkeyPatch) -> None:
    """Solo clear_unowned strips the live overlay; only the paired hooks may run it."""
    monkeypatch.setattr(sys, "argv", ["nct6687_vrm_dkms_inject.py", "--pre-upgrade"])
    with pytest.raises(SystemExit) as exc:
        cli.main()
    assert exc.value.code == 2


def test_bare_run_refuses(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sys, "argv", ["nct6687_vrm_dkms_inject.py"])
    assert cli.main() == 2
