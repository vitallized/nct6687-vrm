"""PAGE sample golden traces: kernel C and userspace Python must match.

tests/golden_vrm_sample.json is the PMBus transaction trace for
nct_vrm_sample_page. The C side runs the real kernel function (plus the real
eSIO mailbox ops) host-compiled against a fake PMBus device; the Python side
replays _sample_page through a FakeXfer serving the same device.
"""

from __future__ import annotations

import errno
import json
import subprocess
from pathlib import Path

import pytest

from nct6687_vrm import _SmbusCache, _sample_page, read_vrm

GOLDEN = json.loads(Path(__file__).with_name("golden_vrm_sample.json").read_text())
SCENARIOS = GOLDEN["scenarios"]
ADDR = 0xC0
MILLISI = ("vout_mv", "vin_mv", "iout_ma", "pout_uw", "temp_mc")


def _int(v: int | str) -> int:
    return int(v, 0) if isinstance(v, str) else v


def _modes(raw: list) -> list[int | None]:
    return [None if m is None else _int(m) for m in raw]


class FakeXfer:
    """Golden PMBus device as a ByteXfer. Records PMBus ops and eSIO calls."""

    def __init__(
        self,
        device: dict | None = None,
        *,
        cfg61: int = 0x10,
        cfg62: int = 0x07,
    ) -> None:
        device = device or {}
        self.bytes_ = {_int(k): _int(v) for k, v in device.get("bytes", {}).items()}
        self.words = {_int(k): _int(v) for k, v in device.get("words", {}).items()}
        self.fail = {
            (f["op"], _int(f["cmd"])): _int(f["status"]) for f in device.get("fail", [])
        }
        rb = device.get("page_readback")
        self.page_readback = None if rb is None else _int(rb)
        self.page = 0
        self.ops: list[str] = []
        self.log: list[tuple] = []
        self.recover_calls = 0
        self.cfg61 = cfg61
        self.cfg62 = cfg62

    def write_byte(self, addr: int, cmd: int, value: int) -> int:
        self.ops.append(f"write_byte {cmd:#04x} {value:#04x}")
        sts = self.fail.get(("write_byte", cmd), 0)
        if not sts and cmd == 0x00:
            self.page = value
        return sts

    def read_byte(self, addr: int, cmd: int) -> tuple[int, int]:
        self.ops.append(f"read_byte {cmd:#04x}")
        if cmd != 0x00:
            value = self.bytes_.get(cmd, 0)
        elif self.page_readback is not None:
            value = self.page_readback
        else:
            value = self.page
        return self.fail.get(("read_byte", cmd), 0), value

    def read_word(self, addr: int, cmd: int) -> tuple[int, int]:
        self.ops.append(f"read_word {cmd:#04x}")
        return self.fail.get(("read_word", cmd), 0), self.words.get(cmd, 0)

    def esio_read(self, page: int, index: int) -> int:
        self.log.append(("esio_read", page, index))
        if index == 0x61:
            return self.cfg61
        if index == 0x62:
            return self.cfg62
        return 0

    def esio_write(self, index: int, value: int) -> None:
        self.log.append(("esio_write", index, value))

    def recover(self) -> None:
        self.recover_calls += 1
        self.log.append(("recover",))


@pytest.fixture(scope="session")
def c_mailbox_bin(tmp_path_factory: pytest.TempPathFactory) -> Path:
    out = tmp_path_factory.mktemp("vrm_sample") / "vrm_mailbox"
    src = Path(__file__).with_name("vrm_mailbox.c")
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


def _c_args(sc: dict) -> list[str]:
    args = ["--cache-page", str(sc["cache"]["page"])]
    for p, m in enumerate(_modes(sc["cache"]["vout_mode"])):
        if m is not None:
            args += ["--cache-mode", f"{p}={m:#x}"]
    dev = sc["device"]
    for k, v in dev.get("bytes", {}).items():
        args += ["--byte", f"{_int(k):#x}={_int(v):#x}"]
    for k, v in dev.get("words", {}).items():
        args += ["--word", f"{_int(k):#x}={_int(v):#x}"]
    for f in dev.get("fail", []):
        args += ["--fail", f"{f['op']}:{_int(f['cmd']):#x}={_int(f['status']):#x}"]
    if dev.get("page_readback") is not None:
        args += ["--page-readback", f"{_int(dev['page_readback']):#x}"]
    return [*args, "sample_page", f"{ADDR:#x}", str(sc["page"]), str(sc["vout_exp"])]


@pytest.mark.parametrize("sc", SCENARIOS, ids=lambda s: s["name"])
def test_c_sample_page_matches_golden(c_mailbox_bin: Path, sc: dict) -> None:
    proc = subprocess.run(
        [str(c_mailbox_bin), *_c_args(sc)], capture_output=True, text=True, check=False
    )
    assert proc.returncode in (0, 1), proc.stderr
    rc = None
    ops: list[str] = []
    out: dict[str, int] | None = None
    cache_page = None
    modes: list[int | None] = [None, None]
    for line in proc.stdout.splitlines():
        if line.startswith("pmbus "):
            ops.append(line.removeprefix("pmbus "))
            continue
        key, _, val = line.partition("=")
        if key == "rc":
            rc = int(val)
        elif key == "out":
            out = dict(zip(MILLISI, (int(v) for v in val.split())))
        elif key == "cache_page":
            cache_page = int(val)
        elif key.startswith("vout_mode"):
            modes[int(key[-1])] = None if val == "-" else int(val, 0)

    exp = sc["expect"]
    assert ops == exp["ops"]
    if exp["error"]:
        assert rc == -errno.EIO
        assert out is None
    else:
        assert rc == 0
        assert out == exp["millisi"]
    assert cache_page == exp["cache"]["page"]
    assert modes == _modes(exp["cache"]["vout_mode"])


@pytest.mark.parametrize("sc", SCENARIOS, ids=lambda s: s["name"])
def test_python_sample_page_matches_golden(sc: dict) -> None:
    xfer = FakeXfer(sc["device"])
    cache = _SmbusCache()
    cache.page = sc["cache"]["page"]
    cache.vout_mode = _modes(sc["cache"]["vout_mode"])

    exp = sc["expect"]
    if exp["error"]:
        with pytest.raises(RuntimeError):
            _sample_page(xfer, cache, ADDR, sc["page"], sc["vout_exp"], False, 0)
    else:
        s = _sample_page(xfer, cache, ADDR, sc["page"], sc["vout_exp"], False, 0)
        assert {k: getattr(s, k) for k in MILLISI} == exp["millisi"]
        # SI floats are the same millisi, scaled.
        assert s.vout_v == pytest.approx(s.vout_mv / 1000.0)
        assert s.iout_a == pytest.approx(s.iout_ma / 1000.0)
        assert s.pout_w == pytest.approx(s.pout_uw / 1_000_000.0)
    assert xfer.ops == exp["ops"]
    assert cache.page == exp["cache"]["page"]
    assert cache.vout_mode == _modes(exp["cache"]["vout_mode"])


# read_vrm wraps the PAGE sample with eSIO mux/baud save/restore and recover;
# those are not PMBus traffic, so they stay outside the golden.

GOOD = SCENARIOS[0]["device"]


def _kinds(log: list[tuple], kind: str) -> list[tuple]:
    return [e for e in log if e[0] == kind]


def test_cfg_61_62_save_and_restore() -> None:
    xfer = FakeXfer(GOOD, cfg61=0x15, cfg62=0x09)
    read_vrm(xfer=xfer, port=0)
    assert ("esio_read", 4, 0x61) in xfer.log
    assert ("esio_read", 4, 0x62) in xfer.log
    writes = _kinds(xfer.log, "esio_write")
    assert ("esio_write", 0x61, (0x15 & ~0x03) | 0x00) in writes
    assert ("esio_write", 0x62, 0x03) in writes
    assert writes[-2:] == [("esio_write", 0x61, 0x15), ("esio_write", 0x62, 0x09)]


def test_failure_calls_recover_and_still_restores_cfg() -> None:
    dev = {**GOOD, "fail": [{"op": "write_byte", "cmd": "0x00", "status": "0x02"}]}
    xfer = FakeXfer(dev)
    with pytest.raises(RuntimeError, match="PAGE write"):
        read_vrm(xfer=xfer, page=0)
    assert xfer.recover_calls >= 1
    writes = _kinds(xfer.log, "esio_write")
    assert ("esio_write", 0x61, 0x10) in writes
    assert ("esio_write", 0x62, 0x07) in writes


def test_read_vrm_reuses_cache_across_calls() -> None:
    xfer = FakeXfer(GOOD)
    read_vrm(xfer=xfer, page=0)
    xfer.ops.clear()
    read_vrm(xfer=xfer, page=0)
    assert xfer.ops == SCENARIOS[1]["expect"]["ops"]


def test_debug_raw_fetches_cap_and_status() -> None:
    dev = {**GOOD, "bytes": {**GOOD["bytes"], "0x19": "0x80", "0x78": "0x01"}}
    xfer = FakeXfer(dev)
    s = read_vrm(xfer=xfer, page=0, debug=True)
    # Golden production ops first, then the debug-only reads.
    assert xfer.ops == [*SCENARIOS[0]["expect"]["ops"], "read_byte 0x19", "read_byte 0x78"]
    assert s.capability == 0x80
    assert s.status == 0x01
