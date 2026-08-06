"""Multi-MCU OTA: S3<->P4 wire parity and orchestration guards."""
import re
from pathlib import Path

from tests.lib.srcread import read_source

S3LINK = read_source("Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.h")
HAT_H = read_source("Firmware/ESP32/src/hat/hat.h")

# Only two opcodes are new. OTA status rides the pre-existing 0x65 (reply
# widened) and the P4's own version the pre-existing 0x60; see
# test_no_duplicate_status_or_version_opcodes below.
NEW_CMDS = {
    "DAQ_RELAY_APPLY": "7A",
    "DAQ_C6_VERSION": "6A",
}


def _struct_body(text: str, struct: str) -> str:
    """The declared fields of `struct`, excluding any prose that names it."""
    m = re.search(rf"typedef struct[^{{]*{{([^{{}}]*)}}\s*{struct}\s*;", text, re.S)
    assert m, f"{struct} not defined"
    return m.group(1)


def _byte(text: str, prefix: str, name: str) -> str:
    m = re.search(rf"{prefix}_{name}\s+0x([0-9A-Fa-f]{{2}})", text)
    assert m, f"{prefix}_{name} not defined"
    return m.group(1).upper()


def test_new_ota_commands_match_on_both_sides_of_the_hat_link():
    for name, expected in NEW_CMDS.items():
        p4 = _byte(S3LINK, "HATP_CMD", name)
        s3 = _byte(HAT_H, "HAT_CMD", name)
        assert p4 == s3 == expected, f"{name}: P4=0x{p4} S3=0x{s3} want 0x{expected}"


def test_new_command_bytes_do_not_collide_with_existing_ones():
    for name, value in NEW_CMDS.items():
        others = re.findall(
            rf"#define\s+HATP_CMD_(?!{name}\b)[A-Z0-9_]+\s+0x({value})u?", S3LINK)
        assert not others, f"0x{value} ({name}) collides with another HATP command"


def test_command_bytes_stay_below_the_response_space():
    """HATP_RSP_OK is 0x80; anything at or above it is a response, not a command."""
    for name, value in NEW_CMDS.items():
        assert int(value, 16) < 0x80, f"{name} 0x{value} is in the response space"


def test_no_duplicate_status_or_version_opcodes():
    """A second opcode returning OTA status or the P4 version would shadow the
    pre-existing 0x65/0x60 and let the two replies drift apart."""
    assert "HATP_CMD_DAQ_OTA_STATUS" not in S3LINK, \
        "DAQ_OTA_STATUS duplicates HATP_CMD_OTA_STATUS (0x65); widen 0x65 instead"
    assert "HAT_CMD_DAQ_OTA_STATUS" not in HAT_H, \
        "DAQ_OTA_STATUS duplicates HAT_CMD_OTA_STATUS (0x65); widen 0x65 instead"
    assert "HATP_CMD_DAQ_FW_INFO" not in S3LINK, \
        "DAQ_FW_INFO duplicates HATP_CMD_GET_VERSION (0x60); use 0x60 + DAQ_C6_VERSION"
    assert "HAT_CMD_DAQ_FW_INFO" not in HAT_H, \
        "DAQ_FW_INFO duplicates HAT_CMD_GET_VERSION (0x60); use 0x60 + DAQ_C6_VERSION"


def test_ota_status_payload_carries_both_durability_models():
    """P4-target resume reads `received`; C6-target resume reads staged_bytes.
    One struct must expose both or the S3 cannot resume one of the two."""
    for field in ("state", "pending_verify", "received", "image_size",
                  "relay_state", "relay_staged_bytes", "relay_pushed_bytes"):
        assert field in S3LINK, f"s3link_ota_status_t missing {field}"
        assert field in HAT_H, f"hat_daq_ota_status_t missing {field}"


def test_ota_status_keeps_the_original_ten_byte_prefix_intact():
    """Older S3 builds read {state, pending_verify, received, image_size} and
    stop. Reordering those four breaks every already-flashed mainboard."""
    for text, struct in ((S3LINK, "s3link_ota_status_t"), (HAT_H, "hat_daq_ota_status_t")):
        body = _struct_body(text, struct)
        fields = re.findall(r"\b(state|pending_verify|received|image_size|"
                            r"relay_state|relay_staged_bytes|relay_pushed_bytes)\s*;", body)
        assert fields[:4] == ["state", "pending_verify", "received", "image_size"], \
            f"{struct} reordered its legacy 10-byte prefix: {fields}"


def test_c6_version_struct_does_not_restate_the_p4_version():
    """The P4 answers for itself via GET_VERSION (0x60); duplicating it here
    creates two sources of truth for the same value."""
    for text, struct in ((S3LINK, "s3link_c6_version_t"), (HAT_H, "hat_daq_c6_version_t")):
        body = _struct_body(text, struct)
        assert "p4_version" not in body, f"{struct} restates the P4 version"
        for field in ("c6_version", "c6_build_id"):
            assert field in body, f"{struct} missing {field}"


HAT_CPP = read_source("Firmware/ESP32/src/hat/hat.cpp")


def _fn_body(text: str, sig: str) -> str:
    """Source of a single function, from its signature to its closing brace."""
    start = text.index(sig)
    return text[start:text.index("\n}\n", start) + 3]


def _code_only(body: str) -> str:
    """Function body with // comments stripped, so assertions about what the
    code does are not satisfied (or broken) by prose that merely names it."""
    return "\n".join(re.sub(r"//.*$", "", ln) for ln in body.splitlines())


def test_ota_commands_mirror_the_p4_side():
    for name, value in (("GET_VERSION", "60"), ("OTA_BEGIN", "61"), ("OTA_DATA", "62"),
                        ("OTA_END", "63"), ("OTA_ABORT", "64"), ("OTA_STATUS", "65")):
        assert _byte(S3LINK, "HATP_CMD", name) == _byte(HAT_H, "HAT_CMD", name) == value, \
            f"{name} differs across the two headers"


def test_wide_frame_sender_is_separate_from_the_32_byte_path():
    """hat_send_frame() rejects payload_len > HAT_FRAME_MAX_LEN (32) and sizes its
    stack buffer for 32. The OTA path needs 236-byte payloads. Merging the two
    would silently raise the cap for every other command and the RP2040 HAT."""
    assert "hat_send_frame_wide" in HAT_CPP
    narrow = _fn_body(HAT_CPP, "static bool hat_send_frame(")
    assert "HAT_FRAME_MAX_LEN" in narrow, "narrow sender lost its 32-byte guard"


def test_wide_sender_buffer_is_sized_for_the_daq_link_payload():
    body = _fn_body(HAT_CPP, "static bool hat_send_frame_wide(")
    assert "HAT_OTA_WIDE_MAX" in body, "wide sender must size its buffer explicitly"
    assert "HAT_FRAME_MAX_LEN" not in body, "wide sender must not reuse the 32-byte cap"


def test_wide_sender_computes_crc_the_same_way_as_the_narrow_one():
    """Both frames are parsed by one P4 receiver. A CRC over a different span
    makes every wide frame fail CRC on the P4 with no other symptom."""
    narrow = _fn_body(HAT_CPP, "static bool hat_send_frame(")
    wide = _fn_body(HAT_CPP, "static bool hat_send_frame_wide(")
    span = re.compile(r"crc8\(&frame\[2\],\s*1 \+ payload_len\)")
    assert span.search(narrow), "narrow CRC span changed; update this test deliberately"
    assert span.search(wide), "wide sender must CRC over CMD+payload, like the narrow one"
    assert "HAT_FRAME_SYNC" in wide, "wide sender must emit the same SYNC byte"


def test_ota_chunk_max_matches_the_p4_wire_budget():
    p4 = re.search(r"HATP_MAX_PAYLOAD\s+(\d+)u?", S3LINK).group(1)
    assert p4 == "240"
    assert re.search(r"HAT_OTA_WIDE_MAX\s+(\d+)", HAT_H).group(1) == p4, \
        "wide budget must equal the P4's HATP_MAX_PAYLOAD"
    assert re.search(r"HAT_OTA_CHUNK_MAX\s+(\d+)", HAT_H).group(1) == "236", \
        "236 = 240 payload budget - 4-byte offset prefix"


def test_ota_meta_layout_matches_the_p4_struct():
    """hat_ota_meta_t is memcpy'd onto the wire and read back as ota_meta_t."""
    body = _struct_body(HAT_H, "hat_ota_meta_t")
    for field in ("image_size", "version_u32", "sha256", "product_id"):
        assert field in body, f"hat_ota_meta_t missing {field}"


def test_ota_senders_exist():
    for sig in ("bool hat_ota_begin(", "bool hat_ota_data(", "bool hat_ota_end("):
        assert sig in HAT_CPP, f"{sig} not implemented"
        assert sig.replace("bool ", "").rstrip("(") in HAT_H, f"{sig} not declared"


def test_ota_senders_hold_the_hat_link_mutex():
    """An OTA transfer must not interleave with telemetry polling on the link."""
    body = _fn_body(HAT_CPP, "static bool hat_ota_txn(")
    assert "s_hat_mutex" in body, "OTA transactions must serialize on the HAT mutex"


BOARD = read_source("Firmware/DAQ_HAT/ESP32P4/src/board/daq_board.c")
S3LINK_C = read_source("Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.c")


def _case_body(cmd: str, span: int = 2200) -> str:
    """The full `case cmd: { ... }` block, found by brace matching rather than a
    fixed character span -- a span silently truncates when a comment is added,
    which turns a real assertion into a false failure (or worse, a false pass)."""
    assert f"case {cmd}" in BOARD, f"{cmd} not dispatched"
    start = BOARD.index(f"case {cmd}")
    open_brace = BOARD.find("{", start)
    if open_brace < 0 or open_brace > start + 200:
        return BOARD[start:start + span]     # braceless case: fall back to span
    depth = 0
    for i in range(open_brace, len(BOARD)):
        if BOARD[i] == "{":
            depth += 1
        elif BOARD[i] == "}":
            depth -= 1
            if depth == 0:
                return BOARD[start:i + 1]
    raise AssertionError(f"unbalanced braces in case {cmd}")


def test_c6_version_command_is_dispatched():
    assert "case HATP_CMD_DAQ_C6_VERSION" in BOARD


def test_ota_status_reply_sources_both_modules():
    body = _case_body("HATP_CMD_OTA_STATUS")
    assert "ota_get_status" in body
    assert "relay_stage_get_status" in body, \
        "0x65 must carry the relay_stage fields; that is why it was widened"


def test_ota_status_widens_both_of_its_branches():
    """0x65 has a C6-target branch and a P4-target branch. Widening only one
    makes the reply length depend on which target was last selected, which the
    S3 cannot distinguish from an older P4's short reply."""
    body = _case_body("HATP_CMD_OTA_STATUS")
    assert "return 10;" not in body, "a branch of 0x65 still returns 10 bytes"
    # Derive the expected length from the struct rather than hardcoding it, so
    # appending a field updates both sides or fails loudly.
    sizes = {"uint8_t": 1, "uint32_t": 4}
    expect = sum(sizes[m] for m in re.findall(r"\b(uint8_t|uint32_t)\s+\w+\s*;",
                                              _struct_body(S3LINK, "s3link_ota_status_t")))
    assert f"return {expect};" in body, \
        f"0x65 must return {expect} bytes to match s3link_ota_status_t"


def test_c6_version_reply_uses_a_cached_version():
    """The C6 version arrives asynchronously over DDP. Answering must not block
    the s3_link RX callback on a DDP round-trip -- the C6 may be absent, busy,
    or held in download mode mid-relay."""
    body = _case_body("HATP_CMD_DAQ_C6_VERSION", 1200)
    assert "c6_fw_major" in body, "must answer from the ddp_master cache"
    assert "ddp_master_request" not in body, "C6_VERSION must not block on DDP"


def test_new_p4_commands_are_in_the_s3_link_dispatch_allow_list():
    """s3_link.c dispatches only an explicit allow-list; a handler in
    daq_board.c that is not listed there is dead code answering RSP_ERROR."""
    assert "case HATP_CMD_DAQ_C6_VERSION:" in S3LINK_C, \
        "HATP_CMD_DAQ_C6_VERSION missing from the s3_link.c allow-list"


def test_c6_version_gets_a_dedicated_response_code():
    """send_ok() carries a zero-length payload, so any command returning data
    needs its own response byte or the payload is silently dropped."""
    assert "HATP_RSP_DAQ_C6_VERSION" in S3LINK_C, \
        "C6_VERSION must reply with its own response code, not send_ok()"
    assert _byte(S3LINK, "HATP_RSP", "DAQ_C6_VERSION") == \
           _byte(HAT_H, "HAT_RSP", "DAQ_C6_VERSION") == "99"


def test_payload_carrying_responses_mirror_on_both_sides():
    for name, value in (("VERSION", "91"), ("OTA_STATUS", "92"),
                        ("DAQ_C6_VERSION", "99")):
        assert _byte(S3LINK, "HATP_RSP", name) == _byte(HAT_H, "HAT_RSP", name) == value, \
            f"HAT_RSP_{name} differs across the two headers"


def test_every_c6_flasher_reset_path_releases_the_boot_straps():
    """c6_flasher_begin() drives C6_BOOT_PIN low and never releases it, while
    both finish() and abort() reset the C6 via esp_loader_reset_target(). If the
    strap is still asserted at that reset the C6 returns to ROM download mode --
    black display, no log, and a P4 reset does not clear it. Releasing after the
    flasher call is too late; the reset happens inside it."""
    for cmd in ("HATP_CMD_OTA_END", "HATP_CMD_OTA_ABORT"):
        body = _case_body(cmd, 900)
        c6 = body[body.index("HATP_OTA_TARGET_C6"):]
        call = min((c6.index(f) for f in ("c6_flasher_finish", "c6_flasher_abort")
                    if f in c6), default=None)
        assert call is not None, f"{cmd} has no C6 flasher call"
        assert "c6_release_boot_straps" in c6[:call], \
            f"{cmd} resets the C6 without releasing the BOOT straps first"


def test_s3_helpers_exist_for_both_queries():
    for fn in ("hat_daq_ota_status", "hat_daq_c6_version"):
        assert f"bool {fn}(" in HAT_CPP, f"{fn} not implemented"
        assert fn in HAT_H, f"{fn} not declared"


def test_ota_status_helper_uses_the_existing_opcode():
    assert "HAT_CMD_OTA_STATUS" in _fn_body(HAT_CPP, "bool hat_daq_ota_status(")


def test_query_helpers_check_the_dedicated_response_codes_not_rsp_ok():
    """The P4 answers these with HATP_RSP_OTA_STATUS / _DAQ_C6_VERSION, because
    its send_ok() carries a zero-length payload. A helper checking HAT_RSP_OK
    would reject every valid reply and drop the payload."""
    for fn, code in (("hat_daq_ota_status", "HAT_RSP_OTA_STATUS"),
                     ("hat_daq_c6_version", "HAT_RSP_DAQ_C6_VERSION")):
        body = _code_only(_fn_body(HAT_CPP, f"bool {fn}("))
        assert code in body, f"{fn} must accept {code}"
        assert "HAT_RSP_OK" not in body, \
            f"{fn} checks HAT_RSP_OK, which the P4 never sends for this command"


def test_ota_status_helper_tolerates_an_older_p4s_short_reply():
    """A P4 predating the widening answers 10 bytes. That is valid and means
    'relay fields unknown' -- rejecting it would break OTA status against every
    already-flashed DAQ HAT."""
    body = _fn_body(HAT_CPP, "bool hat_daq_ota_status(")
    assert "HAT_OTA_STATUS_LEGACY_LEN" in body, \
        "helper must accept the 10-byte legacy prefix, not require the full struct"


def test_ota_status_helper_zeroes_the_struct_before_a_short_copy():
    """On a 10-byte reply the relay_* tail is never written. Without an explicit
    zero it would carry stack garbage, and a garbage relay_state could read as
    RELAY_STAGED and wave through the Task 8 safeguard."""
    body = _fn_body(HAT_CPP, "bool hat_daq_ota_status(")
    zero = body.index("memset")
    copy = body.index("memcpy")
    assert zero < copy, "must zero the output struct before copying a short reply"


RELAY_C6_H = read_source("Firmware/DAQ_HAT/ESP32P4/src/ota/relay_c6.h")


def test_relay_apply_is_dispatched_and_allow_listed():
    assert "case HATP_CMD_DAQ_RELAY_APPLY" in BOARD, "RELAY_APPLY not dispatched"
    assert "case HATP_CMD_DAQ_RELAY_APPLY:" in S3LINK_C, \
        "RELAY_APPLY missing from the s3_link.c allow-list -- it would answer RSP_ERROR"


def test_relay_apply_does_not_push_in_the_callback_context():
    """relay_c6.h: 'Blocks until done/failed; call from a dedicated task, not
    from the s3_link callback context.' The push takes ~2 minutes; running it
    inline would stall the whole HAT link and trip the S3's command timeouts."""
    assert "call from a dedicated task" in RELAY_C6_H, \
        "relay_c6.h no longer states the threading contract; re-check this test"
    body = _code_only(_case_body("HATP_CMD_DAQ_RELAY_APPLY", 1600))
    assert "relay_c6_push" not in body, \
        "RELAY_APPLY calls relay_c6_push() inline instead of on a worker task"
    assert "xTaskCreate" in body, "RELAY_APPLY must spawn a worker task"


def test_relay_apply_requires_a_verified_staged_image():
    """relay_stage only reaches RELAY_STAGED after its SHA-256 check passes.
    Pushing from any other state flashes an unverified or partial image."""
    body = _code_only(_case_body("HATP_CMD_DAQ_RELAY_APPLY", 1600))
    assert "RELAY_STAGED" in body, "RELAY_APPLY must gate on RELAY_STAGED"
    assert "RELAY_TARGET_C6" in body, "RELAY_APPLY must confirm the staged target is the C6"


def test_relay_apply_worker_restores_the_ddp_link_around_the_push():
    """relay_c6_push() drives c6_flasher directly and does no DDP handling of
    its own, so the board layer must release UART2 first and rebuild the link
    afterwards -- otherwise the C6 comes back with no command channel."""
    worker = _code_only(_fn_body(BOARD, "static void relay_apply_task("))
    assert "ddp_master_deinit" in worker, "worker must release UART2 before pushing"
    push = worker.index("relay_c6_push")
    assert worker.index("ddp_master_deinit") < push, "DDP released too late"
    assert "c6_link_restart" in worker and worker.index("c6_link_restart") > push, \
        "worker must rebuild the DDP link after the push"


def test_relay_apply_rejects_a_second_concurrent_push():
    """Two concurrent pushes would interleave on UART2 and strand the C6 in
    ROM download mode."""
    body = _code_only(_case_body("HATP_CMD_DAQ_RELAY_APPLY"))
    # Must READ the flag as a guard, not merely assign it -- checking only that
    # the name appears would still pass if the early-return were deleted.
    assert re.search(r"if\s*\(\s*s_relay_apply_busy\s*\)", body), \
        "RELAY_APPLY must refuse a concurrent push (no `if (s_relay_apply_busy)` guard)"
    guard = body.index("s_relay_apply_busy")
    create = body.index("xTaskCreate")
    assert guard < create, "the busy guard must precede the task create"
    assert re.search(r"s_relay_apply_busy\s*=\s*true", body[:create]), \
        "busy flag must be set BEFORE the create; pdPASS does not mean the task ran"


def test_relay_apply_worker_stack_is_internal_ram():
    """This build sets CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y, so a plain
    xTaskCreate() may put the stack in PSRAM. relay_c6_push() persists to NVS,
    which disables the D-cache across both cores -- PSRAM is reached through
    that cache, so a PSRAM stack frame is corrupted inside the write window.
    See patterns/firmware-autoupdate.md."""
    body = _code_only(_case_body("HATP_CMD_DAQ_RELAY_APPLY", 2000))
    assert "MALLOC_CAP_INTERNAL" in body, "relay_apply stack must be internal RAM"
    assert "xTaskCreatePinnedToCoreWithCaps" in body
    worker = _code_only(_fn_body(BOARD, "static void relay_apply_task("))
    assert "vTaskDeleteWithCaps" in worker, \
        "a WithCaps task must be deleted with vTaskDeleteWithCaps or its stack leaks"


def test_every_daq_helper_gates_on_a_daq_hat_being_present():
    """The HAT header also carries the RP2040 LA HAT. Without this gate these
    commands are sent to whatever is attached -- 0x61-0x65 and 0x6A/0x7A mean
    entirely different things to the RP2040 firmware. Every pre-existing
    hat_daq_* helper carries this check; the OTA ones must too."""
    for fn in ("hat_daq_ota_status", "hat_daq_c6_version", "hat_ota_begin",
               "hat_ota_data", "hat_ota_end", "hat_ota_abort"):
        body = _code_only(_fn_body(HAT_CPP, f"bool {fn}("))
        assert "HAT_TYPE_DAQ_POWER" in body, \
            f"{fn} does not verify a DAQ HAT is attached before sending"


UPD_H = read_source("Firmware/ESP32/src/update/update_manager.h")
UPD_CPP = read_source("Firmware/ESP32/src/update/update_manager.cpp")


def test_update_targets_are_distinct_bits():
    bits = dict(re.findall(r"UPDATE_TARGET_(\w+)\s*=\s*1u\s*<<\s*(\d+)", UPD_H))
    assert set(bits) == {"RP2040", "ESP32", "P4", "C6"}, f"unexpected targets: {bits}"
    assert len(set(bits.values())) == 4, f"two targets share a bit: {bits}"


def test_multi_target_order_puts_c6_before_p4_and_esp32_last():
    """C6 before P4 because the C6's ROM-loader push is driven BY the P4, which
    must still be running its current image. ESP32 last because rebooting it
    ends the sequence."""
    order = re.search(r"#define UPDATE_TARGET_ORDER\s*\\\s*\{([^}]*)\}", UPD_H).group(1)
    names = re.findall(r"UPDATE_TARGET_(\w+)", order)
    assert names.index("C6") < names.index("P4"), "C6 must be applied before the P4"
    assert names[-1] == "ESP32", "the ESP32 must be updated last"


def test_apply_takes_a_target_mask_not_two_booleans():
    assert "update_manager_apply(uint32_t targets" in UPD_H
    assert "update_manager_apply_release_index(uint8_t index, uint32_t targets" in UPD_H
    assert "bool update_rp2040, bool update_esp32" not in UPD_H, \
        "the old boolean API is still exposed; callers can bypass target validation"


def test_daq_targets_are_gated_on_a_daq_hat_being_attached():
    body = _code_only(_fn_body(UPD_CPP, "uint32_t update_manager_available_targets("))
    assert "HAT_TYPE_DAQ_POWER" in body, "P4/C6 must only be offered with a DAQ HAT"
    assert "UPDATE_TARGETS_DAQ_HAT" in body


def test_unavailable_targets_are_rejected_not_silently_dropped():
    """Silently dropping a target reports success for an update that never
    happened, which is worse than an error a client can surface."""
    body = _code_only(_fn_body(UPD_CPP, "static bool targets_are_valid("))
    assert "update_manager_available_targets()" in body
    assert "return false" in body


def test_every_apply_call_site_uses_the_mask():
    """A missed call site would still compile if it passed bools that implicitly
    converted to uint32_t -- 'true, true' would silently mean RP2040|ESP32."""
    for path in ("Firmware/ESP32/src/net/api_core.cpp",
                 "Firmware/ESP32/src/web/webserver.cpp",
                 "Firmware/ESP32/src/cli/cli_cmds_sys.cpp",
                 "Firmware/ESP32/src/cli/cli_menu.cpp"):
        src = read_source(path)
        for m in re.finditer(r"update_manager_apply(?:_release_index)?\(([^;]*?)&out|&root", src):
            args = m.group(1) or ""
            assert "true" not in args and "false" not in args, \
                f"{path} still passes booleans to the target mask: {args.strip()}"


def test_daq_download_streams_to_the_hat_link_with_no_local_file():
    """The P4's staging partition is already the durable buffer. A 2 MB local
    copy would consume two thirds of the 3 MB `scripts` SPIFFS that holds user
    MicroPython files."""
    body = _code_only(_fn_body(UPD_CPP, "static esp_err_t hat_ota_event_handler("))
    assert "hat_ota_data" in body, "handler must feed the HAT link directly"
    assert "fopen" not in body and "fwrite" not in body, \
        "the DAQ path must not stage to a local file"


def test_daq_handler_splits_into_hat_link_chunks():
    """The HTTP buffer delivers up to buffer_size bytes; the DAQ link carries
    HAT_OTA_CHUNK_MAX per frame. Passing the whole buffer would truncate."""
    body = _code_only(_fn_body(UPD_CPP, "static esp_err_t hat_ota_event_handler("))
    assert "HAT_OTA_CHUNK_MAX" in body
    assert "while" in body, "handler must loop over the buffer, not send it once"


def test_daq_resume_offset_comes_from_the_p4_not_a_local_counter():
    """After a failed frame the S3 cannot know how much the P4 kept, and the P4
    rejects out-of-order offsets -- so the resume point must be re-queried."""
    body = _code_only(_fn_body(UPD_CPP, "static uint32_t daq_resume_offset("))
    assert "hat_daq_ota_status" in body
    assert "st.received" in body and "st.relay_staged_bytes" in body, \
        "the two targets track progress in different fields; both must be read"


def test_daq_ota_uses_a_range_header_to_resume():
    body = _code_only(_fn_body(UPD_CPP, "static bool apply_daq_ota("))
    assert '"Range"' in body, "resume must re-request with an HTTP Range header"
    assert "206" in body, "a resumed request must require 206, not accept a 200 restart"


def test_daq_ota_does_not_hash_locally():
    """On a Range-resume the S3 never sees the earlier bytes, so any hash it
    computed would cover the wrong range. The SHA travels in the OTA_BEGIN meta
    and the P4 verifies it at OTA_END."""
    body = _code_only(_fn_body(UPD_CPP, "static bool apply_daq_ota("))
    assert "mbedtls_sha256_update" not in body, \
        "the S3 must not hash a resumable stream; the P4 verifies"
    assert "hat_ota_end" in body


def test_c6_goes_through_staging_and_p4_does_not():
    """C6 must be SHA-verified in the P4's staging partition before the
    ROM-loader push starts -- an aborted push strands it in download mode. The
    P4 target bypasses staging and streams to its own A/B slot."""
    body = _code_only(_fn_body(UPD_CPP, "static bool apply_daq_targets("))
    c6 = body[body.index("UPDATE_TARGET_C6"):body.index("UPDATE_TARGET_P4")]
    assert "HAT_OTA_TARGET_STAGE" in c6, "the C6 image must be staged"
    assert "hat_daq_relay_apply" in c6, "staging alone does not flash the C6"
    p4 = body[body.index("UPDATE_TARGET_P4"):]
    assert "HAT_OTA_TARGET_P4" in p4


def test_daq_apply_order_puts_c6_before_p4_in_the_code_too():
    body = _code_only(_fn_body(UPD_CPP, "static bool apply_daq_targets("))
    assert body.index("UPDATE_TARGET_C6") < body.index("UPDATE_TARGET_P4"), \
        "the C6 leg must run before the P4 leg, matching UPDATE_TARGET_ORDER"


def test_both_apply_entry_points_handle_the_daq_targets():
    """apply_release_index() silently ignoring P4/C6 would report success for an
    update that never ran -- the exact failure the target validation prevents."""
    for fn in ("esp_err_t update_manager_apply(",
               "esp_err_t update_manager_apply_release_index("):
        body = _code_only(_fn_body(UPD_CPP, fn))
        assert "apply_daq_targets" in body, f"{fn} drops the DAQ targets"


def test_missing_daq_image_in_a_release_is_an_error():
    body = _code_only(_fn_body(UPD_CPP, "static bool apply_daq_targets("))
    assert body.count("component_available") >= 2, \
        "both DAQ legs must check the release actually carries an image"


def test_ota_status_carries_the_relay_target():
    """Without the target the S3 cannot tell a staged C6 image from one staged
    for itself. Both sides must agree on the field."""
    for text, struct in ((S3LINK, "s3link_ota_status_t"), (HAT_H, "hat_daq_ota_status_t")):
        assert "relay_target" in _struct_body(text, struct), f"{struct} missing relay_target"


def test_s3_pull_path_requires_a_verified_stage_for_itself():
    """apply_esp32_ota_from_p4_stage() writes to the S3's OWN OTA slot and sets
    it bootable. Pulling a partial image, or a C6 image, bricks the mainboard --
    and unlike the DAQ HAT there is no second MCU left to recover it."""
    body = _code_only(_fn_body(UPD_CPP, "esp_err_t apply_esp32_ota_from_p4_stage("))
    guard = body[:body.index("esp_ota_get_next_update_partition")]
    assert "HAT_RELAY_STAGED" in guard, \
        "must require RELAY_STAGED (the only SHA-verified state) before pulling"
    assert "HAT_RELAY_TARGET_S3" in guard, \
        "must require the staged image be for the S3, not the C6"


def test_relay_state_constants_mirror_the_p4_enum():
    """These gate a brick-risk decision; a drifted value silently changes which
    state counts as verified."""
    for name, value in (("IDLE", 0), ("STAGING", 1), ("STAGED", 2),
                        ("PUSHING", 3), ("DONE", 4), ("FAILED", 5)):
        m = re.search(rf"#define HAT_RELAY_{name}\s+(\d+)u", HAT_H)
        assert m and int(m.group(1)) == value, f"HAT_RELAY_{name} should be {value}"
    for name, value in (("C6", 1), ("S3", 2)):
        m = re.search(rf"#define HAT_RELAY_TARGET_{name}\s+(\d+)u", HAT_H)
        assert m and int(m.group(1)) == value, f"HAT_RELAY_TARGET_{name} should be {value}"


def test_status_reports_which_target_is_active():
    body = _code_only(_fn_body(UPD_CPP, "cJSON *update_manager_status_json("))
    assert "activeTarget" in body, "status must say which MCU is being updated"
    assert "availableTargets" in body, "clients need to know which targets exist"


def test_each_download_phase_tags_its_target():
    """A progress bar that cannot name the MCU is useless in a multi-target run."""
    src = UPD_CPP
    for state in ("UPDATE_STATE_DOWNLOADING_RP2040", "UPDATE_STATE_DOWNLOADING_ESP32",
                  "UPDATE_STATE_DOWNLOADING_P4", "UPDATE_STATE_DOWNLOADING_C6"):
        idx = src.index(f"set_state({state}")
        assert "set_target(" in src[max(0, idx - 200):idx], \
            f"{state} does not record which target it belongs to"


WEBSERVER = read_source("Firmware/ESP32/src/web/webserver.cpp")


def test_http_apply_route_parses_every_target():
    """webserver.cpp's /api/update/apply is a SEPARATE handler from api_core's.
    It previously only read rp2040/esp32 and defaulted both to true, so a body
    naming just {"p4":true,"c6":true} silently updated the mainboard and LA HAT
    instead -- pulling the GitHub nightly over whatever was installed."""
    body = _code_only(_fn_body(WEBSERVER, "static esp_err_t handle_post_update_apply("))
    for key, bit in (('"rp2040"', "UPDATE_TARGET_RP2040"), ('"esp32"', "UPDATE_TARGET_ESP32"),
                     ('"p4"', "UPDATE_TARGET_P4"), ('"c6"', "UPDATE_TARGET_C6")):
        assert key in body, f"HTTP apply route does not parse {key}"
        assert bit in body, f"HTTP apply route does not map {key} to {bit}"


def test_http_apply_body_naming_targets_does_not_inherit_defaults():
    """An explicit body must select targets from scratch; inheriting the
    rp2040+esp32 defaults is what made the bug destructive rather than a no-op."""
    body = _code_only(_fn_body(WEBSERVER, "static esp_err_t handle_post_update_apply("))
    assert "named_any" in body, "explicit target lists must reset the default mask"
    assert "targets == 0" in body, "an empty selection must be rejected, not applied"


def test_both_http_surfaces_agree_on_target_keys():
    """api_core.cpp (HTTP+BLE) and webserver.cpp must accept the same JSON keys
    or a client's request means different things depending on the transport."""
    api = read_source("Firmware/ESP32/src/net/api_core.cpp")
    api_body = _code_only(_fn_body(api, "static char *api_ota_apply("))
    web_body = _code_only(_fn_body(WEBSERVER, "static esp_err_t handle_post_update_apply("))
    for key in ('"rp2040"', '"esp32"', '"p4"', '"c6"'):
        assert key in api_body and key in web_body, f"{key} missing from one surface"


def test_withcaps_tasks_are_deleted_with_withcaps():
    """A task created by xTaskCreate*WithCaps() owns its stack and TCB, and only
    vTaskDeleteWithCaps() can free them. Plain vTaskDelete() leaks the entire
    stack -- measured at 12 KB of INTERNAL RAM per update on the S3, which took
    the largest free internal block from 14 KB to 8 KB and made every subsequent
    update fail to start (the worker needs 12 KB contiguous internal)."""
    roots = ["Firmware/ESP32/src", "Firmware/DAQ_HAT/ESP32P4/src"]
    offenders = []
    for root in roots:
        for path in Path(root).rglob("*.c*"):
            src = path.read_text(errors="ignore")
            if "WithCaps(" not in src:
                continue
            if not any(c in src for c in ("xTaskCreateWithCaps(",
                                          "xTaskCreatePinnedToCoreWithCaps(")):
                continue
            code = _code_only(src)
            # A file that creates a WithCaps task and also self-deletes a task
            # must use the WithCaps deleter for it.
            if "vTaskDelete(" in code and "vTaskDeleteWithCaps(" not in code:
                offenders.append(str(path))
    assert not offenders, (
        "these files create WithCaps tasks but delete with plain vTaskDelete, "
        f"leaking each task's stack: {offenders}")
