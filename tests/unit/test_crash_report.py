"""Host-side consumers of the boot report / crash dump API (bugbuster.crash,
BugBuster.get_crash_info & co., and the MCP crash tools).

The JSON shapes are the ones Firmware/ESP32/src/diag/crash_report.cpp emits."""
from __future__ import annotations

import base64
import json
from unittest.mock import patch

import pytest

from bugbuster import BugBuster
from bugbuster.crash import (
    decode_chunk, parse_boot_report_json, parse_bootrpt_lines, parse_crash_json,
)
from bugbuster_mcp import session
from bugbuster_mcp.tools import discovery
from tests.unit._mock_client import make_client_mock

PRE = {"boot": 4, "streak": 1, "up_ms": 88123, "phase": "running", "int_free": 18000,
       "int_min": 12000, "int_largest": 6000, "psram_free": 5_000_000, "psram_min": 4_900_000,
       "min_stack_free": 200, "min_stack_task": "httpd", "wifi_sta": True, "hat": True, "usb": False}

CRASH = {
    "ready": True, "present": True, "valid": True, "ok": True,
    "reset": {"reason": "PANIC", "code": 4, "abnormal": True, "boot": 5, "streak": 1},
    "size": 3000, "chunk_max": 768, "panic": "Guru Meditation Error: LoadProhibited",
    "task": "httpd", "tcb": "0x3fc90000", "pc": "0x4037cec2", "exccause": 28,
    "exccause_name": "LoadProhibited", "vaddr": "0x00000000",
    "bt": ["0x4037cec2", "0x42012345"], "bt_corrupt": False,
    "dump_elf": "abcdef123456", "elf_match": False,
    "regs": [f"0x{i:08x}" for i in range(16)], "pre": PRE,
}

CLEAN = {"ready": True, "present": False, "valid": False,
         "reset": {"reason": "POWERON", "code": 1, "abnormal": False, "boot": 1, "streak": 0}}


def test_parse_crash_json_full():
    c = parse_crash_json(CRASH)
    assert c.has_dump and c.abnormal and c.task == "httpd"
    assert c.pc == 0x4037CEC2 and c.exccause_name == "LoadProhibited"
    assert c.backtrace == (0x4037CEC2, 0x42012345) and len(c.regs) == 16
    assert c.pre and c.pre.min_stack_task == "httpd"
    assert "in task 'httpd' at pc=0x4037cec2" in c.summary()
    w = " | ".join(c.warnings())
    assert "abnormal" in w and "different firmware build" in w and "abcdef123" in w
    assert "internal heap was down to 18000 B" in w and "httpd had 200 B" in w
    assert c.addr2line_command("fw.elf").startswith("xtensa-esp32s3-elf-addr2line -pfiaC -e fw.elf 0x4037cec2")
    assert c.decode_command("c.elf", "fw.elf") == "espcoredump.py info_corefile -c c.elf fw.elf"


def test_parse_crash_json_clean_boot():
    c = parse_crash_json(CLEAN)
    assert not c.has_dump and not c.abnormal and not c.bootloop
    assert c.warnings() == [] and c.summary().endswith("no coredump stored")
    assert c.addr2line_command() == ""


def test_not_ready_and_bootloop_and_invalid_dump():
    c = parse_crash_json({"ready": False, "present": False, "valid": False})
    assert "not ready" in c.warnings()[0]
    loop = dict(CLEAN, reset={"reason": "TASK_WDT", "code": 6, "abnormal": True, "boot": 9, "streak": 3})
    assert parse_crash_json(loop).bootloop
    bad = dict(CLEAN, present=True, check="ESP_ERR_INVALID_CRC")
    assert any("failed its check (ESP_ERR_INVALID_CRC)" in x for x in parse_crash_json(bad).warnings())


def test_decode_chunk_validates():
    raw = b"\x7fELF" + bytes(10)
    ok = {"ok": True, "offset": 0, "len": 14, "total": 14, "eof": True,
          "data": base64.b64encode(raw).decode()}
    assert decode_chunk(ok) == raw
    with pytest.raises(ValueError, match="no coredump"):
        decode_chunk({"ok": False, "error": "no coredump stored"})
    with pytest.raises(ValueError, match="length mismatch"):
        decode_chunk(dict(ok, len=99))


def _lines(boot=5, split_mem=True, drop=None, end=True):
    mem = {"int": {"free": 90000, "min": 20000, "total": 300000, "largest": 40000},
           "tasks": [{"n": "httpd", "size": 4096, "hwm": 300, "low": True}], "low_stacks": 1}
    secs = {"sys": {"fw": "1.2.3", "up_ms": 30500, "reset": CRASH["reset"]},
            "mem": mem, "hw": {"hat": {"degraded": True}}, "crash": dict(CRASH)}
    out = []
    for name, obj in secs.items():
        text = json.dumps(obj, separators=(",", ":"))
        n = 2 if (name == "mem" and split_mem) else 1
        step = -(-len(text) // n)
        for i in range(n):
            if drop == (name, i + 1):
                continue
            out.append(f"W (30123) bootrpt: BOOTRPT {boot} {name} {i + 1}/{n} {text[i * step:(i + 1) * step]}")
    if end:
        out.append(f"\x1b[0;32mI (30200) bootrpt: BOOTRPT {boot} end 1/1 " + '{"ms":12}\x1b[0m')
    return out


def test_parse_bootrpt_lines_reassembles_parts_and_ignores_noise():
    noisy = ["boot: unrelated line"] + _lines() + ["BOOTRPT garbage"]
    (r,) = parse_bootrpt_lines(noisy)
    assert r.boot == 5 and r.complete and r.incomplete_sections == []
    assert r.sections["mem"]["tasks"][0]["n"] == "httpd"
    assert r.firmware == "1.2.3" and r.crash.task == "httpd"
    w = r.warnings()
    assert any("httpd has only 300 B" in x for x in w) and any("HAT link is degraded" in x for x in w)
    assert any("minimum 20000 B" in x for x in w)
    assert "boot 5" in r.summary() and "fw 1.2.3" in r.summary()


def test_parse_bootrpt_lines_missing_part_and_multiple_boots():
    reports = parse_bootrpt_lines("\n".join(_lines(boot=7, drop=("mem", 2), end=False) + _lines(boot=6)))
    assert [r.boot for r in reports] == [6, 7]
    assert reports[0].complete and not reports[1].complete
    assert "mem" not in reports[1].sections and reports[1].incomplete_sections == ["mem"]
    assert any("missing log lines" in x for x in reports[1].warnings())


def test_parse_boot_report_json_bundle():
    r = parse_boot_report_json({"sys": {"reset": {"boot": 3}}, "crash": CLEAN, "ignored": 1})
    assert r.boot == 3 and set(r.sections) == {"sys", "crash"} and r.complete


# --- client methods ---------------------------------------------------------

class _Http:
    """Just enough of HTTPTransport for the client's crash helpers."""

    def __init__(self, dump: bytes | None):
        self.dump, self.calls, self.cleared = dump, [], False

    def get(self, path, params=None, headers=None):
        self.calls.append((path, params))
        params = params or {}
        if "offset" in params:
            if self.dump is None:
                return {"ok": False, "error": "no coredump stored"}
            off, ln = params["offset"], min(params["len"], 768)
            part = self.dump[off:off + ln]
            return {"ok": True, "offset": off, "len": len(part), "total": len(self.dump),
                    "eof": off + len(part) >= len(self.dump), "data": base64.b64encode(part).decode()}
        if params.get("report"):
            return {"sys": {"reset": {"boot": 5}}, "crash": CRASH}
        return CRASH

    def post(self, path, body=None, headers=None):
        self.cleared = True
        return {"ok": True, "had_dump": self.dump is not None}


def _client(dump):
    bb = BugBuster.__new__(BugBuster)
    bb._usb = False
    bb._t = _Http(dump)
    bb._admin_token = "t"
    return bb


def test_client_download_chunks_and_writes_file(tmp_path):
    dump = bytes(range(256)) * 12 + b"tail"           # 3076 B -> 5 slices of <=768
    bb = _client(dump)
    seen = []
    out = bb.download_coredump(str(tmp_path / "c.elf"), progress=lambda d, t: seen.append((d, t)))
    assert out == dump and (tmp_path / "c.elf").read_bytes() == dump
    assert seen[-1] == (len(dump), len(dump)) and len(bb._t.calls) == 5


def test_client_download_without_dump_and_http_only():
    with pytest.raises(FileNotFoundError):
        _client(None).download_coredump()
    usb = _client(b"x")
    usb._usb = True
    for fn in (usb.get_crash_info, usb.get_boot_report, usb.download_coredump, usb.clear_coredump):
        with pytest.raises(NotImplementedError, match="HTTP"):
            fn()


def test_client_info_report_clear():
    bb = _client(b"abc")
    assert bb.get_crash_info().task == "httpd"
    assert bb.get_boot_report().boot == 5
    assert bb.clear_coredump() is True and bb._t.cleared


# --- MCP tools --------------------------------------------------------------

class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def deco(fn):
            self.tools[fn.__name__] = fn
            return fn
        return deco


@pytest.fixture
def tools():
    session.configure(transport="http", host="10.0.0.2")
    m = _DummyMCP()
    discovery.register(m)
    return m.tools


@pytest.fixture
def bb():
    c = make_client_mock()
    with patch("bugbuster_mcp.session.get_client", return_value=c):
        yield c


def test_mcp_crash_info_with_dump(tools, bb):
    bb.get_crash_info.return_value = parse_crash_json(CRASH)
    res = tools["crash_info"]()
    assert res["has_dump"] and res["dump"]["exccause_name"] == "LoadProhibited"
    assert res["dump"]["pc"] == "0x4037cec2" and res["pre_crash"]["tightest_stack"]["task"] == "httpd"
    assert res["dump"]["matches_running_firmware"] is False
    assert any("crash_dump_save" in s for s in res["next_steps"])
    assert res["reset"]["abnormal"] is True


def test_mcp_crash_info_clean_and_usb_error(tools, bb):
    bb.get_crash_info.return_value = parse_crash_json(CLEAN)
    res = tools["crash_info"]()
    assert not res["has_dump"] and "dump" not in res and res["warnings"] == []
    bb.get_crash_info.side_effect = NotImplementedError("only available over HTTP")
    assert "HTTP" in tools["crash_info"]()["error"]


def test_mcp_boot_report_from_logs_and_live(tools, bb):
    res = tools["boot_report"](log_text="\n".join(_lines(boot=2) + _lines(boot=3)))
    assert res["count"] == 2 and res["reports"][1]["boot"] == 3 and "summary" in res["reports"][0]
    assert "error" in tools["boot_report"](log_text="no marker at all")
    bb.get_boot_report.return_value = parse_boot_report_json({"sys": {"fw": "9"}, "crash": CLEAN}, boot=4)
    live = tools["boot_report"]()
    assert live["boot"] == 4 and live["complete"]


def test_mcp_crash_dump_save(tools, bb, tmp_path):
    bb.get_crash_info.return_value = parse_crash_json(CRASH)
    bb.download_coredump.return_value = b"\x7fELF" + bytes(20)
    res = tools["crash_dump_save"](directory=str(tmp_path))
    assert res["saved"] and res["elf_magic_ok"] and res["size_bytes"] == 24
    assert res["path"].endswith("bugbuster-coredump-boot5-abcdef123.elf")
    assert (tmp_path / "bugbuster-coredump-boot5-abcdef123.elf").exists()
    assert "info_corefile" in res["decode"]
    bb.get_crash_info.return_value = parse_crash_json(CLEAN)
    assert tools["crash_dump_save"](directory=str(tmp_path))["saved"] is False


def test_mcp_crash_clear_requires_confirm(tools, bb):
    with pytest.raises(ValueError, match="confirm=True"):
        tools["crash_clear"]()
    bb.clear_coredump.assert_not_called()
    bb.clear_coredump.return_value = True
    assert tools["crash_clear"](confirm=True) == {"cleared": True, "had_dump": True}
