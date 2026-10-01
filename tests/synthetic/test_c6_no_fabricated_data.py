"""C6-22 (+C6-9): the C6 front panel must never show invented numbers.

A: with the P4 link stale the home screen ran a log-swept demo generator and
labelled it FLT/SIM, and the Diagnostics menu filled every rail with
plausible-looking values plus ``valid = 0xFFFF`` - which the menu's own
USB-PD guard then trusted (20 V / 5 A) to allow a DUT enable.

T1 (static): the render paths carry no generator and no blanket valid mask.
The visual check (pull the P4 link, look at the panel) is weak evidence and is
recorded separately.
"""

import re


from tests.firmware_host.fwhost import extract_function
from tests.lib.srcread import REPO_ROOT

C6 = REPO_ROOT / "Firmware/DAQ_HAT/ESP32C6/src"


def _code(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def test_home_screen_has_no_demo_generator():
    main = _code((C6 / "main.c").read_text(encoding="utf-8"))
    assert "sim_data" not in main
    assert "DDP_STATE_SIM" not in main


def test_diag_refresh_does_not_fabricate_rails():
    body = _code(extract_function(C6 / "menu.c", r"static void diag_refresh\(void\)"))
    assert "0xFFFF" not in body
    assert "wob(" not in body
    # Stale data must leave every validity bit clear.
    assert re.search(r"memset\(&s_dg, 0, sizeof\(s_dg\)\)", body)


def test_value_cards_render_dashes_without_live_data():
    ui = _code((C6 / "ui.c").read_text(encoding="utf-8"))
    render = _code(extract_function(C6 / "ui.c", r"void ui_render\(uint32_t t_ms\)"))
    assert "no_data" in render and "DDP_STATE_LIVE" in render
    card = _code(extract_function(C6 / "ui.c", r"static void draw_card\("))
    assert '"--"' in card
    assert "no_data" in ui


def test_sparkline_skips_samples_while_value_is_dashes():
    upd = _code(extract_function(C6 / "menu.c", r"menu_status_t menu_update\("))
    assert '"--"' in upd
