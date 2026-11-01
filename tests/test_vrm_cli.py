"""Userspace reader must not loop against a loaded nct6687.ko."""

from __future__ import annotations

import json
import subprocess
import sys
from dataclasses import asdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import nct6687_vrm as vrm  # noqa: E402

# nct6687_vrm_port.c stdout. Millisi are already decoded; raw words are separate.
_PORT_STDOUT = """\
page=0
addr=192
base=2592
vout_mv=1234
vin_mv=12000
iout_ma=22900
pout_uw=50000000
temp_mc=40000
iin_ma=5610
pin_uw=101000000
eff_pcm=49505
vout_mode=64
vout_raw=229
vin_raw=1200
iout_raw=229
pout_raw=50
temp_raw=40
iin_raw=561
pin_raw=101
"""


def test_loaded_without_force_is_blocked() -> None:
    assert vrm.userspace_guard(loaded=True, force=False, looping=False) is not None


def test_one_shot_force_is_allowed() -> None:
    assert vrm.userspace_guard(loaded=True, force=True, looping=False) is None


def test_force_loop_while_loaded_is_blocked() -> None:
    msg = vrm.userspace_guard(loaded=True, force=True, looping=True)
    assert msg is not None
    assert "--loop" in msg


def test_unloaded_loop_is_allowed() -> None:
    assert vrm.userspace_guard(loaded=False, force=False, looping=True) is None


def test_port_tool_compiles(tmp_path: Path) -> None:
    src = Path(__file__).resolve().parents[1] / "nct6687_vrm_port.c"
    out = tmp_path / "nct6687_vrm_port"
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


def test_port_text_json_emits_millisi() -> None:
    sample = vrm._sample_from_port_text(
        _PORT_STDOUT,
        base=0xA20,
        addr=0xC0,
        page=0,
        vout_exp=-10,
    )
    sensors = vrm.format_sensors(sample)
    assert "1.234" in sensors
    assert "direct-R3" in sensors

    payload = {**asdict(sample), "rail": vrm.PAGE_NAMES.get(sample.page, f"PAGE{sample.page}")}
    data = json.loads(json.dumps(payload))
    assert data["rail"] == "CPU"
    assert data["vout_mv"] == 1234
    assert data["vin_mv"] == 12000
    assert data["iout_ma"] == 22900
    assert data["iin_ma"] == 5610
    assert data["pout_uw"] == 50000000
    assert data["pin_uw"] == 101000000
    assert data["eff_pcm"] == 49505
    assert data["temp_mc"] == 40000
    assert data["vout_raw"] == 229
    assert "sts_page_wr" not in data
