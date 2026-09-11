"""Hardware-free tests for the DKMS splice (local fixture, not /usr/src)."""

from __future__ import annotations

import difflib
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import nct6687_vrm_dkms_inject as inject  # noqa: E402

FIXTURE = Path(__file__).resolve().parent / "fixtures" / "nct6687_snippet.c"


@pytest.fixture
def snippet() -> str:
    return FIXTURE.read_text()


def snippet_patch(tmp_path: Path, snippet: str) -> Path:
    spliced = inject.inject_text(snippet)
    path = tmp_path / "snippet.patch"
    path.write_text(
        "".join(
            difflib.unified_diff(
                snippet.splitlines(keepends=True),
                spliced.splitlines(keepends=True),
                fromfile="a/nct6687.c",
                tofile="b/nct6687.c",
            )
        )
    )
    return path


def test_inject_text_succeeds_once(snippet: str) -> None:
    out = inject.inject_text(snippet)
    assert inject.MARKER in out
    assert out != snippet


def test_second_inject_raises(snippet: str) -> None:
    once = inject.inject_text(snippet)
    with pytest.raises(SystemExit, match="Already injected"):
        inject.inject_text(once)


@pytest.mark.parametrize(
    "old, fragment",
    [
        ("#define IOREGION_LENGTH 4", "IOREGION_LENGTH 4"),
        ("const struct attribute_group *extra_groups[2];", "extra_groups[2]"),
        ("\tstruct mutex update_lock;", "struct mutex update_lock"),
        (
            "static struct nct6687_data *nct6687_update_device(struct device *dev)",
            "nct6687_update_device signature",
        ),
        ("\tnct6687_setup_voltages(data);\n", "nct6687_setup_voltages"),
    ],
)
def test_missing_anchor_names_it(snippet: str, old: str, fragment: str) -> None:
    broken = snippet.replace(old, "/* gone */", 1)
    with pytest.raises(SystemExit, match=re.escape(fragment)):
        inject.inject_text(broken)


def test_extra_groups_grows_to_three(snippet: str) -> None:
    out = inject.inject_text(snippet)
    assert "extra_groups[2]" not in out
    assert "const struct attribute_group *extra_groups[3];" in out


def test_ioregion_length_grows_to_eight(snippet: str) -> None:
    out = inject.inject_text(snippet)
    assert "#define IOREGION_LENGTH 4" not in out
    assert "#define IOREGION_LENGTH 8" in out


def test_vrm_include_spliced(snippet: str) -> None:
    out = inject.inject_text(snippet)
    assert f'#include "{inject.INC_NAME}"' in out
    assert inject.MARKER in out


def test_update_vrm_hook_after_unlock(snippet: str) -> None:
    out = inject.inject_text(snippet)
    unlock = out.index("\tmutex_unlock(&data->update_lock);")
    hook = out.index("\tnct6687_update_vrm(data);")
    ret = out.index("\treturn data;")
    assert unlock < hook < ret


def test_vrm_data_include_spliced(snippet: str) -> None:
    out = inject.inject_text(snippet)
    assert inject.VRM_DATA_INCLUDE in out
    assert out.index(inject.VRM_DATA_INCLUDE) < out.index("\tstruct mutex update_lock;")
    header = ROOT / "dkms" / inject.DATA_NAME
    text = header.read_text()
    assert "bool vrm_enabled;" in text
    assert "int vrm_smbus_page;" in text
    assert "bool vrm_hist_init;" in text


def test_kernels_for_rebuild_puts_running_first() -> None:
    assert inject.kernels_for_rebuild(
        ["6.1.0-lts", "6.12.0-current", "6.13.0-rc"],
        "6.12.0-current",
    ) == ["6.12.0-current", "6.1.0-lts", "6.13.0-rc"]


def test_re_inject_from_stock_restores_data_include(snippet: str, tmp_path: Path) -> None:
    """--install used to copy a new include onto an old spliced nct6687.c."""
    stock = tmp_path / "nct6687.c"
    stock.write_text(snippet)
    first = inject.inject_text(snippet)
    stale = first.replace(inject.VRM_DATA_INCLUDE, "")
    assert inject.DATA_NAME not in stale
    bak = tmp_path / "nct6687.c.pre-vrm"
    bak.write_text(snippet)
    stock.write_text(stale)
    stock.write_text(inject.inject_text(bak.read_text()))
    assert inject.VRM_DATA_INCLUDE in stock.read_text()


def test_forward_decl_is_only_update_vrm(snippet: str) -> None:
    out = inject.inject_text(snippet)
    assert "static void nct6687_update_vrm(struct nct6687_data *data);" in out
    paired = (
        "static struct nct6687_data *nct6687_update_device(struct device *dev);\n"
        "static void nct6687_update_vrm"
    )
    assert paired not in out


def test_kbuild_modules_argv_is_m_tree(tmp_path: Path) -> None:
    headers = tmp_path / "hdr"
    tree = tmp_path / "src"
    argv = inject.kbuild_modules_argv(headers, tree)
    assert argv[:3] == ["make", "-C", str(headers)]
    assert argv[3] == f"M={tree}"
    assert argv[-1] == "modules"
    assert not any(a.startswith("TARGET=") for a in argv)


def test_kbuild_extra_argv_clang(tmp_path: Path) -> None:
    headers = tmp_path / "hdr"
    headers.mkdir()
    (headers / ".config").write_text("CONFIG_CC_IS_CLANG=y\n")
    assert inject.kbuild_extra_argv(headers) == ["LLVM=1"]
    argv = inject.kbuild_modules_argv(headers, tmp_path / "src")
    assert "LLVM=1" in argv


def test_kbuild_extra_argv_not_clang(tmp_path: Path) -> None:
    headers = tmp_path / "hdr"
    headers.mkdir()
    (headers / ".config").write_text("CONFIG_CC_IS_GCC=y\n")
    assert inject.kbuild_extra_argv(headers) == []


def test_stage_m_tree_writes_vrm_files(snippet: str, tmp_path: Path) -> None:
    pkg = tmp_path / "pkg"
    dest = tmp_path / "scratch"
    pkg.mkdir()
    (pkg / "Kbuild").write_text(inject.MINIMAL_KBUILD)
    inject.stage_m_tree(dest, snippet, pkg_dir=pkg, patch=snippet_patch(tmp_path, snippet))
    assert (dest / "Kbuild").read_text() == inject.MINIMAL_KBUILD
    out = (dest / "nct6687.c").read_text()
    assert inject.MARKER in out
    assert inject.VRM_DATA_INCLUDE in out
    for name in inject.VRM_FILES:
        assert (dest / name).is_file()
    assert not (dest / "Makefile").exists()


def test_stage_m_tree_synthesizes_kbuild_in_scratch(snippet: str, tmp_path: Path) -> None:
    pkg = tmp_path / "pkg"
    dest = tmp_path / "scratch"
    pkg.mkdir()
    inject.stage_m_tree(
        dest, snippet, pkg_dir=pkg, synthesize_kbuild=True, patch=snippet_patch(tmp_path, snippet)
    )
    assert (dest / "Kbuild").read_text() == inject.MINIMAL_KBUILD


def test_stage_m_tree_does_not_invent_kbuild_on_pkg(snippet: str, tmp_path: Path) -> None:
    pkg = tmp_path / "pkg"
    pkg.mkdir()
    inject.stage_m_tree(
        pkg, snippet, pkg_dir=pkg, synthesize_kbuild=False, patch=snippet_patch(tmp_path, snippet)
    )
    assert not (pkg / "Kbuild").exists()
    assert inject.MARKER in (pkg / "nct6687.c").read_text()


def test_apply_splice_text_matches_inject_text(snippet: str, tmp_path: Path) -> None:
    patch = snippet_patch(tmp_path, snippet)
    assert inject.apply_splice_text(snippet, patch) == inject.inject_text(snippet)


def test_apply_splice_text_rejects_mismatch(snippet: str, tmp_path: Path) -> None:
    patch = snippet_patch(tmp_path, snippet)
    with pytest.raises(SystemExit, match="rejected"):
        inject.apply_splice_text("not a driver\n", patch)


def test_committed_patch_matches_inject_text_on_stock() -> None:
    stocks = sorted(Path("/usr/src").glob("nct6687d*/nct6687.c.pre-vrm"))
    if not stocks:
        pytest.skip("no stock nct6687.c backup")
    text = stocks[-1].read_text()
    assert inject.apply_splice_text(text) == inject.inject_text(text)
