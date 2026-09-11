"""eSIO / SMBus sequences against a recording fake — no /dev/port."""

from __future__ import annotations

import os

import pytest

from nct6687_vrm import (
    PROTO_WRITE_BYTE,
    PROTO_WORD,
    SMB_EN,
    SMB_START,
    esio_read,
    esio_write,
    smbus_read,
    smbus_write_byte,
)

WRITE_OP = 0x04
IDLE_PAGE = 0xFF


class FakePorts:
    """In-memory ByteIO adapter: records outb/inb and models page/index/data."""

    def __init__(self, base: int = 0xA20, *, idle_ok: bool = True) -> None:
        self.base = base
        self.cmd = base + 4
        self.idx = base + 5
        self.dat = base + 6
        self.idle_ok = idle_ok
        self.log: list[tuple[str, int, int]] = []
        self._page = IDLE_PAGE
        self._index = 0
        self.regs: dict[int, int] = {0x03: 0, 0x60: 0, 0x61: 0, 0x62: 0}

    def outb(self, port: int, val: int) -> None:
        val &= 0xFF
        self.log.append(("outb", port, val))
        if port == self.cmd:
            self._page = val
        elif port == self.idx:
            self._index = val
        elif port == self.dat:
            if self._index == 0x60:
                if val & SMB_START:
                    self.regs[0x03] = 0  # successful host completion
                self.regs[0x60] = val & ~SMB_START
            elif self._index in (0x03, 0x04) and val == 0xFF:
                self.regs[self._index] = 0  # status/error clear
            else:
                self.regs[self._index] = val

    def inb(self, port: int) -> int:
        if port == self.cmd:
            val = self._page if self.idle_ok else 0x00
        elif port == self.idx:
            val = self._index
        elif port == self.dat:
            val = self.regs.get(self._index, 0)
        else:
            val = 0
        self.log.append(("inb", port, val))
        return val


@pytest.fixture(autouse=True)
def no_dev_port(monkeypatch: pytest.MonkeyPatch) -> None:
    real_open = os.open

    def guarded(path, *args, **kwargs):
        if path == "/dev/port":
            raise AssertionError("tests must not open /dev/port")
        return real_open(path, *args, **kwargs)

    monkeypatch.setattr(os, "open", guarded)
    monkeypatch.setattr("nct6687_vrm.os.open", guarded)


@pytest.fixture(autouse=True)
def no_sleep(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("nct6687_vrm.time.sleep", lambda _s: None)


def outbs(p: FakePorts) -> list[tuple[int, int]]:
    return [(port, val) for op, port, val in p.log if op == "outb"]


def mailbox_writes(p: FakePorts) -> list[tuple[int, int]]:
    """(index, value) for each completed write-op (PAGE 0x04) sequence."""
    found: list[tuple[int, int]] = []
    seq = outbs(p)
    i = 0
    while i + 2 < len(seq):
        (p0, v0), (p1, v1), (p2, v2) = seq[i], seq[i + 1], seq[i + 2]
        if p0 == p.cmd and v0 == WRITE_OP and p1 == p.idx and p2 == p.dat:
            found.append((v1, v2))
            i += 3
            if i < len(seq) and seq[i] == (p.cmd, IDLE_PAGE):
                i += 1
            continue
        i += 1
    return found


def test_esio_write_op_04_index_data_then_idle_page() -> None:
    p = FakePorts()
    esio_write(p, 0x63, PROTO_WRITE_BYTE)
    assert outbs(p) == [
        (p.cmd, WRITE_OP),
        (p.idx, 0x63),
        (p.dat, PROTO_WRITE_BYTE),
        (p.cmd, IDLE_PAGE),
    ]
    assert p.log[0] == ("inb", p.cmd, IDLE_PAGE)
    assert all(port in (p.cmd, p.idx, p.dat) for _, port, _ in p.log)


def test_esio_read_page_index_data_then_idle_page() -> None:
    p = FakePorts()
    p.regs[0xB0] = 0xA5
    assert esio_read(p, 4, 0xB0) == 0xA5
    assert outbs(p) == [
        (p.cmd, 4),
        (p.idx, 0xB0),
        (p.cmd, IDLE_PAGE),
    ]
    dat_inbs = [(port, val) for op, port, val in p.log if op == "inb" and port == p.dat]
    assert dat_inbs == [(p.dat, 0xA5)]


def test_smbus_write_byte_programs_proto_addr_cmd_payload_then_start() -> None:
    p = FakePorts()
    addr, cmd, payload = 0xC0, 0x00, 0x01
    assert smbus_write_byte(p, addr, cmd, payload) == 0
    writes = mailbox_writes(p)
    start = next(i for i, w in enumerate(writes) if w == (0x63, PROTO_WRITE_BYTE))
    assert writes[start : start + 6] == [
        (0x63, PROTO_WRITE_BYTE),
        (0x65, addr),
        (0x66, cmd),
        (0x70, payload),
        (0x60, SMB_EN),
        (0x60, SMB_EN | SMB_START),
    ]


def test_smbus_read_word_programs_proto_83_and_reads_b0_b1() -> None:
    p = FakePorts()
    p.regs[0xB0] = 0x34
    p.regs[0xB1] = 0x12
    addr, cmd = 0xC0, 0x8B
    sts, data = smbus_read(p, addr, cmd, True)
    assert sts == 0
    assert data == bytes([0x34, 0x12])
    writes = mailbox_writes(p)
    start = next(i for i, w in enumerate(writes) if w == (0x63, PROTO_WORD))
    assert writes[start : start + 5] == [
        (0x63, PROTO_WORD),
        (0x65, addr),
        (0x66, cmd),
        (0x60, SMB_EN),
        (0x60, SMB_EN | SMB_START),
    ]
    idx_outbs = [val for port, val in outbs(p) if port == p.idx]
    assert idx_outbs.count(0xB0) >= 1
    assert idx_outbs.count(0xB1) >= 1


def test_esio_idle_failure_raises() -> None:
    p = FakePorts(idle_ok=False)
    with pytest.raises(RuntimeError, match="eSIO idle failed"):
        esio_write(p, 0x63, PROTO_WRITE_BYTE)
    with pytest.raises(RuntimeError, match="eSIO idle failed"):
        esio_read(p, 4, 0x60)
