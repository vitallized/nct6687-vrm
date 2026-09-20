"""Userspace reader must not loop against a loaded nct6687.ko."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import nct6687_vrm as vrm  # noqa: E402


def test_loaded_without_force_is_blocked() -> None:
    assert vrm.userspace_guard(loaded=True, force=False, looping=False) is not None


def test_one_shot_force_is_allowed() -> None:
    assert vrm.userspace_guard(loaded=True, force=True, looping=False) is None


def test_force_loop_while_loaded_is_blocked() -> None:
    msg = vrm.userspace_guard(loaded=True, force=True, looping=True)
    assert msg is not None
    assert "--loop" in msg


def test_unloaded_loop_is_allowed() -> None:
    assert vrm.userspace_guard(loaded=False, force=False, looping=True) is None
