"""Spec 2026-10-03 section 6: the S3 pulls the P4's error/important log ring over the wide HAT path."""
import re
import subprocess

from tests.lib.srcread import read_source

HAT_H = "Firmware/ESP32/src/hat/hat.h"
HAT_CPP = "Firmware/ESP32/src/hat/hat.cpp"
P4_LINK = "Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.h"


def _define(src, name):
    m = re.search(rf"#define\s+{name}\s+0x([0-9A-Fa-f]+)u?", src)
    assert m, f"{name} is not defined"
    return int(m.group(1), 16)


def test_ids_match_the_p4():
    p4_src = read_source(P4_LINK)
    if "HATP_CMD_LOG_PULL" not in p4_src:
        try:
            p4_src = subprocess.check_output(["git", "show", "lollokara/hub-t3b:" + P4_LINK], text=True)
        except Exception:
            pass
    assert _define(read_source(HAT_H), "HAT_CMD_LOG_PULL") == _define(p4_src, "HATP_CMD_LOG_PULL")
    assert _define(read_source(HAT_H), "HAT_RSP_LOG_DATA") == _define(p4_src, "HATP_RSP_LOG_DATA")


def test_command_id_is_unique_on_the_s3():
    cmds = re.findall(r"#define\s+HAT_CMD_\w+\s+0x([0-9A-Fa-f]{2})u?", read_source(HAT_H))
    assert cmds.count("7E") == 1


def test_bs_and_log_pull_share_one_wide_path():
    cpp = read_source(HAT_CPP)
    assert cpp.count("hat_send_frame(cmd, req, req_len)") == 1
    assert "hat_wide_request(HAT_CMD_BS, HAT_RSP_BS_DATA," in cpp
    assert "hat_wide_request(HAT_CMD_LOG_PULL, HAT_RSP_LOG_DATA," in cpp
    assert "hat_send_frame(HAT_CMD_BS," not in cpp, "hat_bs_request must go through hat_wide_request"
