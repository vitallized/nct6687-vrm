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
from pathlib import Path

# Present in the #include splice and inside nct6687_vrm.inc.c
MARKER = "NCT6687_VRM_PMBUS_INJECT"
INC_NAME = "nct6687_vrm.inc.c"
REPO_ROOT = Path(__file__).resolve().parent

STRUCT_FIELDS = """
	/* VRM PMBus (eSIO): PAGE0=CPU, PAGE1=GT */
	bool vrm_enabled;
	bool vrm_valid;
	bool vrm_gt_valid;
	unsigned long vrm_last_updated;
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

FORWARD_DECL = "\nstatic void nct6687_update_vrm(struct nct6687_data *data);\n"

# Thin splice — bulk implementation is in INC_NAME
VRM_INCLUDE = f"""
/* {MARKER} — see {INC_NAME} */
#include "{INC_NAME}"

"""

PROBE_ENABLE = """
	data->vrm_enabled = vrm;
	data->vrm_last_updated = 0;
	if (data->vrm_enabled)
		dev_info(dev, "VRM PMBus eSIO sensors enabled (addr=0x%02x vout_exp=%d gt=%d)\\n",
			 vrm_addr & 0xff, vrm_vout_exp, vrm_gt ? 1 : 0);
	else
		dev_info(dev, "VRM PMBus eSIO sensors built-in but disabled (modprobe nct6687 vrm=1)\\n");

"""

GROUP_INSERT = """\
	if (data->vrm_enabled)
		data->groups[groups++] = &nct6687_vrm_group;

"""

# Stock Makefile `build:` only copies nct6687.c into ${kver}/ — include must go too.
MAKEFILE_CP_OLD = "cp ${curpwd}/Makefile ${curpwd}/nct6687.c ${curpwd}/${kver}"
MAKEFILE_CP_NEW = (
    f"cp ${{curpwd}}/Makefile ${{curpwd}}/nct6687.c "
    f"${{curpwd}}/{INC_NAME} ${{curpwd}}/${{kver}}"
)


def find_inc() -> Path:
    candidates = [
        REPO_ROOT / "dkms" / INC_NAME,
        REPO_ROOT / INC_NAME,
    ]
    for p in candidates:
        if p.is_file():
            return p
    raise SystemExit(
        f"Missing {INC_NAME} (tried: {', '.join(str(c) for c in candidates)})"
    )


def find_src() -> Path:
    matches = sorted(glob.glob("/usr/src/nct6687d*/nct6687.c"))
    if not matches:
        raise SystemExit("No /usr/src/nct6687d*/nct6687.c found")
    return Path(matches[-1])


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
        out = subprocess.check_output(["dkms", "status", "-m", pname, "-v", pver], text=True)
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
    text = mf.read_text()
    if MAKEFILE_CP_NEW in text:
        return
    if MAKEFILE_CP_OLD not in text:
        print(
            "WARNING: Makefile cp line not found — verify-compile may fail; "
            "DKMS in-tree build may still work if the include sits beside nct6687.c"
        )
        return
    bak = Path(str(mf) + ".pre-vrm")
    if not bak.exists():
        shutil.copy2(mf, bak)
    mf.write_text(text.replace(MAKEFILE_CP_OLD, MAKEFILE_CP_NEW, 1))
    print("Patched", mf)


def inject_text(text: str) -> str:
    if MARKER in text:
        raise SystemExit("Already injected (marker present)")

    if "#define IOREGION_LENGTH 4" not in text:
        raise SystemExit("IOREGION_LENGTH 4 not found — driver layout changed")
    text = text.replace("#define IOREGION_LENGTH 4", "#define IOREGION_LENGTH 8", 1)

    needle = "\tstruct mutex update_lock;"
    if needle not in text:
        raise SystemExit("struct field anchor not found")
    if "const struct attribute_group *groups[6]" not in text:
        raise SystemExit("groups[6] not found — cannot safely add VRM group")
    text = text.replace(needle, STRUCT_FIELDS + "\n" + needle, 1)

    upd_sig = "static struct nct6687_data *nct6687_update_device(struct device *dev)"
    if upd_sig not in text:
        raise SystemExit("nct6687_update_device signature not found")
    text = text.replace(upd_sig, FORWARD_DECL + "\n" + upd_sig, 1)

    anchor = "/*\n * Sysfs callback functions\n */"
    if anchor not in text:
        raise SystemExit("sysfs anchor not found")
    text = text.replace(anchor, VRM_INCLUDE + anchor, 1)

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
    if upd_end not in text:
        raise SystemExit("update_device end anchor not found")
    text = text.replace(upd_end, upd_end_new, 1)

    probe_anchor = "\tnct6687_setup_voltages(data);\n"
    if probe_anchor not in text:
        raise SystemExit("probe setup anchor not found")
    text = text.replace(probe_anchor, probe_anchor + PROBE_ENABLE, 1)

    idx = text.find("scnprintf(build, sizeof(build)")
    if idx < 0:
        raise SystemExit("probe group anchor (scnprintf build) not found")
    line_start = text.rfind("\n", 0, idx) + 1
    text = text[:line_start] + GROUP_INSERT + text[line_start:]
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
        shutil.rmtree(build_root)
    build_root.mkdir(parents=True)
    raw = src.read_text()
    # Prefer stock Makefile backup so we don't copy an already-patched live Makefile
    mf_src = Path(str(makefile) + ".pre-vrm")
    shutil.copy2(mf_src if mf_src.is_file() else makefile, build_root / "Makefile")
    patch_makefile(build_root)
    if MARKER in raw and f'#include "{INC_NAME}"' in raw:
        (build_root / "nct6687.c").write_text(raw)
        live_inc = pkg_dir / INC_NAME
        shutil.copy2(live_inc if live_inc.is_file() else find_inc(), build_root / INC_NAME)
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
    for kver in kvers:
        # install --force alone reuses stale builds; source was patched in-place
        print(f"--- dkms build -k {kver} --force ---")
        subprocess.check_call(["dkms", "build", "-m", pname, "-v", pver, "-k", kver, "--force"])
        print(f"--- dkms install -k {kver} --force ---")
        subprocess.check_call(["dkms", "install", "-m", pname, "-v", pver, "-k", kver, "--force"])
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
            print("Loaded with vrm=0. Enable later: modprobe -r nct6687 && modprobe nct6687 vrm=1")
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
    """Pacman-hook path: patch wiped stock sources, force-rebuild DKMS."""
    if MARKER not in src.read_text():
        print("Re-injecting VRM patch into", src)
        inject(src)
    else:
        print("VRM patch already present in", src)
        install_inc(src.parent)
    rebuild(src, reload=reload, load_vrm=load_vrm)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verify-compile", action="store_true")
    ap.add_argument("--install", action="store_true")
    ap.add_argument("--restore", action="store_true")
    ap.add_argument("--rebuild", action="store_true")
    ap.add_argument(
        "--reinject",
        action="store_true",
        help="Re-apply patch after package upgrade (pacman hook); skips verify-compile",
    )
    ap.add_argument("--no-reload", action="store_true")
    ap.add_argument("--enable-vrm", action="store_true")
    ap.add_argument("--src", type=Path, default=None)
    args = ap.parse_args()
    if not any([args.verify_compile, args.install, args.restore, args.rebuild, args.reinject]):
        ap.print_help()
        print("\nRefusing bare run. Use --verify-compile first, then --install.", file=sys.stderr)
        return 2
    if args.verify_compile:
        verify_compile(args.src or find_src())
        print("\nCompile OK. Install with: sudo python3", Path(__file__).name, "--install")
        return 0
    if os.geteuid() != 0 and (args.install or args.restore or args.rebuild or args.reinject):
        print("Need root", file=sys.stderr)
        return 1
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
        if MARKER not in src.read_text():
            print("Step 1/3: verify-compile...")
            verify_compile(src)
            print("Step 2/3: inject...")
            inject(src)
        else:
            print("Already injected; refreshing include + rebuilding...")
            install_inc(src.parent)
        print("Step 3/3: dkms install...")
        rebuild(src, reload=not args.no_reload, load_vrm=load_vrm)
        return 0
    if args.rebuild:
        rebuild(src, reload=not args.no_reload, load_vrm=load_vrm)
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
