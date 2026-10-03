"""Spec 2026-10-03 §2 over HTTP: run-file returns immediately with {id, name};
a busy slot is a 409 the client turns into ScriptBusyError naming the holder;
status carries name/source/state/lastExit."""

import pytest

import bugbuster as bb
from bugbuster.client import ScriptBusyError, ScriptStatusResult
from bugbuster.transport.http import HTTPLogicalError


class _Resp:
    def __init__(self, status_code, data):
        self.status_code = status_code
        self._data = data

    def json(self):
        return self._data


class _Transport:
    def __init__(self, post_reply=None, post_error=None, get_reply=None):
        self.calls = []
        self._post_reply = post_reply or {"ok": True}
        self._post_error = post_error
        self._get_reply = get_reply or {}

    def get(self, path, params=None):
        self.calls.append(("get", path))
        return self._get_reply

    def post(self, path, body=None, headers=None):
        self.calls.append(("post", path))
        if self._post_error:
            raise self._post_error
        return self._post_reply


def _busy(code=409):
    data = {"ok": False, "error": "a script is running", "running": "loop.py", "id": 12}
    return HTTPLogicalError("HTTP 409: a script is running", response=_Resp(code, data))


def test_run_file_sends_background_and_replace_flags():
    t = _Transport(post_reply={"ok": True, "id": 5, "name": "a.py", "background": True})
    r = bb.BugBuster(t).script_run_file("a.py", background=True, replace=True)
    assert t.calls == [("post", "/scripts/run-file?name=a.py&background=1&replace=1")]
    assert r.script_id == 5 and r.is_running


def test_run_file_busy_raises_script_busy_error():
    t = _Transport(post_error=_busy())
    with pytest.raises(ScriptBusyError) as ei:
        bb.BugBuster(t).script_run_file("b.py")
    assert ei.value.running == "loop.py" and ei.value.script_id == 12
    assert isinstance(ei.value, RuntimeError)


def test_other_http_errors_pass_through():
    t = _Transport(post_error=HTTPLogicalError("HTTP 400: script not found",
                                               response=_Resp(400, {"error": "script not found"})))
    with pytest.raises(HTTPLogicalError):
        bb.BugBuster(t).script_run_file("missing.py")


def test_eval_busy_raises_script_busy_error():
    t = _Transport(post_error=_busy())
    with pytest.raises(ScriptBusyError):
        bb.BugBuster(t).script_eval("print(1)")


def test_status_parses_runtime_v2_fields():
    t = _Transport(get_reply={
        "running": True, "currentScriptId": 9, "totalRuns": 3, "totalErrors": 0, "lastError": "",
        "mode": "EPHEMERAL", "name": "loop.py", "source": "autorun", "state": "running",
        "lastExit": "none", "startedAt": 1234, "fileSlotId": 9})
    st = bb.BugBuster(t).script_status()
    assert (st.name, st.source, st.state, st.last_exit, st.started_at, st.file_slot_id) == \
        ("loop.py", "autorun", "running", "none", 1234, 9)


def test_status_defaults_keep_old_positional_equality():
    assert ScriptStatusResult(False, 3, 2, 0, "", 0, 12, 1, 0, 123, 20, False).name == ""
