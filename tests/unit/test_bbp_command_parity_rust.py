"""PROTO-4: the desktop app's opcode table (DesktopApp/.../src-tauri/src/bbp.rs)
must cover every BBP command and event the firmware defines in bbp.h.

Compared by VALUE (Rust names are often spelled differently). A firmware
opcode with no Rust constant means the desktop can neither send it nor
recognise it on the wire (A: 0xF5-0xFD, 0x64/0x65, 0x47, EVT 0x88, 0x0C, ...)."""

import re

from tests.lib.srcread import read_source

BBP_H = read_source("Firmware/ESP32/src/bbp/bbp.h")
RS = read_source("DesktopApp/BugBuster/src-tauri/src/bbp.rs")

# Rust-only command constants with a reason (an unexplained entry hides gaps).
_RUST_ONLY_CMDS = {
    0xEC,  # CMD_HAT_LA_LOG_GET: HTTP-only pseudo-command (drains the S3 log ring);
           # never sent over BBP, shares its byte with EVT_LA_LOG by design.
}


def _fw(prefix: str) -> dict[int, str]:
    return {int(v, 16): n for n, v in
            re.findall(rf"#define\s+(BBP_{prefix}_\w+)\s+0x([0-9A-Fa-f]{{2}})\b", BBP_H)}


def _rust(prefix: str) -> set[int]:
    return {int(v, 16) for v in re.findall(rf"pub const {prefix}_\w+: u8 = 0x([0-9A-Fa-f]{{2}});", RS)}


def test_every_firmware_command_has_a_rust_constant():
    missing = {f"0x{k:02X} {n}" for k, n in _fw("CMD").items() if k not in _rust("CMD")}
    assert not missing, sorted(missing)


def test_every_firmware_event_has_a_rust_constant():
    missing = {f"0x{k:02X} {n}" for k, n in _fw("EVT").items() if k not in _rust("EVT")}
    assert not missing, sorted(missing)


def test_rust_defines_no_command_the_firmware_lacks():
    extra = {f"0x{k:02X}" for k in _rust("CMD") if k not in _fw("CMD") and k not in _RUST_ONLY_CMDS}
    assert not extra, sorted(extra)
