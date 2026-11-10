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
        elif name == "noted":
            got["noted"] = int(vals[0])
        else:
            val, ready, mn, mx = (int(v) for v in vals)
            got[name] = (val, bool(ready), mn, mx)
    assert set(got) == {"valid", "noted", *METRICS}
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


def test_eff_smooth_converges_and_caps_and_resets(c_decode_bin: Path) -> None:
    mid_p, mid_i, late_p, late_i = (
        int(x)
        for x in subprocess.check_output(
            [str(c_decode_bin), "eff_converge"], text=True
        ).split()
    )
    # 100 ms steps toward 1 W / 2 W. At 10 s the blend is most of the way there.
    assert 500_000 < mid_p < 750_000
    assert 1_000_000 < mid_i < 1_500_000
    assert late_p > 950_000
    assert late_i > 1_900_000

    def published(pout: str, pin: str) -> str:
        return subprocess.check_output(
            [str(c_decode_bin), "eff_publish", pout, pin], text=True
        ).strip()

    assert published("50000000", "100000000") == "100"
    assert published("100000000", "100000000") == "200"
    assert published("120000000", "100000000") == "200"


def test_eff_ignores_a_lone_spike_and_follows_a_real_step(c_decode_bin: Path) -> None:
    lines = subprocess.check_output(
        [str(c_decode_bin), "eff_spike"], text=True
    ).splitlines()
    # Default 100x lets 190 W beside 30 W through. 2.5x would reject it.
    assert lines[0] == "1"
    assert lines[1] == "0"
    # 30 W / 36 W is 83%. Skipping that spike leaves it there.
    assert abs(int(lines[2]) - 83) <= 2
    step_pct, step_pout, step_pin = (int(x) for x in lines[3].split())
    assert step_pct == 100
    assert step_pout > 120_000_000
    assert step_pin > 120_000_000

    reset = subprocess.check_output(
        [str(c_decode_bin), "eff_reset"], text=True
    ).splitlines()
    assert reset == ["1000 2000", "nodata"]

    held = subprocess.check_output(
        [str(c_decode_bin), "eff_hold"], text=True
    ).splitlines()
    # 30 W / 36 W stays 83% when the next PIN is negative. A negative first pair is nodata.
    assert held == ["83 30000000 36000000", "nodata"]


def test_cpu_full_is_one_line_in_rail_order(c_decode_bin: Path) -> None:
    out = subprocess.check_output(
        [
            str(c_decode_bin),
            "cpu_full",
            "1255",
            "12120",
            "20500",
            "2850",
            "36000000",
            "32000000",
        ],
        text=True,
    )
    assert out == "1255 12120 20500 2850 36000000 32000000\n"


def test_signed_current_words_are_small_negatives(c_decode_bin: Path) -> None:
    # 0xFFF6 is -10 counts → -1.0 A. 0xFFDE is -34 counts → -0.34 A.
    iout = subprocess.check_output(
        [str(c_decode_bin), "decode_iout_ma", "0xFFF6"], text=True
    ).strip()
    iin = subprocess.check_output(
        [str(c_decode_bin), "decode_iin_ma", "0xFFDE"], text=True
    ).strip()
    assert iout == "-1000"
    assert iin == "-340"


def test_pin_all_ones_does_not_stick_as_65kw_high(c_decode_bin: Path) -> None:
    # 0xFFFF is -1 W. Unsigned it is 65535 W, which sensors prints as 65.53 kW.
    pin = subprocess.check_output(
        [str(c_decode_bin), "decode_pin_uw", "0xFFFF"], text=True
    ).strip()
    assert pin == "-1000000"
    r = _rail(
        c_decode_bin,
        (1260, 12130, 22900, 32000000, 43000, 1830, 33000000),
        (1260, 12130, 22900, 32000000, 43000, 1830, int(pin)),
    )
    assert r["noted"] == 1
    assert r["pin"] == (33000000, True, 33000000, 33000000)


def test_negative_iout_pout_spike_does_not_become_hist_min(c_decode_bin: Path) -> None:
    r = _rail(
        c_decode_bin,
        (1234, 12000, 17614, 22000000, 40000, 5610, 101000000),
        (1234, 12000, -13900, -17000000, 40000, -10, -1000000),
    )
    # Live stays on the last non-negative sample. Hist does not move.
    assert r["noted"] == 4
    assert r["iout"] == (17614, True, 17614, 17614)
    assert r["pout"] == (22000000, True, 22000000, 22000000)
    assert r["iin"] == (5610, True, 5610, 5610)
    assert r["pin"] == (101000000, True, 101000000, 101000000)


def test_first_negative_iout_pout_does_not_seed_their_hist(c_decode_bin: Path) -> None:
    r = _rail(c_decode_bin, (1234, 12000, -13900, -17000000, 40000, -10, -1000000))
    assert r["valid"] is True
    assert r["noted"] == 4
    assert r["iout"][:2] == (0, False)
    assert r["pout"][:2] == (0, False)
    assert r["iin"][:2] == (0, False)
    assert r["pin"][:2] == (0, False)
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



def test_temp_above_200c_is_not_published(c_decode_bin: Path) -> None:
    # 200 °C is the last published reading; one step above keeps the last one.
    r = _rail(
        c_decode_bin,
        (1234, 12000, 1000, 1000000, 40000, 5610, 101000000),
        (1234, 12000, 1000, 1000000, 200001, 5610, 101000000),
    )
    assert r["noted"] == 1
    assert r["temp"] == (40000, True, 40000, 40000)
    r = _rail(c_decode_bin, (1234, 12000, 1000, 1000000, 200000, 5610, 101000000))
    assert r["noted"] == 0
    assert r["temp"] == (200000, True, 200000, 200000)


def test_first_temp_above_200c_does_not_seed_its_hist(c_decode_bin: Path) -> None:
    r = _rail(c_decode_bin, (1234, 12000, 1000, 1000000, 1023000, 5610, 101000000))
    assert r["valid"] is True
    assert r["temp"][:2] == (0, False)
    # VOUT and VIN still seed on that sample.
    assert r["vout"] == (1234, True, 1234, 1234)


def _ctrl(c_decode_bin: Path, *samples: int) -> dict[str, int]:
    out = subprocess.check_output(
        [str(c_decode_bin), "ctrl_seq", *[str(s) for s in samples]], text=True
    )
    return {k: int(v) for k, v in (line.split() for line in out.splitlines())}


def test_ctrl_temp_above_200c_keeps_the_last_reading(c_decode_bin: Path) -> None:
    assert _ctrl(c_decode_bin, 35000, 250000) == {
        "valid": 1, "cur": 35000, "min": 35000, "max": 35000, "noted": 1,
    }
    # Never published: stays invalid, so sensors shows nothing for it.
    assert _ctrl(c_decode_bin, 250000)["valid"] == 0

def _mos(c_decode_bin: Path, *samples: int) -> tuple[int, int, int]:
    out = subprocess.check_output(
        [str(c_decode_bin), "mos_seq", *[str(s) for s in samples]], text=True
    )
    got = {}
    for line in out.splitlines():
        name, val = line.split()
        got[name] = int(val)
    return got["init"], got["min"], got["max"]


def test_hist_hides_until_the_live_value_is_back(c_decode_bin: Path) -> None:
    def published(live: str, ready: str) -> str:
        return subprocess.check_output(
            [str(c_decode_bin), "publish_hist", live, ready], text=True
        ).strip()

    assert published("0", "1") == "0"
    assert published("1", "0") == "0"
    assert published("1", "1") == "1"


def test_mos_milli_matches_stock_half_degree(c_decode_bin: Path) -> None:
    # 36 °C and the 0.5 °C fraction bit. Negative byte 0xFB is -5 °C.
    assert subprocess.check_output(
        [str(c_decode_bin), "mos_milli", "36", "0x80"], text=True
    ).strip() == "36500"
    assert subprocess.check_output(
        [str(c_decode_bin), "mos_milli", "0xfb", "0"], text=True
    ).strip() == "-5000"


def test_ctrl_seeds_current_and_tracks_signed_temp(c_decode_bin: Path) -> None:
    out = subprocess.check_output(
        [str(c_decode_bin), "ctrl_seq", "35000", "37000", "34000"], text=True
    )
    got = {}
    for line in out.splitlines():
        key, val = line.split()
        got[key] = int(val)
    assert got == {"valid": 1, "cur": 34000, "min": 34000, "max": 37000, "noted": 0}


def test_mos_seeds_and_tracks_signed_temp(c_decode_bin: Path) -> None:
    assert _mos(c_decode_bin, 38000, 40000, 37000) == (1, 37000, 40000)


def test_mos_first_negative_seeds(c_decode_bin: Path) -> None:
    assert _mos(c_decode_bin, -5000, 1000) == (1, -5000, 1000)


# sensors prints these from the hwmon filename. Banned names are the ones
# that would print in the wrong order, alias a fan pwm, or are not published.
_PUBLISHED_NEED = (
    "power1_input_lowest",
    "power1_input_highest",
    "power3_input_lowest",
    "power3_input_highest",
    "pwm9_label",
    "temp3_min",
    "temp3_max",
    "temp20_min",
    "temp20_max",
)
_PUBLISHED_BANNED = (
    "power1_min",
    "power1_max",
    "power3_min",
    "pwm10",
    "humidity1",
    "pwm9_enable",
)


def _attr_names(c_decode_bin: Path) -> set[str]:
    """Published hwmon filenames from `vrm_decode attr_names`."""
    lines = subprocess.check_output(
        [str(c_decode_bin), "attr_names"], text=True
    ).splitlines()
    names = [line for line in lines if line]
    assert names
    assert len(names) == len(set(names))
    return set(names)


def test_sensors_prints_pout_low_then_high_and_mos_hist(c_decode_bin: Path) -> None:
    """sensors labels come from the published hwmon filename, not stored min/max.

    The name list is the host contract (`vrm_decode attr_names`). Rail and MOS
    hist values stay on rail_seq and mos_seq.
    """
    names = _attr_names(c_decode_bin)
    missing = [n for n in _PUBLISHED_NEED if n not in names]
    assert not missing
    banned = [
        n
        for n in names
        if any(n == b or n.startswith(b + "_") for b in _PUBLISHED_BANNED)
    ]
    assert not banned
