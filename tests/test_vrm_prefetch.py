"""Prefetch timing: one sample per poll, started so it finishes just before the next read."""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest


@pytest.fixture(scope="module")
def prefetch_bin(tmp_path_factory: pytest.TempPathFactory) -> dict[str, str]:
    out = tmp_path_factory.mktemp("vrm_prefetch") / "vrm_prefetch"
    src = Path(__file__).with_name("vrm_prefetch.c")
    subprocess.check_call(
        ["cc", "-Wall", "-Werror", "-Wno-unused-function", "-o", str(out), str(src)]
    )
    text = subprocess.check_output([str(out)], text=True)
    got: dict[str, str] = {}
    for line in text.splitlines():
        key, val = line.split("=", 1)
        got[key] = val
    return got


def test_first_reads_and_a_burst_do_not_prefetch(prefetch_bin: dict[str, str]) -> None:
    assert prefetch_bin["first"] == "now 0"
    assert prefetch_bin["burst"] == "none 0"
    assert prefetch_bin["seed"] == "now 0"
    # Stable on this read, but the sample is still a full interval old, so
    # the act stays now. The delay is the next period: 1000 - 80 - 40.
    assert prefetch_bin["confirm"] == "now 880"


def test_steady_1s_poll_starts_before_the_next_read(prefetch_bin: dict[str, str]) -> None:
    # The confirming sample measured 70 ms. Margin is 40. 1000 - 70 - 40 = 890.
    assert prefetch_bin["fresh_1s"] == "later 890"


def test_fallback_completion_arms_the_next_prefetch_from_the_burst(
    prefetch_bin: dict[str, str],
) -> None:
    # Confirming read at t=2000, sample ends at 2070, measured duration 70.
    # Start at 2000 + 1000 - 70 - 40 = 2890, so delay from 2070 is 820.
    assert prefetch_bin["followup"] == "820"


def test_250ms_and_2s_polls_use_the_same_margin(prefetch_bin: dict[str, str]) -> None:
    assert prefetch_bin["poll_250"] == "later 130"
    assert prefetch_bin["poll_2000"] == "later 1880"


def test_a_poll_faster_than_the_sample_stays_on_kick(prefetch_bin: dict[str, str]) -> None:
    assert prefetch_bin["fast"] == "now 0"


def test_a_30_percent_miss_drops_prefetch_until_the_cadence_returns(
    prefetch_bin: dict[str, str],
) -> None:
    assert prefetch_bin["jitter"] == "now 0"
    assert prefetch_bin["stable_after_jitter"] == "0"


def test_debug_age_is_frozen_at_the_demand_burst(prefetch_bin: dict[str, str]) -> None:
    assert prefetch_bin["seed_age"] == "900"
    assert prefetch_bin["seed_act"] == "now"
    # A read 20 ms later is the same burst. It must not replace the frozen age.
    assert prefetch_bin["held_age"] == "900"
    assert prefetch_bin["held_act"] == "now"
    # 60 ms is one sample, still the same poll pass.
    assert prefetch_bin["pass_60"] == "none 0"
    assert prefetch_bin["pass_60_age"] == "10"


def test_steady_poll_stays_a_hit_after_lock_in(prefetch_bin: dict[str, str]) -> None:
    # 20 reads at 1.13 s. After the first few, every one is a prefetch hit
    # under 100 ms. A hit that does not re-arm shows up as poll_late > 0.
    assert prefetch_bin["poll_late"] == "0"
    assert prefetch_bin["poll_4"].startswith("later ")
    age = int(prefetch_bin["poll_4"].split()[1])
    assert age < 100
    assert prefetch_bin["poll_19"].startswith("later ")


def test_two_paces_fall_back(prefetch_bin: dict[str, str]) -> None:
    assert prefetch_bin["jitter"] == "now 0"
    assert prefetch_bin["jitter_act"] == "now"


def test_an_early_prefetch_shrinks_the_duration_used_to_schedule(
    prefetch_bin: dict[str, str],
) -> None:
    # 300 ms old, margin 40. 327 - 260 = 67. Still inside this gap, so no
    # second refresh. The next one is 1000 - 67 - 40 = 893 ms out.
    assert prefetch_bin["early"] == "later 893"
    assert prefetch_bin["corrected_dur"] == "67"
    assert prefetch_bin["after_correct"] == "later 893"
    assert prefetch_bin["floor_dur"] == "40"
    assert prefetch_bin["dur_ign"] == "70"


def test_idle_is_three_intervals_after_the_last_burst(prefetch_bin: dict[str, str]) -> None:
    assert prefetch_bin["not_idle"] == "0"
    assert prefetch_bin["idle"] == "1"
