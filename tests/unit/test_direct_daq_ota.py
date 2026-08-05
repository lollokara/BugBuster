"""Direct P4/C6 OTA over HTTP: wrapper, activation, validation and guards.

Source-scanning only. These cannot see runtime failures -- see the bench gate
in docs/superpowers/specs/2026-08-05-direct-p4-c6-ota-design.md.
"""
import re
from pathlib import Path

HAT_H = Path("Firmware/ESP32/src/hat/hat.h").read_text()
HAT_C = Path("Firmware/ESP32/src/hat/hat.cpp").read_text()


def _fn_body(text: str, signature: str) -> str:
    """Body of a top-level function, matched by braces rather than a fixed span.

    Braces inside "..." string literals, '...' char literals, // line comments
    and /* */ block comments do not count -- a JSON literal such as
    "{\\"stage\\":\\"done\\"}" embedded in C++ source must not desync the depth
    counter and truncate or overrun the extracted body.
    """
    start = text.index(signature)
    brace = text.index("{", start)
    depth, i, n = 1, brace + 1, len(text)
    while depth and i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            i = text.find("\n", i)
            if i == -1:
                break
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            i = n if end == -1 else end + 2
            continue
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return text[brace:i]


def test_hat_ota_confirm_is_declared_and_defined():
    assert "bool hat_ota_confirm(void);" in HAT_H
    assert "bool hat_ota_confirm(void)" in HAT_C


def test_hat_ota_confirm_sends_the_existing_0x66_opcode():
    body = _fn_body(HAT_C, "bool hat_ota_confirm(void)")
    assert "HAT_CMD_OTA_CONFIRM" in body, "must send the OTA_CONFIRM opcode"


def test_hat_ota_confirm_is_gated_on_a_daq_hat():
    """The RP2040 LA HAT reads these opcodes differently."""
    body = _fn_body(HAT_C, "bool hat_ota_confirm(void)")
    assert "HAT_TYPE_DAQ_POWER" in body
    assert "s_state.connected" in body


def test_fn_body_ignores_braces_inside_string_literals():
    """Task 6 will run _fn_body() on C++ containing JSON string literals such as
    "{\\"stage\\":\\"done\\",\\"ok\\":true}" -- those braces must not desync the
    depth counter and truncate or overrun the extracted body."""
    text = (
        'static bool has_json_braces(void)\n'
        '{\n'
        '    const char *s = "{\\"stage\\":\\"done\\",\\"ok\\":true}";\n'
        '    char c = \'{\';\n'
        '    // a lone { in a line comment\n'
        '    /* a lone { in a\n'
        '       block comment } */\n'
        '    if (c == \'}\') {\n'
        '        return true;\n'
        '    }\n'
        '    return false;\n'
        '}\n'
        'static bool next_fn(void) { return false; }\n'
    )
    body = _fn_body(text, "static bool has_json_braces(void)")
    assert body.startswith("{")
    assert body.endswith("}")
    assert "return true;" in body
    assert "return false;" in body
    assert "next_fn" not in body


UPD_C = Path("Firmware/ESP32/src/update/update_manager.cpp").read_text()


def test_p4_activation_helper_exists():
    assert "static bool daq_activate_p4(" in UPD_C


def test_p4_activation_resets_then_confirms_in_that_order():
    body = _fn_body(UPD_C, "static bool daq_activate_p4(")
    assert "hat_reset()" in body, "must reset so the new image boots"
    assert "hat_ota_confirm()" in body, "must cancel rollback"
    assert body.index("hat_reset()") < body.index("hat_ota_confirm()"), \
        "confirm must come after the reset, never before"


def test_p4_activation_checks_the_running_version_before_confirming():
    body = _fn_body(UPD_C, "static bool daq_activate_p4(")
    assert "hat_get_version" in body, "must observe the new image running"
    assert body.index("hat_get_version") < body.index("hat_ota_confirm()"), \
        "never confirm an image that has not been seen running"


def test_release_path_activates_the_p4():
    """apply_daq_targets() used to set did_p4 and never reboot or confirm."""
    body = _fn_body(UPD_C, "static bool apply_daq_targets(")
    assert "daq_activate_p4(" in body, \
        "the GitHub release path must activate too, or P4 updates silently revert"
