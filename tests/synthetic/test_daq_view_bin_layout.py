"""DESK-25: the DAQ view travels as a binary layout encoded in src-tauri and
decoded in the frontend crate. The two copies cannot share code, so pin the
magic and the field order here."""

import re
from pathlib import Path

DESK = Path(__file__).resolve().parents[2] / "DesktopApp" / "BugBuster"
HOST = DESK / "src-tauri" / "src" / "daq_store.rs"
UI = DESK / "src" / "tauri_bridge.rs"

ARRAYS = ["i_min", "i_max", "v_min", "v_max", "p_min", "p_max", "didt", "source", "gap"]


def _magic(text: str) -> str:
    m = re.search(r'DAQ_VIEW_BIN_MAGIC: \[u8; 4\] = \*b"(\w{4})"', text)
    assert m, "magic not found"
    return m.group(1)


def test_magic_matches():
    assert _magic(HOST.read_text(encoding="utf-8")) == _magic(UI.read_text(encoding="utf-8"))


def test_array_order_matches():
    host = HOST.read_text(encoding="utf-8")
    enc = host[host.index("fn to_view_bytes"):]
    enc = enc[: enc.index("\n    }\n")]
    host_order = re.findall(r"&self\.(\w+)", enc.split("for a in", 1)[1])
    host_order = [f for f in host_order if f in ARRAYS]

    ui = UI.read_text(encoding="utf-8")
    dec = ui[ui.index("fn from_view_bytes"):]
    ui_order = re.findall(r"let (\w+) = r\.(?:f32s|u8s)\(\)\?;", dec)
    assert host_order == ARRAYS
    assert ui_order == ARRAYS
