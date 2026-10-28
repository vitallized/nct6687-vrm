"""Overlay the distro nct6687d DKMS tree: stage the splice, then rebuild.

Finds the live tree, separates package-owned files from overlay extras,
writes a DKMS M= tree (spliced nct6687.c + overlay files), and drives
dkms build/install plus the modprobe reload.

Safety: verify_compile uses a scratch tree; reload loads vrm=0 unless asked;
modprobe -r must succeed.
"""

from __future__ import annotations

import glob
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

import nct6687_vrm_overlay_splice as splice
import nct6687_vrm_persist as manifest

PACMAN_LOCAL = Path("/var/lib/pacman/local")
MINIMAL_KBUILD = "obj-m += nct6687.o\n"
INC_NAME = "nct6687_vrm.inc.c"


def backup_path(src: Path) -> Path:
    return Path(str(src) + ".pre-vrm")


def _pacman_owned_files(pkg_prefix: str = "nct6687d-dkms") -> set[str]:
    """Paths from the local alpm files db (no `pacman` CLI — safe in hooks)."""
    owned: set[str] = set()
    if not PACMAN_LOCAL.is_dir():
        return owned
    for files_db in PACMAN_LOCAL.glob(f"{pkg_prefix}*/files"):
        in_files = False
        try:
            text = files_db.read_text()
        except OSError:
            continue
        for line in text.splitlines():
            if line == "%FILES%":
                in_files = True
                continue
            if line.startswith("%"):
                in_files = False
                continue
            if in_files and line:
                owned.add(line.rstrip("/"))
    return owned


def src_dirs(globs: tuple[str, ...] | None = None) -> list[Path]:
    """nct6687d* trees, including leftovers that no longer have nct6687.c."""
    patterns = globs if globs is not None else ("/usr/src/nct6687d*",)
    found: set[Path] = set()
    for pattern in patterns:
        for match in glob.glob(pattern):
            p = Path(match)
            if p.is_dir():
                found.add(p)
            elif p.is_file():
                found.add(p.parent)
    return sorted(found)


def find_src() -> Path:
    owned = _pacman_owned_files()
    for rel in sorted(owned):
        if rel.endswith("/nct6687.c"):
            p = Path("/") / rel
            if p.is_file():
                return p
    matches = glob.glob("/usr/src/nct6687d*/nct6687.c")
    if not matches:
        raise SystemExit("No /usr/src/nct6687d*/nct6687.c found")
    matches.sort(key=lambda p: Path(p).stat().st_mtime)
    return Path(matches[-1])


def is_overlay_extra(path: Path) -> bool:
    return path.name in manifest.DKMS_FILES or path.name.endswith(".pre-vrm")


def unowned_toplevel(pkg_dir: Path) -> list[Path]:
    """Top-level files in the DKMS tree that the installed package does not own."""
    owned = _pacman_owned_files()
    extras: list[Path] = []
    if not pkg_dir.is_dir():
        return extras
    for child in sorted(pkg_dir.iterdir()):
        if not child.is_file():
            continue
        rel = child.as_posix().lstrip("/")
        if owned and rel in owned:
            continue
        if not owned and not is_overlay_extra(child):
            # No files db — only drop extras we created, never Kbuild/etc.
            continue
        extras.append(child)
    return extras


def clear_unowned(pkg_dirs: list[Path] | None = None) -> list[Path]:
    """Remove unowned top-level files so pacman can extract newly packaged ones.

    This is the Kbuild trap: an extra file we (or a manual copy) dropped into
    /usr/src/… later became a package file, and the upgrade aborted.
    Only safe as the PreTransaction half of upgrade persist: it also strips
    the live overlay extras, which post_transaction puts back.
    """
    removed: list[Path] = []
    for pkg_dir in pkg_dirs or src_dirs():
        for path in unowned_toplevel(pkg_dir):
            path.unlink()
            print("Removed unowned", path)
            removed.append(path)
    return removed


def copy_overlay_files(dest: Path) -> None:
    for name in manifest.DKMS_FILES:
        out = dest / name
        shutil.copy2(manifest.resolve(name), out)
        print("Installed", out)


def stage_m_tree(
    dest: Path,
    stock_c: str,
    *,
    pkg_dir: Path,
    synthesize_kbuild: bool = False,
    patch: Path | None = None,
) -> None:
    """Write a DKMS M= tree: spliced nct6687.c, Kbuild, overlay files.

    Does not rewrite Makefile. Do not synthesize Kbuild into a live DKMS dir
    (that was the Kbuild trap); scratch verify-compile may.
    """
    spliced = splice.apply(stock_c, patch)
    dest.mkdir(parents=True, exist_ok=True)
    dest_kbuild = dest / "Kbuild"
    kbuild_src = pkg_dir / "Kbuild"
    if kbuild_src.is_file():
        if kbuild_src.resolve() != dest_kbuild.resolve():
            shutil.copy2(kbuild_src, dest_kbuild)
    elif synthesize_kbuild:
        dest_kbuild.write_text(MINIMAL_KBUILD)
        print("WARNING: no Kbuild in", pkg_dir, "— synthesized a minimal one")
    copy_overlay_files(dest)
    (dest / "nct6687.c").write_text(spliced)


def inject(src: Path) -> None:
    """Splice the live tree. Re-splices from .pre-vrm when already spliced."""
    pkg = src.parent
    text = src.read_text()
    bak = backup_path(src)
    if splice.is_spliced(text):
        if not bak.is_file():
            copy_overlay_files(pkg)
            print(
                "Already injected; refreshed",
                ", ".join(manifest.DKMS_FILES),
                "(no .pre-vrm — hooks not rewritten)",
            )
            return
        stage_m_tree(pkg, bak.read_text(), pkg_dir=pkg)
        print("Re-injected hooks from", bak)
        return
    if not bak.exists():
        shutil.copy2(src, bak)
        print("Backup:", bak)
    elif bak.read_bytes() != src.read_bytes():
        shutil.copy2(src, bak)
        print("Replaced stale backup:", bak)
    stage_m_tree(pkg, text, pkg_dir=pkg)
    print("Patched", src)


def restore(src: Path) -> None:
    bak = backup_path(src)
    if not bak.exists():
        raise SystemExit(f"No backup {bak}")
    shutil.copy2(bak, src)
    print("Restored", src, "from", bak)
    mf_bak = Path(str(src.parent / "Makefile") + ".pre-vrm")
    if mf_bak.exists():
        shutil.copy2(mf_bak, src.parent / "Makefile")
        print("Restored", src.parent / "Makefile")
    for name in manifest.DKMS_FILES:
        extra = src.parent / name
        if extra.exists():
            extra.unlink()
            print("Removed", extra)


def headers_for(kver: str) -> Path:
    return Path(f"/lib/modules/{kver}/build")


def kbuild_extra_argv(headers: Path) -> list[str]:
    """Match nct6687d Makefile: LLVM=1 when the kernel was built with clang."""
    cfg = headers / ".config"
    try:
        text = cfg.read_text()
    except OSError:
        return []
    if "CONFIG_CC_IS_CLANG=y" in text:
        return ["LLVM=1"]
    return []


def kbuild_modules_argv(headers: Path, tree: Path) -> list[str]:
    """DKMS M= invoke, plus LLVM=1 when .config says clang."""
    return [
        "make",
        "-C",
        str(headers),
        f"M={tree}",
        *kbuild_extra_argv(headers),
        "modules",
    ]


def verify_compile(src: Path) -> Path:
    pkg_dir = src.parent
    kver = os.uname().release
    headers = headers_for(kver)
    if not headers.is_dir():
        raise SystemExit(f"No kernel headers at {headers}")
    build_root = manifest.REPO_ROOT / ".vrm-verify-build"
    if build_root.exists():
        try:
            shutil.rmtree(build_root)
        except OSError:
            # Prior sudo verify-compile can leave a root-owned tree
            build_root = Path(tempfile.mkdtemp(prefix="nct6687-vrm-verify-"))
            print("WARNING: using", build_root, "(could not clear .vrm-verify-build)")
    raw = src.read_text()
    bak = backup_path(src)
    if splice.is_spliced(raw):
        if bak.is_file():
            stage_m_tree(
                build_root, bak.read_text(), pkg_dir=pkg_dir, synthesize_kbuild=True
            )
        elif f'#include "{INC_NAME}"' in raw:
            build_root.mkdir(parents=True, exist_ok=True)
            kbuild_src = pkg_dir / "Kbuild"
            if kbuild_src.is_file():
                shutil.copy2(kbuild_src, build_root / "Kbuild")
            else:
                (build_root / "Kbuild").write_text(MINIMAL_KBUILD)
            (build_root / "nct6687.c").write_text(raw)
            copy_overlay_files(build_root)
            print(
                "WARNING: live nct6687.c is injected but",
                bak.name,
                "is missing — compile uses stale struct fields",
            )
        else:
            raise SystemExit(
                "Live nct6687.c has an old-style VRM inject. "
                f"Restore stock first ({bak.name}) or run --restore, then --verify-compile."
            )
    else:
        stage_m_tree(build_root, raw, pkg_dir=pkg_dir, synthesize_kbuild=True)
    print(f"Verify-compile M={build_root}")
    subprocess.check_call(kbuild_modules_argv(headers, build_root))
    kos = list(build_root.rglob("nct6687.ko"))
    if not kos:
        raise SystemExit("nct6687.ko not found")
    print("OK: built", kos[0])
    return kos[0]


def parse_dkms(pkg_dir: Path) -> tuple[str, str]:
    conf = (pkg_dir / "dkms.conf").read_text()
    pname = pver = None
    for line in conf.splitlines():
        if line.startswith("PACKAGE_NAME="):
            pname = line.split("=", 1)[1].strip().strip('"')
        if line.startswith("PACKAGE_VERSION="):
            pver = line.split("=", 1)[1].strip().strip('"')
    if not pname or not pver:
        raise SystemExit("Could not parse dkms.conf")
    return pname, pver


def installed_kernels(pname: str, pver: str) -> list[str]:
    try:
        out = subprocess.check_output(
            ["dkms", "status", "-m", pname, "-v", pver], text=True
        )
    except subprocess.CalledProcessError:
        return [os.uname().release]
    kvers = []
    for line in out.splitlines():
        m = re.search(r",\s*([^,]+),\s*\w+:\s*installed", line)
        if m:
            kvers.append(m.group(1).strip())
    return kvers or [os.uname().release]


def modules_with_headers(modules_root: Path | None = None) -> list[str]:
    """Kernels that have a headers tree, whether or not DKMS has installed them."""
    root = modules_root or Path("/usr/lib/modules")
    if not root.is_dir():
        return []
    found: list[str] = []
    for build in sorted(root.glob("*/build")):
        if build.is_dir():
            found.append(build.parent.name)
    return found


def kernels_for_rebuild(
    installed: list[str], current: str, extra: list[str] | None = None
) -> list[str]:
    """Running kernel first so a stale extra kernel cannot block the live one.

    `extra` is usually kernels that have headers but are not yet in `dkms status`.
    """
    seen = {current}
    out = [current]
    for k in [*installed, *(extra or [])]:
        if k not in seen:
            seen.add(k)
            out.append(k)
    return out


def rebuild_skip_reason(kver: str, current: str, headers: Path) -> str | None:
    """Skip kernels with no headers. Never fatal just because kver is current.

    Topgrade often upgrades linux (drops outgoing headers) before nct6687d.
    The post-upgrade hook still runs on the old kernel; aborting there left
    the new kernel's .ko stock.
    """
    if headers.is_dir():
        return None
    if kver == current:
        return (
            f"Skipping running kernel {kver}: no headers at {headers} "
            "(kernel already upgraded; rebuild the others)"
        )
    return f"Skipping {kver}: no kernel headers at {headers}"


def rebuild(src: Path, reload: bool, load_vrm: bool = False) -> None:
    pkg_dir = src.parent
    pname, pver = parse_dkms(pkg_dir)
    kvers = kernels_for_rebuild(
        installed_kernels(pname, pver),
        os.uname().release,
        extra=modules_with_headers(),
    )
    current = os.uname().release
    print(f"Rebuilding {pname}/{pver} for kernels: {', '.join(kvers)}")
    built = 0
    failed: list[str] = []
    for kver in kvers:
        headers = headers_for(kver)
        skip = rebuild_skip_reason(kver, current, headers)
        if skip:
            print(skip)
            continue
        # install --force alone reuses stale builds; source was patched in-place
        print(f"--- dkms build -k {kver} --force ---")
        try:
            subprocess.check_call(
                ["dkms", "build", "-m", pname, "-v", pver, "-k", kver, "--force"]
            )
            print(f"--- dkms install -k {kver} --force ---")
            subprocess.check_call(
                ["dkms", "install", "-m", pname, "-v", pver, "-k", kver, "--force"]
            )
        except subprocess.CalledProcessError as e:
            if kver == current:
                raise SystemExit(
                    f"DKMS failed for running kernel {kver} (rc={e.returncode}). "
                    "See /var/lib/dkms/{pname}/{pver}/build/make.log"
                ) from e
            print(
                f"WARNING: DKMS failed for {kver} (rc={e.returncode}); "
                "continuing with other kernels"
            )
            failed.append(kver)
            continue
        built += 1
    if built == 0:
        raise SystemExit("DKMS rebuild skipped every kernel (no headers?)")
    if failed:
        print("WARNING: DKMS failed for extra kernels:", ", ".join(failed))
    if not reload:
        print("Skipped modprobe reload (--no-reload).")
        return
    vrm_arg = "vrm=1" if load_vrm else "vrm=0"
    print(f"Reloading nct6687 {vrm_arg}...")
    rc = subprocess.call(["modprobe", "-r", "nct6687"])
    if rc != 0:
        raise SystemExit(
            f"modprobe -r nct6687 failed (rc={rc}). "
            "Something still holds the module — close hwmon clients and retry. "
            "DKMS is built but the LIVE module was NOT replaced."
        )
    # After --restore the module is stock (no vrm param). Only pass vrm=* when patched.
    patched = src.is_file() and splice.is_spliced(src.read_text())
    if patched:
        subprocess.check_call(["modprobe", "nct6687", vrm_arg])
    else:
        subprocess.check_call(["modprobe", "nct6687"])
    vrm_sys = Path("/sys/module/nct6687/parameters/vrm")
    if patched:
        if not vrm_sys.exists():
            raise SystemExit(
                "Reload finished but /sys/module/nct6687/parameters/vrm is missing — "
                "the live module is still the unpatched build. "
                "Run: sudo python3 nct6687_vrm_dkms_inject.py --rebuild"
            )
        print("Live module param vrm=" + vrm_sys.read_text().strip())
        if load_vrm:
            print("Loaded WITH VRM. Rollback: modprobe nct6687 vrm=0")
        else:
            print(
                "Loaded with vrm=0. Enable later: modprobe -r nct6687 && modprobe nct6687 vrm=1"
            )
    else:
        print("Loaded stock nct6687 (no VRM patch in sources).")


def want_vrm_enabled(cli_enable: bool) -> bool:
    """CLI --enable-vrm wins; else honor /etc/modprobe.d/*nct6687* options."""
    if cli_enable:
        return True
    for path in sorted(Path("/etc/modprobe.d").glob("*.conf")):
        try:
            text = path.read_text()
        except OSError:
            continue
        for line in text.splitlines():
            s = line.split("#", 1)[0].strip()
            if not s.startswith("options"):
                continue
            parts = s.split()
            if len(parts) < 3 or parts[1] != "nct6687":
                continue
            for tok in parts[2:]:
                if tok in ("vrm=1", "vrm=Y", "vrm=y", "vrm=true"):
                    return True
                if tok in ("vrm=0", "vrm=N", "vrm=n", "vrm=false"):
                    return False
    return False


def reinject(src: Path, reload: bool, load_vrm: bool) -> None:
    """Re-apply the splice after a package upgrade, force-rebuild DKMS."""
    print("Re-injecting VRM patch into", src)
    inject(src)
    rebuild(src, reload=reload, load_vrm=load_vrm)
