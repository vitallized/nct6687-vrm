#!/usr/bin/env python3
"""NCT6687 eSIO PMBus VRM reader (MSI MS-7D89 / RAA-class @ SMBus 0xC0).

Uses the HWiNFO-proven path:
  base from nct6687 platform device (fallback 0xA20)
  page@base+4, index@base+5, data@base+6
  port 0, addr 0xC0
  PAGE 0 = CPU Vcore, PAGE 1 = GT/iGPU (19+1+1)
  write-byte proto 0x02 (payload @ 0x70); read-byte/word 0x82/0x83

Examples:
  sudo python3 nct6687_vrm.py --page 0
  sudo python3 nct6687_vrm.py --force   # allow with nct6687.ko loaded
  sudo python3 nct6687_vrm.py --loop 1

Safety: no address scan, no block reads. Refuses if nct6687.ko is loaded
unless --force (shared A24–A26 window has no userspace lock).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path

DEFAULT_BASE = 0xA20
SMB_EN, SMB_START, SMB_CLEAR = 0x80, 0x40, 0x08
PROTO_WRITE_BYTE, PROTO_BYTE, PROTO_WORD = 0x02, 0x82, 0x83

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


class Ports:
    def __init__(self, base: int) -> None:
        self.base = base
        self.cmd = base + 4
        self.idx = base + 5
        self.dat = base + 6
        self.fd = os.open("/dev/port", os.O_RDWR | os.O_SYNC)

    def close(self) -> None:
        os.close(self.fd)

    def outb(self, port: int, val: int) -> None:
        os.lseek(self.fd, port, os.SEEK_SET)
        n = os.write(self.fd, bytes([val & 0xFF]))
        if n != 1:
            raise OSError(f"short write to /dev/port ({n})")

    def inb(self, port: int) -> int:
        os.lseek(self.fd, port, os.SEEK_SET)
        b = os.read(self.fd, 1)
        if not b:
            raise OSError("short read from /dev/port")
        return b[0]


def idle(p: Ports, spins: int = 5) -> bool:
    """Force eSIO PAGE to 0xFF. Stock nct6687.ko leaves PAGE != 0xFF."""
    if p.inb(p.cmd) == 0xFF:
        return True
    p.outb(p.cmd, 0xFF)
    for _ in range(spins):
        if p.inb(p.cmd) == 0xFF:
            return True
        time.sleep(0.0001)
    return p.inb(p.cmd) == 0xFF


def esio_write(p: Ports, index: int, value: int) -> None:
    if not idle(p):
        raise RuntimeError("eSIO idle failed (PAGE never 0xFF)")
    p.outb(p.cmd, 0x04)
    p.outb(p.idx, index & 0xFF)
    p.outb(p.dat, value & 0xFF)
    p.outb(p.cmd, 0xFF)


def esio_read(p: Ports, page: int, index: int) -> int:
    if not idle(p):
        raise RuntimeError("eSIO idle failed (PAGE never 0xFF)")
    p.outb(p.cmd, page & 0xFF)
    p.outb(p.idx, index & 0xFF)
    val = p.inb(p.dat)
    p.outb(p.cmd, 0xFF)
    return val


def prep_clear(p: Ports) -> None:
    esio_write(p, 0x03, 0xFF)
    esio_write(p, 0x04, 0xFF)
    ctrl = esio_read(p, 4, 0x60)
    esio_write(p, 0x60, (ctrl | SMB_CLEAR) & ~SMB_START)
    esio_write(p, 0x60, ctrl & ~(SMB_START | SMB_CLEAR))


def wait_start_clear(p: Ports, ms: int = 100) -> bool:
    for _ in range(ms):
        if (esio_read(p, 4, 0x60) & SMB_START) == 0:
            return True
        time.sleep(0.001)
    return False


def set_port(p: Ports, port: int) -> int:
    """Set SMBus port mux; return previous cfg (reg 0x61)."""
    cfg = esio_read(p, 4, 0x61)
    esio_write(p, 0x61, (cfg & ~0x03) | (port & 0x03))
    return cfg


def set_baud_100k(p: Ports) -> int:
    prev = esio_read(p, 4, 0x62)
    esio_write(p, 0x62, 0x03)
    return prev


def bus_recover(p: Ports) -> None:
    try:
        prep_clear(p)
        esio_write(p, 0x60, 0x00)
    except (OSError, RuntimeError):
        pass


def smbus_write_byte(p: Ports, addr8: int, cmd: int, value: int) -> int:
    """Return status byte (0=ok). -2 = START timeout."""
    prep_clear(p)
    esio_write(p, 0x63, PROTO_WRITE_BYTE)
    esio_write(p, 0x65, addr8 & 0xFF)
    esio_write(p, 0x66, cmd & 0xFF)
    esio_write(p, 0x70, value & 0xFF)
    esio_write(p, 0x60, SMB_EN)
    time.sleep(0.001)
    esio_write(p, 0x60, SMB_EN | SMB_START)
    if not wait_start_clear(p):
        return -2
    return esio_read(p, 4, 0x03)


def smbus_read(p: Ports, addr8: int, cmd: int, word: bool) -> tuple[int, bytes]:
    prep_clear(p)
    esio_write(p, 0x63, PROTO_WORD if word else PROTO_BYTE)
    esio_write(p, 0x65, addr8 & 0xFF)
    esio_write(p, 0x66, cmd & 0xFF)
    esio_write(p, 0x60, SMB_EN)
    time.sleep(0.001)
    esio_write(p, 0x60, SMB_EN | SMB_START)
    if not wait_start_clear(p):
        return -2, b""
    sts = esio_read(p, 4, 0x03)
    if sts:
        return sts, b""
    lo = esio_read(p, 4, 0xB0)
    if not word:
        return sts, bytes([lo])
    hi = esio_read(p, 4, 0xB1)
    return sts, bytes([lo, hi])


def linear11(raw: int) -> float:
    raw &= 0xFFFF
    exp = (raw >> 11) & 0x1F
    if exp >= 16:
        exp -= 32
    mant = raw & 0x7FF
    if mant >= 1024:
        mant -= 2048
    return mant * (2.0**exp)


def linear16(raw: int, exp: int) -> float:
    return (raw & 0xFFFF) * (2.0**exp)


def decode_vout(raw: int, vout_mode: int, fallback_exp: int) -> tuple[float, str]:
    """Return (volts, method). Respects PMBus VOUT_MODE when possible."""
    mode = (vout_mode >> 5) & 0x7
    if mode == 2:
        # Direct — Renesas DMPVR2 uses R=3 → mV = raw
        return (raw & 0xFFFF) * 0.001, "direct-R3"
    if mode == 0:
        exp = vout_mode & 0x1F
        if exp >= 16:
            exp -= 32
        exp = max(-16, min(15, exp))
        return linear16(raw, exp), f"linear16-mode(exp={exp})"
    exp = max(-16, min(15, fallback_exp))
    return linear16(raw, exp), f"linear16-fallback(exp={exp})"


@dataclass
class VrmSample:
    addr: int
    page: int
    sts_page_wr: int
    capability: int
    status: int
    vout_mode: int
    vout_raw: int
    iout_raw: int
    pout_raw: int
    vin_raw: int
    temp_raw: int
    vout_v: float
    iout_a: float
    pout_w: float
    vin_v: float
    temp_c: float
    vout_method: str
    iout_method: str
    base: int


PAGE_NAMES = {0: "CPU", 1: "GT"}


def read_vrm(
    p: Ports,
    addr: int = DEFAULT_ADDR,
    port: int = DEFAULT_PORT,
    page: int = 0,
    vout_exp: int = DEFAULT_VOUT_EXP,
) -> VrmSample:
    cfg61 = set_port(p, port)
    cfg62 = set_baud_100k(p)
    esio_write(p, 0x60, SMB_EN)

    try:
        sts_wr = smbus_write_byte(p, addr, 0x00, page)
        if sts_wr == -2:
            bus_recover(p)
            raise RuntimeError("SMBus START timeout on PAGE write — power-cycle recommended")
        if sts_wr != 0:
            bus_recover(p)
            raise RuntimeError(f"PAGE write NACK/status={sts_wr:#04x}")

        def rb(cmd: int) -> int:
            sts, data = smbus_read(p, addr, cmd, False)
            if sts == -2:
                bus_recover(p)
                raise RuntimeError(f"START timeout on cmd {cmd:#04x}")
            if sts != 0:
                bus_recover(p)
                raise RuntimeError(f"cmd {cmd:#04x} status={sts:#04x}")
            return data[0] if data else 0

        def rw(cmd: int) -> int:
            sts, data = smbus_read(p, addr, cmd, True)
            if sts == -2:
                bus_recover(p)
                raise RuntimeError(f"START timeout on cmd {cmd:#04x}")
            if sts != 0:
                bus_recover(p)
                raise RuntimeError(f"cmd {cmd:#04x} status={sts:#04x}")
            if len(data) < 2:
                bus_recover(p)
                raise RuntimeError(f"cmd {cmd:#04x} short read")
            return data[0] | (data[1] << 8)

        page_r = rb(0x00)
        if page_r != page:
            bus_recover(p)
            raise RuntimeError(f"PAGE readback mismatch: wrote {page}, got {page_r}")

        cap = rb(0x19)
        vout_mode = rb(0x20)
        status = rb(0x78)
        vout = rw(0x8B)
        iout = rw(0x8C)
        pout = rw(0x96)
        vin = rw(0x88)
        temp = rw(0x8D)

        vout_v, vout_method = decode_vout(vout, vout_mode, vout_exp)
        pout_w = linear11(pout)
        temp_c = linear11(temp)
        if vout_v > 0.2:
            iout_a = pout_w / vout_v
            iout_method = "P/V"
        else:
            iout_a = linear16(iout, -3)
            iout_method = "linear16-N=-3"
        # Renesas DMPVR2 Direct VIN: m=1,b=0,R=2 → 10 mV/LSB
        vin_v = vin * 0.01

        return VrmSample(
            addr=addr,
            page=page_r,
            sts_page_wr=sts_wr,
            capability=cap,
            status=status,
            vout_mode=vout_mode,
            vout_raw=vout,
            iout_raw=iout,
            pout_raw=pout,
            vin_raw=vin,
            temp_raw=temp,
            vout_v=vout_v,
            iout_a=iout_a,
            pout_w=pout_w,
            vin_v=vin_v,
            temp_c=temp_c,
            vout_method=vout_method,
            iout_method=iout_method,
            base=p.base,
        )
    finally:
        try:
            esio_write(p, 0x60, 0x00)
            esio_write(p, 0x61, cfg61)
            esio_write(p, 0x62, cfg62)
        except (OSError, RuntimeError):
            bus_recover(p)


def format_sensors(s: VrmSample, name: str | None = None) -> str:
    rail = name or PAGE_NAMES.get(s.page, f"PAGE{s.page}")
    lines = [
        f"nct6687-vrm-0x{s.addr:02x}-{rail.lower()}",
        f"Adapter: NCT6687 eSIO SMBus @ 0x{s.base:04x}",
        f"VRM {rail} Voltage: {s.vout_v:8.3f} V  (RAW 0x{s.vout_raw:04x}, {s.vout_method})",
        f"VRM {rail} Current: {s.iout_a:8.3f} A  ({s.iout_method}; RAW 0x{s.iout_raw:04x})",
        f"VRM {rail} Power:   {s.pout_w:8.3f} W  (RAW 0x{s.pout_raw:04x})",
        f"VRM {rail} VIN:     {s.vin_v:8.3f} V  (Direct 10mV; RAW 0x{s.vin_raw:04x})",
        f"VRM {rail} Temp:    {s.temp_c:8.1f} °C (RAW 0x{s.temp_raw:04x})",
        f"CAPABILITY:    0x{s.capability:02x}",
        f"STATUS_BYTE:   0x{s.status:02x}",
        f"VOUT_MODE:     0x{s.vout_mode:02x}",
        f"PAGE:          {s.page} ({rail})",
    ]
    return "\n".join(lines) + "\n"


def module_loaded(name: str) -> bool:
    return Path(f"/sys/module/{name}").is_dir()


def process_running(name: str) -> bool:
    for d in Path("/proc").iterdir():
        if not d.name.isdigit():
            continue
        try:
            comm = (d / "comm").read_text().strip()
        except OSError:
            continue
        if comm == name:
            return True
    return False


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
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--loop", type=float, nargs="?", const=1.0, default=None)
    ap.add_argument(
        "--force",
        action="store_true",
        help="Allow run while nct6687.ko / coolercontrold are active (unsafe race)",
    )
    args = ap.parse_args()

    if os.geteuid() != 0:
        print("Need root (ioperm / /dev/port)", file=sys.stderr)
        return 1
    if args.port not in (0, 1):
        print("Only ports 0/1 are safe on this board", file=sys.stderr)
        return 1

    blockers = []
    if module_loaded("nct6687"):
        blockers.append("nct6687.ko is loaded (shares eSIO A24–A26)")
    if process_running("coolercontrold"):
        blockers.append("coolercontrold is running")
    if blockers and not args.force:
        for b in blockers:
            print(f"Refusing: {b}", file=sys.stderr)
        print("Pause CoolerControl / unload nct6687, or pass --force", file=sys.stderr)
        return 3

    base = args.base if args.base is not None else discover_base()
    pages = [args.page] if args.page is not None else [0, 1]
    vout_exp = max(-16, min(15, args.vout_exp))

    p = Ports(base)
    had_page_error = False
    try:
        while True:
            samples = []
            for page in pages:
                try:
                    samples.append(read_vrm(p, args.addr, args.port, page, vout_exp))
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
                # Always a list when multiple pages requested; single object only for --page N
                out = payload[0] if args.page is not None and len(payload) == 1 else payload
                print(json.dumps(out, indent=2 if args.loop is None else None))
            else:
                for s in samples:
                    print(format_sensors(s), end="")
                    if args.raw:
                        print(
                            f"# raw page_wr_sts={s.sts_page_wr} "
                            f"vout={s.vout_raw:#06x} iout={s.iout_raw:#06x} "
                            f"pout={s.pout_raw:#06x} vin={s.vin_raw:#06x} temp={s.temp_raw:#06x}"
                        )
                    print()
            if args.loop is None:
                break
            sys.stdout.flush()
            time.sleep(args.loop)
        return 1 if had_page_error else 0
    except (RuntimeError, OSError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    finally:
        try:
            bus_recover(p)
            idle(p, 10)
        except Exception:
            pass
        p.close()


if __name__ == "__main__":
    raise SystemExit(main())
