"""Direct P4/C6 OTA over HTTP: wrapper, activation, validation and guards.

Source-scanning only. These cannot see runtime failures -- see the bench gate
in docs/superpowers/specs/2026-08-05-direct-p4-c6-ota-design.md.
"""
import re
from pathlib import Path

HAT_H = Path("Firmware/ESP32/src/hat/hat.h").read_text()
HAT_C = Path("Firmware/ESP32/src/hat/hat.cpp").read_text()


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


def test_release_path_and_push_local_use_the_shared_product_id_constants():
    """Both OTA_BEGIN call sites must reference the shared constants rather
    than re-embedding their own literals -- that duplication is exactly how
    this bug shipped (the release path and the local-upload path drifted
    from the devices independently)."""
    release_body = _strip_noise(_fn_body(UPD_C, "static bool apply_daq_ota("))
    assert "DAQ_PRODUCT_ID_P4" in release_body and "DAQ_PRODUCT_ID_C6" in release_body

    push_local_body = _strip_noise(_fn_body(UPD_C, "esp_err_t update_manager_push_local("))
    assert "DAQ_PRODUCT_ID_P4" in push_local_body and "DAQ_PRODUCT_ID_C6" in push_local_body
