"""
Pin BBP `CmdId` values that the Python library exposes to the firmware's
`bbp.h` so a future renumber on either side surfaces here instead of as a
silent transport mismatch on real hardware.

Background: Selftest worker (0x0B) and quick-setup (0xF0..0xF4) shipped on
firmware (Firmware/ESP32/src/bbp.h:76,200-204) and on the desktop
(DesktopApp/BugBuster/src-tauri/src/bbp.rs:75,185-189) before the Python
`CmdId` enum had matching entries. There is no CI gate that diffs the three
sources — this is the cheapest place to catch drift.
"""

import re
from pathlib import Path

import pytest

from bugbuster.constants import CmdId


REPO = Path(__file__).resolve().parents[2]
ESP32_BBP_H = REPO / "Firmware" / "ESP32" / "src" / "bbp.h"
DESKTOP_BBP_RS = REPO / "DesktopApp" / "BugBuster" / "src-tauri" / "src" / "bbp.rs"


# (firmware constant name, python CmdId attribute name)
PINNED = [
    ("BBP_CMD_SELFTEST_WORKER", "SELFTEST_WORKER"),
    ("BBP_CMD_QS_LIST",         "QS_LIST"),
    ("BBP_CMD_QS_GET",          "QS_GET"),
    ("BBP_CMD_QS_SAVE",         "QS_SAVE"),
    ("BBP_CMD_QS_APPLY",        "QS_APPLY"),
    ("BBP_CMD_QS_DELETE",       "QS_DELETE"),
]


def _firmware_value(name: str) -> int:
    text = ESP32_BBP_H.read_text(encoding="utf-8")
    m = re.search(rf"#define\s+{re.escape(name)}\s+(0x[0-9A-Fa-f]+|\d+)", text)
    assert m, f"Constant {name!r} not found in {ESP32_BBP_H}"
    raw = m.group(1)
    return int(raw, 16) if raw.lower().startswith("0x") else int(raw)


def _desktop_value(rust_name: str) -> int:
    text = DESKTOP_BBP_RS.read_text(encoding="utf-8")
    m = re.search(
        rf"pub\s+const\s+{re.escape(rust_name)}\s*:\s*u8\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;",
        text,
    )
    assert m, f"Constant {rust_name!r} not found in {DESKTOP_BBP_RS}"
    raw = m.group(1)
    return int(raw, 16) if raw.lower().startswith("0x") else int(raw)


@pytest.mark.parametrize("fw_name, py_attr", PINNED)
def test_python_cmdid_matches_firmware(fw_name, py_attr):
    fw = _firmware_value(fw_name)
    py = int(getattr(CmdId, py_attr))
    assert py == fw, (
        f"Python CmdId.{py_attr} = 0x{py:02X} but firmware {fw_name} = 0x{fw:02X}. "
        f"Update python/bugbuster/constants.py or Firmware/ESP32/src/bbp.h to align."
    )


@pytest.mark.parametrize("fw_name, py_attr", PINNED)
def test_desktop_const_matches_python(fw_name, py_attr):
    rust_name = fw_name.removeprefix("BBP_")  # BBP_CMD_QS_LIST -> CMD_QS_LIST
    desktop = _desktop_value(rust_name)
    py = int(getattr(CmdId, py_attr))
    assert py == desktop, (
        f"Python CmdId.{py_attr} = 0x{py:02X} but desktop {rust_name} = 0x{desktop:02X}. "
        f"Update python/bugbuster/constants.py or DesktopApp/.../bbp.rs to align."
    )


def test_quicksetup_block_is_contiguous():
    # The firmware reserves 0xF0..0xF4 explicitly; if anyone inserts a value
    # in between we want to see the slot map redesigned, not silently drift.
    expected = [
        (0xF0, CmdId.QS_LIST),
        (0xF1, CmdId.QS_GET),
        (0xF2, CmdId.QS_SAVE),
        (0xF3, CmdId.QS_APPLY),
        (0xF4, CmdId.QS_DELETE),
    ]
    for value, member in expected:
        assert int(member) == value, f"{member.name} drifted from 0x{value:02X}"
