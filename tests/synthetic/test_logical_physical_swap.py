"""AN-03: logical channels C/D are wired to AD74416H physical channels D/C
(`tasks_logical_to_physical`). Every API takes a LOGICAL channel, so any
per-channel register access must translate first. RTD config, DAC readback
(BBP, HTTP) and the CLI `dac` / `status` readbacks used the logical number as
the physical register index, so on C/D they configured or read the wrong
channel."""

import re
from pathlib import Path

import pytest

SRC = Path(__file__).resolve().parents[2] / "Firmware" / "ESP32" / "src"


def _body(path: Path, signature: str) -> str:
    text = path.read_text(encoding="utf-8")
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for i in range(brace, len(text)):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        if depth == 0:
            return text[brace:i + 1]
    raise AssertionError(f"unbalanced body for {signature}")


SITES = [
    # (file, body anchor, raw-access regex that must NOT see a bare logical index)
    ("tasks.cpp", "case CMD_SET_RTD_CONFIG:", r"AD74416H_REG_RTD_CONFIG\(cmd\.channel\)"),
    ("bbp/bbp.cpp", "bool bbp_dac_read_active(", r"getDacActive\(ch\)"),
    ("web/webserver.cpp", "static esp_err_t handle_get_dac_readback(", r"AD74416H_REG_DAC_ACTIVE\(ch\)"),
    ("cli/cli_cmds_dev.cpp", "extern \"C\" void cli_cmd_dac(", r"getDacActive\(\(uint8_t\)ch\)"),
    ("cli/cli_cmds_dev.cpp", "extern \"C\" void cli_cmd_status(", r"(getChannelFunction|readAdcResult|getDacActive|readChannelAlertStatus)\(ch[,)]|dinComp >> ch\b"),
]


@pytest.mark.xfail(strict=True, reason="AN-03")
@pytest.mark.parametrize("rel,anchor,bad", SITES, ids=[f"{s[0]}:{s[1][:30]}" for s in SITES])
def test_per_channel_access_translates_logical_to_physical(rel, anchor, bad):
    body = _body(SRC / rel, anchor)
    assert not re.search(bad, body), f"{rel}: logical channel used as physical index"
    assert "tasks_logical_to_physical(" in body
