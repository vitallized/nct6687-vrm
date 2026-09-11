"""Hardware-free tests for upgrade persist (fake prefix, no pacman)."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import nct6687_vrm_dkms_inject as inject  # noqa: E402
import nct6687_vrm_persist as persist  # noqa: E402

FIXTURE = Path(__file__).resolve().parent / "fixtures" / "nct6687_snippet.c"


def _layout(tmp_path: Path) -> persist.PersistLayout:
    return persist.PersistLayout(
        lib=tmp_path / "lib",
        hook_dir=tmp_path / "hooks",
        modprobe_d=tmp_path / "modprobe.d",
    )


def test_install_writes_full_payload(tmp_path: Path) -> None:
    layout = _layout(tmp_path)
    persist.install(ROOT, layout)
    assert (layout.lib / "source.env").read_text().startswith("SOURCE_REPO=")
    assert (layout.lib / "nct6687_vrm_persist.py").is_file()
    assert (layout.lib / "nct6687_vrm_mailbox.h").is_file()
    assert (layout.lib / inject.PATCH_NAME).is_file()
    assert (layout.hook_dir / "nct6687-vrm-preupgrade.hook").is_file()
    assert (layout.hook_dir / "nct6687-vrm-reinject.hook").is_file()
    assert (layout.modprobe_d / "nct6687-vrm.conf").is_file()
    hook = (layout.hook_dir / "nct6687-vrm-reinject.hook").read_text()
    assert "nct6687_vrm_persist.py post_transaction" in hook
    assert "/usr/local/sbin/" not in hook


def test_refresh_updates_hooks(tmp_path: Path) -> None:
    layout = _layout(tmp_path)
    persist.install(ROOT, layout)
    stale = layout.lib / "nct6687-vrm-reinject.hook"
    stale.write_text("# stale\n")
    assert persist.refresh(layout, ROOT) is True
    assert "post_transaction" in stale.read_text()


def test_pre_transaction_clears_unowned(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    layout = _layout(tmp_path)
    persist.install(ROOT, layout)
    pkg = tmp_path / "src"
    pkg.mkdir()
    leftover = pkg / inject.INC_NAME
    leftover.write_text("old\n")
    stock = pkg / "nct6687.c"
    stock.write_text("stock\n")
    monkeypatch.setattr(inject, "_pacman_owned_files", lambda: set())
    rc = persist.pre_transaction(layout, ROOT, pkg_dirs=[pkg])
    assert rc == 0
    assert not leftover.exists()
    assert stock.is_file()


def test_pre_missing_payload_is_zero(tmp_path: Path) -> None:
    layout = _layout(tmp_path)
    assert persist.pre_transaction(layout, repo=tmp_path / "gone") == 0


def test_post_missing_payload_is_one(tmp_path: Path) -> None:
    layout = _layout(tmp_path)
    assert persist.post_transaction(layout, repo=tmp_path / "gone") == 1


def test_post_transaction_splices_and_records_rebuild(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    layout = _layout(tmp_path)
    persist.install(ROOT, layout)
    pkg = tmp_path / "src"
    pkg.mkdir()
    src = pkg / "nct6687.c"
    src.write_text(FIXTURE.read_text())
    rebuilt: list[Path] = []

    import difflib

    spliced = inject.inject_text(FIXTURE.read_text())
    snip_patch = tmp_path / "snippet.patch"
    snip_patch.write_text(
        "".join(
            difflib.unified_diff(
                FIXTURE.read_text().splitlines(keepends=True),
                spliced.splitlines(keepends=True),
                fromfile="a/nct6687.c",
                tofile="b/nct6687.c",
            )
        )
    )
    monkeypatch.setattr(inject, "find_patch", lambda: snip_patch)

    rc = persist.post_transaction(
        layout, ROOT, src=src, rebuild=rebuilt.append
    )
    assert rc == 0
    assert inject.MARKER in src.read_text()
    assert rebuilt == [src]
