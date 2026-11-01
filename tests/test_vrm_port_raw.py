"""--raw reads CAP/STATUS inside the armed window, before the mux restore."""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="session")
def port_host_bin(tmp_path_factory: pytest.TempPathFactory) -> Path:
    out = tmp_path_factory.mktemp("vrm_port_host") / "vrm_port_host"
    subprocess.check_call(
        [
            "cc",
            "-Wall",
            "-Werror",
            "-Wno-unused-function",
            "-DNCT_VRM_MAILBOX_HOST",
            "-DNCT_VRM_PORT_TEST",
            "-o",
            str(out),
            str(ROOT / "tests" / "vrm_port_host.c"),
            str(ROOT / "nct6687_vrm_port.c"),
        ],
        cwd=ROOT,
    )
    return out


def _run(port_host_bin: Path, *args: str) -> dict[str, int]:
    out = subprocess.check_output([str(port_host_bin), *args], text=True)
    got: dict[str, int] = {}
    for line in out.splitlines():
        key, val = line.split("=", 1)
        got[key] = int(val)
    return got


def test_raw_restore_keeps_the_master_off(port_host_bin: Path) -> None:
    got = _run(port_host_bin, "--raw")
    assert got["rc"] == 0
    assert got["saw_cap"] == 1
    assert got["saw_status"] == 1
    assert got["cap"] == 0x19
    assert got["status"] == 0x78
    assert got["en_during_mux"] == 0
    assert got["cfg"] == 0x05
    assert got["baud"] == 0x11


def test_port_refuses_a_loaded_module_without_force(port_host_bin: Path) -> None:
    got = _run(port_host_bin, "--refuse")
    assert got["loaded"] == 1
    assert got["force"] == 0
    assert got["clear"] == 0


def test_plain_sample_does_not_read_cap(port_host_bin: Path) -> None:
    got = _run(port_host_bin)
    assert got["rc"] == 0
    assert got["saw_cap"] == 0
    assert got["saw_status"] == 0
    assert got["en_during_mux"] == 0
    assert got["cfg"] == 0x05
    assert got["baud"] == 0x11
