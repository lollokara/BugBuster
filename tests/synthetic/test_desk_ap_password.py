"""DESK-9: the desktop app can change the SoftAP password (BBP 0xEF over USB,
POST /api/wifi/ap_password over HTTP) from the WiFi card. A: the opcode was
defined in bbp.rs but nothing used it."""

from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2] / "DesktopApp" / "BugBuster"


@pytest.mark.xfail(strict=True, reason="DESK-9")
def test_ap_password_wired_end_to_end():
    lib = (ROOT / "src-tauri" / "src" / "lib.rs").read_text(encoding="utf-8")
    cmds = (ROOT / "src-tauri" / "src" / "commands.rs").read_text(encoding="utf-8")
    http = (ROOT / "src-tauri" / "src" / "http_transport.rs").read_text(encoding="utf-8")
    ui = (ROOT / "src" / "tabs" / "diag.rs").read_text(encoding="utf-8")
    assert "commands::wifi_set_ap_password" in lib
    assert "pub async fn wifi_set_ap_password" in cmds
    assert "bbp::CMD_WIFI_SET_AP_PASSWORD =>" in http and "/api/wifi/ap_password" in http
    assert '"wifi_set_ap_password"' in ui
