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
