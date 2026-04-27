"""
Unit tests for python -m bugbuster.script CLI (V2-H).

Invokes ``bugbuster.script.main()`` directly with a mock client so no device
or network connection is required.
"""

import io
import os
import sys
import tempfile
import pytest
from unittest.mock import MagicMock, patch

from bugbuster.client import ScriptStatusResult
from bugbuster.script import main as cli_main


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _mock_client():
    """Return a MagicMock that quacks like a connected BugBuster client."""
    c = MagicMock()
    c.script_eval.return_value = ScriptStatusResult(
        is_running=True, script_id=1, total_runs=1,
        total_errors=0, last_error="",
    )
    c.script_logs.side_effect = ["[eval:1] pass\n", ""]
    c.script_status.return_value = ScriptStatusResult(
        is_running=False, script_id=1, total_runs=1,
        total_errors=0, last_error="", mode=0,
        globals_bytes_est=0, globals_count=0, auto_reset_count=0,
        last_eval_at_ms=0, idle_for_ms=0, watermark_soft_hit=False,
    )
    return c


def _run(argv, mock_client=None, capsys=None):
    """
    Call cli_main() with patched _make_client.

    Returns (exit_code, stdout_text).
    """
    if mock_client is None:
        mock_client = _mock_client()

    with patch("bugbuster.script._make_client", return_value=mock_client):
        try:
            rc = cli_main(argv)
        except SystemExit as e:
            rc = e.code if isinstance(e.code, int) else 1
    return rc, mock_client


# ---------------------------------------------------------------------------
# Help / argparse
# ---------------------------------------------------------------------------

class TestCLIHelp:
    def test_help_exits_zero(self, capsys):
        """--help prints usage and exits 0."""
        with pytest.raises(SystemExit) as exc_info:
            cli_main(["--host", "1.2.3.4", "--help"])
        assert exc_info.value.code == 0

    def test_no_subcommand_exits_nonzero(self, capsys):
        """Calling without a subcommand exits non-zero."""
        with pytest.raises(SystemExit) as exc_info:
            cli_main(["--host", "1.2.3.4"])
        assert exc_info.value.code != 0

    def test_no_connection_flag_exits_nonzero(self):
        """run without --host or --usb raises SystemExit."""
        mock_client = _mock_client()
        # _make_client is NOT patched here; it should raise SystemExit itself
        with pytest.raises(SystemExit):
            cli_main(["run", "somefile.py"])


# ---------------------------------------------------------------------------
# run subcommand
# ---------------------------------------------------------------------------

class TestCLIRun:
    def test_run_reads_file_and_calls_eval(self, tmp_path):
        """run <path> reads the file and calls script_eval."""
        script = tmp_path / "test.py"
        script.write_text("print('hello')")

        rc, mock_client = _run(["--host", "1.2.3.4", "run", str(script)])

        assert rc == 0
        mock_client.script_eval.assert_called_once_with("print('hello')", persist=False)

    def test_run_persist_flag(self, tmp_path):
        """run --persist passes persist=True to script_eval."""
        script = tmp_path / "p.py"
        script.write_text("x = 1")

        rc, mock_client = _run(["--host", "1.2.3.4", "run", str(script), "--persist"])

        assert rc == 0
        mock_client.script_eval.assert_called_once_with("x = 1", persist=True)

    def test_run_drains_logs(self, tmp_path, capsys):
        """run drains and prints logs to stdout."""
        script = tmp_path / "log.py"
        script.write_text("pass")

        mock_client = _mock_client()
        mock_client.script_logs.side_effect = ["hello from device\n", ""]

        with patch("bugbuster.script._make_client", return_value=mock_client):
            cli_main(["--host", "1.2.3.4", "run", str(script)])

        captured = capsys.readouterr()
        assert "hello from device" in captured.out


# ---------------------------------------------------------------------------
# logs subcommand
# ---------------------------------------------------------------------------

class TestCLILogs:
    def test_logs_once(self, capsys):
        """logs (without --tail) drains and prints."""
        mock_client = _mock_client()
        mock_client.script_logs.side_effect = ["line 1\n", ""]

        rc, _ = _run(["--host", "1.2.3.4", "logs"], mock_client=mock_client)

        assert rc == 0
        captured = capsys.readouterr()
        assert "line 1" in captured.out


# ---------------------------------------------------------------------------
# reset subcommand
# ---------------------------------------------------------------------------

class TestCLIReset:
    def test_reset_calls_script_reset(self, capsys):
        """reset subcommand calls client.script_reset()."""
        rc, mock_client = _run(["--host", "1.2.3.4", "reset"])

        assert rc == 0
        mock_client.script_reset.assert_called_once()


# ---------------------------------------------------------------------------
# status subcommand
# ---------------------------------------------------------------------------

class TestCLIStatus:
    def test_status_prints_fields(self, capsys):
        """status subcommand pretty-prints ScriptStatusResult fields."""
        rc, _ = _run(["--host", "1.2.3.4", "status"])

        assert rc == 0
        captured = capsys.readouterr()
        assert "is_running" in captured.out
        assert "total_runs" in captured.out
        assert "mode" in captured.out


# ---------------------------------------------------------------------------
# autorun-set / autorun-disable subcommands
# ---------------------------------------------------------------------------

class TestCLIAutorun:
    def test_autorun_set_calls_enable(self):
        """autorun-set <name> calls script_autorun_enable(name)."""
        rc, mock_client = _run(["--host", "1.2.3.4", "autorun-set", "boot.py"])

        assert rc == 0
        mock_client.script_autorun_enable.assert_called_once_with("boot.py")

    def test_autorun_disable_calls_disable(self):
        """autorun-disable calls script_autorun_disable()."""
        rc, mock_client = _run(["--host", "1.2.3.4", "autorun-disable"])

        assert rc == 0
        mock_client.script_autorun_disable.assert_called_once()
