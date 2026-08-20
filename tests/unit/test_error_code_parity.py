"""Cross-surface parity for the BBP error code table.

The firmware defines error codes in bbp.h; Python copies them into ErrorCode.
This enum was hand-copied once and the copy drifted: 0x13 was CAL_INVALID in
Python (a HAT UART error, not a BBP error) while the firmware defined
BBP_ERR_ADGS_ROUTE_REJECTED = 0x13. A rejected MUX route was then reported as
a calibration failure.

This test PARSES bbp.h and asserts Python matches it exactly - every name,
every value, both directions. This is the test that would have caught the 0x13
collision before it reached a live MCP call.
"""

import re

from bugbuster.constants import ErrorCode
from tests.lib.srcread import read_source

BBP_H = read_source("Firmware/ESP32/src/bbp/bbp.h")


def _firmware_error_codes() -> dict[str, int]:
    """Parse all BBP_ERR_* defines from bbp.h.
    
    Returns a dict of {name: value} where name has the BBP_ERR_ prefix stripped.
    Example: BBP_ERR_TIMEOUT = 0x11 becomes {"TIMEOUT": 0x11}.
    """
    return {
        name: int(val, 16)
        for name, val in re.findall(
            r"#define\s+BBP_ERR_(\w+)\s+0x([0-9A-Fa-f]+)", BBP_H
        )
    }


# Names that differ slightly between firmware and Python. The firmware uses
# abbreviated names to fit within line-length constraints; Python spells them
# out for clarity.
_NAME_ALIASES = {
    "INVALID_CH": "INVALID_CHANNEL",  # BBP_ERR_INVALID_CH -> ErrorCode.INVALID_CHANNEL
}


def test_every_firmware_error_code_exists_in_python():
    """A firmware error code missing from Python surfaces as a bare hex string
    in DeviceError messages instead of a named exception."""
    fw = _firmware_error_codes()
    missing = []
    for fw_name, code in fw.items():
        py_name = _NAME_ALIASES.get(fw_name, fw_name)
        if py_name not in ErrorCode.__members__:
            missing.append(f"{fw_name} (0x{code:02X})")
    assert not missing, (
        f"ErrorCode is missing firmware error codes {sorted(missing)}. "
        "Add them to python/bugbuster/constants.py ErrorCode enum."
    )


def test_every_python_error_code_exists_in_firmware():
    """A Python-only error code is never sent by the device, so it's dead
    code that can confuse callers."""
    fw = _firmware_error_codes()
    # Reverse the alias map: Python name -> firmware name
    py_to_fw = {v: k for k, v in _NAME_ALIASES.items()}
    
    extra = []
    for py_name, code in ErrorCode.__members__.items():
        fw_name = py_to_fw.get(py_name, py_name)
        if fw_name not in fw:
            extra.append(f"{py_name} (0x{code:02X})")
    assert not extra, (
        f"ErrorCode has entries not defined in bbp.h: {sorted(extra)}. "
        "These are never sent by the device - remove them."
    )


def test_error_code_values_match():
    """The same name must map to the same numeric value in both. This catches
    the 0x13 collision that made it to production: CAL_INVALID = 0x13 in Python,
    ADGS_ROUTE_REJECTED = 0x13 in firmware."""
    fw = _firmware_error_codes()
    mismatches = []
    for py_name, py_code in ErrorCode.__members__.items():
        fw_name = {v: k for k, v in _NAME_ALIASES.items()}.get(py_name, py_name)
        if fw_name in fw and fw[fw_name] != py_code:
            mismatches.append(
                f"{py_name}: Python=0x{py_code:02X}, firmware({fw_name})=0x{fw[fw_name]:02X}"
            )
    assert not mismatches, (
        "ErrorCode values diverge from firmware:\n  " + "\n  ".join(mismatches)
    )


def test_firmware_error_table_has_expected_count():
    """This is a sanity check that the parser found the right section of bbp.h.
    If the firmware adds or removes an error code, this test fails with a clear
    count diff so the dev knows to review the new code."""
    fw = _firmware_error_codes()
    # As of Task 1 (2026-08-20), there are exactly 14 BBP_ERR_* codes in bbp.h.
    # Verify this count yourself before changing it - grep Firmware/ESP32/src/bbp/bbp.h.
    expected = 14
    assert len(fw) == expected, (
        f"Expected {expected} BBP_ERR_* defines in bbp.h, found {len(fw)}. "
        f"Codes: {sorted(fw.keys())}"
    )


def test_no_gaps_in_error_code_space():
    """The firmware error code space is NOT contiguous (0x01-0x0A, then 0x11-0x13),
    but within each contiguous block there should be no gaps. This test documents
    the expected layout and catches accidental skips."""
    fw = _firmware_error_codes()
    codes = sorted(fw.values())
    
    # Expected layout verified from bbp.h:
    # Block 1: 0x01-0x0A (10 codes)
    # Block 2: 0x11-0x14 (4 codes, was 3 before Task 1)
    block1 = [c for c in codes if 0x01 <= c <= 0x0A]
    block2 = [c for c in codes if 0x11 <= c <= 0x14]
    
    # Check block 1 is contiguous
    if block1:
        expected_block1 = list(range(min(block1), max(block1) + 1))
        assert block1 == expected_block1, (
            f"Gap in error code block 0x01-0x0A: expected {expected_block1}, "
            f"got {block1}"
        )
    
    # Check block 2 is contiguous
    if block2:
        expected_block2 = list(range(min(block2), max(block2) + 1))
        assert block2 == expected_block2, (
            f"Gap in error code block 0x11-0x14: expected {expected_block2}, "
            f"got {block2}"
        )
    
    # All codes should be in one of these blocks
    assert len(codes) == len(block1) + len(block2), (
        f"Some error codes are outside expected blocks 0x01-0x0A and 0x11-0x14: "
        f"{[f'0x{c:02X}' for c in codes if c not in block1 and c not in block2]}"
    )
