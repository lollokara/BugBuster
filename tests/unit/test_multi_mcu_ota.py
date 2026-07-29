"""Multi-MCU OTA: S3<->P4 wire parity and orchestration guards."""
import re
from pathlib import Path

S3LINK = Path("Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.h").read_text()
HAT_H = Path("Firmware/ESP32/src/hat/hat.h").read_text()

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


HAT_CPP = Path("Firmware/ESP32/src/hat/hat.cpp").read_text()


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


BOARD = Path("Firmware/DAQ_HAT/ESP32P4/src/board/daq_board.c").read_text()
S3LINK_C = Path("Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.c").read_text()


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
    assert "return 19;" in body, "0x65 must return the full 19-byte status"


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


RELAY_C6_H = Path("Firmware/DAQ_HAT/ESP32P4/src/ota/relay_c6.h").read_text()


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
