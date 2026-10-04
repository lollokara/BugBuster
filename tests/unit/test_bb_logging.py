"""Unit tests for bb_logging module (frozen MicroPython helper).

Guards that bb_logging info/warn/error/debug map to level characters
'I', 'W', 'E', 'D', format timestamps with bugbuster.ticks_ms(), and
fall back to print() when bugbuster.log is not present.
"""

import types
from unittest.mock import MagicMock, patch

from tests.lib.srcread import REPO_ROOT


def _load_bb_logging(mock_bugbuster):
    """Execute bb_logging.py with bugbuster injected into sys.modules during execution."""
    source = (REPO_ROOT / "python" / "firmware_modules" / "bb_logging.py").read_text(encoding="utf-8")
    mod = types.ModuleType("bb_logging")
    with patch.dict("sys.modules", {"bugbuster": mock_bugbuster}):
        exec(compile(source, "bb_logging.py", "exec"), mod.__dict__)
    return mod


def test_bb_logging_maps_levels_to_bugbuster_log():
    mock_bb = MagicMock()
    mock_bb.ticks_ms.return_value = 12345
    bbl = _load_bb_logging(mock_bb)

    bbl.info("info message")
    mock_bb.log.assert_called_with("I", "[     12345] INFO  info message")

    bbl.warn("warn message")
    mock_bb.log.assert_called_with("W", "[     12345] WARN  warn message")

    bbl.error("error message")
    mock_bb.log.assert_called_with("E", "[     12345] ERROR error message")

    bbl.debug("debug message")
    mock_bb.log.assert_called_with("D", "[     12345] DEBUG debug message")


def test_bb_logging_fallback_timestamp_when_ticks_ms_missing():
    mock_bb = MagicMock(spec=["log"])  # no ticks_ms
    bbl = _load_bb_logging(mock_bb)

    bbl.info("test without ticks")
    mock_bb.log.assert_called_with("I", "[----------] INFO  test without ticks")


def test_bb_logging_fallback_to_print_when_log_missing(capsys):
    mock_bb = MagicMock(spec=["ticks_ms"])  # no log
    mock_bb.ticks_ms.return_value = 999
    bbl = _load_bb_logging(mock_bb)

    bbl.warn("fallback warning")
    captured = capsys.readouterr()
    assert captured.out.strip() == "[       999] WARN  fallback warning"
