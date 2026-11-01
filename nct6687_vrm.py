#!/usr/bin/env python3
"""NCT6687 eSIO PMBus VRM reader (MSI MS-7D89 / RAA-class @ SMBus 0xC0).

Adapter around nct6687_vrm_port (key=value on stdout). Base from the
nct6687 platform device (fallback 0xA20). PAGE 0 = CPU Vcore, PAGE 1 = GT.

Examples:
  sudo python3 nct6687_vrm.py --page 0
  sudo python3 nct6687_vrm.py --page 0 --temps  # also read 0x8E and 0x8F
  sudo python3 nct6687_vrm.py --force   # allow with nct6687.ko loaded
  sudo python3 nct6687_vrm.py --loop 1

Safety: no address scan, no block reads. Refuses if nct6687.ko is loaded
unless --force (one-shot only). --loop is refused while the module is
loaded — even with --force — because /dev/port has no lock vs EC_io_lock.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path

DEFAULT_BASE = 0xA20
DEFAULT_ADDR = 0xC0
DEFAULT_PORT = 0
# Fallback when VOUT_MODE is not Linear/Direct-recognizable
DEFAULT_VOUT_EXP = -10


def discover_base() -> int:
    """BAR from platform nct6687.<addr> (addr is ISA base)."""
    platforms = sorted(Path("/sys/devices/platform").glob("nct6687.*"))
    for p in platforms:
        try:
            return int(p.name.split(".")[-1])
        except ValueError:
            continue
    return DEFAULT_BASE


@dataclass
class VrmSample:
    addr: int
    page: int
    vout_mode: int
    vout_raw: int
    iout_raw: int
    iin_raw: int
    pout_raw: int
    pin_raw: int
    vin_raw: int
    temp_raw: int
    vout_v: float
    iout_a: float
    iin_a: float
    pout_w: float
    pin_w: float
    vin_v: float
    temp_c: float
    vout_method: str
    iout_method: str
    base: int
    # millisi, same values as kernel nct_vrm_sample_page out[].
    vout_mv: int
    vin_mv: int
    iout_ma: int
    iin_ma: int
    pout_uw: int
    pin_uw: int
    eff_pcm: int
    temp_mc: int
    # CAP 0x19 / STATUS 0x78 are debug-only (CLI --raw), not the kernel sample.
    capability: int | None = None
    status: int | None = None


PAGE_NAMES = {0: "CPU", 1: "GT"}


def format_sensors(s: VrmSample, name: str | None = None) -> str:
    rail = name or PAGE_NAMES.get(s.page, f"PAGE{s.page}")
    # PAGE 0 matches sensors: VRM VOUT. PAGE 1 keeps GT so the rails stay distinct.
    tag = "VRM" if s.page == 0 else f"VRM {rail}"
    lines = [
        f"nct6687-vrm-0x{s.addr:02x}-{rail.lower()}",
        f"Adapter: NCT6687 eSIO SMBus @ 0x{s.base:04x}",
        f"{tag} VOUT: {s.vout_v:8.3f} V  (RAW 0x{s.vout_raw:04x}, {s.vout_method})",
        f"{tag} IOUT: {s.iout_a:8.3f} A  ({s.iout_method}; RAW 0x{s.iout_raw:04x})",
        f"{tag} IIN:  {s.iin_a:8.3f} A  (READ_IIN 0.01A; RAW 0x{s.iin_raw:04x})",
        f"{tag} POUT: {s.pout_w:8.3f} W  (RAW 0x{s.pout_raw:04x})",
        f"{tag} PIN:  {s.pin_w:8.3f} W  (READ_PIN 1W; RAW 0x{s.pin_raw:04x})",
        (
            f"{tag} EFFICIENCY: {(s.eff_pcm + 500) // 1000:3d}%"
            if s.eff_pcm > 0
            else f"{tag} EFFICIENCY:   n/a"
        ),
        f"{tag} VIN:  {s.vin_v:8.3f} V  (Direct 10mV; RAW 0x{s.vin_raw:04x})",
        f"{tag} TEMP: {s.temp_c:8.1f} °C (RAW 0x{s.temp_raw:04x})",
        f"VOUT_MODE:     0x{s.vout_mode:02x}",
        f"PAGE:          {s.page} ({rail})",
    ]
    if s.capability is not None:
        lines.append(f"CAPABILITY:    0x{s.capability:02x}")
    if s.status is not None:
        lines.append(f"STATUS_BYTE:   0x{s.status:02x}")
    return "\n".join(lines) + "\n"


def module_loaded(name: str) -> bool:
    return Path(f"/sys/module/{name}").is_dir()


def userspace_guard(*, loaded: bool, force: bool, looping: bool) -> str | None:
    """None = allow. Else a refusal line for stderr.

    --force is a one-shot escape when nct6687.ko is loaded. Looping that
    race against a live module (especially vrm=Y) is how you wedge the EC.
    """
    if not loaded:
        return None
    if looping:
        return "Refusing: --loop while nct6687.ko is loaded (unlocked /dev/port vs EC_io_lock)"
    if not force:
        return "Refusing: nct6687.ko is loaded (shares eSIO base+4..+6)"
    return None


def _port_bin() -> Path:
    root = Path(__file__).resolve().parent
    src = root / "nct6687_vrm_port.c"
    out = root / "nct6687_vrm_port"
    if not out.exists() or out.stat().st_mtime < src.stat().st_mtime:
        subprocess.check_call(
            [
                "cc",
                "-Wall",
                "-Werror",
                "-Wno-unused-function",
                "-DNCT_VRM_MAILBOX_HOST",
                "-o",
                str(out),
                str(src),
            ]
        )
    return out


def _sample_from_port_text(
    text: str,
    *,
    base: int,
    addr: int,
    page: int,
    vout_exp: int,
) -> VrmSample:
    """Build a sample from nct6687_vrm_port stdout. No /dev/port."""
    vals: dict[str, int] = {}
    for line in text.splitlines():
        key, sep, raw = line.partition("=")
        if sep != "=" or key == "":
            continue
        vals[key] = int(raw, 10)
    vout_mv = vals["vout_mv"]
    vin_mv = vals["vin_mv"]
    iout_ma = vals["iout_ma"]
    iin_ma = vals["iin_ma"]
    pout_uw = vals["pout_uw"]
    pin_uw = vals["pin_uw"]
    temp_mc = vals["temp_mc"]
    vout_mode = vals["vout_mode"]
    return VrmSample(
        addr=addr,
        page=page,
        vout_mode=vout_mode,
        vout_raw=vals["vout_raw"],
        iout_raw=vals["iout_raw"],
        iin_raw=vals["iin_raw"],
        pout_raw=vals["pout_raw"],
        pin_raw=vals["pin_raw"],
        vin_raw=vals["vin_raw"],
        temp_raw=vals["temp_raw"],
        vout_v=vout_mv / 1000.0,
        iout_a=iout_ma / 1000.0,
        iin_a=iin_ma / 1000.0,
        pout_w=pout_uw / 1_000_000.0,
        pin_w=pin_uw / 1_000_000.0,
        vin_v=vin_mv / 1000.0,
        temp_c=temp_mc / 1000.0,
        vout_method=_vout_method(vout_mode, vout_exp),
        iout_method="READ_IOUT 0.1A",
        base=base,
        vout_mv=vout_mv,
        vin_mv=vin_mv,
        iout_ma=iout_ma,
        iin_ma=iin_ma,
        pout_uw=pout_uw,
        pin_uw=pin_uw,
        eff_pcm=vals["eff_pcm"],
        temp_mc=temp_mc,
        capability=vals.get("capability"),
        status=vals.get("status"),
    )


def _vout_method(vout_mode: int, fallback_exp: int) -> str:
    mode = (vout_mode >> 5) & 0x7
    if mode == 2:
        return "direct-R3"
    exp = vout_mode & 0x1F if mode == 0 else fallback_exp
    if exp >= 16:
        exp -= 32
    exp = max(-16, min(15, exp))
    kind = "linear16-mode" if mode == 0 else "linear16-fallback"
    return f"{kind}(exp={exp})"


def format_temp_probe(s: VrmSample, vals: dict[str, int]) -> str:
    """0x8E controller die and 0x8F TEMP pin, beside the power-stage read.

    Both words are LINEAR11 millisi from nct6687_vrm_port, same as the kernel's
    0x8E decode. A signed 16-bit reading of 0x07FB is 2043, not −5 °C.
    """

    def line(cmd: str, name: str, prefix: str) -> str:
        if not vals.get(f"{prefix}_ok"):
            return f"  {cmd} {name:<12} no answer"
        raw = vals.get(f"{prefix}_raw", 0)
        mc = vals.get(f"{prefix}_mc", 0)
        return f"  {cmd} {name:<12} {mc / 1000:6.1f} °C  RAW 0x{raw:04x}"

    stage = f"  0x8D {'power stage':<12} {s.temp_c:6.1f} °C  RAW 0x{s.temp_raw:04x}"
    body = "\n".join(
        [
            f"PAGE {s.page} temperature registers:",
            stage,
            line("0x8E", "controller", "temp2"),
            line("0x8F", "TEMP pin", "temp3"),
        ]
    )
    return body + "\n"


def _sample_port(
    base: int,
    addr: int,
    port: int,
    page: int,
    vout_exp: int,
    *,
    debug: bool,
    temps: bool,
    force: bool = False,
) -> tuple[VrmSample, dict[str, int]]:
    """One PAGE sample through the C mailbox on /dev/port."""
    cmd = [
        str(_port_bin()),
        "--base",
        hex(base),
        "--addr",
        hex(addr),
        "--port",
        str(port),
        "--page",
        str(page),
        "--vout-exp",
        str(vout_exp),
    ]
    if debug:
        cmd.append("--raw")
    if temps:
        cmd.append("--temps")
    if force:
        cmd.append("--force")
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        err = proc.stderr.strip() or "PAGE sample failed"
        raise RuntimeError(err)
    vals: dict[str, int] = {}
    for line in proc.stdout.splitlines():
        key, sep, raw = line.partition("=")
        if sep == "=" and key:
            vals[key] = int(raw, 10)
    return (
        _sample_from_port_text(
            proc.stdout,
            base=base,
            addr=addr,
            page=page,
            vout_exp=vout_exp,
        ),
        vals,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--addr", type=lambda x: int(x, 0), default=DEFAULT_ADDR)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--base", type=lambda x: int(x, 0), default=None, help="EC I/O base (default: autodetect)")
    ap.add_argument(
        "--page",
        type=int,
        default=None,
        choices=(0, 1),
        help="PMBus PAGE only (0=CPU, 1=GT). Default: both",
    )
    ap.add_argument(
        "--vout-exp",
        type=int,
        default=DEFAULT_VOUT_EXP,
        help="Fallback LINEAR16 exp if VOUT_MODE unknown (clamped -16..15)",
    )
    ap.add_argument("--json", action="store_true")
    ap.add_argument(
        "--raw",
        action="store_true",
        help="Also fetch CAP 0x19 / STATUS 0x78 (debug-only; not on the kernel sample path)",
    )
    ap.add_argument(
        "--temps",
        action="store_true",
        help="Also read 0x8E (controller) and 0x8F (TEMP pin). Not on the kernel sample path.",
    )
    ap.add_argument("--loop", type=float, nargs="?", const=1.0, default=None)
    ap.add_argument(
        "--force",
        action="store_true",
        help="Allow run while nct6687.ko is loaded (unsafe: races the driver's EC I/O)",
    )
    args = ap.parse_args()

    if os.geteuid() != 0:
        print("Need root (ioperm / /dev/port)", file=sys.stderr)
        return 1
    if args.port not in (0, 1):
        print("Only ports 0/1 are safe on this board", file=sys.stderr)
        return 1

    block = userspace_guard(
        loaded=module_loaded("nct6687"),
        force=args.force,
        looping=args.loop is not None,
    )
    if block:
        print(block, file=sys.stderr)
        if "nct6687.ko is loaded (shares" in block:
            print("Unload it, or pass --force for a one-shot", file=sys.stderr)
        return 3

    base = args.base if args.base is not None else discover_base()
    pages = [args.page] if args.page is not None else [0, 1]
    vout_exp = max(-16, min(15, args.vout_exp))

    had_page_error = False
    try:
        while True:
            samples = []
            probes: list[dict[str, int]] = []
            for page in pages:
                try:
                    sample, vals = _sample_port(
                        base,
                        args.addr,
                        args.port,
                        page,
                        vout_exp,
                        debug=args.raw,
                        temps=args.temps,
                        force=args.force,
                    )
                    samples.append(sample)
                    probes.append(vals)
                except (RuntimeError, OSError) as e:
                    had_page_error = True
                    if len(pages) == 1:
                        raise
                    print(f"# PAGE {page}: {e}", file=sys.stderr)
            if not samples:
                print("ERROR: no successful PAGE samples", file=sys.stderr)
                return 2
            if args.json:
                payload = [{**asdict(s), "rail": PAGE_NAMES.get(s.page, f"PAGE{s.page}")} for s in samples]
                out = payload[0] if args.page is not None and len(payload) == 1 else payload
                print(json.dumps(out, indent=2 if args.loop is None else None))
            else:
                for s, vals in zip(samples, probes):
                    print(format_sensors(s), end="")
                    if args.temps:
                        print(format_temp_probe(s, vals), end="")
                    print()
            if args.loop is None:
                break
            sys.stdout.flush()
            time.sleep(args.loop)
        return 1 if had_page_error else 0
    except (RuntimeError, OSError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
