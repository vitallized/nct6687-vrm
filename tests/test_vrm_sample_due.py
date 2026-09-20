"""Host-compiled VRM PAGE sample/skip: first demand after idle must not use 1 Hz.

Live box (2026-09-20): after 1.2 s with no vrm_cpu reads, first cat returned
in 0.11 ms (stale cache). Immediate second read took 48.7 ms (PAGE sample).
A fan-triggered update_vrm had last_updated inside 1 s; first demand used
gap>=HZ → interval=HZ and skipped. Floor is 20 ms.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

# Independent jiffy literals: HZ=1000 so 1 jiffy = 1 ms.
HZ = 1000
FLOOR = 20  # 20 ms demand floor
IDLE_GAP = 1200  # 1.2 s since last vrm_cpu
FAN_CACHE_AGE = 100  # recent background PAGE, older than floor, younger than 1 s


@pytest.fixture(scope="session")
def c_decode_bin(tmp_path_factory: pytest.TempPathFactory) -> Path:
    out = tmp_path_factory.mktemp("vrm_sample_due") / "vrm_decode"
    src = Path(__file__).with_name("vrm_decode.c")
    subprocess.check_call(
        [
            "cc",
            "-Wall",
            "-Werror",
            "-DNCT_VRM_DECODE_HOST",
            "-o",
            str(out),
            str(src),
        ]
    )
    return out


def _should_sample(
    c_decode_bin: Path,
    valid: int,
    demanded: int,
    last_read_set: int,
    gap: int,
    last_updated_age: int,
    hz: int = HZ,
    floor: int = FLOOR,
) -> bool:
    cmd = [
        str(c_decode_bin),
        "should_sample",
        str(valid),
        str(demanded),
        str(last_read_set),
        str(gap),
        str(last_updated_age),
        str(hz),
        str(floor),
    ]
    out = subprocess.check_output(cmd, text=True).strip()
    return out == "1"


def test_first_demand_after_1_2s_idle_samples_if_cache_older_than_20ms(
    c_decode_bin: Path,
) -> None:
    assert _should_sample(
        c_decode_bin,
        valid=1,
        demanded=1,
        last_read_set=1,
        gap=IDLE_GAP,
        last_updated_age=FAN_CACHE_AGE,
    )


def test_background_valid_skips_when_cache_younger_than_1s(c_decode_bin: Path) -> None:
    assert not _should_sample(
        c_decode_bin,
        valid=1,
        demanded=0,
        last_read_set=1,
        gap=IDLE_GAP,
        last_updated_age=FAN_CACHE_AGE,
    )


def test_background_valid_samples_after_1s(c_decode_bin: Path) -> None:
    assert _should_sample(
        c_decode_bin,
        valid=1,
        demanded=0,
        last_read_set=0,
        gap=0,
        last_updated_age=1001,
    )


def test_invalid_demand_skips_inside_hz_over_4(c_decode_bin: Path) -> None:
    assert not _should_sample(
        c_decode_bin,
        valid=0,
        demanded=1,
        last_read_set=1,
        gap=50,
        last_updated_age=200,
    )


def test_invalid_samples_after_hz_over_4(c_decode_bin: Path) -> None:
    assert _should_sample(
        c_decode_bin,
        valid=0,
        demanded=0,
        last_read_set=0,
        gap=0,
        last_updated_age=251,
    )


def test_demand_matches_50ms_poll_gap(c_decode_bin: Path) -> None:
    assert not _should_sample(
        c_decode_bin,
        valid=1,
        demanded=1,
        last_read_set=1,
        gap=50,
        last_updated_age=40,
    )
    assert _should_sample(
        c_decode_bin,
        valid=1,
        demanded=1,
        last_read_set=1,
        gap=50,
        last_updated_age=51,
    )


def test_demand_floor_blocks_sub_20ms(c_decode_bin: Path) -> None:
    assert not _should_sample(
        c_decode_bin,
        valid=1,
        demanded=1,
        last_read_set=1,
        gap=10,
        last_updated_age=15,
    )
    assert _should_sample(
        c_decode_bin,
        valid=1,
        demanded=1,
        last_read_set=1,
        gap=10,
        last_updated_age=21,
    )


def test_first_ever_demand_uses_floor_not_stale_or_synthetic_gap(
    c_decode_bin: Path,
) -> None:
    # last_read==0 must not inherit a leftover 500 ms gap or synthetic HZ.
    assert _should_sample(
        c_decode_bin,
        valid=1,
        demanded=1,
        last_read_set=0,
        gap=500,
        last_updated_age=FAN_CACHE_AGE,
    )
    assert _should_sample(
        c_decode_bin,
        valid=1,
        demanded=1,
        last_read_set=0,
        gap=HZ,
        last_updated_age=FAN_CACHE_AGE,
    )


def test_first_ever_demand_still_respects_20ms_floor(c_decode_bin: Path) -> None:
    assert not _should_sample(
        c_decode_bin,
        valid=1,
        demanded=1,
        last_read_set=0,
        gap=0,
        last_updated_age=10,
    )


def test_never_updated_cache_samples_on_demand(c_decode_bin: Path) -> None:
    assert _should_sample(
        c_decode_bin,
        valid=1,
        demanded=1,
        last_read_set=0,
        gap=0,
        last_updated_age=100000,
    )


def test_kernel_calls_should_sample_and_does_not_synthesize_hz_gap() -> None:
    inc = Path(__file__).resolve().parents[1] / "dkms" / "nct6687_vrm.inc.c"
    text = inc.read_text()
    assert "nct_vrm_should_sample(" in text
    assert "vrm_last_read ? now - data->vrm_last_read : HZ" not in text
