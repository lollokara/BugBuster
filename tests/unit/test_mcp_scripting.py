"""MCP-5 / PLT-08: run_device_script can wait for the script to finish and
return all of its logs; the stored-script API (list/get/put/delete/autorun) is
reachable from the MCP.

A: run_device_script stopped draining at the first empty log read, so output
printed after the first poll was lost, and none of the script file tools existed."""

import unittest
from types import SimpleNamespace
from unittest.mock import patch


from bugbuster_mcp import session
from bugbuster_mcp.tools.scripting import register
from tests.unit._mock_client import make_client_mock


class DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(func):
            self.tools[func.__name__] = func
            return func
        return decorator


def _status(running, sid=7, errors=0, last_error=""):
    return SimpleNamespace(is_running=running, script_id=sid, total_runs=1, total_errors=errors,
                           last_error=last_error, mode=0, globals_bytes_est=0, globals_count=0,
                           auto_reset_count=0, last_eval_at_ms=0, idle_for_ms=0,
                           watermark_soft_hit=False)


class TestScriptingTools(unittest.TestCase):
    def setUp(self):
        session.configure(transport="usb", port="/dev/null", vlogic=3.3)
        self.mcp = DummyMCP()
        register(self.mcp)
        self.bb = make_client_mock()
        p = patch("bugbuster_mcp.session.get_client", return_value=self.bb)
        p.start()
        self.addCleanup(p.stop)
        s = patch("bugbuster_mcp.tools.scripting.time.sleep", lambda _s: None)
        s.start()
        self.addCleanup(s.stop)

    def test_wait_collects_logs_until_the_script_ends(self):
        self.bb.script_eval.return_value = _status(True)
        # running, running, done; logs arrive while running and after the first empty read
        self.bb.script_status.side_effect = [_status(False), _status(True), _status(True),
                                             _status(False), _status(False)]
        logs = iter(["a\n", "", "b\n", "c\n", ""])
        self.bb.script_logs.side_effect = lambda: next(logs, "")
        r = self.mcp.tools["run_device_script"](src="print(1)", wait=True, timeout_s=5)
        self.assertTrue(r["success"])
        self.assertEqual(r["logs"], "a\nb\nc\n")
        self.assertFalse(r["timed_out"])

    def test_wait_waits_for_a_queued_script_to_start(self):
        # Bench: right after eval the engine still reports the previous script
        # (idle), so treating "not running" as "finished" lost every log line.
        self.bb.script_eval.return_value = _status(False, sid=8)
        self.bb.script_status.side_effect = [_status(False, sid=7), _status(False, sid=7),
                             _status(True, sid=8), _status(False, sid=8),
                             _status(False, sid=8)]
        logs = iter(["", "", "x\n", ""])
        self.bb.script_logs.side_effect = lambda: next(logs, "")
        r = self.mcp.tools["run_device_script"](src="print(1)", wait=True, timeout_s=5)
        self.assertEqual(r["logs"], "x\n")

    def test_wait_times_out(self):
        self.bb.script_eval.return_value = _status(True)
        self.bb.script_status.return_value = _status(True)
        self.bb.script_logs.return_value = ""
        r = self.mcp.tools["run_device_script"](src="while True: pass", wait=True, timeout_s=0)
        self.assertTrue(r["timed_out"])

    def test_wait_recognizes_script_completed_before_first_status(self):
        self.bb.script_eval.return_value = _status(True, sid=8)
        before = _status(False, sid=0)
        before.total_runs = 1
        after = _status(False, sid=0)
        after.total_runs = 2
        self.bb.script_status.side_effect = [before, after, after]
        self.bb.script_logs.side_effect = ["pulse complete\n", "", ""]
        result = self.mcp.tools["run_device_script"](src="print(1)", wait=True, timeout_s=5)
        self.assertFalse(result["timed_out"])
        self.assertEqual(result["logs"], "pulse complete\n")

    def test_script_file_tools(self):
        t = self.mcp.tools
        self.bb.script_list.return_value = ["a.py"]
        self.bb.script_get.return_value = "print(1)"
        self.assertEqual(t["script_list"]()["scripts"], ["a.py"])
        self.assertEqual(t["script_get"](name="a.py")["src"], "print(1)")
        t["script_put"](name="b.py", src="x=1")
        self.bb.script_upload.assert_called_once_with("b.py", "x=1")
        t["script_delete"](name="b.py")
        self.bb.script_delete.assert_called_once_with("b.py")
        self.bb.script_autorun_status.return_value = SimpleNamespace(enabled=True, name="a.py")
        self.assertTrue(t["script_autorun"](action="status")["success"])
        self.bb.script_autorun_status.return_value = SimpleNamespace(
            _asdict=lambda: {"enabled": True})
        result = t["script_autorun"](action="status")
        self.assertTrue(result["autorun"]["enabled"])
        self.assertIn("is_running", result["engine"])
        t["script_autorun"](action="enable", name="a.py")
        self.bb.script_autorun_enable.assert_called_once_with("a.py")
        with self.assertRaises(ValueError):
            t["script_autorun"](action="bogus")

    def test_script_run_file_reports_the_busy_holder(self):
        from bugbuster.client import ScriptBusyError
        self.bb.script_run_file.side_effect = ScriptBusyError("loop.py", 12)
        r = self.mcp.tools["script_run_file"](name="b.py")
        self.assertFalse(r["success"])
        self.assertEqual((r["running"], r["id"]), ("loop.py", 12))

    def test_script_run_file_replace_starts_in_background(self):
        self.bb.script_run_file.return_value = _status(True, sid=13)
        r = self.mcp.tools["script_run_file"](name="b.py", replace=True)
        self.bb.script_run_file.assert_called_once_with("b.py", background=True, replace=True)
        self.assertEqual(r, {"success": True, "id": 13, "name": "b.py"})
