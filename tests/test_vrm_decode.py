"""Hardware-free golden tests: host-compiled C millisi decode."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

GOLDEN = json.loads((Path(__file__).with_name("golden_vrm_decode.json")).read_text())
CASES = GOLDEN["cases"]


@pytest.fixture(scope="session")
def c_decode_bin(tmp_path_factory: pytest.TempPathFactory) -> Path:
    out = tmp_path_factory.mktemp("vrm_decode") / "vrm_decode"
    src = Path(__file__).with_name("vrm_decode.c")
    subprocess.check_call(
        [
            "cc",
            "-Wall",
            "-Werror",
            "-DNCT_VRM_DECODE_HOST",
            "-o",
            str(out),
            str(src),
        ]
    )
    return out


@pytest.mark.parametrize("case", CASES, ids=lambda c: c["name"])
def test_c_matches_golden(c_decode_bin: Path, case: dict) -> None:
    cmd = [str(c_decode_bin), case["fn"], *[str(a) for a in case["args"]]]
    got = subprocess.check_output(cmd, text=True).strip()
    assert int(got) == case["expect"]
