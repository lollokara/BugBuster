"""IO-8 + MUX-4: a MUX write refused by the U17-S3 / U23 self-test interlock
must never look like success.

- `adgs_set_switch_safe()` returned void, so `adgs_set_api_switch_safe()`
  answered `true` for a refused close and BBP MUX_SET_SWITCH / HTTP
  /api/mux/switch replied success for a route that was never made.
- Five call sites ignored or only logged the result.

B: every write in these files uses its return value; refusals surface as BBP
0x13 ROUTE_REJECTED / HTTP 409 (MUX_SET_SWITCH, MUX_SET_ALL) or a reported
failure (planner, quick setup, channel-function auto-route)."""

import re
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "Firmware" / "ESP32" / "src"
FILES = ["bbp/cmds/cmd_status.cpp", "web/webserver.cpp", "tasks.cpp",
         "bus/bus_planner.cpp", "web/quicksetup.cpp"]
CALL = re.compile(r"\badgs_set_(api_)?(switch|all)_safe\s*\(")


def _unchecked(rel: str) -> list[str]:
    bad = []
    for n, line in enumerate((SRC / rel).read_text(encoding="utf-8").splitlines(), 1):
        code = line.split("//", 1)[0]
        if not CALL.search(code):
            continue
        if re.search(r"(if\s*\(\s*!?|=\s*|return\s+|&&|\|\|)\s*adgs_set_", code):
            continue
        bad.append(f"{rel}:{n}: {line.strip()}")
    return bad


def test_switch_safe_reports_refusal():
    hdr = (SRC / "hal" / "adgs2414d.h").read_text(encoding="utf-8")
    assert re.search(r"\bbool\s+adgs_set_switch_safe\s*\(", hdr)


def test_every_mux_write_checks_its_result():
    bad = [b for f in FILES for b in _unchecked(f)]
    assert not bad, "\n".join(bad)


def test_refusals_map_to_route_rejected():
    errs = (SRC / "bbp" / "cmd_errors.h").read_text(encoding="utf-8")
    assert re.search(r"CMD_ERR_ROUTE_REJECTED\s*:\s*return 0x13", errs) or \
        re.search(r"case CMD_ERR_ROUTE_REJECTED:\s*return 0x13", errs)
    status = (SRC / "bbp" / "cmds" / "cmd_status.cpp").read_text(encoding="utf-8")
    for fn in ("handler_mux_set_all", "handler_mux_set_switch"):
        body = status.split(f"static int {fn}(", 1)[1].split("\n}\n", 1)[0]
        assert "CMD_ERR_ROUTE_REJECTED" in body, fn
    web = (SRC / "web" / "webserver.cpp").read_text(encoding="utf-8")
    body = web.split("static esp_err_t handle_post_mux_switch(", 1)[1].split("\n}\n", 1)[0]
    assert "send_error(req, 409" in body
