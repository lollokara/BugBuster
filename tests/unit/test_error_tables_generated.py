"""FEAT-6: the BBP error table is generated from bbp.h into every host copy, and
the MCP actually maps device errors.

A (before):
- python/bugbuster_mcp/error_mapping.py still described 0x13 as
  "calibration invalid OR route rejected" and had no 0x14;
- the Rust table (DesktopApp bbp.rs) was hand-copied with no gate;
- tool_wrappers matched r'device error (0x..)' but DeviceError prints NAMES
  ("Device error TIMEOUT"), so no device error was ever mapped;
- no generator / CI gate existed.
"""

import re
import subprocess
import sys

import pytest

from bugbuster.transport.usb import DeviceError
from tests.lib.srcread import REPO_ROOT, read_source

BBP_H = read_source("Firmware/ESP32/src/bbp/bbp.h")
FW = {name: int(val, 16) for name, val in
      re.findall(r"#define\s+BBP_ERR_(\w+)\s+0x([0-9A-Fa-f]+)", BBP_H)}


def test_mcp_error_table_matches_firmware():
    from bugbuster_mcp.error_mapping import ERROR_MESSAGES
    assert set(ERROR_MESSAGES) == set(FW.values())
    assert ERROR_MESSAGES[0x13][0] == "adgs_route_rejected"
    assert ERROR_MESSAGES[0x14][0] == "unsupported_hat"


def test_rust_error_table_matches_firmware():
    rs = read_source("DesktopApp/BugBuster/src-tauri/src/bbp.rs")
    rust = {n: int(v, 16) for n, v in re.findall(r"pub const ERR_(\w+): u8 = 0x([0-9A-Fa-f]+);", rs)}
    assert rust == FW


def test_mcp_wrapper_maps_device_errors():
    from bugbuster_mcp.tool_wrappers import with_error_context

    @with_error_context("demo_tool")
    def boom():
        raise DeviceError(0x13, 7)

    with pytest.raises(RuntimeError) as ei:
        boom()
    assert "MUX" in str(ei.value) and "self-test" in str(ei.value)


def test_generated_tables_are_current():
    gen = REPO_ROOT / "Firmware" / "tools" / "gen_error_tables.py"
    assert gen.exists(), "Firmware/tools/gen_error_tables.py missing"
    r = subprocess.run([sys.executable, str(gen), "--check"], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
