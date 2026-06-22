"""Static HAT command parity checks.

These tests intentionally do not require hardware.  They guard against adding a
new HAT BBP command in one layer without either wiring it through the other
layers or classifying it as deliberately deferred / transport-specific.
"""
from __future__ import annotations

import re
from pathlib import Path

from bugbuster.constants import CmdId

ROOT = Path(__file__).resolve().parents[2]


_NAME_ALIASES = {
    # Python kept the historical short name while ESP32/Rust use the explicit
    # voltage spelling.
    "HAT_SET_IO_VOLT": "HAT_SET_IO_VOLTAGE",
}

_EVENT_ONLY = {
    "HAT_LA_LOG_EVT",
}

# Known desktop HTTP gaps owned by the parallel agent or explicitly
# USB-only bulk-stream operations.  If a command is removed from this set, the
# desktop HTTP transport must contain a `bbp::CMD_<name>` match arm.
_DESKTOP_HTTP_ALLOWED_GAPS = {
    "HAT_SET_POWER",      # parallel-agent Tier 1/desktop transport area
    "HAT_GET_POWER",      # parallel-agent Tier 1/desktop transport area
    "HAT_LA_CONFIG",      # USB-oriented LA setup path in desktop today
    "HAT_LA_ARM",         # USB-oriented LA capture control
    "HAT_LA_FORCE",       # USB-oriented LA capture control
    "HAT_LA_READ",        # USB capture data path
    "HAT_LA_STOP",        # USB capture control
    "HAT_LA_TRIGGER",     # USB capture control
    "HAT_LA_USB_RESET",   # USB-only endpoint recovery
    "HAT_LA_STREAM_START",# USB bulk stream start
}


def _canonical(name: str) -> str:
    return _NAME_ALIASES.get(name, name)


def _python_hat_cmds() -> dict[str, int]:
    return {
        _canonical(cmd.name): int(cmd)
        for cmd in CmdId
        if cmd.name.startswith("HAT_") and cmd.name not in _EVENT_ONLY
    }


def _esp32_hat_cmds() -> dict[str, int]:
    text = (ROOT / "Firmware/ESP32/src/bbp/bbp.h").read_text()
    out: dict[str, int] = {}
    for name, value in re.findall(r"#define\s+BBP_CMD_(HAT_[A-Z0-9_]+)\s+0x([0-9A-Fa-f]+)", text):
        out[_canonical(name)] = int(value, 16)
    return out


def _desktop_hat_cmds() -> dict[str, int]:
    text = (ROOT / "DesktopApp/BugBuster/src-tauri/src/bbp.rs").read_text()
    out: dict[str, int] = {}
    for name, value in re.findall(r"pub const CMD_(HAT_[A-Z0-9_]+):\s*u8\s*=\s*0x([0-9A-Fa-f]+)", text):
        out[_canonical(name)] = int(value, 16)
    return out


def _desktop_http_mapped_cmds() -> set[str]:
    text = (ROOT / "DesktopApp/BugBuster/src-tauri/src/http_transport.rs").read_text()
    return {
        _canonical(name)
        for name in re.findall(r"bbp::CMD_(HAT_[A-Z0-9_]+)\s*=>", text)
    }


def test_hat_cmd_ids_are_in_lockstep_across_host_layers():
    """HAT command IDs must not drift between firmware, Python, and desktop."""
    py = _python_hat_cmds()
    esp = _esp32_hat_cmds()
    desktop = _desktop_hat_cmds()

    missing_in_esp = sorted(set(py) - set(esp))
    missing_in_py = sorted(set(esp) - set(py))
    missing_in_desktop = sorted(set(py) - set(desktop))

    assert not missing_in_esp, f"Python HAT commands missing in ESP32 bbp.h: {missing_in_esp}"
    assert not missing_in_py, f"ESP32 HAT commands missing in Python CmdId: {missing_in_py}"
    assert not missing_in_desktop, f"Python HAT commands missing in desktop bbp.rs: {missing_in_desktop}"

    mismatched = {
        name: (py[name], esp[name], desktop.get(name))
        for name in sorted(set(py) & set(esp) & set(desktop))
        if py[name] != esp[name] or py[name] != desktop[name]
    }
    assert not mismatched, f"HAT command ID mismatch: {mismatched}"


def test_desktop_http_hat_commands_are_mapped_or_explicitly_classified():
    """Every desktop HAT command is either mapped over HTTP or documented here."""
    desktop = set(_desktop_hat_cmds()) - _EVENT_ONLY
    mapped = _desktop_http_mapped_cmds()
    unclassified = sorted(desktop - mapped - _DESKTOP_HTTP_ALLOWED_GAPS)
    stale_allowlist = sorted(_DESKTOP_HTTP_ALLOWED_GAPS & mapped)

    assert not unclassified, (
        "Desktop HTTP transport has unclassified HAT commands. Add a match arm "
        f"or classify as deferred/USB-only in this test: {unclassified}"
    )
    assert not stale_allowlist, (
        "These HAT commands are now mapped in desktop HTTP transport; remove "
        f"them from _DESKTOP_HTTP_ALLOWED_GAPS: {stale_allowlist}"
    )

