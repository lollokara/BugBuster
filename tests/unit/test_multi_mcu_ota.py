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
    assert f"case {cmd}" in BOARD, f"{cmd} not dispatched"
    start = BOARD.index(f"case {cmd}")
    return BOARD[start:start + span]


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
