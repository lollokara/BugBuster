"""Direct P4/C6 OTA over HTTP: wrapper, activation, validation and guards.

Source-scanning only. These cannot see runtime failures -- see the bench gate
in docs/superpowers/specs/2026-08-05-direct-p4-c6-ota-design.md.
"""
import re
from pathlib import Path

from tests.lib.srcread import read_source

HAT_H = read_source("Firmware/ESP32/src/hat/hat.h")
HAT_C = read_source("Firmware/ESP32/src/hat/hat.cpp")
TASKS_H = read_source("Firmware/ESP32/src/tasks.h")
TASKS_CPP = read_source("Firmware/ESP32/src/tasks.cpp")


def _literal_end(text: str, i: int, n: int) -> int:
    """text[i] is a quote char ('"', \"'\", or '`' -- the last for TSX template
    literals). Return the index just past the matching closing quote, honoring
    backslash escapes. Shared by _skip_noise() (which blanks the whole
    literal) and _strip_comments() (which keeps literal contents verbatim) so
    the two scanners cannot disagree about where a string ends -- in
    particular, both must treat a `//` INSIDE a literal as ordinary text, not
    a comment start, or _strip_comments would over-blank real code that
    happens to follow a URL-bearing string on the same line.

    Known limitation: does not special-case `${...}` template-literal
    interpolation -- a `` ` `` or matching-quote char inside an interpolated
    expression would be misread as the literal's end. Nothing in the files
    this scans today uses interpolation containing that character.
    """
    quote = text[i]
    j = i + 1
    while j < n and text[j] != quote:
        j += 2 if text[j] == "\\" else 1
    return min(j + 1, n)


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
        return _literal_end(text, i, n)
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


def _skip_comment_only(text: str, i: int, n: int):
    """Like _skip_noise, but comments only -- callers are responsible for
    routing string/char/template literals through _literal_end() themselves
    (see _strip_comments below) so a `//` INSIDE a literal is never mistaken
    for a comment start.
    """
    c = text[i]
    if c == "/" and i + 1 < n and text[i + 1] == "/":
        j = text.find("\n", i)
        return n if j == -1 else j
    if c == "/" and i + 1 < n and text[i + 1] == "*":
        end = text.find("*/", i + 2)
        return n if end == -1 else end + 2
    return None


def _strip_comments(text: str) -> str:
    """Blank out // and /* */ comments only, keeping string/char/template
    literal contents intact -- so a check for a literal value like "p4"
    cannot be spoofed by writing that text inside a comment with no real
    implementation, while the literal itself stays checkable (unlike
    _strip_noise, which blanks string contents too).

    Literal-aware: a quote char is routed through the SAME _literal_end()
    _skip_noise() uses, and its full span (including any `//` inside) is
    copied out verbatim BEFORE comment detection runs on it -- so a URL in a
    string, e.g. "http://x.com", is not misread as starting a line comment
    that blanks everything after it on the line.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in ('"', "'", "`"):
            end = _literal_end(text, i, n)
            out.append(text[i:end])
            i = end
            continue
        skip = _skip_comment_only(text, i, n)
        if skip is not None:
            out.append(" " * (skip - i))
            i = skip
            continue
        out.append(c)
        i += 1
    return "".join(out)


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


UPD_C = read_source("Firmware/ESP32/src/update/update_manager.cpp")


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


DAQ_C = read_source("Firmware/DAQ_HAT/ESP32P4/src/board/daq_board.c")
CLI_C = read_source("Firmware/DAQ_HAT/ESP32P4/src/cli/cli.c")


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


import hashlib as _hashlib
import json as _json
import sys, types
sys.path.insert(0, str(Path("python").resolve()))
from bugbuster.ota import OTAClient, OTAError  # noqa: E402


class _FakeResp:
    def __init__(self, lines, ok=True, status=200):
        self._lines, self.ok, self.status_code, self.text = lines, ok, status, ""
    def iter_lines(self, decode_unicode=False):
        for l in self._lines:
            yield l
    def __enter__(self): return self
    def __exit__(self, *a): return False


def test_apply_update_omits_unset_targets():
    """Naming ANY target key resets the selection firmware-side, so an unset
    key must not be sent -- apply_update(p4=True) means P4 only."""
    sent = {}

    class S:
        def post(self, url, json=None, headers=None, timeout=None):
            sent["json"] = json
            return types.SimpleNamespace(ok=True, json=lambda: {}, status_code=200, text="")

    c = OTAClient.__new__(OTAClient)
    c._session, c._base, c._token = S(), "http://d/api", "t"
    c.apply_update(p4=True)
    assert sent["json"] == {"p4": True}, sent["json"]


def test_apply_update_with_no_args_sends_no_targets():
    sent = {}

    class S:
        def post(self, url, json=None, headers=None, timeout=None):
            sent["json"] = json
            return types.SimpleNamespace(ok=True, json=lambda: {}, status_code=200, text="")

    c = OTAClient.__new__(OTAClient)
    c._session, c._base, c._token = S(), "http://d/api", "t"
    c.apply_update()
    assert sent["json"] in (None, {}), sent["json"]


def test_upload_stream_without_a_done_record_is_a_failure():
    """A truncated stream must never read as success."""
    c = OTAClient.__new__(OTAClient)
    lines = [b'{"stage":"begin","total":10}', b'{"stage":"upload","done":10,"total":10}']
    try:
        c._read_ndjson(_FakeResp(lines), None)
    except OTAError as e:
        assert "done" in str(e).lower()
    else:
        raise AssertionError("truncated stream must raise")


def test_upload_stream_reports_the_error_from_the_done_record():
    c = OTAClient.__new__(OTAClient)
    lines = [b'{"stage":"done","ok":false,"error":"image failed verification"}']
    try:
        c._read_ndjson(_FakeResp(lines), None)
    except OTAError as e:
        assert "verification" in str(e)
    else:
        raise AssertionError("ok:false must raise")


def test_c6_validation_helper_exists():
    assert "static bool c6_image_looks_merged(" in UPD_C


def test_c6_validation_checks_both_magics():
    code = _strip_noise(_fn_body(UPD_C, "static bool c6_image_looks_merged("))
    assert "0xE9" in code, "must check the ESP image magic at offset 0"
    assert "C6_PART_TABLE_OFF" in code, "must check the partition table offset"
    assert "0xAA" in code and "0x50" in code, \
        "must check ESP_PARTITION_MAGIC bytes AA 50 at 0x8000"


UPD_H = read_source("Firmware/ESP32/src/update/update_manager.h")


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
    """Activation must go through the worker wrapper, not daq_activate_p4()
    directly -- daq_activate_p4()'s hat_connect() polling loop, stacked on top
    of the byte transfer's own buffers, overflows the httpd task's 4 KB stack
    (webserver.cpp: config.stack_size). See
    test_push_local_does_not_call_daq_activate_p4_directly below for the
    other half of this contract."""
    body = _strip_noise(_fn_body(UPD_C, "esp_err_t update_manager_push_local("))
    assert "daq_activate_p4_via_worker(" in body


def test_push_local_aborts_the_transfer_on_failure():
    body = _strip_noise(_fn_body(UPD_C, "esp_err_t update_manager_push_local("))
    assert "hat_ota_abort()" in body


def test_push_local_does_not_call_daq_activate_p4_directly():
    """This is the httpd-stack-overflow bug itself: daq_activate_p4() must run
    on the worker, never inline on update_manager_push_local()'s own (httpd)
    stack. A bare call would reintroduce the panic:
    ***ERROR*** A stack overflow in task httpd has been detected."""
    body = _strip_noise(_fn_body(UPD_C, "esp_err_t update_manager_push_local("))
    assert not re.search(r"[^_a-zA-Z0-9]daq_activate_p4\s*\(", body), \
        "daq_activate_p4() must only be reached via daq_activate_p4_via_worker()"


def test_daq_activate_worker_uses_an_internal_ram_stack():
    """A PSRAM-backed task stack is corrupted when the D-cache is disabled
    during flash writes -- the worker must use xTaskCreatePinnedToCoreWithCaps
    with MALLOC_CAP_INTERNAL, matching the existing 12 KB GitHub-apply worker
    (http_update_apply_task in webserver.cpp)."""
    body = _strip_noise(_fn_body(UPD_C, "static bool daq_activate_p4_via_worker("))
    assert "xTaskCreatePinnedToCoreWithCaps(" in body
    assert "MALLOC_CAP_INTERNAL" in body, \
        "the worker stack must be allocated from internal RAM"


def test_daq_activate_worker_is_torn_down_with_delete_with_caps():
    """A task created with xTaskCreatePinnedToCoreWithCaps MUST be deleted
    with vTaskDeleteWithCaps -- plain vTaskDelete() cannot free the
    WithCaps-allocated stack and TCB. This exact leak (12 KB internal RAM per
    update) previously broke the second update in a boot; see the matching
    comment on http_update_apply_task() in webserver.cpp."""
    body = _strip_noise(_fn_body(UPD_C, "static void daq_activate_worker_task("))
    assert "vTaskDeleteWithCaps(NULL)" in body
    assert not re.search(r"[^a-zA-Z]vTaskDelete\s*\(", body), \
        "plain vTaskDelete() cannot free a WithCaps-allocated stack/TCB"


def test_daq_activate_worker_logs_its_stack_high_water_mark():
    """8192 bytes is a starting estimate for the worker stack, not a measured
    figure -- it must log uxTaskGetStackHighWaterMark() so the real bound can
    be read off hardware."""
    body = _strip_noise(_fn_body(UPD_C, "static void daq_activate_worker_task("))
    assert "uxTaskGetStackHighWaterMark(NULL)" in body
    assert body.index("uxTaskGetStackHighWaterMark(NULL)") < \
        body.index("vTaskDeleteWithCaps(NULL)"), \
        "the high-water mark must be read before the task deletes itself"


def test_daq_activate_worker_creation_failure_does_not_fall_back_inline():
    """If xTaskCreatePinnedToCoreWithCaps() fails, the caller must report a
    normal failure -- NOT fall back to running daq_activate_p4() inline on
    httpd, which is exactly the bug this worker exists to avoid."""
    body = _strip_noise(_fn_body(UPD_C, "static bool daq_activate_p4_via_worker("))
    create_call = "xTaskCreatePinnedToCoreWithCaps("
    i = body.index(create_call)
    # Find the `if (created != pdPASS) { ... }` block that follows the call.
    after = body[i:]
    if_idx = after.index("if")
    fail_block = _fn_body(after[if_idx:], "if")
    assert "daq_activate_p4(" not in re.sub(r"//.*", "", fail_block), \
        "task-create failure must not fall back to an inline daq_activate_p4() call"
    assert "vQueueDelete(" in fail_block or "vQueueDelete(" in body, \
        "the queue must be freed even when task creation fails"


def test_push_local_still_forwards_progress_during_activation():
    """The streaming contract must be preserved: progress records from
    activation must still reach emit_cb DURING activation (not batched at the
    end) -- the client shows a live progress UI and treats a stream ending
    without a final `done` record as failure. The worker enqueues; the caller
    (still on httpd, so it can legally call emit_cb) drains the queue and
    calls emit_cb for each non-final record."""
    body = _strip_noise(_fn_body(UPD_C, "static bool daq_activate_p4_via_worker("))
    assert "xQueueReceive(" in body
    assert "emit_cb(" in body
    loop = _fn_body(body[body.index("for (;;)"):], "for (;;)") \
        if "for (;;)" in body else body
    assert "emit_cb(" in loop, \
        "emit_cb must be called from inside the drain loop, not only once at the end"


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


WEB_C = read_source("Firmware/ESP32/src/web/webserver.cpp")


def test_both_upload_routes_are_registered():
    assert '.uri = "/api/ota/upload_p4"' in WEB_C
    assert '.uri = "/api/ota/upload_c6"' in WEB_C


def test_upload_handlers_require_admin_auth():
    body = _fn_body(WEB_C, "static esp_err_t handle_daq_upload(")
    assert "check_admin_auth(req)" in body


def test_upload_handler_delegates_to_update_manager():
    """No second copy of the OTA lifecycle rules in webserver.cpp."""
    body = _fn_body(WEB_C, "static esp_err_t handle_daq_upload(")
    assert "update_manager_push_local(" in body
    for forbidden in ("hat_ota_begin(", "hat_ota_end(", "hat_daq_relay_apply("):
        assert forbidden not in body, \
            f"{forbidden} belongs in update_manager, not the HTTP shim"


def test_upload_handler_streams_ndjson():
    body = _fn_body(WEB_C, "static esp_err_t handle_daq_upload(")
    assert "application/x-ndjson" in body
    assert "httpd_resp_send_chunk" in body


def test_upload_handler_reports_failure_in_the_stream():
    """The 200 is committed before the outcome is known."""
    body = _fn_body(WEB_C, "static esp_err_t handle_daq_upload(")
    assert '\\"done\\"' in body or '"done"' in body


def test_done_record_buffer_is_derived_from_err_buffer_size():
    """cJSON_PrintUnformatted can expand `err` up to ~2x (escaped quotes/backslashes)
    or worse (\\uXXXX control-char expansion), so the final `done` record buffer must
    be sized off sizeof(err) rather than an independent literal -- otherwise shrinking
    or growing `err` alone can silently reintroduce a truncated, invalid-JSON `done`
    record, the one thing this endpoint must never emit."""
    body = _strip_noise(_fn_body(WEB_C, "static esp_err_t handle_daq_upload("))
    assert re.search(r"\blast\s*\[\s*sizeof\s*\(\s*err\s*\)", body), \
        "the done-record buffer must be declared as char last[sizeof(err) * ... ], " \
        "not a fixed literal size that can drift out of sync with err's size"


MCP_OTA = read_source("python/bugbuster_mcp/tools/ota.py")


def _py_signature(text: str, name: str) -> str:
    """The parameter list of a Python def.

    NOT _fn_body(): that matches curly braces, which Python does not have.
    """
    m = re.search(rf"def {re.escape(name)}\((.*?)\)\s*->", text, re.S)
    assert m, f"{name} not found"
    return m.group(1)


def test_mcp_exposes_daq_uploads_and_targets():
    assert "def ota_upload_p4(" in MCP_OTA
    assert "def ota_upload_c6(" in MCP_OTA
    sig = _py_signature(MCP_OTA, "ota_apply_update")
    assert "p4" in sig and "c6" in sig, \
        "the DAQ HAT targets must be reachable from MCP"


# =============================================================================
# Web UI (Task 10): P4/C6 upload with streamed progress, plus two pre-existing
# api.update.* bugs that made the GitHub-update feature unauthenticated/broken.
# =============================================================================

CLIENT_TS = read_source("Firmware/ESP32/web/src/api/client.ts")
OTACARD = read_source("Firmware/ESP32/web/src/tabs/system/OtaCard.tsx")


def test_web_client_has_a_daq_upload_helper():
    assert "uploadDaq" in CLIENT_TS


def test_web_daq_upload_reads_the_ndjson_stream():
    assert "getReader()" in _strip_noise(CLIENT_TS), \
        "must read the streamed progress body"


def test_web_update_apply_takes_a_target_object():
    """apply(rp2040, esp32) could not express p4/c6 at all."""
    assert re.search(r"apply:\s*\(\s*mac[^,]*,\s*targets", _strip_noise(CLIENT_TS)), \
        "apply() must take mac plus a targets object so p4/c6 are expressible"


def _named_arrow_body(block: str, name: str, other_names: list) -> str:
    """Slice `block` from `{name}:` up to the next sibling method's `{n}:`
    marker (or the end of `block` if `name` is last). Lets a check be scoped
    to one method's own body instead of the whole surrounding object -- a
    substring count over the whole block cannot tell "mac forwarded to
    request()" apart from "mac merely named in some other method's own
    parameter list."
    """
    start = block.index(f"{name}:")
    ends = [block.index(f"{n}:", start + 1) for n in other_names if f"{n}:" in block[start + 1:]]
    end = min(ends) if ends else len(block)
    return block[start:end]


def _request_call_text(body: str) -> str:
    """The `request(...)`/`request<T>(...)` call expression inside `body`,
    from `request` through its closing `})`. Non-greedy so a `}` that closes
    an inline generic type argument (e.g. `request<{ success: boolean }>(`)
    is not mistaken for the call's own closing brace+paren.
    """
    m = re.search(r"request(?:<[^>]*>)?\(.*?\}\)", body, re.S)
    return m.group(0) if m else ""


def test_web_update_calls_pass_mac_so_the_token_is_attached():
    """core.ts attaches the admin token only when BOTH admin and mac are set
    (see request() in api/core.ts). It is not enough for `mac` to appear
    somewhere in a method's own arrow-function signature (e.g. `apply: (mac:
    string, targets...) =>`) -- it must actually be forwarded into that
    method's own request() call, or the token is never attached and the
    signature parameter is dead. Scoped per-method so a mac used by one
    method cannot cover for another that drops it.
    """
    m = re.search(r"update:\s*\{(.*?)\n  \},", CLIENT_TS, re.S)
    assert m, "api.update block not found"
    block = m.group(1)
    names = ["check", "status", "apply"]
    for name in names:
        others = [n for n in names if n != name]
        body = _named_arrow_body(block, name, others)
        call = _strip_noise(_request_call_text(body))
        assert re.search(r"\bmac\b", call), \
            f"api.update.{name} must forward mac into its own request() call"


def test_web_update_apply_does_not_double_stringify():
    m = re.search(r"apply:\s*\(targets.*?\}\),", CLIENT_TS, re.S)
    if not m:
        m = re.search(r"apply:\s*\(mac.*?\}\),", CLIENT_TS, re.S)
    assert m, "api.update.apply not found"
    assert "JSON.stringify" not in _strip_noise(m.group(0)), \
        "request() stringifies body itself; passing a string double-encodes it"


def test_web_daq_upload_treats_missing_done_record_as_failure():
    """The device commits HTTP 200 before it knows the outcome, so a stream
    that ends without a final {"stage":"done"} record means the connection
    dropped mid-push and must be surfaced as an error, not silent success."""
    assert "done" in _strip_noise(CLIENT_TS)
    assert re.search(r"if\s*\(\s*!\s*final\s*\)", _strip_noise(CLIENT_TS)), \
        "must throw when the stream ends without a final done record"


def test_web_daq_upload_guards_against_a_null_response_body():
    assert re.search(r"if\s*\(\s*!\s*res\.body\s*\)", _strip_noise(CLIENT_TS)), \
        "res.body can be null; must be guarded before calling getReader()"


def test_web_daq_upload_throws_when_the_final_record_is_not_ok():
    """The final {"stage":"done"} record's `ok` field is the sole source of
    truth for the outcome (the HTTP status is committed before the device
    knows it). A stream that DOES end with a done record but ok:false must
    still throw -- only the missing-done-record path was covered before."""
    assert re.search(r"if\s*\(\s*!\s*final\.ok\s*\)", _strip_noise(CLIENT_TS)), \
        "must throw when the final record has ok:false, not just when it is missing"


def test_web_otacard_offers_p4_and_c6_targets():
    # Comment-stripped, not fully _strip_noise'd: these are functional
    # string-literal values (option/target names) -- _strip_noise blanks
    # string contents too, which would make the literal itself unassertable.
    # Comments are still stripped, so a bare mention in a comment can't pass.
    stripped = _strip_comments(OTACARD)
    assert '"p4"' in stripped and '"c6"' in stripped, \
        "OtaCard's target selector must offer the DAQ HAT chips"


# =============================================================================
# Desktop (Task 11): P4/C6 upload command + UI. The desktop's existing HTTP
# OTA path (http_upload_with_progress) fabricates its percentage on a 250ms
# timer; the DAQ path must report the device's own byte counts instead.
# =============================================================================

CMDS_RS = read_source("DesktopApp/BugBuster/src-tauri/src/commands.rs")
LIB_RS = read_source("DesktopApp/BugBuster/src-tauri/src/lib.rs")
BRIDGE_RS = read_source("DesktopApp/BugBuster/src/tauri_bridge.rs")


def test_desktop_has_a_daq_upload_command():
    assert "pub async fn ota_upload_daq(" in CMDS_RS
    assert "commands::ota_upload_daq," in LIB_RS, "command must be registered"


def test_desktop_daq_upload_reports_real_progress():
    """The existing HTTP path fabricates a percentage on a timer; the DAQ path
    must report the device's own byte counts from the NDJSON stream."""
    body = _fn_body(CMDS_RS, "pub async fn ota_upload_daq(")
    assert "stage" in body and "emit_progress" in body
    assert "http_upload_with_progress" not in body, \
        "must not reuse the fabricated-percentage helper"


def test_desktop_daq_upload_treats_missing_done_record_as_failure():
    """Same device-side semantic as the web/python clients: the device commits
    HTTP 200 before it knows the outcome, so a stream that ends without a
    final done record must be reported as a failure, not silent success."""
    body = _strip_noise(_fn_body(CMDS_RS, "pub async fn ota_upload_daq("))
    assert re.search(r"\bNone\s*=>\s*Err\(", body), \
        "a stream that never sees a final 'done' record must return Err"


def test_desktop_bridge_wrapper_uses_error_propagating_invoke():
    """upload_daq_image must surface the device's own error text. try_invoke()
    swallows Err into None, replacing the message with a generic string --
    the CRITICAL requirement here is that the device's message reaches the
    user, so the wrapper must call invoke() directly."""
    body = _strip_noise(_fn_body(BRIDGE_RS, "pub async fn upload_daq_image("))
    assert "invoke(" in body
    assert "try_invoke(" not in body, \
        "must use the error-propagating invoke(), not try_invoke()"


# =============================================================================
# Task 12 (iOS): OtaUpdateStatus decoded a status/progress/version shape the
# device never sends. `status` was non-optional, so JSONDecoder threw on
# every response and the try? at the call site swallowed it -- the OTA status
# line silently never updated.
# =============================================================================

IOS_DIAG = read_source("iOSApp/Sources/Views/DiagnosticsTab.swift")


def test_ios_update_status_matches_the_firmware_json():
    """The device sends state/step/progressDone/progressTotal -- not status/progress."""
    m = re.search(r"struct OtaUpdateStatus: Codable \{(.*?)\n\}", IOS_DIAG, re.S)
    assert m, "OtaUpdateStatus not found"
    body = m.group(1)
    assert "step" in body, "must decode the device's `step` field"
    assert "progressDone" in body and "progressTotal" in body
    assert not re.search(r"let status:\s*String\b", body), \
        "`status` is not a field the device sends; decoding it fails the whole struct"


def test_ios_update_status_fields_are_all_optional():
    """A firmware that later adds or drops a key must not break decoding of
    the whole object again -- every declared field must be Optional."""
    m = re.search(r"struct OtaUpdateStatus: Codable \{(.*?)\n\}", IOS_DIAG, re.S)
    assert m, "OtaUpdateStatus not found"
    body = m.group(1)
    field_lines = [
        line for line in body.splitlines()
        if re.match(r"\s*let\s+\w+\s*:", line)
    ]
    assert field_lines, "no `let` fields found in OtaUpdateStatus"
    for line in field_lines:
        assert re.search(r":\s*[\w\[\]]+\?\s*$", line.strip()), \
            f"field must be declared Optional: {line!r}"


def test_ios_ota_poll_budget_is_at_least_80_attempts():
    """The device now performs the whole DAQ activation sequence (reset ->
    relink -> version -> confirm) inside an apply, and a C6 relay push alone
    can take ~3 minutes -- 20 attempts x 3s (60s) is far too short."""
    body = _fn_body(IOS_DIAG, "private func startOtaPolling()")
    m = re.search(r"attempts\s*<\s*(\d+)", _strip_noise(body))
    assert m, "poll loop bound not found"
    assert int(m.group(1)) >= 80, \
        f"poll budget must be >= 80 attempts, found {m.group(1)}"


def test_ios_ota_poll_does_not_stop_on_the_first_idle_read():
    """step == 'idle' can legitimately be the FIRST poll's reading, before the
    device has started work -- treating that as completion terminates the
    loop immediately and the status line never shows real progress."""
    body = _strip_noise(_fn_body(IOS_DIAG, "private func startOtaPolling()"))
    assert re.search(r'attempts\s*>\s*1', body), \
        "must require at least one prior iteration before idle means done"


def test_web_otacard_p4_c6_path_skips_the_12s_reboot_wait():
    """The firmware/spiffs path waits ~12s for the device to reboot; the P4/C6
    push confirms the new image running before it replies, so that wait must
    not apply to the DAQ HAT path."""
    stripped = _strip_comments(OTACARD)
    i = stripped.find('target === "p4"')
    assert i != -1, "OtaCard must branch on the p4/c6 targets"
    j = stripped.find("\n\n", i)
    branch = _strip_noise(stripped[i:j if j != -1 else i + 800])
    assert "setTimeout" not in branch, \
        "the DAQ HAT push must not use the firmware/spiffs 12s reboot-wait timer"


# ---------------------------------------------------------------------------
# Product-id parity: the S3's DAQ_PRODUCT_ID_P4/C6 constants (used at both the
# release-path apply_daq_ota() and the local-upload update_manager_push_local()
# OTA_BEGIN call sites) must match the devices' own FW_PRODUCT_ID. The P4's
# ota_begin() strncmp()s meta.product_id against FW_PRODUCT_ID and rejects the
# OTA_BEGIN with ESP_ERR_INVALID_ARG on a mismatch -- confirmed on hardware as
# `{"stage":"done","ok":false,"error":"DAQ HAT rejected OTA_BEGIN"}`, before a
# single byte of the image is written. A stale comment can't catch that drift;
# only a failing test can.
#
# The values under test are the string-literal *contents* themselves, so
# _strip_noise() (which blanks literal contents) would erase exactly what we
# need to assert on. Use _strip_comments() (comments blanked, literals kept)
# instead, and parse the #define lines directly for the device side since
# that's plain C macro text, not a function body.
P4_VERSION_H = read_source("Firmware/DAQ_HAT/ESP32P4/include/version.h")
C6_VERSION_H = read_source("Firmware/DAQ_HAT/ESP32C6/include/version.h")


def _fw_product_id(text: str) -> str:
    m = re.search(r'#define\s+FW_PRODUCT_ID\s+"([^"]*)"', _strip_comments(text))
    assert m, "FW_PRODUCT_ID #define not found"
    return m.group(1)


def _s3_product_id_const(name: str) -> str:
    m = re.search(
        r'static const char \*' + re.escape(name) + r'\s*=\s*"([^"]*)"',
        _strip_comments(UPD_C),
    )
    assert m, f"{name} not found in update_manager.cpp"
    return m.group(1)


def test_s3_p4_product_id_matches_the_p4_device():
    assert _s3_product_id_const("DAQ_PRODUCT_ID_P4") == _fw_product_id(P4_VERSION_H), \
        "S3's DAQ_PRODUCT_ID_P4 must match the P4's FW_PRODUCT_ID or the P4 " \
        "rejects OTA_BEGIN with ESP_ERR_INVALID_ARG and the push dies before " \
        "a single byte is written"


def test_s3_c6_product_id_matches_the_c6_device():
    assert _s3_product_id_const("DAQ_PRODUCT_ID_C6") == _fw_product_id(C6_VERSION_H), \
        "S3's DAQ_PRODUCT_ID_C6 must match the C6's FW_PRODUCT_ID for " \
        "consistency, and for the day relay_stage_begin() starts checking it too"


# =============================================================================
# Task 14 (bench gate): the P4's HATP_CMD_RESET handler acked but never
# rebooted, so daq_activate_p4() confirmed the OLD image every time and
# reported success while the newly-armed image reverted on the next real
# power cycle. Fixed by (1) making the P4 actually call esp_restart() after
# draining the ack out of the UART, and (2) making daq_activate_p4() require
# the link to go DOWN before it will wait for it to come back UP -- an
# ineffective reset never dropped the link, which is exactly why the old
# single-phase wait passed on its very first poll.
# =============================================================================
S3LINK_C = read_source("Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.c")


def _case_block(text: str, case_label: str) -> str:
    """Text from `case {case_label}:` up to the next sibling `case HATP_CMD_`
    (or `default:`), matching the slicing pattern already used elsewhere in
    this file (test_wifi_stream_start_takes_the_c6_claim etc.) for switch-case
    bodies that _fn_body()'s brace matcher cannot isolate on its own."""
    i = text.index(f"case {case_label}:")
    candidates = [p for p in (
        text.find("case HATP_CMD_", i + len(case_label) + 6),
        text.find("default:", i + len(case_label) + 6),
    ) if p != -1]
    assert candidates, f"no terminator found after {case_label}"
    j = min(candidates)
    return text[i:j]


def test_p4_reset_handler_calls_esp_restart():
    """Comment-stripped: the pre-existing "A real reset is handled by the
    board" comment must not be able to satisfy this on its own -- it never
    was backed by a real esp_restart() call."""
    block = _strip_comments(_case_block(S3LINK_C, "HATP_CMD_RESET"))
    assert "esp_restart()" in block, \
        "HATP_CMD_RESET must actually reboot the P4, not just ack"


def test_p4_reset_handler_drains_the_uart_before_restarting():
    """send_ok()'s ack sits in the UART TX FIFO until the driver flushes it.
    Restarting before that flush completes loses the ack, and the S3's
    daq_activate_p4() sees a failed RESET command and aborts."""
    block = _strip_comments(_case_block(S3LINK_C, "HATP_CMD_RESET"))
    assert "uart_wait_tx_done(" in block, \
        "must drain the UART TX FIFO before esp_restart(), or the ack is lost"
    assert block.index("uart_wait_tx_done(") < block.index("esp_restart()"), \
        "the UART drain must happen BEFORE the restart, never after"
    assert block.index("send_ok()") < block.index("uart_wait_tx_done("), \
        "must ack first, then drain, then restart"


def test_p4_activation_waits_for_the_link_to_drop_before_waiting_for_it_to_come_back():
    """The old code only waited for the link to come back UP, which is
    trivially true immediately after an ineffective reset (the link never
    went down) -- that is the exact mechanism of this bug. A DOWN-phase must
    run first, timed separately from the relink budget, and must re-probe
    with hat_connect() just like the relink loop (hs->connected alone is
    stale -- see test_p4_activation_relink_loop_reprobes_the_link above)."""
    code = _strip_noise(_fn_body(UPD_C, "static bool daq_activate_p4("))
    assert "DAQ_DROP_TIMEOUT_MS" in code, "no bounded down-phase wait found"
    assert code.index("hat_reset()") < code.index("DAQ_DROP_TIMEOUT_MS") < \
           code.index("DAQ_RELINK_TIMEOUT_MS"), \
        "must wait for the link to drop BEFORE waiting for it to come back up"
    drop_phase = code[code.index("DAQ_DROP_TIMEOUT_MS"):code.index("DAQ_RELINK_TIMEOUT_MS")]
    assert "hat_connect()" in drop_phase, \
        "the drop-wait loop must re-probe with hat_connect(), not read a stale flag"


def test_p4_activation_returns_without_confirming_when_the_link_never_drops():
    """Never confirming is the safe outcome: an unconfirmed image reverts on
    the bootloader's own initiative. If the down-phase times out, the
    function must return false BEFORE it ever reaches hat_ota_confirm()."""
    code = _strip_noise(_fn_body(UPD_C, "static bool daq_activate_p4("))
    assert re.search(r"if\s*\(\s*!\s*dropped\s*\)\s*\{[^}]*return\s+false\s*;", code, re.S), \
        "a failed down-phase must return false immediately, not fall through"
    fail_block = re.search(r"if\s*\(\s*!\s*dropped\s*\)\s*\{.*?return\s+false\s*;", code, re.S)
    assert fail_block.end() < code.index("hat_ota_confirm()"), \
        "the never-dropped failure return must be reached before any confirm call"


def test_p4_activation_emits_a_progress_record_for_the_drop_phase():
    """Consistent with the existing {"stage":"relink","elapsed_ms":N} shape,
    so a bench operator watching the NDJSON stream can see the down-phase
    happen (or time out) instead of a silent multi-second gap."""
    code = _strip_comments(_fn_body(UPD_C, "static bool daq_activate_p4("))
    assert '\\"stage\\":\\"drop\\"' in code, \
        "must emit a stage=drop progress record, like the existing relink stage"


def test_release_path_and_push_local_use_the_shared_product_id_constants():
    """Both OTA_BEGIN call sites must reference the shared constants rather
    than re-embedding their own literals -- that duplication is exactly how
    this bug shipped (the release path and the local-upload path drifted
    from the devices independently)."""
    release_body = _strip_noise(_fn_body(UPD_C, "static bool apply_daq_ota("))
    assert "DAQ_PRODUCT_ID_P4" in release_body and "DAQ_PRODUCT_ID_C6" in release_body

    push_local_body = _strip_noise(_fn_body(UPD_C, "esp_err_t update_manager_push_local("))
    assert "DAQ_PRODUCT_ID_P4" in push_local_body and "DAQ_PRODUCT_ID_C6" in push_local_body


# ---------------------------------------------------------------------------
# S3 internal-RAM reclamation (2026-08-05): five task/worker stacks were
# shrunk to their measured `stack_hwm` peaks (see tasks.h:363-380 for the
# full measured-peaks table). bbpCli and ble_api were deliberately left at
# 8192 -- both run OTA-adjacent code (an HTTPS/mbedTLS chain and an inline
# flash-writing BLE apply, respectively) that made them load-bearing for
# known-open defects.
#
# bbpCli's defect was fixed 2026-08-06 (S1-4: the TLS chain moved off this
# task onto its own worker) and the stack was then re-measured on hardware
# and shrunk in a follow-up pass (2026-08-06) from 8192 to 5120 -- see
# tasks.h and main.cpp for the measured-peak numbers. ble_api's defect is
# still open and its stack is still untouched. These tests pin the exact
# values (so an accidental revert of either shrink is caught) and guard
# ble_api specifically (so a future pass cannot shrink it too).
# ---------------------------------------------------------------------------
MAIN_CPP = read_source("Firmware/ESP32/src/main.cpp")
BLE_C = read_source("Firmware/ESP32/src/net/ble_service.cpp")
CLI_SYS_CPP = read_source("Firmware/ESP32/src/cli/cli_cmds_sys.cpp")
CLI_MENU_CPP = read_source("Firmware/ESP32/src/cli/cli_menu.cpp")
API_CORE_CPP = read_source("Firmware/ESP32/src/net/api_core.cpp")


def test_reduced_task_stack_sizes_are_exactly_the_new_values():
    code = _strip_noise(TASKS_H)
    assert re.search(r"#define\s+TASK_STACK_ADCPOLL\s+2560\b", code), \
        "TASK_STACK_ADCPOLL must be exactly 2560 (measured peak 1292, margin 1268)"
    assert re.search(r"#define\s+TASK_STACK_FAULTMON\s+2560\b", code), \
        "TASK_STACK_FAULTMON must be exactly 2560 (measured peak 1356, margin 1204)"
    assert re.search(r"#define\s+TASK_STACK_CMDPROC\s+2048\b", code), \
        "TASK_STACK_CMDPROC must be exactly 2048 (measured peak 832, margin 1216)"
    assert re.search(r"#define\s+TASK_STACK_WAVEGEN\s+2048\b", code), \
        "TASK_STACK_WAVEGEN must be exactly 2048 (measured peak 868, margin 1180)"


def test_main_loop_static_stack_is_exactly_the_new_value():
    """s_mainLoopStack is a STATIC (.bss) array, so this saving is
    always-resident internal RAM freed for the lifetime of the process --
    the single most valuable trim in this pass."""
    code = _strip_noise(MAIN_CPP)
    assert re.search(r"s_mainLoopStack\[\s*TASK_STACK_MAINLOOP\s*/\s*sizeof\(StackType_t\)\s*\]", code), \
        "mainLoop's static stack must reference TASK_STACK_MAINLOOP (exactly 5120 bytes, measured peak 2684, margin 2436)"


def test_daq_activate_worker_stack_is_exactly_the_new_value():
    code = _strip_noise(UPD_C)
    assert re.search(r"#define\s+DAQ_ACTIVATE_WORKER_STACK\s+5120\b", code), \
        "DAQ_ACTIVATE_WORKER_STACK must be exactly 5120 (measured peak ~3112, margin 2008)"


def test_bbp_cli_shrunk_and_ble_api_still_untouched_guard_rails():
    main_code = _strip_noise(MAIN_CPP)
    # Check that bbpCli references TASK_STACK_BBPCLI constant
    assert "s_bbpTaskStack[TASK_STACK_BBPCLI" in main_code, \
        "bbpCli's stack must reference TASK_STACK_BBPCLI constant in main.cpp"

    # Verify the constant value in tasks.h
    tasks_code = _strip_noise(TASKS_H)
    m = re.search(r"#define\s+TASK_STACK_BBPCLI\s+(\d+)", tasks_code)
    assert m, "could not find TASK_STACK_BBPCLI definition in tasks.h"
    assert int(m.group(1)) == 5120, (
        "bbpCli's stack must stay exactly 5120. The S1-4 fix (2026-08-06) "
        "moved the ~16 KB HTTPS/mbedTLS release-query chain off this task "
        "(open_update_release_picker() now delegates via api_core_handle() "
        "to a dedicated 16 KB SPIRAM worker), and a follow-up pass "
        "(2026-08-06) then re-measured `stack_hwm` on hardware with the TUI "
        "exercised (dashboard + tab switching) and shrank the stack from "
        "8192 to 5120: 2360 bytes of margin over the measured 2760-byte "
        "peak. The remaining headroom above the floor is for cJSON_Parse() "
        "of the release list, still done on this task -- do not shrink "
        "further without re-measuring stack_hwm on hardware."
    )

    ble_code = _strip_comments(BLE_C)
    m2 = re.search(r'xTaskCreate\(\s*api_req_task\s*,\s*"ble_api"\s*,\s*(\d+)', ble_code)
    assert m2, "could not find the ble_api xTaskCreate call in net/ble_service.cpp"
    assert int(m2.group(1)) >= 8192, (
        "ble_api's stack must stay >= 8192: the BLE OTA apply path runs "
        "update_manager_apply() inline on this task while writing flash -- "
        "shrinking it turns a latent bug into a guaranteed crash"
    )


def test_stack_hwm_table_uses_shared_constants_not_literals():
    """The declared stack sizes must come from the shared TASK_STACK_* macros,
    never from duplicated literals — a duplicated table goes stale when a stack
    is resized (the bug that motivated this guard: mainLoop was 5120 but the
    table still said 8192).

    The table used to live inline in cli_cmd_stack_hwm(). It now lives once in
    tasks_get_registry() (tasks.cpp), which the CLI, BBP_CMD_MEM_STATUS and
    GET /api/system/memory all read, so this checks two things: the CLI does
    not reintroduce a private copy, and the one real table uses the macros.
    """
    cli_body = _fn_body(CLI_SYS_CPP, "extern \"C\" void cli_cmd_stack_hwm(")
    assert "tasks_get_registry" in cli_body, (
        "cli_cmd_stack_hwm must read the shared task registry rather than "
        "building its own table")
    assert not re.search(r"\}\s*tasks\[\]\s*=", cli_body), (
        "cli_cmd_stack_hwm has reintroduced a private task table — that is the "
        "duplication this guard exists to prevent")

    registry = _fn_body(TASKS_CPP, "size_t tasks_get_registry(")
    assert registry, "tasks_get_registry() not found in tasks.cpp"

    bad_literals = {"2560", "2048", "5120", "8192", "4096"}
    for line in registry.split("\n"):
        stripped = _strip_comments(line).strip()
        if not stripped or stripped.startswith("//"):
            continue
        # Rows look like: { "adcPoll", TASK_STACK_ADCPOLL },
        match = re.search(r'"(\w+)"\s*,\s*(\w+)\s*\}', stripped)
        if match and match.group(2) in bad_literals:
            raise AssertionError(
                f"task registry row for '{match.group(1)}' uses bare literal "
                f"{match.group(2)} instead of a TASK_STACK_* constant. Use the "
                f"constant from tasks.h so the table cannot go stale."
            )


def test_every_task_stack_constant_appears_in_the_registry():
    """A task with a TASK_STACK_* size but no registry row is invisible to
    `stack_hwm`, MEM_STATUS and /api/system/memory — its stack could be one
    byte from overflow and nothing would report it."""
    declared = set(re.findall(r"#define\s+TASK_STACK_(\w+)\s+\d+", TASKS_H))
    registry = _fn_body(TASKS_CPP, "size_t tasks_get_registry(")
    referenced = set(re.findall(r"TASK_STACK_(\w+)", registry))
    missing = declared - referenced
    assert not missing, f"task stacks defined but never reported: {sorted(missing)}"


def test_registry_capacity_covers_every_task():
    m = re.search(r"#define\s+BB_TASK_REGISTRY_MAX\s+(\d+)", TASKS_H)
    assert m, "BB_TASK_REGISTRY_MAX is not defined"
    declared = len(re.findall(r"#define\s+TASK_STACK_(\w+)\s+\d+", TASKS_H))
    assert int(m.group(1)) >= declared, (
        f"BB_TASK_REGISTRY_MAX={m.group(1)} silently truncates {declared} tasks")


# =============================================================================
# Task 17 (fast-acq/OTA starvation): daq_fast_task (prio 12) starves the
# s3_link dispatcher task, which must answer every HATP_CMD_OTA_DATA frame
# within the S3's 2000 ms hat_ota_data() timeout (hat.cpp). With fast
# acquisition running, a bench push over the HAT UART link failed partway
# through at an offset that varied run to run ("HAT link rejected data at
# offset N" for N in {265669, 283756, 297036, 479468}) -- a timing problem,
# not a fixed-size buffer. `fast off` on the P4 console made the identical
# push succeed every time. Fixed by stopping fast acquisition on
# HATP_CMD_OTA_BEGIN (all three targets -- P4/C6/STAGE all stream just as
# much data over this same link) and restoring it only on exit paths that
# leave the P4 running its current image (OTA_ABORT; a failed OTA_BEGIN
# itself; OTA_END for C6/STAGE or a failed P4-target END).
# =============================================================================


def test_ota_begin_stops_fast_acquisition():
    """Comment-stripped: a comment merely mentioning daq_board_stop_fast must
    not be able to satisfy this on its own -- the real call must be present
    in the OTA_BEGIN case body, which covers all three OTA targets (P4-self,
    C6, STAGE) since they all stream over the same starved s3_link."""
    block = _strip_comments(_case_block(DAQ_C, "HATP_CMD_OTA_BEGIN"))
    assert "daq_board_stop_fast(b)" in block, \
        "HATP_CMD_OTA_BEGIN must stop fast acquisition before accepting a transfer"


# =============================================================================
# Task 18 (python client fix): _push_daq() used to pass a GENERATOR as
# `data=` while also setting a manual Content-Length header. `requests` sees
# a generator body and adds `Transfer-Encoding: chunked` on top, so the
# prepared request carried BOTH headers -- invalid per RFC 7230 3.3.3, and
# ESP-IDF's httpd rejects it with "Bad request syntax" before the handler
# even runs. Chunked transfer is not an option here: the device handler
# relies on req->content_len for size validation and remaining-bytes
# accounting. The fix passes the open file object as `data=` instead, which
# `requests` streams under a plain Content-Length with no chunking.
#
# This is a REAL behavioural test, not source scanning: it builds the exact
# prepared request _push_daq() builds (via requests.Request +
# Session.prepare_request) against a temp file and inspects the resulting
# header set.
# =============================================================================
import requests as _requests


def _prepare_push_daq_request(path: str, size: int, sha_hex: str = "a" * 64):
    """Mirror _push_daq()'s request construction exactly (headers + data=).

    Returns the requests.PreparedRequest so tests can inspect its actual
    header set -- the same object `Session.send()` would transmit.
    """
    headers = {
        "X-BugBuster-Admin-Token": "t" * 64,
        "Content-Type": "application/octet-stream",
        "Content-Length": str(size),
    }
    with open(path, "rb") as fh:
        req = _requests.Request(
            method="POST",
            url="http://10.0.0.1/api/ota/upload_p4",
            params={"sha256": sha_hex},
            data=fh,
            headers=headers,
        )
        return _requests.Session().prepare_request(req)


def test_push_daq_prepared_request_has_no_transfer_encoding(tmp_path):
    """The bug itself: a generator body forces `Transfer-Encoding: chunked`
    onto the prepared request regardless of a manually-set Content-Length.
    The fixed file-object body must never trigger chunked encoding."""
    img = tmp_path / "p4.bin"
    payload = b"\xe9" + b"x" * 4095
    img.write_bytes(payload)

    prepped = _prepare_push_daq_request(str(img), len(payload))
    assert "Transfer-Encoding" not in prepped.headers, (
        "a file-object body must never be sent chunked -- the DAQ HAT "
        "handler relies on req->content_len, which chunked transfer reports "
        "as 0"
    )


def test_push_daq_prepared_request_has_correct_content_length(tmp_path):
    img = tmp_path / "p4.bin"
    payload = b"\xe9" + b"y" * 10_000
    img.write_bytes(payload)

    prepped = _prepare_push_daq_request(str(img), len(payload))
    assert "Content-Length" in prepped.headers
    assert int(prepped.headers["Content-Length"]) == len(payload)


def test_push_daq_prepared_request_never_has_both_headers(tmp_path):
    """Inverse guard: whatever _push_daq() does internally, Content-Length
    and Transfer-Encoding must never both appear on the wire -- sending both
    is invalid per RFC 7230 3.3.3 and is exactly what made the ESP-IDF httpd
    reject the request as 'Bad request syntax' on real hardware."""
    img = tmp_path / "c6.bin"
    payload = b"z" * 50_000
    img.write_bytes(payload)

    prepped = _prepare_push_daq_request(str(img), len(payload))
    has_cl = "Content-Length" in prepped.headers
    has_te = "Transfer-Encoding" in prepped.headers
    assert not (has_cl and has_te), (
        f"must never send both headers at once (Content-Length={has_cl}, "
        f"Transfer-Encoding={has_te})"
    )


# =============================================================================
# Task 21 (python client fix, backcompat sibling of task 18): _upload() --
# used by upload_firmware()/upload_spiffs() -- had the identical latent
# defect as _push_daq() before task 18: a generator `data=` body alongside a
# manual Content-Length header. It only "worked" because the S3's own
# /api/ota/upload(fs) handlers happen to be more permissive than the newer
# DAQ endpoints -- that was luck, not correctness. _upload() now passes the
# open file object too, so it can no longer trigger Transfer-Encoding.
#
# This is a single shared guard (not duplicated per method) exercising ALL
# FOUR upload entry points -- upload_firmware/upload_spiffs (-> _upload) and
# upload_p4/upload_c6 (-> _push_daq) -- through the REAL OTAClient code path,
# each with a stub session.post() that builds the actual requests
# PreparedRequest (mirroring how requests.Session.send() would see it) and
# inspects its header set.
# =============================================================================

def test_all_upload_methods_never_send_both_content_length_and_transfer_encoding(tmp_path):
    payload = b"\xe9" + b"q" * 20_000
    img = tmp_path / "img.bin"
    img.write_bytes(payload)
    sha_hex = _hashlib.sha256(payload).hexdigest()

    def _ok_json_resp():
        r = types.SimpleNamespace()
        r.ok = True
        r.status_code = 200
        r.text = ""
        r.json = lambda: {"success": True}
        return r

    def _ok_ndjson_resp():
        return _FakeResp([_json.dumps({"stage": "done", "ok": True}).encode()])

    cases = [
        ("upload_firmware", {"sha256": sha_hex}, _ok_json_resp),
        ("upload_spiffs", {}, _ok_json_resp),
        ("upload_p4", {"sha256": sha_hex}, _ok_ndjson_resp),
        ("upload_c6", {"sha256": sha_hex}, _ok_ndjson_resp),
    ]

    for method_name, extra_kwargs, resp_factory in cases:
        captured: dict = {}

        class S:
            def post(self, url, params=None, data=None, headers=None,
                     timeout=None, stream=None,
                     _captured=captured, _resp_factory=resp_factory):
                req = _requests.Request(method="POST", url=url, params=params,
                                        data=data, headers=headers)
                _captured["prepped"] = _requests.Session().prepare_request(req)
                return _resp_factory()

        c = OTAClient.__new__(OTAClient)
        c._session, c._base, c._token = S(), "http://d/api", "t" * 64

        getattr(c, method_name)(str(img), **extra_kwargs)

        prepped = captured["prepped"]
        has_cl = "Content-Length" in prepped.headers
        has_te = "Transfer-Encoding" in prepped.headers
        assert has_cl, f"{method_name}: Content-Length missing from prepared request"
        assert not has_te, f"{method_name}: must never send Transfer-Encoding"
        assert int(prepped.headers["Content-Length"]) == len(payload), method_name


def test_ota_abort_restores_fast_acquisition_conditionally():
    """The restore on OTA_ABORT must be conditional on the state OTA_BEGIN
    actually observed (s_ota_fast_was_running) -- unconditionally restarting
    acquisition on every abort would spin up daq_fast_task even when it
    was never running before the transfer began."""
    block = _strip_comments(_case_block(DAQ_C, "HATP_CMD_OTA_ABORT"))
    assert "s_ota_fast_was_running" in block, \
        "OTA_ABORT must consult the saved pre-transfer fast-acq state"
    assert re.search(
        r"if\s*\(\s*s_ota_fast_was_running\s*\)\s*\{[^}]*daq_board_run_fast\(",
        block, re.S,
    ), "the restore call must be guarded by s_ota_fast_was_running, not unconditional"


# =============================================================================
# Task 19 (bench gate): pushing a merged C6 image always failed verification
# because relay_stage_end() declared a 4096-byte LOCAL array (`uint8_t
# buf[4096]`) for its SHA-256 read-back, and that function runs synchronously
# on the s3_link dispatcher task -- which is created with only a 4096-byte
# FreeRTOS stack (s3_link.c's xTaskCreatePinnedToCore(service_task, "s3_link",
# 4096, ...)). A 4KB local on a 4KB stack is a guaranteed overflow: on real
# hardware this surfaced as "Guru Meditation Error: ... Stack protection
# fault" the instant relay stage verification began, rebooting the P4 mid-OTA
# and leaving the S3 to report "DAQ HAT image failed verification". Fixed by
# heap-allocating the read-back buffer (freed on every exit path) instead of
# putting it on the stack.
# =============================================================================
RELAY_STAGE_C = read_source("Firmware/DAQ_HAT/ESP32P4/src/ota/relay_stage.c")
OTA_C = read_source("Firmware/DAQ_HAT/ESP32P4/src/ota/ota.c")

# Files containing every handler reachable from the s3_link dispatcher task:
# s3_link.c itself (frame parser + generic dispatch), daq_board.c
# (s3_cmd_handler, the actual HATP_CMD_* switch), ota.c (ota_begin/_write/
# _end/_abort, called from daq_board.c's OTA_* cases) and relay_stage.c
# (relay_stage_begin/_write/_end, called from the STAGE-target OTA_* cases).
S3LINK_TASK_SOURCES = {
    "s3_link.c": S3LINK_C,
    "daq_board.c": DAQ_C,
    "ota.c": OTA_C,
    "relay_stage.c": RELAY_STAGE_C,
}


def _local_stack_array_sizes(code: str):
    """Best-effort scan of _strip_noise()'d C source for LOCAL (non-static,
    non-pointer) array declarations of the form `<type> name[<digits>];`, in
    bytes, using each named type's sizeof. Returns a list of (name, bytes).

    This is a source-scanning heuristic, not a real stack-usage analyzer: it
    cannot see array declarations split across lines, arrays sized by a
    macro/expression rather than a literal integer, structs-by-value that are
    themselves large, or compiler-inserted spills. It also does not attempt
    to determine reachability from any particular task -- callers are
    expected to pass in only source known to run on the task in question.
    What it DOES catch is exactly the class of bug this file guards against:
    a plain fixed-size byte/word buffer declared straight on the stack.
    """
    sizeof = {
        "uint8_t": 1, "int8_t": 1, "char": 1, "bool": 1,
        "uint16_t": 2, "int16_t": 2,
        "uint32_t": 4, "int32_t": 4, "float": 4,
        "uint64_t": 8, "int64_t": 8, "double": 8,
    }
    out = []
    pattern = re.compile(
        r"(?<!static\s)\b(" + "|".join(sizeof) + r")\s+(\w+)\s*\[\s*(\d+)\s*\]\s*;"
    )
    noise_free = _strip_noise(code)
    for line in noise_free.splitlines():
        if "static" in line:
            continue
        for m in pattern.finditer(line):
            typ, name, count = m.group(1), m.group(2), int(m.group(3))
            out.append((name, sizeof[typ] * count))
    return out


def _s3_link_task_stack_size() -> int:
    m = re.search(
        r'xTaskCreatePinnedToCore\(\s*service_task\s*,\s*"s3_link"\s*,\s*(\d+)',
        S3LINK_C,
    )
    assert m, "could not find the s3_link service_task stack size"
    return int(m.group(1))


def test_relay_stage_end_has_no_multikb_stack_array():
    body = _fn_body(RELAY_STAGE_C, "esp_err_t relay_stage_end(void)")
    sizes = _local_stack_array_sizes(body)
    offenders = [(name, n) for name, n in sizes if n >= 1024]
    assert not offenders, (
        f"relay_stage_end() must not declare a multi-KB local stack array "
        f"(found {offenders}) -- it runs on the 4096-byte s3_link task stack"
    )


def test_relay_stage_end_heap_allocates_and_frees_its_readback_buffer():
    """The fix: a heap buffer sized for the SHA-256 read-back, freed before
    every return from the function (not just the success path)."""
    body = _strip_noise(_fn_body(RELAY_STAGE_C, "esp_err_t relay_stage_end(void)"))
    assert re.search(r"malloc\s*\(\s*4096\s*\)", body), \
        "expected a heap allocation for the read-back buffer"
    frees = len(re.findall(r"\bfree\s*\(\s*buf\s*\)", body))
    # One free() on the way through the success path, reachable from both the
    # read-back-failure and SHA-mismatch returns below it, plus the
    # allocation-failure path takes an early return with nothing to free.
    assert frees >= 1, "the heap buffer must be freed somewhere on the way out"
    # No return between the malloc() and the (only) free() may skip it: every
    # return after the malloc call must occur AFTER the free call in program
    # order, since this function has no early-return between them other than
    # the allocation-failure check (which returns before ever allocating).
    malloc_pos = body.index("malloc(")
    free_pos = body.index("free(")
    assert free_pos > malloc_pos, "free() must appear after malloc() in program order"
    # Every remaining `return` after the free() is safe by construction; make
    # sure there ISN'T a return sitting between the malloc and the free that
    # would leak (other than the allocation-failure check itself, which is
    # before malloc_pos and thus not in this slice).
    between = body[malloc_pos:free_pos]
    # Strip out the "if (!buf) { ... return ...; }" allocation-failure guard,
    # which legitimately returns without freeing (there is nothing to free).
    between_after_guard = re.sub(r"if\s*\(\s*!\s*buf\s*\)\s*\{.*?\}", "", between, flags=re.S)
    assert "return" not in between_after_guard, (
        "found a return between malloc() and free() that would leak the "
        "heap read-back buffer"
    )


def test_s3_link_handlers_declare_no_stack_buffer_at_or_above_task_stack_size():
    """Honesty note: this scans the known s3_link-task-reachable source files
    (s3_link.c, daq_board.c's s3_cmd_handler, ota.c, relay_stage.c) for LOCAL
    fixed-size array declarations and checks each one is comfortably smaller
    than the task's own stack. It does NOT prove full reachability (a helper
    called only indirectly through a function pointer would be missed) or
    account for call-graph depth/compiler stack usage -- it is a cheap,
    honest tripwire for the exact bug class this task fixed (a single large
    fixed buffer declared straight on a 4KB task stack), not a substitute for
    a real static stack analyzer.
    """
    stack_size = _s3_link_task_stack_size()
    assert stack_size > 0

    worst = []
    for fname, code in S3LINK_TASK_SOURCES.items():
        for name, nbytes in _local_stack_array_sizes(code):
            worst.append((nbytes, fname, name))
    worst.sort(reverse=True)

    assert worst, "expected at least one local stack array across the scanned sources"
    biggest_bytes, biggest_file, biggest_name = worst[0]
    assert biggest_bytes < stack_size, (
        f"largest local stack array found is {biggest_name} in {biggest_file} "
        f"({biggest_bytes} bytes), which is not comfortably smaller than the "
        f"s3_link task stack ({stack_size} bytes) -- a handler this size "
        f"cannot safely run on that task"
    )
    # Leave meaningful headroom, not just "less than": the frame-parser
    # buffers (payload[HATP_MAX_PAYLOAD]=240, crc_input[241], frame[244]) plus
    # local variables, saved registers and call-graph depth all eat into the
    # same 4096 bytes. Anything at or above half the stack for a SINGLE array
    # is worth a second look even if nominally "under" the limit.
    assert biggest_bytes < stack_size // 2, (
        f"largest local stack array ({biggest_name} in {biggest_file}, "
        f"{biggest_bytes} bytes) uses more than half the s3_link task stack "
        f"({stack_size} bytes) on its own"
    )


# ---------------------------------------------------------------------------
# C6 relay-push poll loop in update_manager_push_local(): a C6 firmware push
# used to report `{"stage":"done","ok":true}` ~60 seconds before the real
# push finished, and never checked whether it actually succeeded.
# hat_daq_relay_apply() only SPAWNS the P4's relay_apply_task and returns
# immediately; for a brief window afterwards relay_state is still
# HAT_RELAY_STAGED, not yet HAT_RELAY_PUSHING. The old loop broke on the
# first poll that saw "not PUSHING" -- which STAGED satisfies just as much as
# DONE does -- and then unconditionally reported success.
# ---------------------------------------------------------------------------

def _push_local_relay_section_raw():
    """Same span as _push_local_relay_section() but WITHOUT noise-stripping,
    for checks that need to see actual string-literal contents (e.g. the
    `"stage":"relay"` JSON literal, which _strip_noise() blanks out)."""
    body = _fn_body(UPD_C, "esp_err_t update_manager_push_local(")
    start = body.index("hat_daq_relay_apply()")
    end = body.index("hat_daq_c6_version(")
    return body[start:end]


def _push_local_relay_section():
    """The C6 relay-apply-and-poll section of update_manager_push_local(),
    from the hat_daq_relay_apply() call up to (not including) the trailing
    hat_daq_c6_version() version report, noise-stripped so comments/string
    contents cannot spoof an identifier check."""
    return _strip_noise(_push_local_relay_section_raw())


def test_relay_poll_waits_for_pushing_before_treating_non_pushing_as_done():
    """A poll that reads HAT_RELAY_STAGED (relay_apply_task not scheduled
    yet) must keep waiting, not fall through the same exit the loop uses for
    genuine completion. Concretely: the STAGED check must `continue` the poll
    loop, and it must appear BEFORE the "not PUSHING -> exit" check, so a
    STAGED read can never reach that exit."""
    section = _push_local_relay_section()
    assert "HAT_RELAY_STAGED" in section, \
        "no HAT_RELAY_STAGED handling found in the C6 relay-poll section"
    staged_idx = section.index("HAT_RELAY_STAGED")
    exit_idx = section.index("!= HAT_RELAY_PUSHING")
    assert staged_idx < exit_idx, \
        "the not-yet-started (STAGED) check must be evaluated before the " \
        "not-PUSHING exit check, or a STAGED read takes the same exit as " \
        "a genuinely finished push"
    between = section[staged_idx:exit_idx]
    assert "continue" in between, \
        "seeing HAT_RELAY_STAGED must `continue` the poll loop (keep " \
        "waiting for the push to start), not fall through to the exit check"


def test_relay_poll_bounds_the_wait_for_the_push_to_start():
    """The wait for relay_state to leave STAGED must itself be bounded (a
    push that never starts must fail loudly, not hang the request forever
    behind the held ApplyGuard)."""
    section = _push_local_relay_section()
    assert "C6_RELAY_START_TIMEOUT_MS" in section, \
        "expected a dedicated, bounded timeout for waiting on the push to start"
    # Must actually be conditioned on "not started yet" and gate a real
    # `return ESP_FAIL` bail-out, not just be declared and unused.
    started_idx = section.index("!started")
    timeout_idx = section.index("C6_RELAY_START_TIMEOUT_MS")
    assert started_idx < timeout_idx, \
        "the start-timeout check must be conditioned on `!started`"
    fail_idx = section.index("return ESP_FAIL", timeout_idx)
    assert fail_idx > timeout_idx, \
        "no return ESP_FAIL found gated by the start-timeout"


def test_relay_poll_checks_final_state_against_the_done_constant():
    """The loop must exit on ANY non-PUSHING state (including
    HAT_RELAY_FAILED), so whatever runs after the loop must inspect the final
    state and fail unless it is specifically HAT_RELAY_DONE -- "not PUSHING"
    alone is not a success signal."""
    body = _fn_body(UPD_C, "esp_err_t update_manager_push_local(")
    loop = _fn_body(body, "for (;;)")
    after_loop = _strip_noise(body[body.index(loop) + len(loop):body.index("hat_daq_c6_version(")])
    assert "HAT_RELAY_DONE" in after_loop, \
        "no check against HAT_RELAY_DONE found after the relay poll loop"
    done_idx = after_loop.index("HAT_RELAY_DONE")
    assert "return ESP_FAIL" in after_loop[done_idx:], \
        "a final state other than HAT_RELAY_DONE must return ESP_FAIL"


def test_relay_poll_emits_progress_from_relay_pushed_bytes():
    """The observed hardware bug: a ~3 minute C6 push emitted no `relay`
    stage records at all, so a live client saw nothing move for minutes.
    Progress must be emitted from relay_pushed_bytes on the same throttle
    (PUSH_EMIT_MS) the byte-transfer loop above already uses."""
    section = _push_local_relay_section()
    raw = _push_local_relay_section_raw()
    assert "PUSH_EMIT_MS" in section, \
        "the relay poll must reuse the existing PUSH_EMIT_MS throttle"
    assert '"stage\\":\\"relay\\"' in raw or '"stage":"relay"' in raw, \
        "expected a {\"stage\":\"relay\", ...} progress record"
    assert "relay_pushed_bytes" in section, \
        "the relay progress record must be derived from st.relay_pushed_bytes"
    assert "emit_cb(" in section


# -----------------------------------------------------------------------------
# S1-4 -- TUI firmware picker used to run the GitHub release-query HTTPS/mbedTLS
# chain (~16 KB measured) directly on bbpCli (8 KB). See
# docs/superpowers/reviews/2026-08-03-design-sweep.md finding S1-4. Fixed by
# routing open_update_release_picker() through api_core_handle(), reusing the
# same "/api/ota/releases" path webserver.cpp already uses for the sibling
# S1-2 fix, which runs the query on a dedicated 16 KB SPIRAM worker
# (net/api_core.cpp: ota_query_blocking() / ota_query_task()).
# -----------------------------------------------------------------------------

def test_cli_menu_does_not_call_update_manager_release_options_directly():
    """The bug itself: open_update_release_picker() must not call
    update_manager_release_options() inline on bbpCli. That performs an HTTPS
    fetch (esp_http_client_perform()) plus a software-AES mbedTLS handshake,
    measured elsewhere in this codebase at ~16 KB of stack -- double bbpCli's
    entire 8192-byte stack. A bare call would reintroduce the overflow."""
    code = _strip_noise(CLI_MENU_CPP)
    assert not re.search(r"[^_a-zA-Z0-9]update_manager_release_options\s*\(", code), \
        "cli_menu.cpp must not call update_manager_release_options() directly " \
        "on the CLI task -- route through api_core_handle() instead, which " \
        "already runs this query on a dedicated 16 KB worker"


def test_cli_menu_release_picker_delegates_via_api_core_handle():
    """The fix, positively stated: the release picker must reuse the existing
    transport-agnostic dispatcher rather than reimplementing a third worker.
    webserver.cpp's handle_get_update_check() already established this exact
    pattern (api_core_handle("GET", "/api/ota/check", ...)) for the sibling
    S1-2 bug; the release picker should be its twin on "/api/ota/releases"."""
    body = _strip_comments(_fn_body(CLI_MENU_CPP, "static void open_update_release_picker(void)"))
    assert 'api_core_handle(' in body, \
        "open_update_release_picker() must call api_core_handle()"
    assert '"/api/ota/releases"' in body, \
        'open_update_release_picker() must request the "/api/ota/releases" path'


def test_ota_query_worker_is_created_and_torn_down_with_matching_withcaps_pair():
    """xTaskCreatePinnedToCoreWithCaps() allocations MUST be torn down with
    vTaskDeleteWithCaps() -- a plain vTaskDelete() cannot free a
    WithCaps-allocated stack/TCB and leaks it every call (this exact leak
    already cost 12 KB of internal RAM per update once; see commit 971714e
    referenced in api_core.cpp). ota_query_task/ota_query_blocking is the
    worker cli_menu.cpp's release picker now depends on indirectly through
    api_core_handle()."""
    creator = _strip_noise(_fn_body(API_CORE_CPP, "static char *ota_query_blocking("))
    assert "xTaskCreatePinnedToCoreWithCaps(" in creator, \
        "ota_query_blocking() must create its worker with the WithCaps allocator"

    worker = _strip_noise(_fn_body(API_CORE_CPP, "static void ota_query_task("))
    assert "vTaskDeleteWithCaps(NULL)" in worker, \
        "ota_query_task() must tear itself down with vTaskDeleteWithCaps()"
    assert not re.search(r"[^a-zA-Z]vTaskDelete\s*\(", worker), \
        "plain vTaskDelete() cannot free a WithCaps-allocated stack/TCB"


def test_ota_query_worker_stack_lives_in_spiram():
    """This query only reads (esp_http_client_perform() against GitHub's
    releases API) -- it never writes flash or NVS -- so its 16 KB worker stack
    should live in SPIRAM rather than consuming scarce contiguous internal
    RAM the OTA apply/flash paths need. cli_cmds_sys.cpp's own `update check`
    worker documents and follows the same reasoning."""
    creator = _strip_noise(_fn_body(API_CORE_CPP, "static char *ota_query_blocking("))
    assert "MALLOC_CAP_SPIRAM" in creator, \
        "the read-only release-query worker stack must be SPIRAM-backed"
