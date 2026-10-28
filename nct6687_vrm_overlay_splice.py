"""DKMS splice: apply the committed splice patch to a stock nct6687.c.

The patch is the only splice language. Hunks may move (offset) but any
context drift rejects: --fuzz=0, same failure mode as DKMS PATCH[#].
"""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

import nct6687_vrm_persist as manifest

# Present in the #include splice and inside nct6687_vrm.inc.c
MARKER = "NCT6687_VRM_PMBUS_INJECT"


def is_spliced(text: str) -> bool:
    return MARKER in text


def find_patch() -> Path:
    return manifest.resolve(manifest.PATCH_NAME)


def apply(stock: str, patch: Path | None = None) -> str:
    """Spliced text, or SystemExit on reject / already-spliced input."""
    if is_spliced(stock):
        raise SystemExit("Already spliced (marker present); splice a stock nct6687.c")
    patch_path = patch if patch is not None else find_patch()
    if not patch_path.is_file():
        raise SystemExit(f"Missing splice patch {patch_path}")
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        (root / "nct6687.c").write_text(stock)
        ran = subprocess.run(
            [
                "patch",
                "-p1",
                "--forward",
                "--fuzz=0",
                "--batch",
                "--directory",
                str(root),
                "-i",
                str(patch_path),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if ran.returncode != 0:
            raise SystemExit(
                f"splice patch rejected ({patch_path}): {ran.stderr or ran.stdout}"
            )
        return (root / "nct6687.c").read_text()
