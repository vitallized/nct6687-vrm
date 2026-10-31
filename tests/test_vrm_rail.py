"""Host-compiled rail slot: nct_vrm_rail_apply stores a PAGE sample and folds
it into the software hist.

Live box (2026-09-20): IOUT hist min=-13900 mA after sane ~17614 mA current;
POUT hist min=-17000000 uW. Those two mins were one LINEAR11 POUT spike via
the old P/V current. Hist still drops a negative IOUT or POUT millisi sample.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

METRICS = ("vout", "vin", "iout", "pout", "temp", "iin", "pin")


@pytest.fixture(scope="session")
def c_decode_bin(tmp_path_factory: pytest.TempPathFactory) -> Path:
    out = tmp_path_factory.mktemp("vrm_rail") / "vrm_decode"
    src = Path(__file__).with_name("vrm_decode.c")
    subprocess.check_call(
        ["cc", "-Wall", "-Werror", "-DNCT_VRM_DECODE_HOST", "-o", str(out), str(src)]
    )
    return out


def _rail(c_decode_bin: Path, *samples: tuple[int, int, int, int, int]) -> dict:
    """Apply samples to one fresh rail slot.

    Returns {"valid": bool, metric: (val, hist_ready, min, max)}.
    """
    args = [",".join(str(v) for v in s) for s in samples]
    out = subprocess.check_output(
        [str(c_decode_bin), "rail_seq", *args], text=True
    ).splitlines()
    got: dict = {}
    for line in out:
        name, *vals = line.split()
        if name == "valid":
            got["valid"] = vals == ["1"]
        else:
            val, ready, mn, mx = (int(v) for v in vals)
            got[name] = (val, bool(ready), mn, mx)
    assert set(got) == {"valid", *METRICS}
    return got


def test_fresh_slot_is_invalid_and_hist_not_ready(c_decode_bin: Path) -> None:
    r = _rail(c_decode_bin)
    assert r["valid"] is False
    assert all(not r[m][1] for m in METRICS)


def test_first_sample_stores_and_seeds_every_metric(c_decode_bin: Path) -> None:
    r = _rail(c_decode_bin, (1234, 12000, 40518, 50000000, 40000, 5610, 101000000))
    assert r["valid"] is True
    assert r["vout"] == (1234, True, 1234, 1234)
    assert r["vin"] == (12000, True, 12000, 12000)
    assert r["iout"] == (40518, True, 40518, 40518)
    assert r["pout"] == (50000000, True, 50000000, 50000000)
    assert r["temp"] == (40000, True, 40000, 40000)
    assert r["iin"] == (5610, True, 5610, 5610)
    assert r["pin"] == (101000000, True, 101000000, 101000000)


def test_store_is_latest_sample_and_hist_spans_all(c_decode_bin: Path) -> None:
    r = _rail(
        c_decode_bin,
        (1200, 12000, 16168, 20000000, 40000, 5000, 90000000),
        (1100, 12100, 17614, 22000000, 45000, 7000, 120000000),
        (1150, 11900, 17000, 21000000, 42000, 6000, 100000000),
    )
    assert r["vout"] == (1150, True, 1100, 1200)
    assert r["vin"] == (11900, True, 11900, 12100)
    assert r["iout"] == (17000, True, 16168, 17614)
    assert r["pout"] == (21000000, True, 20000000, 22000000)
    assert r["temp"] == (42000, True, 40000, 45000)
    assert r["iin"] == (6000, True, 5000, 7000)
    assert r["pin"] == (100000000, True, 90000000, 120000000)


def test_negative_iout_pout_spike_does_not_become_hist_min(c_decode_bin: Path) -> None:
    r = _rail(
        c_decode_bin,
        (1234, 12000, 17614, 22000000, 40000, 5610, 101000000),
        (1234, 12000, -13900, -17000000, 40000, -10, -1000000),
    )
    # Input still shows the spike; only the hist rejects it.
    assert r["iout"] == (-13900, True, 17614, 17614)
    assert r["pout"] == (-17000000, True, 22000000, 22000000)
    assert r["iin"] == (-10, True, 5610, 5610)
    assert r["pin"] == (-1000000, True, 101000000, 101000000)


def test_first_negative_iout_pout_does_not_seed_their_hist(c_decode_bin: Path) -> None:
    r = _rail(c_decode_bin, (1234, 12000, -13900, -17000000, 40000, -10, -1000000))
    assert r["valid"] is True
    assert r["iout"][:2] == (-13900, False)
    assert r["pout"][:2] == (-17000000, False)
    assert r["iin"][:2] == (-10, False)
    assert r["pin"][:2] == (-1000000, False)
    # VOUT/VIN/TEMP share one seed and are ready after the same sample.
    assert r["vout"] == (1234, True, 1234, 1234)
    assert r["vin"] == (12000, True, 12000, 12000)
    assert r["temp"] == (40000, True, 40000, 40000)


def test_iout_and_pout_seed_independently(c_decode_bin: Path) -> None:
    r = _rail(
        c_decode_bin,
        (1234, 12000, -5, 1000000, 40000, 5610, 101000000),
        (1234, 12000, 0, -1, 40000, 5610, 101000000),
        (1234, 12000, 16168, 2000000, 40000, 5610, 101000000),
    )
    assert r["iout"] == (16168, True, 0, 16168)
    assert r["pout"] == (2000000, True, 1000000, 2000000)


def test_vout_vin_temp_still_latch_negative(c_decode_bin: Path) -> None:
    r = _rail(
        c_decode_bin,
        (1309, 12000, 1000, 1000000, 40000, 5610, 101000000),
        (-5, -10, 1000, 1000000, -5, 5610, 101000000),
    )
    assert r["vout"] == (-5, True, -5, 1309)
    assert r["vin"] == (-10, True, -10, 12000)
    assert r["temp"] == (-5, True, -5, 40000)


def test_sensors_prints_pout_low_then_high_and_mos_hist() -> None:
    """sensors labels come from the sysfs name, not from the stored min/max."""
    text = (Path(__file__).resolve().parents[1] / "dkms" / "nct6687_vrm.inc.c").read_text()
    assert "power1_input_lowest" in text
    assert "power1_input_highest" in text
    assert "power3_input_lowest" in text
    assert "power3_input_highest" in text
    assert "power1_min" not in text
    assert "power3_min" not in text
    assert "power1_max" not in text
    assert "humidity1_input" in text and "humidity1_label" in text
    assert "humidity1_min" not in text
    assert "temp3_min" in text and "temp3_max" in text
    assert "temp20_min" in text and "temp20_max" in text
