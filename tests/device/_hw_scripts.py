"""Shared helpers for the scripting / DAQ-module hardware tests.

Not a test module (leading underscore): it holds the HTTP client for the
``/api/scripts/*`` contract (spec 2026-10-03 §2, plan "Shared JSON contract")
and the fixtures the ``test_hw_*`` files import.

These tests need a REAL board reachable over HTTP. The simulator has no
``/api/scripts/*`` v2 surface (state / lastExit / chunk upload / 409 busy), so
under ``--sim`` / ``--sim-full`` every fixture here skips with that reason.

Run (see tests/device/COVERAGE-scripting-daq.md for the full table):

    PYTHONPATH=python pytest tests/device/test_hw_scripts_runtime.py \
        --device-http=<ip> --device-usb=<port>          # token read over USB
    PYTHONPATH=python pytest tests/device/test_hw_scripts_runtime.py \
        --device-http=<ip> --admin-token=<token>        # no USB needed
"""

from __future__ import annotations

import ast
import base64
import re
import time
from typing import Optional

import pytest

TEST_PREFIX = "bbt_hw_"          # every script / run this suite creates starts with it
LOG_LINE = re.compile(r"^(\d+) ([EWID]) (mpy|sys) (.*)$")
STATES = {"idle", "running", "stopping", "error", "done"}
EXITS = {"ok", "error", "stopped"}
SOURCES = {"manual", "autorun", "repl"}

# A script that runs until it is stopped (cooperative stop lands on a bytecode
# boundary, so keep the loop in Python and sleep in short slices).
LONG_RUNNING_SRC = (
    "import time\n"
    "print('long-start')\n"
    "for _ in range(1200):\n"
    "    time.sleep_ms(50)\n"
    "print('long-finished-unexpectedly')\n"
)


class ScriptsApi:
    """Thin client over the scripts routes; remembers what it created."""

    def __init__(self, session, base: str):
        self.s = session
        self.base = base
        self.created: list[str] = []

    # ---- raw ------------------------------------------------------------
    def get(self, path: str, **kw):
        return self.s.get(self.base + path, timeout=kw.pop("timeout", 10), **kw)

    def post(self, path: str, **kw):
        return self.s.post(self.base + path, timeout=kw.pop("timeout", 15), **kw)

    # ---- status / control -----------------------------------------------
    def status(self) -> dict:
        r = self.get("/api/scripts/status")
        assert r.status_code == 200, "status: HTTP %d %s" % (r.status_code, r.text[:200])
        return r.json()

    def stop(self):
        return self.post("/api/scripts/stop")

    def wait_state(self, want, timeout: float = 20.0, poll: float = 0.25) -> dict:
        """Poll status until ``state`` is in *want*; return the last status."""
        want = {want} if isinstance(want, str) else set(want)
        end = time.time() + timeout
        st = {}
        while time.time() < end:
            st = self.status()
            if st.get("state") in want:
                return st
            time.sleep(poll)
        raise AssertionError("state never reached %s in %.0fs; last status: %r" % (sorted(want), timeout, st))

    def wait_not_running(self, timeout: float = 20.0) -> dict:
        return self.wait_state({"idle", "done", "error"}, timeout=timeout)

    def ensure_idle(self) -> dict:
        """Stop whatever holds the slot and wait for it to let go."""
        st = self.status()
        if st.get("state") in ("running", "stopping") or st.get("running"):
            self.stop()
            st = self.wait_not_running(timeout=25.0)
        return st

    # ---- files ----------------------------------------------------------
    def list_files(self) -> list[str]:
        r = self.get("/api/scripts/files")
        assert r.status_code == 200, "files: HTTP %d %s" % (r.status_code, r.text[:200])
        j = r.json()
        raw = j.get("files", j) if isinstance(j, dict) else j
        out = []
        for e in raw:
            out.append(e["name"] if isinstance(e, dict) else str(e))
        return out

    def get_file(self, name: str) -> Optional[bytes]:
        r = self.get("/api/scripts/files/get", params={"name": name})
        return r.content if r.status_code == 200 else None

    def delete_file(self, name: str):
        return self.post("/api/scripts/files/delete", params={"name": name})

    def chunk(self, name: str, off: int, data: bytes, final: bool = False):
        body = {"name": name, "off": off, "b64": base64.b64encode(data).decode(), "final": final}
        return self.post("/api/scripts/files/chunk", json=body)

    def upload(self, name: str, src: str | bytes, chunk_bytes: int = 600) -> None:
        """Chunked upload of the whole of *src*; asserts every chunk is accepted."""
        data = src.encode() if isinstance(src, str) else src
        self.created.append(name)
        off = 0
        while True:
            piece = data[off:off + chunk_bytes]
            final = off + len(piece) >= len(data)
            r = self.chunk(name, off, piece, final)
            assert r.status_code == 200 and r.json().get("ok") is True, \
                "chunk off=%d rejected: HTTP %d %s" % (off, r.status_code, r.text[:200])
            off += len(piece)
            if final:
                return

    # ---- running --------------------------------------------------------
    def run_file(self, name: str, background: bool = True, replace: bool = False):
        params = {"name": name}
        if background:
            params["background"] = "1"
        if replace:
            params["replace"] = "1"
        return self.post("/api/scripts/run-file", params=params)

    def eval(self, src: str):
        return self.post("/api/scripts/eval", data=src.encode(),
                         headers={"Content-Type": "text/x-python"})

    def run_source(self, name: str, src: str, timeout: float = 30.0) -> tuple[dict, str]:
        """Upload, run in the background, wait for it to finish; return (status, log text)."""
        before = self.ensure_idle().get("totalRuns", 0)
        cursor = self.log_cursor()
        self.upload(name, src)
        r = self.run_file(name)
        assert r.status_code == 200, "run-file %s: HTTP %d %s" % (name, r.status_code, r.text[:200])
        # A script this short can finish between polls, and the slot still reads
        # the PREVIOUS run's terminal state until the VM picks the new one up, so
        # wait for a terminal state AND totalRuns to have moved past `before`.
        end = time.time() + timeout
        st = {}
        while time.time() < end:
            st = self.status()
            if st.get("state") in ("done", "error", "idle") and st.get("totalRuns", 0) > before:
                break
            time.sleep(0.25)
        else:
            raise AssertionError("script %s did not finish in %.0fs: %r" % (name, timeout, st))
        text, _ = self.logs_since(cursor)
        return st, text

    # ---- logs -----------------------------------------------------------
    def logs_since(self, since: int) -> tuple[str, int]:
        """Return (text, next_cursor) for ``logs?since=``.

        HTTP re-frames the log page as text/plain + ``X-BugBuster-Log-Next``;
        the BLE-shaped JSON reply is accepted too so the helper keeps working if
        a build serves it directly.
        """
        r = self.get("/api/scripts/logs", params={"since": since})
        assert r.status_code == 200, "logs: HTTP %d %s" % (r.status_code, r.text[:200])
        ctype = r.headers.get("Content-Type", "")
        if "json" in ctype:
            j = r.json()
            return base64.b64decode(j.get("data", "")).decode(errors="replace"), int(j["next"])
        nxt = r.headers.get("X-BugBuster-Log-Next")
        assert nxt is not None, "logs?since= reply carries no X-BugBuster-Log-Next header: %r" % dict(r.headers)
        return r.text, int(nxt)

    def log_cursor(self) -> int:
        """Offset just past everything already in the ring (for 'only my lines')."""
        cur, text = 0, None
        for _ in range(64):
            text, nxt = self.logs_since(cur)
            if nxt == cur or not text:
                return nxt
            cur = nxt
        return cur

    @staticmethod
    def parse_lines(text: str) -> list[tuple[int, str, str, str]]:
        out = []
        for ln in text.splitlines():
            m = LOG_LINE.match(ln)
            if m:
                out.append((int(m.group(1)), m.group(2), m.group(3), m.group(4)))
        return out

    # ---- MicroPython result helper --------------------------------------
    @staticmethod
    def results(log_text: str, tag: str = "R") -> list:
        """Values a script printed as ``print("R", repr(x))`` (ast.literal_eval'd)."""
        vals = []
        for _, _, src, text in ScriptsApi.parse_lines(log_text):
            if src == "mpy" and text.startswith(tag + " "):
                vals.append(ast.literal_eval(text[len(tag) + 1:]))
        return vals

    @staticmethod
    def kv_results(log_text: str, tag: str = "R") -> dict:
        """``print("R", key, repr(value))`` lines -> {key: literal_eval(value)}.

        One short line per value: the firmware's log-line cap splits a long line,
        which breaks ``ast.literal_eval`` on a printed dict.
        """
        out = {}
        for _, _, src, text in ScriptsApi.parse_lines(log_text):
            if src == "mpy" and text.startswith(tag + " "):
                parts = text[len(tag) + 1:].split(" ", 1)
                if len(parts) == 2:
                    out[parts[0]] = ast.literal_eval(parts[1])
        return out

    def cleanup(self) -> None:
        try:
            self.ensure_idle()
        except Exception:
            pass
        for name in self.created:
            try:
                self.delete_file(name)
            except Exception:
                pass
        self.created.clear()


# ---------------------------------------------------------------------------
# Fixtures (registered for the whole tier in tests/device/conftest.py)
# ---------------------------------------------------------------------------

def _real_board_http(request, token):
    cfg = request.config
    if cfg.getoption("--sim", default=False) or cfg.getoption("--sim-full", default=False):
        pytest.skip("needs a real board: the simulator has no /api/scripts v2 surface "
                    "(state/lastExit, chunk upload, 409 busy) and no MicroPython VM")
    host = cfg.getoption("--device-http", default=None)
    if not host:
        pytest.skip("needs --device-http <ip> (the scripts API is HTTP / BLE-tunnel only)")
    if not token:
        pytest.skip("need --admin-token, or --device-usb so the token can be read from the device")
    import requests
    s = requests.Session()
    s.headers.update({"X-BugBuster-Admin-Token": token})
    base = "http://%s" % host
    try:
        r = s.get(base + "/api/scripts/status", timeout=8)
    except Exception as exc:
        pytest.skip("device not reachable at %s: %s" % (base, exc))
    if r.status_code == 404:
        pytest.skip("firmware has no /api/scripts/status (flash a build with scripting runtime v2)")
    if r.status_code in (401, 403):
        pytest.skip("admin token rejected (HTTP %d)" % r.status_code)
    return s, base


@pytest.fixture(scope="session")
def hw_scripts_session(request, _session_admin_token):
    s, base = _real_board_http(request, _session_admin_token)
    yield s, base
    s.close()


@pytest.fixture
def scripts(hw_scripts_session):
    """A ScriptsApi bound to the board, idle slot on entry, cleaned on exit."""
    s, base = hw_scripts_session
    api = ScriptsApi(s, base)
    api.ensure_idle()
    yield api
    api.cleanup()


def script_name(tag: str) -> str:
    n = "%s%s.py" % (TEST_PREFIX, tag)
    assert len(n) <= 32, n
    return n
