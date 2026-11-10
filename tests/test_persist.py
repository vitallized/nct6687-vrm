"""Hardware-free tests for the payload manifest and upgrade persist (fake prefix, no pacman)."""

from __future__ import annotations

import ast
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

import nct6687_vrm_overlay_splice as splice
import nct6687_vrm_overlay_tree as tree
import nct6687_vrm_persist as persist

ROOT = Path(__file__).resolve().parents[1]
STOCK_R263 = Path(__file__).resolve().parent / "fixtures" / "nct6687_r263.c"


def _layout(tmp_path: Path) -> persist.PersistLayout:
    return persist.PersistLayout(
        lib=tmp_path / "lib",
        hook_dir=tmp_path / "hooks",
        modprobe_d=tmp_path / "modprobe.d",
    )


def test_checkout_payload_is_complete() -> None:
    assert persist.payload_complete(ROOT)


def test_payload_covers_every_overlay_import() -> None:
    """A new sibling module the CLI or hooks import must be installed too."""
    names = {f.name for f in persist.PAYLOAD}
    for f in persist.PAYLOAD:
        if not f.name.endswith(".py"):
            continue
        for node in ast.walk(ast.parse((ROOT / f.rel).read_text())):
            if isinstance(node, ast.Import):
                mods = [a.name for a in node.names]
            elif isinstance(node, ast.ImportFrom) and node.module:
                mods = [node.module]
            else:
                continue
            for mod in mods:
                if mod.startswith("nct6687_vrm"):
                    assert f"{mod}.py" in names, f"{f.name} imports {mod}"


def test_persist_imports_without_sibling_modules(tmp_path: Path) -> None:
    """Old hooks refresh /usr/local with a shorter list; persist must still load."""
    shutil.copy2(ROOT / "nct6687_vrm_persist.py", tmp_path / "nct6687_vrm_persist.py")
    subprocess.run(
        [
            sys.executable,
            "-I",
            "-c",
            "import sys; sys.path.insert(0, '.'); import nct6687_vrm_persist",
        ],
        cwd=tmp_path,
        check=True,
    )


def test_dkms_files_are_the_dkms_flagged_entries() -> None:
    assert persist.DKMS_FILES == tuple(
        Path(f.rel).name for f in persist.PAYLOAD if f.rel.startswith("dkms/")
    )


def test_resolve_prefers_running_tree_over_lib(tmp_path: Path) -> None:
    repo, lib = tmp_path / "repo", tmp_path / "lib"
    (repo / "dkms").mkdir(parents=True)
    lib.mkdir()
    (lib / tree.INC_NAME).write_text("lib\n")
    assert persist.resolve(tree.INC_NAME, repo=repo, lib=lib) == lib / tree.INC_NAME
    (repo / "dkms" / tree.INC_NAME).write_text("repo\n")
    assert persist.resolve(tree.INC_NAME, repo=repo, lib=lib) == repo / "dkms" / tree.INC_NAME
    with pytest.raises(SystemExit, match="Missing"):
        persist.resolve(persist.PATCH_NAME, repo=repo, lib=lib)


def test_source_repo_parsing(tmp_path: Path) -> None:
    lib = tmp_path / "lib"
    lib.mkdir()
    assert persist.source_repo(lib) is None
    (lib / "source.env").write_text(f"SOURCE_REPO={tmp_path}\n")
    assert persist.source_repo(lib) == tmp_path
    (lib / "source.env").write_text(f"SOURCE_REPO={tmp_path / 'moved'}\n")
    assert persist.source_repo(lib) is None


def test_install_writes_full_payload(tmp_path: Path) -> None:
    layout = _layout(tmp_path)
    persist.install(ROOT, layout)
    assert (layout.lib / "source.env").read_text().startswith("SOURCE_REPO=")
    for f in persist.PAYLOAD:
        assert (layout.lib / f.name).read_bytes() == (ROOT / f.rel).read_bytes()
    assert (layout.hook_dir / "nct6687-vrm-preupgrade.hook").is_file()
    assert (layout.hook_dir / "nct6687-vrm-reinject.hook").is_file()
    assert (layout.modprobe_d / "nct6687-vrm.conf").is_file()
    hook = (layout.hook_dir / "nct6687-vrm-reinject.hook").read_text()
    assert "nct6687_vrm_persist.py post_transaction" in hook
    assert "Type = Path" in hook
    assert "usr/lib/modules/*/build/include/" in hook
    assert "/usr/local/sbin/" not in hook
    pre = (layout.hook_dir / "nct6687-vrm-preupgrade.hook").read_text()
    assert "Target = nct6687d-dkms*" in pre
    assert "nct6687_vrm_persist.py pre_transaction" in pre


def test_refresh_updates_hooks(tmp_path: Path) -> None:
    layout = _layout(tmp_path)
    persist.install(ROOT, layout)
    stale = layout.lib / "nct6687-vrm-reinject.hook"
    stale.write_text("# stale\n")
    assert persist.refresh(layout, ROOT) is True
    assert "post_transaction" in stale.read_text()


def test_refresh_fills_modules_an_older_hook_never_copied(tmp_path: Path) -> None:
    layout = _layout(tmp_path)
    persist.install(ROOT, layout)
    (layout.lib / "nct6687_vrm_overlay_tree.py").unlink()
    assert not persist.payload_installed(layout)
    assert persist.refresh(layout, ROOT) is True
    assert persist.payload_installed(layout)


def test_refresh_updates_etc_hooks_when_present(tmp_path: Path) -> None:
    layout = _layout(tmp_path)
    persist.install(ROOT, layout)
    stale = layout.hook_dir / "nct6687-vrm-reinject.hook"
    stale.write_text("# stale etc hook\n")
    assert persist.refresh(layout, ROOT) is True
    assert stale.read_text() == (ROOT / "pacman-hook" / "nct6687-vrm-reinject.hook").read_text()


def test_install_leaves_an_existing_vrm0_conf(tmp_path: Path) -> None:
    layout = _layout(tmp_path)
    layout.modprobe_d.mkdir(parents=True)
    conf = layout.modprobe_d / "nct6687-vrm.conf"
    conf.write_text("options nct6687 vrm=0\n")
    persist.install(ROOT, layout)
    assert conf.read_text() == "options nct6687 vrm=0\n"


def test_want_vrm_later_file_wins(tmp_path: Path) -> None:
    modprobe_d = tmp_path / "modprobe.d"
    modprobe_d.mkdir()
    (modprobe_d / "10-on.conf").write_text("options nct6687 vrm=1\n")
    (modprobe_d / "20-off.conf").write_text("options nct6687 vrm=0\n")
    assert tree.want_vrm_enabled(False, modprobe_d) is False
    (modprobe_d / "20-off.conf").write_text("options nct6687 vrm=0\noptions nct6687 vrm=1\n")
    assert tree.want_vrm_enabled(False, modprobe_d) is True
    assert tree.want_vrm_enabled(True, modprobe_d) is True


def test_pre_transaction_restores_spliced_source_before_clear(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    layout = _layout(tmp_path)
    persist.install(ROOT, layout)
    pkg = tmp_path / "src"
    pkg.mkdir()
    stock = STOCK_R263.read_text()
    src = pkg / "nct6687.c"
    src.write_text(splice.apply(stock))
    bak = tree.backup_path(src)
    bak.write_text(stock)
    (pkg / tree.INC_NAME).write_text("inc\n")
    monkeypatch.setattr(tree, "_pacman_owned_files", lambda: set())
    assert persist.pre_transaction(layout, ROOT, pkg_dirs=[pkg]) == 0
    assert src.read_text() == stock
    assert not (pkg / tree.INC_NAME).exists()
    assert not bak.exists()


def test_pre_transaction_keeps_includes_when_spliced_without_backup(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    layout = _layout(tmp_path)
    persist.install(ROOT, layout)
    pkg = tmp_path / "src"
    pkg.mkdir()
    src = pkg / "nct6687.c"
    src.write_text(splice.apply(STOCK_R263.read_text()))
    inc = pkg / tree.INC_NAME
    inc.write_text("inc\n")
    monkeypatch.setattr(tree, "_pacman_owned_files", lambda: set())
    assert persist.pre_transaction(layout, ROOT, pkg_dirs=[pkg]) == 0
    assert inc.read_text() == "inc\n"
    assert splice.is_spliced(src.read_text())


def test_refresh_does_not_touch_modprobe_conf(tmp_path: Path) -> None:
    layout = _layout(tmp_path)
    persist.install(ROOT, layout)
    conf = layout.modprobe_d / "nct6687-vrm.conf"
    conf.write_text("options nct6687 vrm=0\n")
    persist.refresh(layout, ROOT)
    assert conf.read_text() == "options nct6687 vrm=0\n"


def test_refresh_does_not_invent_etc_hooks(tmp_path: Path) -> None:
    layout = _layout(tmp_path)
    assert persist.refresh(layout, ROOT) is True
    assert not (layout.hook_dir / "nct6687-vrm-reinject.hook").exists()
    assert not (layout.hook_dir / "nct6687-vrm-preupgrade.hook").exists()


def test_pre_transaction_clears_leftover_tree_without_c(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    src_root = tmp_path / "usr" / "src"
    leftover = src_root / "nct6687d-dkms-git-r42"
    leftover.mkdir(parents=True)
    inc = leftover / tree.INC_NAME
    hdr = leftover / "nct6687_vrm_data.h"
    bak = leftover / "nct6687.c.pre-vrm"
    kbuild = leftover / "Kbuild"
    inc.write_text("stale-inc\n")
    hdr.write_text("stale-h\n")
    bak.write_text("old-stock\n")
    kbuild.write_text("obj-m += nct6687.o\n")
    layout = persist.PersistLayout(
        lib=tmp_path / "lib",
        hook_dir=tmp_path / "hooks",
        modprobe_d=tmp_path / "modprobe.d",
        src_globs=(str(src_root / "nct6687d*"),),
    )
    persist.install(ROOT, layout)
    monkeypatch.setattr(tree, "_pacman_owned_files", lambda: set())
    rc = persist.pre_transaction(layout, ROOT)
    assert rc == 0
    assert leftover.is_dir()
    assert not inc.exists()
    assert not hdr.exists()
    assert not bak.exists()
    assert kbuild.read_text() == "obj-m += nct6687.o\n"


def test_clear_unowned_keeps_package_owned_in_leftover(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    leftover = tmp_path / "nct6687d-dkms-git-r42"
    leftover.mkdir()
    inc = leftover / tree.INC_NAME
    owned = leftover / "Makefile"
    inc.write_text("stale\n")
    owned.write_text("obj-m += nct6687.o\n")
    rel = owned.as_posix().lstrip("/")
    monkeypatch.setattr(tree, "_pacman_owned_files", lambda: {rel})
    tree.clear_unowned([leftover])
    assert leftover.is_dir()
    assert not inc.exists()
    assert owned.read_text() == "obj-m += nct6687.o\n"


def test_pre_transaction_clears_unowned(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    layout = _layout(tmp_path)
    persist.install(ROOT, layout)
    pkg = tmp_path / "src"
    pkg.mkdir()
    leftover = pkg / tree.INC_NAME
    leftover.write_text("old\n")
    stock = pkg / "nct6687.c"
    stock.write_text("stock\n")
    monkeypatch.setattr(tree, "_pacman_owned_files", lambda: set())
    rc = persist.pre_transaction(layout, ROOT, pkg_dirs=[pkg])
    assert rc == 0
    assert not leftover.exists()
    assert stock.is_file()


def test_pre_missing_payload_is_zero(tmp_path: Path) -> None:
    layout = _layout(tmp_path)
    assert persist.pre_transaction(layout, repo=tmp_path / "gone") == 0


def test_post_missing_src_is_zero(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    layout = _layout(tmp_path)
    persist.install(ROOT, layout)

    def gone() -> Path:
        raise SystemExit("No /usr/src/nct6687d*/nct6687.c found")

    monkeypatch.setattr(tree, "find_src", gone)
    assert persist.post_transaction(layout, ROOT) == 0


def test_post_splice_reject_is_one(tmp_path: Path) -> None:
    layout = _layout(tmp_path)
    persist.install(ROOT, layout)
    pkg = tmp_path / "src"
    pkg.mkdir()
    src = pkg / "nct6687.c"
    src.write_text("upstream rewrote everything\n")
    assert persist.post_transaction(layout, ROOT, src=src, rebuild=lambda _s: None) == 1
    assert not splice.is_spliced(src.read_text())


def test_post_missing_payload_is_one(tmp_path: Path) -> None:
    layout = _layout(tmp_path)
    assert persist.post_transaction(layout, repo=tmp_path / "gone") == 1


def _ready_splice(tmp_path: Path) -> tuple[persist.PersistLayout, Path]:
    layout = _layout(tmp_path)
    persist.install(ROOT, layout)
    pkg = tmp_path / "src"
    pkg.mkdir()
    src = pkg / "nct6687.c"
    shutil.copy2(STOCK_R263, src)
    return layout, src


def test_post_transaction_splices_and_records_rebuild(tmp_path: Path) -> None:
    layout, src = _ready_splice(tmp_path)
    rebuilt: list[Path] = []
    rc = persist.post_transaction(layout, ROOT, src=src, rebuild=rebuilt.append)
    assert rc == 0
    assert src.read_text() == splice.apply(STOCK_R263.read_text())
    assert rebuilt == [src]


def test_post_rebuild_systemexit_is_one(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    layout, src = _ready_splice(tmp_path)

    def boom(_src: Path) -> None:
        raise SystemExit("DKMS rebuild skipped every kernel (no headers?)")

    rc = persist.post_transaction(layout, ROOT, src=src, rebuild=boom)
    assert rc == 1
    assert splice.is_spliced(src.read_text())
    assert "persist:" in capsys.readouterr().err


def test_post_default_rebuild_systemexit_is_one(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    layout, src = _ready_splice(tmp_path)

    def boom(_src: Path, reload: bool = False, load_vrm: bool = False) -> None:
        raise SystemExit("DKMS failed for running kernel 7.2.3")

    monkeypatch.setattr(tree, "rebuild", boom)
    rc = persist.post_transaction(layout, ROOT, src=src)
    assert rc == 1
    assert splice.is_spliced(src.read_text())
    assert "persist:" in capsys.readouterr().err


def _conf_options(text: str) -> list[str]:
    toks: list[str] = []
    for line in text.splitlines():
        parts = line.split("#", 1)[0].split()
        if len(parts) >= 3 and parts[:2] == ["options", "nct6687"]:
            toks += parts[2:]
    return toks


def test_shipped_conf_sets_every_vrm_option_once(tmp_path: Path) -> None:
    # The conf is the reference: each module_param of the VRM include appears
    # once, with a legend above it. A new option has to be added there too.
    src = (ROOT / "dkms" / "nct6687_vrm.inc.c").read_text()
    params = re.findall(r"^module_param\((\w+),", src, re.M)
    conf = (ROOT / "pacman-hook" / "nct6687-vrm.conf").read_text()
    names = [tok.split("=", 1)[0] for tok in _conf_options(conf)]
    assert sorted(names) == sorted(params)
    for name in params:
        assert f"# {name}:" in conf, name
    # Shipped values the README and install path rely on.
    opts = dict(tok.split("=", 1) for tok in _conf_options(conf))
    assert (opts["vrm"], opts["vrm_fast"], opts["vrm_mux_keep"]) == ("1", "31", "0")
    # The legend's own text never reads as a setting.
    (tmp_path / "nct6687-vrm.conf").write_text(conf)
    assert tree.want_vrm_enabled(False, tmp_path) is True
