"""Host-compiled software hist: IOUT/POUT must not latch a negative millisi sample.

Live box (2026-09-20): curr1_min=-13900 mA after sane ~17614 mA current;
power1_min=-17000000 uW. Those two mins are one LINEAR11 POUT spike via P/V.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest


@pytest.fixture(scope="session")
def c_decode_bin(tmp_path_factory: pytest.TempPathFactory) -> Path:
    out = tmp_path_factory.mktemp("vrm_hist") / "vrm_decode"
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


def _hist_seq(c_decode_bin: Path, mode: str, *vals: int) -> tuple[int, int, int]:
    cmd = [str(c_decode_bin), "hist_seq", mode, *[str(v) for v in vals]]
    out = subprocess.check_output(cmd, text=True).strip().split()
    return int(out[0]), int(out[1]), int(out[2])


def test_negative_iout_does_not_become_hist_min(c_decode_bin: Path) -> None:
    init, mn, mx = _hist_seq(c_decode_bin, "nonneg", 17614, -13900)
    assert (init, mn, mx) == (1, 17614, 17614)


def test_negative_pout_does_not_become_hist_min(c_decode_bin: Path) -> None:
    init, mn, mx = _hist_seq(c_decode_bin, "nonneg", 22000000, -17000000)
    assert (init, mn, mx) == (1, 22000000, 22000000)


def test_first_negative_does_not_seed_hist(c_decode_bin: Path) -> None:
    init, mn, mx = _hist_seq(c_decode_bin, "nonneg", -13900)
    assert (init, mn, mx) == (0, 0, 0)


def test_zero_iout_is_a_valid_hist_sample(c_decode_bin: Path) -> None:
    init, mn, mx = _hist_seq(c_decode_bin, "nonneg", 0, 16168)
    assert (init, mn, mx) == (1, 0, 16168)


def test_always_mode_still_latches_negative(c_decode_bin: Path) -> None:
    init, mn, mx = _hist_seq(c_decode_bin, "always", 1309, -5)
    assert (init, mn, mx) == (1, -5, 1309)
