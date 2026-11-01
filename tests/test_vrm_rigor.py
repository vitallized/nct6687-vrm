"""Every 16-bit word, then the publish and efficiency rules.

The hand-picked cases miss a wrap that is one count away from the sample
they named. This walks all 65536 values through the real decode and the
real rail slot.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest


@pytest.fixture(scope="session")
def sweep(tmp_path_factory: pytest.TempPathFactory) -> str:
    out = tmp_path_factory.mktemp("vrm_rigor") / "vrm_decode"
    src = Path(__file__).with_name("vrm_decode.c")
    subprocess.check_call(
        ["cc", "-Wall", "-Werror", "-DNCT_VRM_DECODE_HOST", "-o", str(out), str(src)]
    )
    return subprocess.check_output([str(out), "sweep_rigor"], text=True)


def _line(sweep: str, key: str) -> str:
    for line in sweep.splitlines():
        if line.startswith(key + " "):
            return line.split(" ", 1)[1]
    raise AssertionError(key)


def _blend(prev: int, sample: int, dt: int, tau: int = 10000) -> int:
    return prev + (sample - prev) * dt // (tau + dt)


def test_every_direct_word_is_twos_complement(sweep: str) -> None:
    assert _line(sweep, "decode_ok") == "196608"
    assert "witness iout_fff6 -1000" in sweep
    assert "witness iin_ffde -340" in sweep
    assert "witness pin_ffff -1000000" in sweep
    assert "witness pout_ffff -500000" in sweep
    bucket = [int(x) for x in _line(sweep, "pin_bucket").split()]
    assert bucket == [(raw - 65536) * 1_000_000 for raw in range(65525, 65536)]
    # sensors prints this whole window as 65.53 kW.
    for raw in range(65525, 65536):
        assert f"{raw / 1000:.2f}" == "65.53"


def test_no_negative_word_is_published_or_kept_as_a_high(sweep: str) -> None:
    # The four words that showed up as 6552 A, 655 A, 65 kW, and a LINEAR11
    # −0.5 W. Live and high stay on the previous good sample.
    assert "held 22900 1830 32000000 33000000 4" in sweep
    # Half of the u16 space is negative. The high is the last positive count.
    assert _line(sweep, "iout_neg") == "32768"
    assert _line(sweep, "iin_neg") == "32768"
    assert _line(sweep, "pin_neg") == "32768"
    assert _line(sweep, "iout_high") == str(32767 * 100)
    assert _line(sweep, "iin_high") == str(32767 * 10)
    assert _line(sweep, "pin_high") == str(32767 * 1_000_000)
    assert _line(sweep, "iout_live") == _line(sweep, "iout_high")
    assert _line(sweep, "iin_live") == _line(sweep, "iin_high")
    assert _line(sweep, "pin_live") == _line(sweep, "pin_high")
    assert int(_line(sweep, "pout_neg")) > 0
    assert int(_line(sweep, "pout_live")) >= 0
    assert int(_line(sweep, "pout_high")) >= int(_line(sweep, "pout_live"))
    assert _line(sweep, "publish_ok") == "262144"


def test_efficiency_skips_a_non_positive_power(sweep: str) -> None:
    assert "eff_neg_pin 83 30000000 36000000" in sweep
    assert "eff_neg_pout 83 30000000 36000000" in sweep
    assert "eff_zero 83 30000000 36000000" in sweep
    # Skipped samples do not move the timer, so the next good pair blends
    # across the whole gap back to the last accepted one (t=0 → t=4000).
    pout = _blend(30_000_000, 40_000_000, 4000)
    pin = _blend(36_000_000, 50_000_000, 4000)
    pcm = pout * 100000 // pin
    pct = (pcm + 500) // 1000
    assert f"eff_resume {pct} {pout} {pin}" in sweep
    assert "eff_idle_clears nodata" in sweep
    assert sweep.strip().endswith("ok")
