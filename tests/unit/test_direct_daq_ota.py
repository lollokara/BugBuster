"""Direct P4/C6 OTA over HTTP: wrapper, activation, validation and guards.

Source-scanning only. These cannot see runtime failures -- see the bench gate
in docs/superpowers/specs/2026-08-05-direct-p4-c6-ota-design.md.
"""
import re
from pathlib import Path

HAT_H = Path("Firmware/ESP32/src/hat/hat.h").read_text()
HAT_C = Path("Firmware/ESP32/src/hat/hat.cpp").read_text()
TASKS_H = Path("Firmware/ESP32/src/tasks.h").read_text()


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


WEB_C = Path("Firmware/ESP32/src/web/webserver.cpp").read_text()


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


MCP_OTA = Path("python/bugbuster_mcp/tools/ota.py").read_text()


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

CLIENT_TS = Path("Firmware/ESP32/web/src/api/client.ts").read_text()
OTACARD = Path("Firmware/ESP32/web/src/tabs/system/OtaCard.tsx").read_text()


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

CMDS_RS = Path("DesktopApp/BugBuster/src-tauri/src/commands.rs").read_text()
LIB_RS = Path("DesktopApp/BugBuster/src-tauri/src/lib.rs").read_text()
BRIDGE_RS = Path("DesktopApp/BugBuster/src/tauri_bridge.rs").read_text()


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

IOS_DIAG = Path("iOSApp/Sources/Views/DiagnosticsTab.swift").read_text()


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
P4_VERSION_H = Path("Firmware/DAQ_HAT/ESP32P4/include/version.h").read_text()
C6_VERSION_H = Path("Firmware/DAQ_HAT/ESP32C6/include/version.h").read_text()


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
S3LINK_C = Path("Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.c").read_text()


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
# flash-writing BLE apply, respectively) that makes them load-bearing for
# known-open defects. These tests pin the five new values exactly (so an
# accidental revert is caught) and guard the two untouched stacks (so a
# future "finish the job" pass cannot shrink them too).
# ---------------------------------------------------------------------------
MAIN_CPP = Path("Firmware/ESP32/src/main.cpp").read_text()
BLE_C = Path("Firmware/ESP32/src/net/ble_service.cpp").read_text()


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
    assert re.search(r"s_mainLoopStack\[\s*5120\s*/\s*sizeof\(StackType_t\)\s*\]", code), \
        "mainLoop's static stack must be exactly 5120 bytes (measured peak 2684, margin 2436)"


def test_daq_activate_worker_stack_is_exactly_the_new_value():
    code = _strip_noise(UPD_C)
    assert re.search(r"#define\s+DAQ_ACTIVATE_WORKER_STACK\s+5120\b", code), \
        "DAQ_ACTIVATE_WORKER_STACK must be exactly 5120 (measured peak ~3112, margin 2008)"


def test_bbp_cli_and_ble_api_stacks_are_still_untouched_guard_rails():
    main_code = _strip_noise(MAIN_CPP)
    m = re.search(r"s_bbpTaskStack\[\s*(\d+)\s*/\s*sizeof\(StackType_t\)\s*\]", main_code)
    assert m, "could not find s_bbpTaskStack declaration in main.cpp"
    assert int(m.group(1)) >= 8192, (
        "bbpCli's stack must stay >= 8192: cli_menu.cpp:1809 calls "
        "update_manager_release_options() on this task, which runs an "
        "HTTPS/mbedTLS chain measured at ~16 KB elsewhere in this codebase -- "
        "shrinking it turns a latent bug into a guaranteed crash"
    )

    ble_code = _strip_comments(BLE_C)
    m2 = re.search(r'xTaskCreate\(\s*api_req_task\s*,\s*"ble_api"\s*,\s*(\d+)', ble_code)
    assert m2, "could not find the ble_api xTaskCreate call in net/ble_service.cpp"
    assert int(m2.group(1)) >= 8192, (
        "ble_api's stack must stay >= 8192: the BLE OTA apply path runs "
        "update_manager_apply() inline on this task while writing flash -- "
        "shrinking it turns a latent bug into a guaranteed crash"
    )
