"""Hardware-free tests for the DKMS splice (local fixture, not /usr/src)."""

from __future__ import annotations

import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import nct6687_vrm_dkms_inject as inject  # noqa: E402

FIXTURE = Path(__file__).resolve().parent / "fixtures" / "nct6687_snippet.c"

STOCK_MAKEFILE = (
    "build:\n"
    "\tcp ${curpwd}/Kbuild ${curpwd}/Makefile ${curpwd}/nct6687.c ${curpwd}/${kver}\n"
)


@pytest.fixture
def snippet() -> str:
    return FIXTURE.read_text()


def test_inject_text_succeeds_once(snippet: str) -> None:
    out = inject.inject_text(snippet)
    assert inject.MARKER in out
    assert out != snippet


def test_second_inject_raises(snippet: str) -> None:
    once = inject.inject_text(snippet)
    with pytest.raises(SystemExit, match="Already injected"):
        inject.inject_text(once)


@pytest.mark.parametrize(
    "old, fragment",
    [
        ("#define IOREGION_LENGTH 4", "IOREGION_LENGTH 4"),
        ("const struct attribute_group *extra_groups[2];", "extra_groups[2]"),
        ("\tstruct mutex update_lock;", "struct mutex update_lock"),
        (
            "static struct nct6687_data *nct6687_update_device(struct device *dev)",
            "nct6687_update_device signature",
        ),
        ("\tnct6687_setup_voltages(data);\n", "nct6687_setup_voltages"),
    ],
)
def test_missing_anchor_names_it(snippet: str, old: str, fragment: str) -> None:
    broken = snippet.replace(old, "/* gone */", 1)
    with pytest.raises(SystemExit, match=re.escape(fragment)):
        inject.inject_text(broken)


def test_extra_groups_grows_to_three(snippet: str) -> None:
    out = inject.inject_text(snippet)
    assert "extra_groups[2]" not in out
    assert "const struct attribute_group *extra_groups[3];" in out


def test_ioregion_length_grows_to_eight(snippet: str) -> None:
    out = inject.inject_text(snippet)
    assert "#define IOREGION_LENGTH 4" not in out
    assert "#define IOREGION_LENGTH 8" in out


def test_vrm_include_spliced(snippet: str) -> None:
    out = inject.inject_text(snippet)
    assert f'#include "{inject.INC_NAME}"' in out
    assert inject.MARKER in out


def test_update_vrm_hook_after_unlock(snippet: str) -> None:
    out = inject.inject_text(snippet)
    unlock = out.index("\tmutex_unlock(&data->update_lock);")
    hook = out.index("\tnct6687_update_vrm(data);")
    ret = out.index("\treturn data;")
    assert unlock < hook < ret


def test_struct_fields_inserted(snippet: str) -> None:
    out = inject.inject_text(snippet)
    assert inject.STRUCT_FIELDS in out
    assert out.index("bool vrm_enabled;") < out.index("\tstruct mutex update_lock;")


def test_forward_decl_is_only_update_vrm(snippet: str) -> None:
    out = inject.inject_text(snippet)
    assert "static void nct6687_update_vrm(struct nct6687_data *data);" in out
    paired = (
        "static struct nct6687_data *nct6687_update_device(struct device *dev);\n"
        "static void nct6687_update_vrm"
    )
    assert paired not in out


def test_patched_makefile_inserts_include() -> None:
    new, changed = inject.patched_makefile_text(STOCK_MAKEFILE, [inject.INC_NAME])
    assert changed
    assert new is not None
    assert f"${{curpwd}}/{inject.INC_NAME}" in new
    assert new.splitlines()[1].endswith("${curpwd}/${kver}")


def test_patched_makefile_idempotent() -> None:
    once, changed = inject.patched_makefile_text(STOCK_MAKEFILE, [inject.INC_NAME])
    assert changed and once is not None
    again, changed_again = inject.patched_makefile_text(once, [inject.INC_NAME])
    assert changed_again is False
    assert again == once
