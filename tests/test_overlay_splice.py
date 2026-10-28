"""The committed splice patch against frozen stock nct6687d r257.

tests/fixtures/nct6687_r257.c is the packaged nct6687.c from
nct6687d-dkms-git r257.ec1030b-1 (sha256 matches the alpm mtree),
GPL-2.0-or-later per its SPDX header. Test input only; never built or shipped.
"""

from __future__ import annotations

from pathlib import Path

import pytest

import nct6687_vrm_overlay_splice as splice

STOCK_R257 = Path(__file__).resolve().parent / "fixtures" / "nct6687_r257.c"


@pytest.fixture(scope="module")
def stock() -> str:
    return STOCK_R257.read_text()


@pytest.fixture(scope="module")
def spliced(stock: str) -> str:
    return splice.apply(stock)


def _func(text: str, signature: str) -> str:
    start = text.index(signature)
    return text[start : text.index("\n}\n", start)]


def test_applies_to_stock_r257(stock: str, spliced: str) -> None:
    assert not splice.is_spliced(stock)
    assert splice.is_spliced(spliced)
    assert '#include "nct6687_vrm.inc.c"' in spliced


def test_data_header_inside_struct_before_update_lock(spliced: str) -> None:
    struct = spliced[spliced.index("struct nct6687_data {") :]
    struct = struct[: struct.index("\n};")]
    assert '#include "nct6687_vrm_data.h"' in struct
    assert struct.index("nct6687_vrm_data.h") < struct.index("struct mutex update_lock;")
    assert "struct mutex EC_io_lock;" in struct
    assert "const struct attribute_group *extra_groups[3];" in struct


def test_ec_window_locked_again(spliced: str) -> None:
    read = _func(spliced, "static u16 nct6687_read(struct nct6687_data *data, u16 address)")
    write = _func(spliced, "static void nct6687_write(struct nct6687_data *data, u16 address, u16 value)")
    for body in (read, write):
        assert body.count("mutex_lock(&data->EC_io_lock);") == 1
        assert body.count("mutex_unlock(&data->EC_io_lock);") == 1
    assert spliced.count("mutex_init(&data->EC_io_lock);") == 1


def test_update_vrm_runs_after_update_lock_drop(spliced: str) -> None:
    sig = "static struct nct6687_data *nct6687_update_device(struct device *dev)"
    decl = "static void nct6687_update_vrm(struct nct6687_data *data);"
    assert spliced.index(decl) < spliced.index(sig)
    body = _func(spliced, sig)
    unlock = body.rindex("mutex_unlock(&data->update_lock);")
    assert unlock < body.index("nct6687_update_vrm(data);") < body.index("return data;")


def test_probe_registers_vrm_group(spliced: str) -> None:
    assert "nct_vrm_data_reset(data, vrm);" in spliced
    assert "data->extra_groups[eg++] = &nct6687_vrm_group;" in spliced
    assert "data->extra_groups[0] = &nct6687_fan_watchdog_group;" not in spliced


def test_ioregion_unchanged_on_offset4(stock: str, spliced: str) -> None:
    """r257 claims BAR+4..+7 already; the splice must not widen it."""
    assert "#define IOREGION_OFFSET 4" in stock
    assert "#define IOREGION_LENGTH 4" in spliced


def test_double_apply_refused(spliced: str) -> None:
    with pytest.raises(SystemExit, match="Already spliced"):
        splice.apply(spliced)


def test_forward_refuses_spliced_text_without_marker(spliced: str) -> None:
    with pytest.raises(SystemExit, match="rejected"):
        splice.apply(spliced.replace(splice.MARKER, "SOMETHING_ELSE"))


def test_drifted_anchor_rejects(stock: str) -> None:
    drifted = stock.replace(
        "const struct attribute_group *extra_groups[2];",
        "const struct attribute_group *extra_groups[4];",
        1,
    )
    with pytest.raises(SystemExit, match="rejected"):
        splice.apply(drifted)


def test_outer_context_drift_rejects_without_fuzz(stock: str) -> None:
    """Fuzz 1 would drop this outermost context line and apply anyway."""
    lines = stock.splitlines(keepends=True)
    assert lines[656] == "\tenum kinds kind;\n"
    lines[656] = "\tenum kinds kind; /* drifted */\n"
    with pytest.raises(SystemExit, match="rejected"):
        splice.apply("".join(lines))


def test_moved_hunks_still_apply(stock: str) -> None:
    out = splice.apply("/* upstream added a line */\n" + stock)
    assert splice.is_spliced(out)


def test_garbage_rejects() -> None:
    with pytest.raises(SystemExit, match="rejected"):
        splice.apply("not a driver\n")
