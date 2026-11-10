"""Host-compiled VRM demand session: due, backoff, and mux restore.

Live box (2026-09-20): after 1.2 s with no vrm_cpu reads, first cat returned
in 0.11 ms (stale cache). Immediate second read took 48.7 ms (PAGE sample).
A fan-triggered update_vrm had last_updated inside 1 s; first demand used
gap>=HZ → interval=HZ and skipped. Floor is 20 ms.

The session is the test surface. io=1 means a PAGE attempt touched the eSIO
window. A skip leaves the window alone.
"""

from __future__ import annotations

import errno
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


def test_vrm_sysfs_read_returns_the_cached_sample() -> None:
    text = (Path(__file__).resolve().parents[1] / "dkms" / "nct6687_vrm.inc.c").read_text()
    enter = text.split("static void nct_vrm_session_enter", 1)[1].split("\nstatic ", 1)[0]
    show = text.split("static ssize_t nct_vrm_show", 1)[1].split("\nstatic ", 1)[0]
    cpu = text.split("static ssize_t vrm_cpu_show", 1)[1].split("\nstatic ", 1)[0]
    sample = text.split("static void nct_vrm_sample_mos", 1)[1].split("\nstatic ", 1)[0]
    assert "nct_vrm_session_enter" not in show
    assert "nct_vrm_session_enter" not in cpu
    assert "nct_vrm_snap_read" in show
    assert "nct_vrm_snap_read" in cpu
    # vrm_cpu shows VOUT IOUT POUT only, so it may get a lite sample; every
    # other VRM read asks for a full one.
    assert "nct_vrm_kick(data, true, true)" in show
    assert "nct_vrm_kick(data, true, false)" in cpu
    assert "cancel_delayed_work_sync" in text
    assert "nct_vrm_prefetch_note" in text
    run = text.split("static void nct_vrm_run_sample", 1)[1].split("\nstatic void nct_vrm_now_work_fn", 1)[0]
    assert "if (prefetch)" in run
    assert "vrm_demand" in run
    assert "vrm_sample_age" in text
    for name in ("vrm_sample_age_show", "vrm_sample_age_at_read_show", "vrm_prefetch_state_show", "vrm_busy_ns_show"):
        body = text.split(f"static ssize_t {name}", 1)[1].split("\nstatic ", 1)[0]
        assert "nct_vrm_kick" not in body
        assert "nct_vrm_prefetch_note(" not in body
    assert "vrm_demand = true" in enter
    assert "nct_vrm_session_lock" in enter
    assert "nct_vrm_sample_mos" in enter
    assert "want_demand" in enter
    assert "time_after" in enter
    assert "last_updated" in enter
    assert "nct6687_read_pair" in sample
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
    assert got["sleep_busy"] == 0


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


def test_demand_samples_when_stamps_share_a_jiffy(c_mailbox_bin: Path) -> None:
    # Production stamps last_updated and last_read from the same jiffy.
    # A 50 ms poll must sample, not wait until the cache is 100 ms old.
    got = _demand(
        c_mailbox_bin,
        now=10_050,
        demanded=True,
        last_read=10_000,
        last_updated=10_000,
        cpu_valid=True,
    )
    assert got["io"] == 1
    assert got["read_gap"] == 50


def test_demand_floor_samples_at_20ms(c_mailbox_bin: Path) -> None:
    got = _demand(
        c_mailbox_bin,
        now=10_020,
        demanded=True,
        last_read=10_000,
        last_updated=10_000,
        cpu_valid=True,
    )
    assert got["io"] == 1
    assert got["read_gap"] == 20


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


def test_prefetch_floor_samples_when_the_gap_rule_skips(c_mailbox_bin: Path) -> None:
    # Kick stamped last_read, then the PAGE sample finished 70 ms later.
    # Prefetch starts 990 ms after that stamp. Cache age is 920. The gap
    # rule wants 990 and skips. The prefetch floor still samples.
    skipped = _demand(
        c_mailbox_bin,
        now=10_990,
        demanded=True,
        last_read=10_000,
        last_updated=10_070,
        cpu_valid=True,
    )
    assert skipped["io"] == 0
    got = _demand(
        c_mailbox_bin,
        now=10_990,
        demanded=True,
        last_read=10_000,
        last_updated=10_070,
        cpu_valid=True,
        extra=("--force-floor",),
    )
    assert got["io"] == 1


def test_prefetch_floor_keeps_the_20ms_floor_and_fail_backoff(c_mailbox_bin: Path) -> None:
    young = _demand(
        c_mailbox_bin,
        now=10_010,
        demanded=True,
        last_read=10_000,
        last_updated=10_000,
        cpu_valid=True,
        extra=("--force-floor",),
    )
    assert young["io"] == 0
    backed_off = _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_updated=10_000 - 100,
        fails=4,
        extra=("--force-floor",),
    )
    assert backed_off["io"] == 0


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
            # A GT attribute read asks for a full sample (a lite one skips GT).
            "--then-full",
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


# Lite samples: a vrm_cpu-only reader gets VOUT, IOUT, POUT (3 words), and the
# other words keep the last full sample, until NCT_VRM_FULL_EVERY (2 * HZ).
PREV_RAIL = "1200,12000,5000,6000000,45000,600,7000000"  # VOUT VIN IOUT POUT TEMP IIN PIN


def _lite(c_mailbox_bin: Path, *extra: str, full_at: int = 9_500) -> dict[str, int]:
    return _demand(
        c_mailbox_bin,
        now=10_000,
        demanded=True,
        last_read=9_900,
        last_updated=9_900,
        cpu_valid=True,
        # PAGE and VOUT_MODE already cached, as after the first sample.
        extra=("--cache-page", "0", "--cache-mode", "0=0x17", "--full-at", str(full_at), "--rail", PREV_RAIL, *extra),
    )


def test_cpu_only_demand_within_full_every_reads_three_words(c_mailbox_bin: Path) -> None:
    got = _lite(c_mailbox_bin)
    # mux save/arm/restore are not PMBus ops; the body is VOUT, IOUT, POUT.
    assert got["pm_ops"] == 3
    assert got["full_at"] == 9_500
    # The words not read keep the previous full sample's values.
    assert (got["rail1"], got["rail4"], got["rail5"], got["rail6"]) == (12000, 45000, 600, 7000000)
    assert got["last_updated"] == 10_000


def test_full_reader_gets_every_word_and_the_controller(c_mailbox_bin: Path) -> None:
    got = _lite(c_mailbox_bin, "--want-full")
    assert got["pm_ops"] == 8  # VOUT IOUT POUT PIN IIN VIN TEMP + controller 0x8E
    assert got["full_at"] == 10_000
    assert got["want_full"] == 0


def test_lite_turns_full_after_full_every(c_mailbox_bin: Path) -> None:
    got = _lite(c_mailbox_bin, full_at=10_000 - 2 * HZ)
    assert got["pm_ops"] == 8
    assert got["full_at"] == 10_000


def test_lite_waits_while_a_full_reader_is_recent(c_mailbox_bin: Path) -> None:
    # hwinfo or sensors read 3 s ago: every sample stays full, so POUT and PIN
    # shown together come from one sample.
    got = _lite(c_mailbox_bin, "--full-asked-at", str(10_000 - 3 * HZ))
    assert got["pm_ops"] == 8
    got = _lite(c_mailbox_bin, "--full-asked-at", str(10_000 - 5 * HZ))
    assert got["pm_ops"] == 3


def test_session_checks_idle_once_and_nopause_keeps_the_access_count(c_mailbox_bin: Path) -> None:
    base = _lite(c_mailbox_bin, "--want-full")
    nopause = _lite(c_mailbox_bin, "--want-full", "--fast", "1")
    # The session left the window idle itself after its first op.
    assert base["idle_checks"] == 1
    # NOPAUSE changes the instruction (inb vs inb_p), not the accesses.
    assert nopause["ports"] == base["ports"]


def _full_fast(c_mailbox_bin: Path, fast: int, *extra: str) -> dict[str, int]:
    return _lite(c_mailbox_bin, "--want-full", "--fast", str(fast),
                 "--word", "0x8b=0x0500", "--word", "0x8c=0x0100", "--word", "0x96=0x0050", *extra)


def test_each_shortcut_cuts_accesses_and_keeps_the_values(c_mailbox_bin: Path) -> None:
    base = _full_fast(c_mailbox_bin, 1)
    seen = {}
    for bits in (2, 4, 8, 14):
        got = _full_fast(c_mailbox_bin, 1 | bits)
        assert got["ports"] < base["ports"], bits
        assert got["shortcuts_dead"] == 0, bits
        assert [got[f"rail{m}"] for m in range(7)] == [base[f"rail{m}"] for m in range(7)], bits
        seen[bits] = got["ports"]
    # All three together: about a third fewer accesses than vrm_fast=1.
    assert seen[14] < base["ports"] * 0.72, (base["ports"], seen)


def test_a_failed_sample_turns_the_shortcuts_off(c_mailbox_bin: Path) -> None:
    got = _full_fast(c_mailbox_bin, 15, "--fail", "read_word:0x96=0x02")
    assert got["cpu_valid"] == 0
    assert got["shortcuts_dead"] == 1
    assert got["shortcuts_warned"] == 14


def test_a_zero_vout_with_shortcuts_fails_the_sample(c_mailbox_bin: Path) -> None:
    got = _full_fast(c_mailbox_bin, 15, "--word", "0x8b=0x0000")
    assert got["shortcuts_dead"] == 1
    # Without shortcuts a zero word is published as before.
    got = _full_fast(c_mailbox_bin, 1, "--word", "0x8b=0x0000")
    assert got["shortcuts_dead"] == 0
    assert got["cpu_valid"] == 1


def test_one_write_start_drops_the_en_write_and_keeps_the_values(c_mailbox_bin: Path) -> None:
    base = _full_fast(c_mailbox_bin, 15)
    one = _full_fast(c_mailbox_bin, 31)
    assert one["cpu_valid"] == 1
    assert one["shortcuts_dead"] == 0
    assert [one[f"rail{m}"] for m in range(7)] == [base[f"rail{m}"] for m in range(7)]
    # One 0x60=EN write (page, index, data, back to 0xFF) fewer per word.
    assert one["pm_ops"] == base["pm_ops"]
    assert one["ports"] == base["ports"] - 4 * base["pm_ops"]


def test_a_failed_sample_turns_one_write_start_off_with_the_rest(c_mailbox_bin: Path) -> None:
    got = _full_fast(c_mailbox_bin, 31, "--fail", "read_word:0x96=0x02")
    assert got["shortcuts_dead"] == 1
    assert got["shortcuts_warned"] == 30


def test_mux_keep_leaves_the_firmware_port_and_speed_alone(c_mailbox_bin: Path) -> None:
    # Port bits 0 and a speed the driver would not pick: nothing writes 0x61/0x62.
    got = _lite(c_mailbox_bin, "--want-full", "--cfg61", "0x04", "--cfg62", "0x31", "--mux-keep")
    assert got["cpu_valid"] == 1
    assert got["mux_kept"] == 1
    assert got["mux_writes"] == 0
    assert (got["cfg61"], got["cfg62"]) == (0x04, 0x31)


def test_mux_keep_still_forces_port_0_when_another_port_is_selected(c_mailbox_bin: Path) -> None:
    got = _lite(c_mailbox_bin, "--want-full", "--cfg61", "0x05", "--cfg62", "0x31", "--mux-keep")
    assert got["cpu_valid"] == 1
    assert got["mux_kept"] == 0
    # Armed (port 0, speed 0x03), then the firmware's pair put back.
    assert got["mux_writes"] == 4
    assert (got["cfg61"], got["cfg62"]) == (0x05, 0x31)


def test_without_mux_keep_the_refresh_forces_and_restores(c_mailbox_bin: Path) -> None:
    got = _lite(c_mailbox_bin, "--want-full", "--cfg61", "0x04", "--cfg62", "0x31")
    assert got["mux_kept"] == 0
    assert got["mux_writes"] == 4
    assert (got["cfg61"], got["cfg62"]) == (0x04, 0x31)


def test_a_good_sample_logs_no_failure(c_mailbox_bin: Path) -> None:
    assert _lite(c_mailbox_bin, "--want-full")["fail_notes"] == 0


def test_a_failed_word_logs_its_command_status_and_the_master(c_mailbox_bin: Path) -> None:
    got = _lite(c_mailbox_bin, "--want-full", "--cfg61", "0x04", "--cfg62", "0x31",
                "--fail", "read_word:0x8c=0x02")
    assert got["fail_notes"] == 1
    assert (got["fail_cmd"], got["fail_rc"], got["fail_sts"]) == (0x8C, -errno.EIO, 0x02)
    # Read while the refresh still had them armed: port 0, speed 0x03.
    assert got["fail_regs_ok"] == 1
    assert (got["fail_cfg"], got["fail_speed"]) == (0x04, 0x03)
    # Hands-off mode shows the firmware's speed instead.
    got = _lite(c_mailbox_bin, "--want-full", "--cfg61", "0x04", "--cfg62", "0x31",
                "--fail", "read_word:0x8c=0x02", "--mux-keep")
    assert (got["fail_cfg"], got["fail_speed"]) == (0x04, 0x31)


def test_a_stuck_start_logs_a_timeout_without_a_status(c_mailbox_bin: Path) -> None:
    got = _lite(c_mailbox_bin, "--want-full", "--stuck-start")
    assert got["cpu_valid"] == 0
    assert got["fail_notes"] == 1
    # The first word of a full sample is VOUT.
    assert (got["fail_cmd"], got["fail_rc"], got["fail_sts"]) == (0x8B, -errno.ETIMEDOUT, -1)
