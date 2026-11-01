"""Host-compiled VRM demand session: due, backoff, and mux restore.

Live box (2026-09-20): after 1.2 s with no vrm_cpu reads, first cat returned
in 0.11 ms (stale cache). Immediate second read took 48.7 ms (PAGE sample).
A fan-triggered update_vrm had last_updated inside 1 s; first demand used
gap>=HZ → interval=HZ and skipped. Floor is 20 ms.

The session is the test surface. io=1 means a PAGE attempt touched the eSIO
window. A skip leaves the window alone.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

# Independent jiffy literals: HZ=1000 so 1 jiffy = 1 ms.
HZ = 1000
FLOOR = 20
IDLE_GAP = 1200  # 1.2 s since last vrm_cpu
FAN_CACHE_AGE = 100  # recent background PAGE, older than floor, younger than 1 s
ADDR = "0xc0"


@pytest.fixture(scope="session")
def c_mailbox_bin(tmp_path_factory: pytest.TempPathFactory) -> Path:
    out = tmp_path_factory.mktemp("vrm_demand") / "vrm_mailbox"
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


def _demand(
    c_mailbox_bin: Path,
    *,
    now: int,
    demanded: bool = False,
    last_read: int = 0,
    last_updated: int = 0,
    fails: int = 0,
    cpu_valid: bool = False,
    read_gap: int = 0,
    extra: tuple[str, ...] = (),
) -> dict[str, int]:
    cmd = [
        str(c_mailbox_bin),
        *extra,
        "--now",
        str(now),
        "--hz",
        str(HZ),
        "--floor",
        str(FLOOR),
        "--last-read",
        str(last_read),
        "--last-updated",
        str(last_updated),
        "--fails",
        str(fails),
        "--read-gap",
        str(read_gap),
    ]
    if demanded:
        cmd.append("--demand")
    if cpu_valid:
        cmd.append("--cpu-valid")
    cmd += ["demand", ADDR]
    out = subprocess.check_output(cmd, text=True)
    got: dict[str, int] = {}
    for line in out.splitlines():
        key, val = line.split("=", 1)
        got[key] = int(val)
    return got


def test_vrm_sysfs_poll_samples_mos_thermistor() -> None:
    text = (Path(__file__).resolve().parents[1] / "dkms" / "nct6687_vrm.inc.c").read_text()
    touch = text.split("nct_vrm_touch_and_update", 1)[1].split("\n}", 1)[0]
    locked = text.split("static int nct_vrm_locked", 1)[1].split("\n}", 1)[0]
    sample = text.split("static void nct_vrm_sample_mos", 1)[1].split("\n}", 1)[0]
    assert "nct_vrm_locked" in touch
    assert "vrm_demand = true" in locked
    assert "nct_vrm_session_lock" in locked
    assert "nct_vrm_sample_mos" in locked
    assert "nct6687_read" in sample
    assert "nct_vrm_note_mos" in sample


def test_first_demand_after_idle_samples_when_cache_older_than_20ms(
    c_mailbox_bin: Path,
) -> None:
    # last_read is 1.2 s behind now. Fan cache is 100 ms old.
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=10_000 - IDLE_GAP,
        last_updated=10_000 - FAN_CACHE_AGE,
        cpu_valid=True,
    )
    assert got["io"] == 1
    assert got["cpu_valid"] == 1
    assert got["fail"] == 0
    assert got["demand"] == 0
    assert got["ctrl_valid"] == 1


def test_background_valid_skips_when_cache_younger_than_1s(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=False,
        last_read=10_000 - 100,
        last_updated=10_000 - FAN_CACHE_AGE,
        cpu_valid=True,
    )
    assert got["io"] == 0
    assert got["fail"] == 0
    assert got["cpu_valid"] == 1


def test_background_valid_samples_after_1s(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=False,
        last_read=10_000 - HZ,
        last_updated=10_000 - (HZ + 1),
        cpu_valid=True,
    )
    assert got["io"] == 1
    assert got["fail"] == 0


def test_invalid_demand_skips_inside_hz_over_4(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=10_000 - 50,
        last_updated=10_000 - 249,
        fails=1,
        cpu_valid=False,
    )
    assert got["io"] == 0
    assert got["fail"] == 1


def test_invalid_samples_after_hz_over_4(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=10_000 - 50,
        last_updated=10_000 - 251,
        fails=1,
    )
    assert got["io"] == 1


def test_invalid_after_many_fails_skips_inside_8hz(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=20_000,
        demanded=True,
        last_updated=20_000 - (8 * HZ - 1),
        fails=4,
    )
    assert got["io"] == 0
    assert got["fail"] == 4


def test_invalid_after_many_fails_samples_after_8hz(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=20_000,
        demanded=True,
        last_updated=20_000 - (8 * HZ + 1),
        fails=4,
    )
    assert got["io"] == 1


def test_demand_matches_50ms_poll_gap(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=10_000 - 50,
        last_updated=10_000 - 51,
        cpu_valid=True,
    )
    assert got["io"] == 1
    assert got["read_gap"] == 50


def test_demand_floor_blocks_sub_20ms(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=10_000 - 10,
        last_updated=10_000 - 10,
        cpu_valid=True,
    )
    assert got["io"] == 0
    assert got["read_gap"] == 10


def test_first_ever_demand_uses_floor_not_a_leftover_gap(c_mailbox_bin: Path) -> None:
    # last_read==0 must not inherit a leftover 500 ms gap or a synthetic HZ.
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=0,
        last_updated=10_000 - FAN_CACHE_AGE,
        read_gap=500,
        cpu_valid=True,
    )
    assert got["io"] == 1
    assert got["read_gap"] == 0
    assert got["fail"] == 0


def test_first_ever_demand_still_respects_20ms_floor(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=0,
        last_updated=10_000 - 10,
        cpu_valid=True,
    )
    assert got["io"] == 0


def test_never_updated_cache_samples_on_demand(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=0,
        last_updated=0,
        cpu_valid=True,
    )
    assert got["io"] == 1


def test_controller_word_is_stored_as_millisi(c_mailbox_bin: Path) -> None:
    # 0x0023 is 35 °C, Direct 1 °C/count, same millisi path as 0x8D when N=0.
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=0,
        last_updated=0,
        extra=("--word", "0x8e=0x0023"),
    )
    assert got["cpu_valid"] == 1
    assert got["ctrl_valid"] == 1
    assert got["ctrl_mc"] == 35000


def test_controller_read_failure_drops_the_sample(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=0,
        last_updated=0,
        extra=("--fail", "read_word:0x8e=1"),
    )
    assert got["io"] == 1
    assert got["cpu_valid"] == 0
    assert got["ctrl_valid"] == 0
    assert got["fail"] == 1


def test_page_failure_increments_fail_and_does_not_clear_it(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=0,
        last_updated=0,
        fails=2,
        extra=("--fail", "read_word:0x8d=1"),
    )
    assert got["io"] == 1
    assert got["cpu_valid"] == 0
    assert got["fail"] == 3


def _kv(c_mailbox_bin: Path, args: tuple[str, ...]) -> dict[str, int]:
    out = subprocess.check_output([str(c_mailbox_bin), *args], text=True)
    got: dict[str, int] = {}
    for line in out.splitlines():
        key, val = line.split("=", 1)
        got[key] = int(val)
    return got


def test_shared_mux_restore_failure_leaves_the_armed_window(c_mailbox_bin: Path) -> None:
    # Saved cfg 0x05 / baud 0x11. Arm forces port 0 and baud 0x03. A rejected
    # restore must leave those armed values; the cycle reports it itself.
    got = _kv(
        c_mailbox_bin,
        (
            "--cfg61",
            "0x05",
            "--cfg62",
            "0x11",
            "--byte",
            "0x20=0x40",
            "--word",
            "0x8b=1000",
            "--reject-mux-restore",
            "mux",
            ADDR,
            "0",
        ),
    )
    assert got["restore_failed"] == 1
    assert got["cfg"] == 0x04
    assert got["baud"] == 0x03


def test_next_cycle_restores_the_pre_arm_mux_after_a_failed_restore(c_mailbox_bin: Path) -> None:
    got = _kv(
        c_mailbox_bin,
        (
            "--cfg61",
            "0x05",
            "--cfg62",
            "0x11",
            "--byte",
            "0x20=0x40",
            "--word",
            "0x8b=1000",
            "--reject-mux-restore",
            "--mux-recover",
            "mux",
            ADDR,
            "0",
        ),
    )
    assert got["restore_failed"] == 0
    assert got["cfg"] == 0x05
    assert got["baud"] == 0x11


def test_shared_mux_cycle_writes_the_saved_regs_back(c_mailbox_bin: Path) -> None:
    got = _kv(
        c_mailbox_bin,
        (
            "--cfg61",
            "0x05",
            "--cfg62",
            "0x11",
            "--byte",
            "0x20=0x40",
            "--word",
            "0x8b=1000",
            "mux",
            ADDR,
            "0",
        ),
    )
    assert got["restore_failed"] == 0
    assert got["cfg"] == 0x05
    assert got["baud"] == 0x11


def test_mux_restore_failure_drops_the_sample_and_backs_off(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=0,
        last_updated=0,
        fails=1,
        extra=("--byte", "0x20=0x40", "--word", "0x8b=1000", "--reject-mux-restore"),
    )
    assert got["io"] == 1
    assert got["cpu_valid"] == 0
    assert got["fail"] == 2
    assert got["vout"] == 1000


def test_gt_failure_backs_off_without_dropping_the_cpu_rail(c_mailbox_bin: Path) -> None:
    # Second 0x8D is PAGE 1. 200 ms later the CPU rail is due again and the
    # GT retry (HZ/4 = 250 ms) is not.
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=0,
        last_updated=0,
        extra=(
            "--gt",
            "--fail-nth",
            "read_word:0x8d=2",
            "--then-now",
            "10200",
            "--then-last-read",
            "10100",
        ),
    )
    assert got["cpu_valid"] == 1
    assert got["gt_valid"] == 0
    assert got["gt_fails"] == 1
    assert got["fail"] == 0
    assert got["last_updated"] == 10200


def test_gt_retries_after_its_backoff(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=0,
        last_updated=0,
        extra=(
            "--gt",
            "--fail-nth",
            "read_word:0x8d=2",
            "--then-now",
            "10251",
            "--then-last-read",
            "10100",
        ),
    )
    assert got["cpu_valid"] == 1
    assert got["gt_valid"] == 1
    assert got["gt_fails"] == 0
    assert got["last_updated"] == 10251


def test_good_sample_stores_vout_and_clears_fail(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=0,
        last_updated=0,
        fails=3,
        extra=("--byte", "0x20=0x40", "--word", "0x8b=1000"),
    )
    assert got["io"] == 1
    assert got["cpu_valid"] == 1
    assert got["fail"] == 0
    assert got["vout"] == 1000
    assert got["last_updated"] == 10_000
