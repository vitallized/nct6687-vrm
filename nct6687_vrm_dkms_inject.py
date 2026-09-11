#!/usr/bin/env python3
"""Inject eSIO PMBus VRM hwmon attrs into nct6687 DKMS sources, then rebuild.

Bulk VRM logic lives in dkms/nct6687_vrm.inc.c (copied beside nct6687.c and
#include'd). This script only splices small hooks into nct6687.c so upstream
driver churn breaks a few anchors — not a 500-line embedded blob.

Safety: --verify-compile; --install loads vrm=0; update_vrm outside update_lock;
GT hidden unless vrm_gt=1; modprobe -r must succeed.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Present in the #include splice and inside nct6687_vrm.inc.c
MARKER = "NCT6687_VRM_PMBUS_INJECT"
INC_NAME = "nct6687_vrm.inc.c"
REPO_ROOT = Path(__file__).resolve().parent

# Splice payload only. The include reads these via struct nct6687_data
# after inject_text inserts them; do not duplicate the list in C.
STRUCT_FIELDS = """
	/* VRM PMBus (eSIO): PAGE0=CPU, PAGE1=GT */
	bool vrm_enabled;
	bool vrm_valid;
	bool vrm_gt_valid;
	bool vrm_demand;
	unsigned long vrm_last_updated;
	unsigned long vrm_last_read;
	unsigned long vrm_read_gap;
	long vrm_vout; /* mV */
	long vrm_vin;  /* mV */
	long vrm_iout; /* mA */
	long vrm_pout; /* uW */
	long vrm_temp; /* mC */
	long vrm_gt_vout;
	long vrm_gt_vin;
	long vrm_gt_iout;
	long vrm_gt_pout;
	long vrm_gt_temp;
"""

# The include defines nct6687_update_vrm and no longer calls
# nct6687_update_device (nct_vrm_touch_and_update goes straight to
# update_vrm). The hook call is in nct6687_update_device, so only
# update_vrm needs a forward declaration before that function.
FORWARD_DECL = "\nstatic void nct6687_update_vrm(struct nct6687_data *data);\n"

# Thin splice — bulk implementation is in INC_NAME
VRM_INCLUDE = f"""
/* {MARKER} — see {INC_NAME} */
#include "{INC_NAME}"

"""

PROBE_ENABLE = """
	data->vrm_enabled = vrm;
	data->vrm_last_updated = 0;
	data->vrm_last_read = 0;
	data->vrm_read_gap = 0;
	data->vrm_demand = false;
	if (data->vrm_enabled)
		dev_info(dev, "VRM PMBus eSIO sensors enabled (addr=0x%02x vout_exp=%d gt=%d)\\n",
			 vrm_addr & 0xff, vrm_vout_exp, vrm_gt ? 1 : 0);
	else
		dev_info(dev, "VRM PMBus eSIO sensors built-in but disabled (modprobe nct6687 vrm=1)\\n");

"""

# `build:` copies sources into ${kver}/. Upstream has shipped this with and
# without Kbuild; we only require the VRM include to be on that cp line.
_CP_TO_KVER = re.compile(
    r"^(\t*cp(?: \$\{curpwd\}/[^\s]+)+) \$\{curpwd\}/\$\{kver\}\s*$",
    re.M,
)

PACMAN_LOCAL = Path("/var/lib/pacman/local")
INSTALLED_LIB = Path("/usr/local/lib/nct6687-vrm")
MINIMAL_KBUILD = "obj-m += nct6687.o\n"


def find_inc() -> Path:
    candidates = [
        REPO_ROOT / "dkms" / INC_NAME,
        REPO_ROOT / INC_NAME,
        INSTALLED_LIB / INC_NAME,
    ]
    for p in candidates:
        if p.is_file():
            return p
    raise SystemExit(
        f"Missing {INC_NAME} (tried: {', '.join(str(c) for c in candidates)})"
    )


def _pacman_owned_files(pkg_prefix: str = "nct6687d-dkms-git-") -> set[str]:
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


def src_dirs() -> list[Path]:
    found = {Path(p).parent for p in glob.glob("/usr/src/nct6687d*/nct6687.c")}
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


def patched_makefile_text(text: str, extra_names: list[str]) -> tuple[str | None, bool]:
    """Insert extra filenames into the stock `cp … ${kver}` line.

    Returns (None, False) if that line is missing.
    """
    m = _CP_TO_KVER.search(text)
    if not m:
        return None, False
    prefix = m.group(1)
    new_prefix = prefix
    for name in extra_names:
        token = f"${{curpwd}}/{name}"
        if token not in new_prefix:
            new_prefix = f"{new_prefix} {token}"
    if new_prefix == prefix:
        return text, False
    return text[: m.start(1)] + new_prefix + text[m.end(1) :], True


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
        if not owned and child.name != INC_NAME and not child.name.endswith(".pre-vrm"):
            # No files db — only drop extras we created, never Kbuild/etc.
            continue
        extras.append(child)
    return extras


def clear_unowned(pkg_dirs: list[Path] | None = None) -> list[Path]:
    """Remove unowned top-level files so pacman can extract newly packaged ones.

    This is the Kbuild trap: an extra file we (or a manual copy) dropped into
    /usr/src/… later became a package file, and the upgrade aborted.
    """
    removed: list[Path] = []
    for pkg_dir in pkg_dirs or src_dirs():
        for path in unowned_toplevel(pkg_dir):
            path.unlink()
            print("Removed unowned", path)
            removed.append(path)
    return removed


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


def install_inc(pkg_dir: Path) -> Path:
    """Copy/refresh the VRM include next to nct6687.c."""
    dst = pkg_dir / INC_NAME
    shutil.copy2(find_inc(), dst)
    print("Installed", dst)
    return dst


def patch_makefile(pkg_dir: Path) -> None:
    """Ensure `make build` copies the VRM include into the per-kernel build dir."""
    mf = pkg_dir / "Makefile"
    if not mf.is_file():
        return
    extra = [INC_NAME]
    if (pkg_dir / "Kbuild").is_file():
        extra.insert(0, "Kbuild")
    new_text, changed = patched_makefile_text(mf.read_text(), extra)
    if new_text is None:
        print(
            "WARNING: Makefile cp line not found — verify-compile may fail; "
            "DKMS in-tree build may still work if the include sits beside nct6687.c"
        )
        return
    if not changed:
        return
    bak = Path(str(mf) + ".pre-vrm")
    if not bak.exists():
        shutil.copy2(mf, bak)
    mf.write_text(new_text)
    print("Patched", mf)


def _require_anchor(text: str, needle: str, name: str, hint: str = "") -> None:
    """Fail with the named exact-string anchor so a mismatch is diagnosable."""
    if needle not in text:
        extra = f" — {hint}" if hint else " — driver layout changed"
        raise SystemExit(f"anchor {name} not found{extra}")


def inject_text(text: str) -> str:
    if MARKER in text:
        raise SystemExit("Already injected (marker present)")

    ioregion = "#define IOREGION_LENGTH 4"
    _require_anchor(text, ioregion, "IOREGION_LENGTH 4")
    text = text.replace(ioregion, "#define IOREGION_LENGTH 8", 1)

    needle = "\tstruct mutex update_lock;"
    _require_anchor(text, needle, "struct mutex update_lock")
    extra_groups_old = "const struct attribute_group *extra_groups[2];"
    extra_groups_new = "const struct attribute_group *extra_groups[3];"
    _require_anchor(
        text,
        extra_groups_old,
        "extra_groups[2]",
        "driver group registration changed again",
    )
    text = text.replace(extra_groups_old, extra_groups_new, 1)
    text = text.replace(needle, STRUCT_FIELDS + "\n" + needle, 1)

    upd_sig = "static struct nct6687_data *nct6687_update_device(struct device *dev)"
    _require_anchor(text, upd_sig, "nct6687_update_device signature")
    text = text.replace(upd_sig, FORWARD_DECL + VRM_INCLUDE + upd_sig, 1)

    upd_end = (
        "\t\tdata->last_updated = jiffies;\n"
        "\t\tdata->valid = true;\n"
        "\t}\n\n"
        "\tmutex_unlock(&data->update_lock);\n\n"
        "\treturn data;\n"
        "}"
    )
    upd_end_new = (
        "\t\tdata->last_updated = jiffies;\n"
        "\t\tdata->valid = true;\n"
        "\t}\n\n"
        "\tmutex_unlock(&data->update_lock);\n\n"
        "\t/* VRM: EC_io_lock only — do not hold update_lock across SMBus */\n"
        "\tnct6687_update_vrm(data);\n\n"
        "\treturn data;\n"
        "}"
    )
    _require_anchor(text, upd_end, "nct6687_update_device end")
    text = text.replace(upd_end, upd_end_new, 1)

    probe_anchor = "\tnct6687_setup_voltages(data);\n"
    _require_anchor(text, probe_anchor, "nct6687_setup_voltages")
    text = text.replace(probe_anchor, probe_anchor + PROBE_ENABLE, 1)

    extra_groups_assign_old = (
        "\tif (nct6687_fan_config_type == FAN_CONFIG_MSI_ALT1 && msi_fan_brute_force)\n"
        "\t\tdata->extra_groups[0] = &nct6687_fan_watchdog_group;\n"
    )
    extra_groups_assign_new = (
        "\t{\n"
        "\t\tint eg = 0;\n\n"
        "\t\tif (nct6687_fan_config_type == FAN_CONFIG_MSI_ALT1 && msi_fan_brute_force)\n"
        "\t\t\tdata->extra_groups[eg++] = &nct6687_fan_watchdog_group;\n"
        "\t\tif (data->vrm_enabled)\n"
        "\t\t\tdata->extra_groups[eg++] = &nct6687_vrm_group;\n"
        "\t}\n"
    )
    _require_anchor(
        text,
        extra_groups_assign_old,
        "fan_watchdog extra_groups assignment",
        "anchor changed",
    )
    text = text.replace(extra_groups_assign_old, extra_groups_assign_new, 1)
    return text


def inject(src: Path) -> None:
    install_inc(src.parent)
    patch_makefile(src.parent)
    text = src.read_text()
    if MARKER in text:
        print("Already injected (hooks); refreshed", INC_NAME)
        return
    bak = Path(str(src) + ".pre-vrm")
    if not bak.exists():
        shutil.copy2(src, bak)
        print("Backup:", bak)
    src.write_text(inject_text(text))
    print("Patched", src)


def restore(src: Path) -> None:
    bak = Path(str(src) + ".pre-vrm")
    if not bak.exists():
        raise SystemExit(f"No backup {bak}")
    shutil.copy2(bak, src)
    print("Restored", src, "from", bak)
    mf_bak = Path(str(src.parent / "Makefile") + ".pre-vrm")
    if mf_bak.exists():
        shutil.copy2(mf_bak, src.parent / "Makefile")
        print("Restored", src.parent / "Makefile")
    inc = src.parent / INC_NAME
    if inc.exists():
        inc.unlink()
        print("Removed", inc)


def verify_compile(src: Path) -> Path:
    pkg_dir = src.parent
    makefile = pkg_dir / "Makefile"
    if not makefile.exists():
        raise SystemExit(f"No Makefile in {pkg_dir}")
    kver = os.uname().release
    build_root = REPO_ROOT / ".vrm-verify-build"
    if build_root.exists():
        try:
            shutil.rmtree(build_root)
        except OSError:
            # Prior sudo verify-compile can leave a root-owned tree
            build_root = Path(tempfile.mkdtemp(prefix="nct6687-vrm-verify-"))
            print("WARNING: using", build_root, "(could not clear .vrm-verify-build)")
    build_root.mkdir(parents=True, exist_ok=True)
    raw = src.read_text()
    # Prefer stock Makefile backup so we don't copy an already-patched live Makefile
    mf_src = Path(str(makefile) + ".pre-vrm")
    shutil.copy2(mf_src if mf_src.is_file() else makefile, build_root / "Makefile")
    kbuild_src = pkg_dir / "Kbuild"
    if kbuild_src.is_file():
        shutil.copy2(kbuild_src, build_root / "Kbuild")
    else:
        (build_root / "Kbuild").write_text(MINIMAL_KBUILD)
        print("WARNING: no Kbuild in", pkg_dir, "— synthesized a minimal one for verify-compile")
    patch_makefile(build_root)
    if MARKER in raw and f'#include "{INC_NAME}"' in raw:
        (build_root / "nct6687.c").write_text(raw)
        # Prefer the checkout include so --verify-compile tests local edits,
        # not a stale copy sitting in /usr/src.
        shutil.copy2(find_inc(), build_root / INC_NAME)
    elif MARKER in raw:
        # Legacy single-file inject — rebuild from stock backup if present
        bak = Path(str(src) + ".pre-vrm")
        if not bak.is_file():
            raise SystemExit(
                "Live nct6687.c has an old-style VRM inject. "
                f"Restore stock first ({bak.name}) or run --restore, then --verify-compile."
            )
        install_inc(build_root)
        (build_root / "nct6687.c").write_text(inject_text(bak.read_text()))
    else:
        install_inc(build_root)
        (build_root / "nct6687.c").write_text(inject_text(raw))
    print(f"Verify-compile in {build_root} for {kver}")
    subprocess.check_call(["make", f"TARGET={kver}", "build"], cwd=build_root)
    kos = list(build_root.rglob("nct6687.ko"))
    if not kos:
        raise SystemExit("nct6687.ko not found")
    print("OK: built", kos[0])
    return kos[0]


def rebuild(src: Path, reload: bool, load_vrm: bool = False) -> None:
    pkg_dir = src.parent
    pname, pver = parse_dkms(pkg_dir)
    kvers = installed_kernels(pname, pver)
    current = os.uname().release
    if current not in kvers:
        kvers.append(current)
    print(f"Rebuilding {pname}/{pver} for kernels: {', '.join(kvers)}")
    built = 0
    for kver in kvers:
        headers = Path(f"/lib/modules/{kver}/build")
        if not headers.is_dir():
            msg = f"Skipping {kver}: no kernel headers at {headers}"
            if kver == current:
                raise SystemExit(
                    f"No kernel headers for running kernel {kver} ({headers})"
                )
            print(msg)
            continue
        # install --force alone reuses stale builds; source was patched in-place
        print(f"--- dkms build -k {kver} --force ---")
        subprocess.check_call(
            ["dkms", "build", "-m", pname, "-v", pver, "-k", kver, "--force"]
        )
        print(f"--- dkms install -k {kver} --force ---")
        subprocess.check_call(
            ["dkms", "install", "-m", pname, "-v", pver, "-k", kver, "--force"]
        )
        built += 1
    if built == 0:
        raise SystemExit("DKMS rebuild skipped every kernel (no headers?)")
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
    src_text = src.read_text() if src.is_file() else ""
    patched = MARKER in src_text
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


def warn_if_hook_stale() -> None:
    installed = INSTALLED_LIB / Path(__file__).name
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
        "  sudo bash pacman-hook/install.sh\n"
        "  (the pacman hook auto-refreshes if source.env still points here)",
        file=sys.stderr,
    )


def check() -> int:
    """Print hook/src status. Exit 1 if stale copy or unexpected unowned files."""
    src = find_src()
    injected = MARKER in src.read_text()
    extras = unowned_toplevel(src.parent)
    expected, unexpected = [], []
    for p in extras:
        if p.name == INC_NAME or p.name.endswith(".pre-vrm"):
            expected.append(p)
        else:
            unexpected.append(p)
    print("DKMS source:", src)
    print("Injected:", injected)
    if expected:
        print("Expected extras (cleared automatically on the next package upgrade):")
        for p in expected:
            print(" ", p)
    if unexpected:
        print("Unexpected unowned files (pacman will refuse the next upgrade):")
        for p in unexpected:
            print(" ", p)
    elif not expected:
        print("Unowned files: none")
    here = Path(__file__).resolve()
    print("This script:", here)
    installed_py = INSTALLED_LIB / here.name
    stale = False
    if installed_py.is_file():
        py_stale = installed_py.read_bytes() != here.read_bytes()
        stale = stale or py_stale
        print("Installed hook copy:", "STALE" if py_stale else "ok")
        inc_i = INSTALLED_LIB / INC_NAME
        if inc_i.is_file():
            inc_stale = inc_i.read_bytes() != find_inc().read_bytes()
            stale = stale or inc_stale
            print("Installed include:", "STALE" if inc_stale else "ok")
    else:
        print("Installed hook copy: missing (run pacman-hook/install.sh)")
        stale = True
    env = INSTALLED_LIB / "source.env"
    if env.is_file():
        print(env.read_text().rstrip())
    else:
        print("source.env: missing")
    if unexpected or stale:
        return 1
    return 0


def reinject(src: Path, reload: bool, load_vrm: bool) -> None:
    """Pacman-hook path: patch wiped stock sources, force-rebuild DKMS."""
    if MARKER not in src.read_text():
        print("Re-injecting VRM patch into", src)
        inject(src)
    else:
        print("VRM patch already present in", src)
        install_inc(src.parent)
        patch_makefile(src.parent)
    rebuild(src, reload=reload, load_vrm=load_vrm)


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
        help="Re-apply patch after package upgrade (pacman hook); skips verify-compile",
    )
    ap.add_argument(
        "--pre-upgrade",
        action="store_true",
        help="Remove unowned files in the DKMS src dir (pacman PreTransaction)",
    )
    ap.add_argument(
        "--check",
        action="store_true",
        help="Print inject/hook status; exit 1 if stale or unowned files",
    )
    ap.add_argument("--no-reload", action="store_true")
    ap.add_argument("--enable-vrm", action="store_true")
    ap.add_argument("--src", type=Path, default=None)
    args = ap.parse_args()
    if args.check:
        return check()
    if not any(
        [
            args.verify_compile,
            args.install,
            args.restore,
            args.rebuild,
            args.reinject,
            args.pre_upgrade,
        ]
    ):
        ap.print_help()
        print(
            "\nRefusing bare run. Use --verify-compile first, then --install.",
            file=sys.stderr,
        )
        return 2
    if args.verify_compile:
        warn_if_hook_stale()
        verify_compile(args.src or find_src())
        print(
            "\nCompile OK. Install with: sudo python3", Path(__file__).name, "--install"
        )
        return 0
    if os.geteuid() != 0 and (
        args.install
        or args.restore
        or args.rebuild
        or args.reinject
        or args.pre_upgrade
    ):
        print("Need root", file=sys.stderr)
        return 1
    if args.pre_upgrade:
        removed = clear_unowned()
        if not removed:
            print("No unowned files to remove")
        return 0
    src = args.src or find_src()
    load_vrm = want_vrm_enabled(args.enable_vrm)
    if args.restore:
        restore(src)
        if args.rebuild:
            rebuild(src, reload=not args.no_reload, load_vrm=False)
        return 0
    if args.reinject:
        reinject(src, reload=not args.no_reload, load_vrm=load_vrm)
        return 0
    if args.install:
        warn_if_hook_stale()
        if MARKER not in src.read_text():
            print("Step 1/3: verify-compile...")
            verify_compile(src)
            print("Step 2/3: inject...")
            inject(src)
        else:
            print("Already injected; refreshing include + rebuilding...")
            install_inc(src.parent)
            patch_makefile(src.parent)
        print("Step 3/3: dkms install...")
        rebuild(src, reload=not args.no_reload, load_vrm=load_vrm)
        return 0
    if args.rebuild:
        warn_if_hook_stale()
        rebuild(src, reload=not args.no_reload, load_vrm=load_vrm)
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
