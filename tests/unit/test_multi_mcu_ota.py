"""Multi-MCU OTA: S3<->P4 wire parity and orchestration guards."""
import re
from pathlib import Path

S3LINK = Path("Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.h").read_text()
HAT_H = Path("Firmware/ESP32/src/hat/hat.h").read_text()

NEW_CMDS = {
    "DAQ_RELAY_APPLY": "7A",
    "DAQ_OTA_STATUS": "7B",
    "DAQ_FW_INFO": "7C",
}


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
    existing = set(re.findall(r"#define\s+HATP_CMD_[A-Z0-9_]+\s+0x([0-9A-Fa-f]{2})", S3LINK))
    for name, value in NEW_CMDS.items():
        others = re.findall(
            rf"#define\s+HATP_CMD_(?!{name}\b)[A-Z0-9_]+\s+0x({value})u?", S3LINK)
        assert not others, f"0x{value} ({name}) collides with another HATP command"
    assert len(existing) == len(
        re.findall(r"#define\s+HATP_CMD_[A-Z0-9_]+\s+0x[0-9A-Fa-f]{2}", S3LINK)
    ), "duplicate HATP command byte detected"


def test_ota_status_payload_carries_both_durability_models():
    """P4-target resume reads ota_received; C6-target resume reads staged_bytes.
    One struct must expose both or the S3 cannot resume one of the two."""
    for field in ("ota_state", "ota_received", "relay_state",
                  "relay_staged_bytes", "relay_pushed_bytes"):
        assert field in S3LINK, f"s3link_ota_status_t missing {field}"
        assert field in HAT_H, f"hat_daq_ota_status_t missing {field}"


def test_fw_info_reports_both_p4_and_c6():
    for field in ("p4_version", "p4_build_id", "c6_version", "c6_build_id"):
        assert field in S3LINK, f"s3link_fw_info_t missing {field}"
        assert field in HAT_H, f"hat_daq_fw_info_t missing {field}"
