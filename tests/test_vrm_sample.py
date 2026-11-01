"""PAGE sample golden traces for the kernel C mailbox.

tests/golden_vrm_sample.json is the PMBus transaction trace for
nct_vrm_sample_page. The host-compiled mailbox runs that function over a
fake eSIO window.
"""

from __future__ import annotations

import errno
import json
import subprocess
from pathlib import Path

import pytest

GOLDEN = json.loads(Path(__file__).with_name("golden_vrm_sample.json").read_text())
SCENARIOS = GOLDEN["scenarios"]
ADDR = 0xC0
MILLISI = ("vout_mv", "vin_mv", "iout_ma", "pout_uw", "temp_mc", "iin_ma", "pin_uw")


def _int(v: int | str) -> int:
    return int(v, 0) if isinstance(v, str) else v


def _modes(raw: list) -> list[int | None]:
    return [None if m is None else _int(m) for m in raw]


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
