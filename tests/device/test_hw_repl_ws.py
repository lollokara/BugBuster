"""Hardware test: the REPL WebSocket is read-only while a file script runs.

Input is refused with the documented message, script output still streams, and
Ctrl-C cannot stop a file script (Firmware/ESP32/src/mp/repl_ws.cpp).

    PYTHONPATH=python pytest tests/device/test_hw_repl_ws.py \
        --device-http=<ip> --admin-token=<tok>
"""

import asyncio

import pytest

from tests.device._hw_scripts import ScriptsApi, script_name

pytestmark = [pytest.mark.timeout(120), pytest.mark.http_only]

READ_ONLY_INPUT = "[read-only: a script is running, output only]"
READ_ONLY_CTRL_C = "[read-only: stop the script from the Scripts panel]"

TICK_SRC = (
    "import time\n"
    "for i in range(600):\n"
    "    print('wstick', i)\n"
    "    time.sleep_ms(100)\n"
)


async def _collect(ws, until, timeout):
    buf = ""
    end = asyncio.get_event_loop().time() + timeout
    while until not in buf:
        left = end - asyncio.get_event_loop().time()
        if left <= 0:
            break
        try:
            m = await asyncio.wait_for(ws.recv(), left)
        except asyncio.TimeoutError:
            break
        buf += m.decode(errors="replace") if isinstance(m, bytes) else m
    return buf


async def _drive(url, token, expect_ticks=True):
    import websockets
    async with websockets.connect(url) as ws:
        await ws.send(token)               # the first text frame is the bearer token
        banner = await _collect(ws, "REPL ready", 8)
        assert "REPL ready" in banner, "no banner after auth: %r" % banner
        out = {}
        out["stream"] = await _collect(ws, "wstick", 10 if expect_ticks else 1)
        await ws.send("print(1)\r")
        out["input"] = await _collect(ws, READ_ONLY_INPUT, 8)
        await ws.send(b"\x03")
        out["ctrlc"] = await _collect(ws, READ_ONLY_CTRL_C, 8)
        out["after"] = await _collect(ws, "wstick", 10 if expect_ticks else 1)
        return out


def test_repl_ws_is_read_only_while_file_script_runs(hw_scripts_session):
    pytest.importorskip("websockets")
    session, base = hw_scripts_session
    scripts = ScriptsApi(session, base)    # no ensure_idle(): never stop a script that is not ours
    st = scripts.status()
    ours = st.get("state") not in ("running", "stopping")
    name = script_name("wsro")
    try:
        if ours:
            scripts.upload(name, TICK_SRC)
            r = scripts.run_file(name)
            assert r.status_code == 200, "run-file: HTTP %d %s" % (r.status_code, r.text[:200])
            st = scripts.wait_state("running", timeout=10)
        else:
            name = st.get("name")      # e.g. the boot autorun.py: observe only

        url = base.replace("http://", "ws://") + "/api/scripts/repl/ws"
        token = session.headers["X-BugBuster-Admin-Token"]
        out = asyncio.run(_drive(url, token, expect_ticks=ours))

        assert READ_ONLY_INPUT in out["input"], out["input"][-200:]
        assert READ_ONLY_CTRL_C in out["ctrlc"], out["ctrlc"][-200:]
        if ours:
            assert "wstick" in out["stream"], "no script output streamed: %r" % out["stream"][-200:]
            assert "wstick" in out["after"], "output stopped after Ctrl-C: %r" % out["after"][-200:]
        st = scripts.status()
        assert st["state"] == "running" and st.get("name") == name, "Ctrl-C stopped the file script: %r" % st
    finally:
        if ours:
            scripts.cleanup()
