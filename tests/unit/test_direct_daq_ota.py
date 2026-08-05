"""Direct P4/C6 OTA over HTTP: wrapper, activation, validation and guards.

Source-scanning only. These cannot see runtime failures -- see the bench gate
in docs/superpowers/specs/2026-08-05-direct-p4-c6-ota-design.md.
"""
import re
from pathlib import Path

HAT_H = Path("Firmware/ESP32/src/hat/hat.h").read_text()
HAT_C = Path("Firmware/ESP32/src/hat/hat.cpp").read_text()


def _skip_noise(text: str, i: int, n: int):
    """If text[i] begins a // line comment, a /* */ block comment, or a
    "..."/'...' literal, return the index just past it; otherwise return
    None. Shared by _fn_body()'s brace matcher and _strip_noise() below so the
    two scanners cannot diverge -- a body that only contains a call name
    inside an explanatory comment must be treated identically by both.

    Known limitation: C++11 raw string literals (R"(...)"), which can contain
    unescaped, unbalanced braces, are NOT handled -- nothing in the files this
    scans today uses them. If that changes, extend the '"' branch to detect
    a preceding R"delim( prefix.
    """
    c = text[i]
    if c == "/" and i + 1 < n and text[i + 1] == "/":
        j = text.find("\n", i)
        return n if j == -1 else j
    if c == "/" and i + 1 < n and text[i + 1] == "*":
        end = text.find("*/", i + 2)
        return n if end == -1 else end + 2
    if c == '"' or c == "'":
        quote = c
        j = i + 1
        while j < n and text[j] != quote:
            j += 2 if text[j] == "\\" else 1
        return min(j + 1, n)
    return None


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
        skip = _skip_noise(text, i, n)
        if skip is not None:
            i = skip
            continue
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return text[brace:i]


def _strip_noise(text: str) -> str:
    """Blank out // and /* */ comments and the contents of string/char
    literals, replacing each with spaces of equal length (so any surviving
    .index() ordering check still lines up with the original text).

    Assertions that check "does this identifier appear in the body" must run
    against this, not the raw body -- otherwise an explanatory comment that
    happens to mention the call (e.g. documenting why hat_connect() belongs in
    a loop) satisfies the check even after the real call is deleted. Built on
    the same _skip_noise() scan _fn_body() uses, so the two cannot disagree
    about what counts as noise.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        skip = _skip_noise(text, i, n)
        if skip is not None:
            out.append(" " * (skip - i))
            i = skip
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


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
    assert "static bool daq_activate_p4(" in _strip_noise(UPD_C)


def test_p4_activation_resets_then_confirms_in_that_order():
    code = _strip_noise(_fn_body(UPD_C, "static bool daq_activate_p4("))
    assert "hat_reset()" in code, "must reset so the new image boots"
    assert "hat_ota_confirm()" in code, "must cancel rollback"
    assert code.index("hat_reset()") < code.index("hat_ota_confirm()"), \
        "confirm must come after the reset, never before"


def test_p4_activation_checks_the_running_version_before_confirming():
    code = _strip_noise(_fn_body(UPD_C, "static bool daq_activate_p4("))
    assert "hat_get_version" in code, "must observe the new image running"
    assert code.index("hat_get_version") < code.index("hat_ota_confirm()"), \
        "never confirm an image that has not been seen running"


def test_p4_activation_relink_loop_reprobes_the_link():
    """hat_reset() does not clear HatState.connected, so the relink loop must
    call hat_connect() on every iteration (the same pattern as the RP2040
    reconnect loop) or it degrades to a fixed ~250ms sleep that reads a dead
    link on real hardware.

    Checked against comment-stripped code, not the raw body: an explanatory
    comment mentioning hat_connect() (there is exactly one, right above the
    real call) must not be able to satisfy this on its own once the real call
    is deleted -- that is the regression this test exists to catch."""
    code = _strip_noise(_fn_body(UPD_C, "static bool daq_activate_p4("))
    assert "hat_connect()" in code, \
        "relink loop must re-probe with hat_connect(); hs->connected alone is stale"
    assert code.index("hat_reset()") < code.index("hat_connect()") < code.index("hat_ota_confirm()")


def test_release_path_activates_the_p4():
    """apply_daq_targets() used to set did_p4 and never reboot or confirm."""
    code = _strip_noise(_fn_body(UPD_C, "static bool apply_daq_targets("))
    assert "daq_activate_p4(" in code, \
        "the GitHub release path must activate too, or P4 updates silently revert"


DAQ_C = Path("Firmware/DAQ_HAT/ESP32P4/src/board/daq_board.c").read_text()
CLI_C = Path("Firmware/DAQ_HAT/ESP32P4/src/cli/cli.c").read_text()


def test_c6_claim_and_release_exist():
    assert "bool daq_c6_claim(" in _strip_noise(DAQ_C)
    assert "void daq_c6_release(" in _strip_noise(DAQ_C)


def test_wifi_stream_start_takes_the_c6_claim():
    """WIFI_STREAM_START and RELAY_APPLY both drive the same C6 UART."""
    i = DAQ_C.index("case HATP_CMD_DAQ_WIFI_STREAM_START:")
    j = DAQ_C.index("case HATP_CMD_", i + 10)
    assert "daq_c6_claim(" in _strip_noise(DAQ_C[i:j]), "WiFi bring-up must claim the C6 bus"


def test_relay_apply_takes_the_c6_claim():
    i = DAQ_C.index("case HATP_CMD_DAQ_RELAY_APPLY:")
    j = DAQ_C.index("return 0;", i)
    assert "daq_c6_claim(" in _strip_noise(DAQ_C[i:j]), "relay apply must claim the C6 bus"


def test_cli_c6_commands_take_the_claim():
    for fn in ("cmd_c6flash", "cmd_c6boot", "cmd_c6relay", "cmd_wifiap"):
        body = _fn_body(CLI_C, f"static int {fn}(")
        assert "daq_c6_claim(" in _strip_noise(body), f"{fn} must claim the C6 bus"


def test_s3_owner_gen_is_assigned_synchronously_in_the_dispatcher():
    """S1-6 / claim-vs-ownership race: s_owner_gen used to be written
    asynchronously inside wifi_stream_bringup_task() once it reached the AP
    stage, while the C6 claim is taken synchronously in the dispatcher's
    WIFI_STREAM_START case. That gap let a stale, orphaned task's cancel
    checkpoint see a matching (but stale) s_owner_gen and release a claim a
    freshly-started successor generation had already re-acquired.

    s_owner_gen must now be assigned in the same dispatcher call that bumps
    s_bringup_gen and takes the C6 claim -- not from inside the task body --
    so there is exactly one (synchronous) writer.
    """
    i = DAQ_C.index("case HATP_CMD_DAQ_WIFI_STREAM_START:")
    j = DAQ_C.index("case HATP_CMD_", i + 10)
    dispatcher_case = _strip_noise(DAQ_C[i:j])
    assert re.search(r"\bs_owner_gen\s*=(?!=)", dispatcher_case), \
        "s_owner_gen must be assigned in the dispatcher's START case"

    task_body = _strip_noise(_fn_body(DAQ_C, "static void wifi_stream_bringup_task(void *arg)"))
    assert not re.search(r"\bs_owner_gen\s*=(?!=)", task_body), \
        "s_owner_gen must NOT be (re-)assigned inside the task body -- the " \
        "dispatcher is the only writer, so a comparison (==) may remain but " \
        "an assignment here reintroduces the async race"


def test_c6_validation_helper_exists():
    assert "static bool c6_image_looks_merged(" in UPD_C


def test_c6_validation_checks_both_magics():
    code = _strip_noise(_fn_body(UPD_C, "static bool c6_image_looks_merged("))
    assert "0xE9" in code, "must check the ESP image magic at offset 0"
    assert "C6_PART_TABLE_OFF" in code, "must check the partition table offset"
    assert "0xAA" in code and "0x50" in code, \
        "must check ESP_PARTITION_MAGIC bytes AA 50 at 0x8000"


UPD_H = Path("Firmware/ESP32/src/update/update_manager.h").read_text()


def test_push_local_is_declared():
    assert "update_manager_push_local(" in UPD_H


def test_push_local_takes_the_apply_guard():
    """A local push and a GitHub apply must not interleave into one partition."""
    body = _strip_noise(_fn_body(UPD_C, "esp_err_t update_manager_push_local("))
    assert "ApplyGuard" in body


def test_push_local_validates_c6_images_before_staging():
    body = _strip_noise(_fn_body(UPD_C, "esp_err_t update_manager_push_local("))
    assert "c6_image_looks_merged(" in body
    assert body.index("c6_image_looks_merged(") < body.index("hat_ota_begin("), \
        "validate before any byte reaches the P4"


def test_push_local_uses_stage_target_for_c6():
    """Staging gets the image SHA-verified inside the P4 before the ROM push."""
    body = _strip_noise(_fn_body(UPD_C, "esp_err_t update_manager_push_local("))
    assert "HAT_OTA_TARGET_STAGE" in body
    assert "HAT_OTA_TARGET_C6" not in body, \
        "the direct C6 target has no resume and no pre-flash integrity check"


def test_push_local_activates_the_p4():
    body = _strip_noise(_fn_body(UPD_C, "esp_err_t update_manager_push_local("))
    assert "daq_activate_p4(" in body


def test_push_local_aborts_the_transfer_on_failure():
    body = _strip_noise(_fn_body(UPD_C, "esp_err_t update_manager_push_local("))
    assert "hat_ota_abort()" in body


def test_push_local_relay_poll_checks_timeout_before_status_continue():
    """A hat_daq_ota_status() read failure used to `continue` past both the
    HAT_RELAY_PUSHING exit check and the C6_RELAY_TIMEOUT_MS timeout check, so
    a UART glitch (or a wedged HAT) during the ~3 minute relay push spun the
    loop forever. ApplyGuard is stack RAII: a function that never returns
    never releases it, so a single transient read error bricks every future
    update -- local push AND GitHub apply, all targets -- until the S3 is
    power-cycled. The timeout must be evaluated unconditionally on every
    iteration, before any `continue` can skip it."""
    body = _fn_body(UPD_C, "esp_err_t update_manager_push_local(")
    loop = _fn_body(body, "for (;;)")
    code = _strip_noise(loop)
    assert "C6_RELAY_TIMEOUT_MS" in code
    assert "continue" in code
    assert code.index("C6_RELAY_TIMEOUT_MS") < code.index("continue"), \
        "the relay-poll timeout must be checked before the status-read " \
        "failure continue, or a stuck HAT link can spin this loop forever"
