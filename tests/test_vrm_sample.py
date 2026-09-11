"""Fake-bus tests for the production PAGE sample (kernel nct_vrm_sample_page)."""

from __future__ import annotations

import pytest

from nct6687_vrm import read_vrm


class FakeXfer:
    """Scripted ByteXfer: record cmds, return canned bytes. No /dev/port."""

    def __init__(
        self,
        *,
        cfg61: int = 0x10,
        cfg62: int = 0x07,
        vout_mode: int = 0x40,
        words: dict[int, int] | None = None,
        bytes_: dict[int, int] | None = None,
        fail_on: tuple[str, int] | None = None,
        fail_sts: int = -2,
    ) -> None:
        self.log: list[tuple] = []
        self.recover_calls = 0
        self.cfg61 = cfg61
        self.cfg62 = cfg62
        self.bytes_ = {0x20: vout_mode, **(bytes_ or {})}
        self.words = dict(words or {})
        self.fail_on = fail_on
        self.fail_sts = fail_sts

    def _maybe_fail(self, kind: str, cmd: int) -> int:
        if self.fail_on == (kind, cmd):
            return self.fail_sts
        return 0

    def write_byte(self, addr: int, cmd: int, value: int) -> int:
        self.log.append(("write_byte", cmd, value))
        sts = self._maybe_fail("write_byte", cmd)
        if sts:
            return sts
        if cmd == 0x00:
            self.bytes_[0x00] = value
        return 0

    def read_byte(self, addr: int, cmd: int) -> tuple[int, int]:
        self.log.append(("read_byte", cmd))
        sts = self._maybe_fail("read_byte", cmd)
        if sts:
            return sts, 0
        return 0, self.bytes_.get(cmd, 0)

    def read_word(self, addr: int, cmd: int) -> tuple[int, int]:
        self.log.append(("read_word", cmd))
        sts = self._maybe_fail("read_word", cmd)
        if sts:
            return sts, 0
        return 0, self.words.get(cmd, 0)

    def esio_read(self, page: int, index: int) -> int:
        self.log.append(("esio_read", page, index))
        if index == 0x61:
            return self.cfg61
        if index == 0x62:
            return self.cfg62
        return 0

    def esio_write(self, index: int, value: int) -> None:
        self.log.append(("esio_write", index, value))

    def recover(self) -> None:
        self.recover_calls += 1
        self.log.append(("recover",))


def _direct_words(
    vout_raw: int,
    *,
    pout_raw: int = 0x0032,
    vin_raw: int = 1200,
    temp_raw: int = 40,
    iout_raw: int = 0x00B8,
) -> dict[int, int]:
    """VOUT_MODE 0x40 → Direct mV. pout 0x0032 = LINEAR11 50 W."""
    return {
        0x8B: vout_raw,
        0x96: pout_raw,
        0x88: vin_raw,
        0x8D: temp_raw,
        0x8C: iout_raw,
    }


def _kinds(log: list[tuple], kind: str) -> list[tuple]:
    return [e for e in log if e[0] == kind]


def _word_cmds(log: list[tuple]) -> list[int]:
    return [e[1] for e in log if e[0] == "read_word"]


def _byte_cmds(log: list[tuple]) -> list[int]:
    return [e[1] for e in log if e[0] == "read_byte"]


def test_vout_above_200mv_skips_iout_0x8c() -> None:
    xfer = FakeXfer(words=_direct_words(1234))
    s = read_vrm(xfer=xfer, page=0)
    assert 0x8C not in _word_cmds(xfer.log)
    assert s.iout_method == "P/V"
    assert s.vout_v == pytest.approx(1.234)
    assert s.pout_w == pytest.approx(50.0)
    assert s.iout_a == pytest.approx(50.0 / 1.234)


def test_vout_at_200mv_reads_iout_0x8c() -> None:
    xfer = FakeXfer(words=_direct_words(200))
    s = read_vrm(xfer=xfer, page=0)
    assert 0x8C in _word_cmds(xfer.log)
    assert s.iout_method == "linear16-N=-3"
    assert s.iout_a == pytest.approx(0x00B8 / 8)


def test_vout_below_200mv_reads_iout_0x8c() -> None:
    xfer = FakeXfer(words=_direct_words(100))
    s = read_vrm(xfer=xfer, page=0)
    assert 0x8C in _word_cmds(xfer.log)
    assert s.iout_method == "linear16-N=-3"


def test_page_write_and_readback() -> None:
    xfer = FakeXfer(words=_direct_words(1234))
    read_vrm(xfer=xfer, page=0)
    writes = _kinds(xfer.log, "write_byte")
    assert ("write_byte", 0x00, 0) in writes
    assert 0x00 in _byte_cmds(xfer.log)


def test_cfg_61_62_save_and_restore() -> None:
    xfer = FakeXfer(cfg61=0x15, cfg62=0x09, words=_direct_words(1234))
    read_vrm(xfer=xfer, port=0)
    assert ("esio_read", 4, 0x61) in xfer.log
    assert ("esio_read", 4, 0x62) in xfer.log
    writes = _kinds(xfer.log, "esio_write")
    assert ("esio_write", 0x61, (0x15 & ~0x03) | 0x00) in writes
    assert ("esio_write", 0x62, 0x03) in writes
    assert writes[-2:] == [("esio_write", 0x61, 0x15), ("esio_write", 0x62, 0x09)]


def test_failure_calls_recover() -> None:
    xfer = FakeXfer(words=_direct_words(1234), fail_on=("write_byte", 0x00))
    with pytest.raises(RuntimeError, match="START timeout"):
        read_vrm(xfer=xfer, page=0)
    assert xfer.recover_calls >= 1
    assert ("recover",) in xfer.log
    # cfg was saved before PAGE write; restore still happens after recover
    writes = _kinds(xfer.log, "esio_write")
    assert ("esio_write", 0x61, 0x10) in writes
    assert ("esio_write", 0x62, 0x07) in writes


def test_production_path_skips_cap_and_status() -> None:
    xfer = FakeXfer(words=_direct_words(1234), bytes_={0x19: 0x80, 0x78: 0x01})
    s = read_vrm(xfer=xfer, page=0)
    assert 0x19 not in _byte_cmds(xfer.log)
    assert 0x78 not in _byte_cmds(xfer.log)
    assert s.capability is None
    assert s.status is None


def test_debug_raw_fetches_cap_and_status() -> None:
    xfer = FakeXfer(words=_direct_words(1234), bytes_={0x19: 0x80, 0x78: 0x01})
    s = read_vrm(xfer=xfer, page=0, debug=True)
    assert 0x19 in _byte_cmds(xfer.log)
    assert 0x78 in _byte_cmds(xfer.log)
    assert s.capability == 0x80
    assert s.status == 0x01
    # still after the production word reads, not instead of them
    assert _word_cmds(xfer.log)[:4] == [0x8B, 0x96, 0x88, 0x8D]
