"""Hardware tests: the on-device script runtime (spec 2026-10-03 §2).

End to end against a real board over HTTP: chunked upload, background runs,
status / logs polling, the single slot (409 busy, replace=1), eval refusal while a
file script runs, cooperative stop. Every script and file created here is named
``bbt_hw_*`` and removed on teardown; the slot is stopped on entry and exit.

    PYTHONPATH=python pytest tests/device/test_hw_scripts_runtime.py -v \
        --device-http=<ip> --device-usb=<port>

Skipped under --sim / --sim-full (no MicroPython VM, no v2 scripts surface) and
when --device-http is not given.
"""

import re
import time

import pytest

from tests.device._hw_scripts import (  # noqa: F401  (fixtures are used by name)
    EXITS, LONG_RUNNING_SRC, SOURCES, STATES, ScriptsApi, hw_scripts_session,
    script_name, scripts,
)

pytestmark = [pytest.mark.timeout(120), pytest.mark.http_only]

LEGACY_STATUS_KEYS = (
    "running", "currentScriptId", "totalRuns", "totalErrors", "lastError", "mode",
    "globalsBytes", "globalsCount", "autoResetCount", "lastEvalAtMs", "idleForMs",
    "watermarkSoftHit",
)
V2_STATUS_KEYS = (
    "name", "source", "state", "lastExit", "startedAt", "startedAtEpoch",
    "fileSlotId", "fileSlotName", "lastScriptId",
)


# ---------------------------------------------------------------------------
# status shape
# ---------------------------------------------------------------------------

def test_status_has_legacy_and_v2_fields(scripts):
    st = scripts.status()
    for k in LEGACY_STATUS_KEYS + V2_STATUS_KEYS:
        assert k in st, "status missing %r: %r" % (k, st)
    assert st["state"] in STATES, st
    assert st["source"] in SOURCES, st
    assert st["running"] is False and st["state"] != "running", "slot not idle at start: %r" % st


# ---------------------------------------------------------------------------
# chunked upload
# ---------------------------------------------------------------------------

def test_chunked_upload_roundtrip_and_temp_file_hidden(scripts):
    name = script_name("chunks")
    body = ("# chunk test\n" + "".join("x%03d = %d\n" % (i, i) for i in range(300))).encode()
    assert len(body) > 2000
    scripts.created.append(name)

    mid = len(body) // 2
    r = scripts.chunk(name, 0, body[:mid])
    assert r.status_code == 200 and r.json() == {"ok": True, "received": mid, "final": False}, r.text
    assert name not in scripts.list_files(), "temp upload file leaked into the file list before final"

    r = scripts.chunk(name, mid, body[mid:], final=True)
    assert r.status_code == 200, r.text
    j = r.json()
    assert j["ok"] is True and j["final"] is True and j["received"] == len(body), j

    assert name in scripts.list_files()
    assert scripts.get_file(name) == body, "downloaded bytes differ from the uploaded bytes"

    assert scripts.delete_file(name).status_code == 200
    assert name not in scripts.list_files()


def test_chunk_offset_zero_restarts_the_upload(scripts):
    name = script_name("restart")
    scripts.created.append(name)
    assert scripts.chunk(name, 0, b"print('stale-1')\n" * 4).json()["ok"] is True
    # an interrupted upload is retried from the top: off=0 must discard the partial
    fresh = b"print('fresh')\n"
    assert scripts.chunk(name, 0, fresh[:5]).json()["ok"] is True
    r = scripts.chunk(name, 5, fresh[5:], final=True)
    assert r.status_code == 200 and r.json()["received"] == len(fresh), r.text
    assert scripts.get_file(name) == fresh


def test_chunk_wrong_offset_is_rejected_naming_the_expected_offset(scripts):
    name = script_name("badoff")
    scripts.created.append(name)
    assert scripts.chunk(name, 0, b"a" * 100).json()["ok"] is True

    r = scripts.chunk(name, 7, b"b" * 10)          # expected 100
    assert r.status_code == 400, "wrong offset must be HTTP 400, got %d: %s" % (r.status_code, r.text)
    j = r.json()
    assert j["ok"] is False and "100" in j["error"], "error must name the expected offset 100: %r" % j

    # the upload is still resumable at the right offset
    r = scripts.chunk(name, 100, b"c" * 5, final=True)
    assert r.status_code == 200 and r.json()["received"] == 105, r.text
    assert scripts.get_file(name) == b"a" * 100 + b"c" * 5


def test_chunk_oversize_upload_is_rejected_and_leaves_nothing(scripts):
    name = script_name("big")
    scripts.created.append(name)
    piece = b"#" * 3072                              # 4096 b64 chars, the per-request cap
    off, rejected = 0, None
    for _ in range(14):                              # 14 * 3072 = 43 008 > SCRIPT_BODY_MAX 32 768
        r = scripts.chunk(name, off, piece, final=False)
        if r.status_code != 200:
            rejected = r
            break
        off += len(piece)
    assert rejected is not None, "uploaded %d bytes without the 32 KB cap rejecting" % off
    assert rejected.status_code == 400 and rejected.json()["ok"] is False, rejected.text
    assert off <= 32768
    assert name not in scripts.list_files(), "oversize upload left a file behind"
    assert scripts.get_file(name) is None
    # the failed upload deleted its temp file: a fresh upload under the same name works
    scripts.upload(name, "print('after-oversize')\n")
    assert scripts.get_file(name) == b"print('after-oversize')\n"


@pytest.mark.parametrize("bad", [
    "../evil.py", "noext", ".hidden.py", "a" * 31 + ".py", "sp ace.py", "x.txt",
])
def test_chunk_bad_names_are_rejected(scripts, bad):
    r = scripts.chunk(bad, 0, b"print(1)\n", final=True)
    assert r.status_code == 400 and r.json()["ok"] is False, "name %r accepted: %s" % (bad, r.text)
    assert bad not in scripts.list_files()


# ---------------------------------------------------------------------------
# named / sourced background runs, status fields, structured logs
# ---------------------------------------------------------------------------

def test_background_run_reports_name_source_state_and_exit(scripts):
    name = script_name("bg")
    scripts.upload(name, "import time\nprint('bg-hello')\ntime.sleep_ms(1500)\nprint('bg-bye')\n")
    cursor = scripts.log_cursor()
    before = scripts.status()["totalRuns"]
    t0 = time.time()

    r = scripts.run_file(name, background=True)
    assert r.status_code == 200, r.text
    j = r.json()
    assert j["ok"] is True and j["name"] == name and j["background"] is True and isinstance(j["id"], int), j
    assert time.time() - t0 < 3.0, "background=1 must return immediately"

    st = scripts.wait_state("running", timeout=5.0)
    assert st["name"] == name and st["source"] == "manual" and st["running"] is True, st
    assert st["fileSlotName"] == name and st["fileSlotId"] == j["id"], st
    assert isinstance(st["startedAt"], int) and st["startedAt"] > 0, st
    assert isinstance(st["startedAtEpoch"], int), st

    st = scripts.wait_state({"done", "idle"}, timeout=15.0)
    assert st["lastExit"] == "ok" and st["running"] is False, st
    assert st["totalRuns"] == before + 1, st
    assert st["name"] == name, "name must stay readable after the run: %r" % st

    text, nxt = scripts.logs_since(cursor)
    lines = scripts.parse_lines(text)
    mine = [ln for ln in lines if ln[3] in ("bg-hello", "bg-bye")]
    assert [ln[3] for ln in mine] == ["bg-hello", "bg-bye"], "script output missing/out of order: %r" % text
    for ts, level, src, _ in mine:
        assert level == "I" and src == "mpy", "print() must log as 'I mpy', got %r" % ((ts, level, src),)
    assert mine[0][0] <= mine[1][0], "timestamps must not go backwards"
    assert nxt > cursor


def test_every_ring_line_is_structured(scripts):
    name = script_name("fmt")
    st, text = scripts.run_source(name, "print('one')\nprint('two')\n")
    assert st["lastExit"] == "ok", st
    assert text.endswith("\n"), "ring page must end on a line boundary"
    for ln in text.splitlines():
        assert re.match(r"^\d+ [EWID] (mpy|sys) ", ln), "unstructured ring line: %r" % ln


def test_partial_last_line_is_flushed_when_the_script_ends(scripts):
    st, text = scripts.run_source(script_name("tail"), "print('no-newline-at-end', end='')\n")
    assert st["lastExit"] == "ok", st
    texts = [ln[3] for ln in scripts.parse_lines(text)]
    assert "no-newline-at-end" in texts, "partial line lost: %r" % text


def test_error_script_logs_a_traceback_at_level_E_and_sets_last_exit_error(scripts):
    before = scripts.status()
    st, text = scripts.run_source(script_name("err"), "print('pre')\nx = 1 // 0\n")
    assert st["state"] in ("error", "done", "idle") and st["lastExit"] == "error", st
    assert st["totalErrors"] == before["totalErrors"] + 1, (before["totalErrors"], st["totalErrors"])
    lvl = {ln[3]: ln[1] for ln in scripts.parse_lines(text)}
    assert lvl.get("pre") == "I"
    errs = [ln for ln in scripts.parse_lines(text) if ln[1] == "E"]
    assert errs and any("ZeroDivisionError" in ln[3] for ln in errs), "no E-level traceback in %r" % text


def test_logs_since_is_non_draining(scripts):
    cursor = scripts.log_cursor()
    st, _ = scripts.run_source(script_name("ndr"), "print('keep-me')\n")
    assert st["lastExit"] == "ok"
    a, na = scripts.logs_since(cursor)
    b, nb = scripts.logs_since(cursor)
    assert "keep-me" in a and a == b and na == nb, "reading with since= must not consume the ring"
    again, nc = scripts.logs_since(na)
    assert again == "" and nc == na, "reading at the returned cursor must be empty and stable"


def test_run_file_missing_script_is_400(scripts):
    r = scripts.run_file(script_name("nosuch"))
    assert r.status_code == 400, r.text
    j = r.json()
    assert j["ok"] is False and "not found" in j["error"], j


# ---------------------------------------------------------------------------
# the single slot: busy 409, replace=1, eval refused, stop
# ---------------------------------------------------------------------------

def test_second_run_is_409_busy_naming_the_holder(scripts):
    a, b = script_name("holdA"), script_name("holdB")
    scripts.upload(a, LONG_RUNNING_SRC)
    scripts.upload(b, "print('B-must-not-run')\n")
    ra = scripts.run_file(a)
    assert ra.status_code == 200, ra.text
    scripts.wait_state("running", timeout=5.0)

    rb = scripts.run_file(b)
    assert rb.status_code == 409, "second run must be 409, got %d: %s" % (rb.status_code, rb.text)
    j = rb.json()
    assert j["ok"] is False and j["running"] == a and j["id"] == ra.json()["id"], j

    st = scripts.status()
    assert st["state"] == "running" and st["name"] == a, "the holder must be untouched by the refusal: %r" % st


def test_replace_stops_the_holder_and_runs_the_new_script(scripts):
    a, b = script_name("replA"), script_name("replB")
    scripts.upload(a, LONG_RUNNING_SRC)
    scripts.upload(b, "print('B-ran')\n")
    scripts.run_file(a)
    scripts.wait_state("running", timeout=5.0)
    cursor = scripts.log_cursor()

    t0 = time.time()
    rb = scripts.run_file(b, replace=True)
    took = time.time() - t0
    assert rb.status_code == 200, "replace=1 must succeed, got %d: %s" % (rb.status_code, rb.text)
    assert rb.json()["name"] == b
    assert took < 6.0, "replace must answer within the ~3 s stop timeout, took %.1fs" % took

    st = scripts.wait_state({"done", "idle"}, timeout=20.0)
    assert st["name"] == b and st["lastExit"] == "ok", st
    text, _ = scripts.logs_since(cursor)
    assert "B-ran" in text, "replacement script did not run: %r" % text
    assert "long-finished-unexpectedly" not in text, "the replaced holder ran to completion"


@pytest.mark.slow
def test_replace_of_a_holder_blocked_in_one_long_call_still_answers_and_recovers(scripts):
    """Review-focus 1: the holder cannot see the stop request for a while.

    A single long ``time.sleep`` keeps the VM in one C call. ``replace=1`` must
    still answer in ~3 s (never hang, never kill the VM task), the board stays
    responsive, and the new script runs once the holder lets go.
    """
    a, b = script_name("stuckA"), script_name("stuckB")
    scripts.upload(a, "import time\nprint('stuck-start')\ntime.sleep(12)\nprint('stuck-end')\n")
    scripts.upload(b, "print('B-after-stuck')\n")
    scripts.run_file(a)
    scripts.wait_state("running", timeout=5.0)
    cursor = scripts.log_cursor()

    t0 = time.time()
    rb = scripts.run_file(b, replace=True)
    assert rb.status_code == 200, rb.text
    assert time.time() - t0 < 6.0, "replace blocked behind the stuck holder for %.1fs" % (time.time() - t0)
    assert scripts.status()["state"] in ("stopping", "running", "done", "idle"), "status must stay answerable"

    scripts.wait_state({"done", "idle"}, timeout=40.0)
    text, _ = scripts.logs_since(cursor)
    assert "B-after-stuck" in text, "replacement never ran after the holder returned: %r" % text


def test_eval_is_refused_with_409_while_a_file_script_runs(scripts):
    a = script_name("evalhold")
    scripts.upload(a, LONG_RUNNING_SRC)
    ra = scripts.run_file(a)
    scripts.wait_state("running", timeout=5.0)

    r = scripts.eval("print('eval-must-not-run')\n")
    assert r.status_code == 409, "eval during a file script must be 409, got %d: %s" % (r.status_code, r.text)
    j = r.json()
    assert j["running"] == a and j["id"] == ra.json()["id"], j

    scripts.stop()
    scripts.wait_not_running(timeout=20.0)
    cursor = scripts.log_cursor()
    r = scripts.eval("print('eval-ok')\n")
    assert r.status_code == 200 and r.json()["ok"] is True, "eval must work again once the slot is free: %s" % r.text
    scripts.wait_not_running(timeout=10.0)
    time.sleep(0.5)
    text, _ = scripts.logs_since(cursor)
    assert "eval-ok" in text


def test_stop_is_classified_stopped_not_error(scripts):
    a = script_name("stopme")
    scripts.upload(a, LONG_RUNNING_SRC)
    errs_before = scripts.status()["totalErrors"]
    scripts.run_file(a)
    scripts.wait_state("running", timeout=5.0)

    r = scripts.stop()
    assert r.status_code == 200, r.text
    st = scripts.wait_not_running(timeout=20.0)
    assert st["lastExit"] == "stopped", "stop must report lastExit=stopped: %r" % st
    assert st["totalErrors"] == errs_before, "a cooperative stop must not count as an error"
    assert st["running"] is False
    # the slot is free again
    b = script_name("afterstop")
    st2, text = scripts.run_source(b, "print('slot-free')\n")
    assert st2["lastExit"] == "ok" and "slot-free" in text


def test_background_flag_is_echoed_either_way(scripts):
    name = script_name("fg")
    st, _ = scripts.run_source(name, "print('fg')\n")
    assert st["lastExit"] == "ok"
    r = scripts.run_file(name, background=False)
    assert r.status_code == 200 and r.json()["background"] is False, r.text
    scripts.wait_not_running(timeout=10.0)


# ---------------------------------------------------------------------------
# autorun (read-only: changing autorun needs a reboot to observe, see the
# coverage table gap)
# ---------------------------------------------------------------------------

def test_autorun_status_has_snake_and_camel_fields(scripts):
    r = scripts.get("/api/scripts/autorun/status")
    assert r.status_code == 200, r.text
    j = r.json()
    for k in ("enabled", "has_script", "io12_high", "last_run_ok", "last_run_id",
              "scriptName", "ranThisBoot", "running"):
        assert k in j, "autorun/status missing %r: %r" % (k, j)
    assert isinstance(j["ranThisBoot"], bool) and isinstance(j["running"], bool), j
    if j["enabled"] and j["has_script"]:
        assert j["scriptName"], "autorun is configured but scriptName is empty: %r" % j
