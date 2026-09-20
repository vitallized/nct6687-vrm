"""Host-compiled C eSIO mailbox — no /dev/port, no EC."""

from __future__ import annotations

import errno
import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
WRITE_OP = 0x04
IDLE_PAGE = 0xFF
PROTO_WRITE_BYTE = 0x02
PROTO_WORD = 0x83
SMB_EN = 0x80
SMB_START = 0x40
BASE = 0xA20
CMD = BASE + 4
IDX = BASE + 5
DAT = BASE + 6


@pytest.fixture(scope="session")
def c_mailbox_bin(tmp_path_factory: pytest.TempPathFactory) -> Path:
    out = tmp_path_factory.mktemp("vrm_mailbox") / "vrm_mailbox"
    src = Path(__file__).with_name("vrm_mailbox.c")
    subprocess.check_call(
        [
            "cc",
            "-Wall",
            "-Werror",
            "-DNCT_VRM_MAILBOX_HOST",
            "-o",
            str(out),
            str(src),
        ]
    )
    return out


@pytest.fixture(autouse=True)
def no_dev_port(monkeypatch: pytest.MonkeyPatch) -> None:
    real_open = os.open

    def guarded(path, *args, **kwargs):
        if path == "/dev/port":
            raise AssertionError("tests must not open /dev/port")
        return real_open(path, *args, **kwargs)

    monkeypatch.setattr(os, "open", guarded)


def run_mailbox(bin_path: Path, *args: str) -> tuple[int, str]:
    proc = subprocess.run(
        [str(bin_path), *args],
        capture_output=True,
        text=True,
        check=False,
    )
    return proc.returncode, proc.stdout


def parse_run(stdout: str) -> tuple[int, list[tuple[str, int, int]], int | None]:
    rc = None
    value = None
    log: list[tuple[str, int, int]] = []
    for line in stdout.splitlines():
        if line.startswith("rc="):
            rc = int(line.split("=", 1)[1])
        elif line.startswith("value="):
            value = int(line.split("=", 1)[1], 0)
        else:
            op, port, val = line.split()
            log.append((op, int(port, 0), int(val, 0)))
    assert rc is not None
    return rc, log, value


def outbs(log: list[tuple[str, int, int]]) -> list[tuple[int, int]]:
    return [(port, val) for op, port, val in log if op == "outb"]


def mailbox_writes(log: list[tuple[str, int, int]]) -> list[tuple[int, int]]:
    found: list[tuple[int, int]] = []
    seq = outbs(log)
    i = 0
    while i + 2 < len(seq):
        (p0, v0), (p1, v1), (p2, v2) = seq[i], seq[i + 1], seq[i + 2]
        if p0 == CMD and v0 == WRITE_OP and p1 == IDX and p2 == DAT:
            found.append((v1, v2))
            i += 3
            if i < len(seq) and seq[i] == (CMD, IDLE_PAGE):
                i += 1
            continue
        i += 1
    return found


def test_esio_write_op_04_index_data_then_idle_page(c_mailbox_bin: Path) -> None:
    rc_proc, stdout = run_mailbox(c_mailbox_bin, "esio_write", "0x63", "0x02")
    rc, log, _ = parse_run(stdout)
    assert rc_proc == 0
    assert rc == 0
    assert outbs(log) == [
        (CMD, WRITE_OP),
        (IDX, 0x63),
        (DAT, PROTO_WRITE_BYTE),
        (CMD, IDLE_PAGE),
    ]
    assert log[0] == ("inb", CMD, IDLE_PAGE)
    assert all(port in (CMD, IDX, DAT) for _, port, _ in log)


def test_esio_read_page_index_data_then_idle_page(c_mailbox_bin: Path) -> None:
    rc_proc, stdout = run_mailbox(
        c_mailbox_bin, "--b0", "0xA5", "esio_read", "4", "0xB0"
    )
    rc, log, value = parse_run(stdout)
    assert rc_proc == 0
    assert rc == 0
    assert value == 0xA5
    assert outbs(log) == [
        (CMD, 4),
        (IDX, 0xB0),
        (CMD, IDLE_PAGE),
    ]
    dat_inbs = [(port, val) for op, port, val in log if op == "inb" and port == DAT]
    assert dat_inbs == [(DAT, 0xA5)]


def test_write_byte_programs_proto_addr_cmd_payload_then_start(
    c_mailbox_bin: Path,
) -> None:
    addr, cmd, payload = 0xC0, 0x00, 0x01
    rc_proc, stdout = run_mailbox(
        c_mailbox_bin, "write_byte", hex(addr), hex(cmd), hex(payload)
    )
    rc, log, _ = parse_run(stdout)
    assert rc_proc == 0
    assert rc == 0
    writes = mailbox_writes(log)
    start = next(i for i, w in enumerate(writes) if w == (0x63, PROTO_WRITE_BYTE))
    assert writes[start : start + 6] == [
        (0x63, PROTO_WRITE_BYTE),
        (0x65, addr),
        (0x66, cmd),
        (0x70, payload),
        (0x60, SMB_EN),
        (0x60, SMB_EN | SMB_START),
    ]


def test_read_word_programs_proto_83_and_reads_b0_b1(c_mailbox_bin: Path) -> None:
    addr, cmd = 0xC0, 0x8B
    rc_proc, stdout = run_mailbox(
        c_mailbox_bin, "--b0", "0x34", "--b1", "0x12", "read_word", hex(addr), hex(cmd)
    )
    rc, log, value = parse_run(stdout)
    assert rc_proc == 0
    assert rc == 0
    assert value == 0x1234
    writes = mailbox_writes(log)
    start = next(i for i, w in enumerate(writes) if w == (0x63, PROTO_WORD))
    assert writes[start : start + 5] == [
        (0x63, PROTO_WORD),
        (0x65, addr),
        (0x66, cmd),
        (0x60, SMB_EN),
        (0x60, SMB_EN | SMB_START),
    ]
    idx_outbs = [val for port, val in outbs(log) if port == IDX]
    assert idx_outbs.count(0xB0) >= 1
    assert idx_outbs.count(0xB1) >= 1


def test_read_byte_reads_b0(c_mailbox_bin: Path) -> None:
    rc_proc, stdout = run_mailbox(
        c_mailbox_bin, "--b0", "0x20", "read_byte", "0xC0", "0x20"
    )
    rc, _, value = parse_run(stdout)
    assert rc_proc == 0
    assert rc == 0
    assert value == 0x20


def test_write_byte_nonzero_status_is_eio(c_mailbox_bin: Path) -> None:
    rc_proc, stdout = run_mailbox(
        c_mailbox_bin, "--status", "0x01", "write_byte", "0xC0", "0x00", "0x01"
    )
    rc, _, _ = parse_run(stdout)
    assert rc_proc == 1
    assert rc == -errno.EIO


def test_esio_idle_failure_is_ebusy(c_mailbox_bin: Path) -> None:
    rc_proc, stdout = run_mailbox(
        c_mailbox_bin, "--stuck-page", "esio_write", "0x63", "0x02"
    )
    rc, _, _ = parse_run(stdout)
    assert rc_proc == 1
    assert rc == -errno.EBUSY
    rc_proc, stdout = run_mailbox(
        c_mailbox_bin, "--stuck-page", "esio_read", "4", "0x60"
    )
    rc, _, _ = parse_run(stdout)
    assert rc_proc == 1
    assert rc == -errno.EBUSY


def test_recover_clears_ctrl(c_mailbox_bin: Path) -> None:
    rc_proc, stdout = run_mailbox(c_mailbox_bin, "recover")
    rc, log, _ = parse_run(stdout)
    assert rc_proc == 0
    assert rc == 0
    writes = mailbox_writes(log)
    assert writes[-1] == (0x60, 0x00)


def test_recover_stuck_page_is_ebusy(c_mailbox_bin: Path) -> None:
    rc_proc, stdout = run_mailbox(c_mailbox_bin, "--stuck-page", "recover")
    rc, _, _ = parse_run(stdout)
    assert rc_proc == 1
    assert rc == -errno.EBUSY


def test_recover_stuck_page_is_ebusy(c_mailbox_bin: Path) -> None:
    rc_proc, stdout = run_mailbox(c_mailbox_bin, "--stuck-page", "recover")
    rc, _, _ = parse_run(stdout)
    assert rc_proc == 1
    assert rc == -errno.EBUSY
