"""Commands that do real device work must not use the short default timeout.

A 5 s default is right for commands the firmware answers from RAM, but
HAT_GET_STATUS is bridged to the P4 over UART and DAQ_CONFIG restarts the whole
acquisition pipeline. Timing those out is indistinguishable from a dead link
while the device is still working - observed repeatedly on the bench.
"""
from unittest.mock import MagicMock

import pytest

from bugbuster.constants import CMD_TIMEOUTS_S, CmdId
from bugbuster.transport.usb import USBTransport


def test_slow_commands_have_generous_timeouts():
    for cmd in (CmdId.HAT_GET_STATUS, CmdId.DAQ_CONFIG, CmdId.DAQ_CAL,
                CmdId.HAT_CALIBRATE_START, CmdId.SELFTEST_AUTO_CAL,
                CmdId.SCRIPT_EVAL):
        assert cmd in CMD_TIMEOUTS_S, f"{cmd.name} needs an explicit timeout"
        assert CMD_TIMEOUTS_S[cmd] > USBTransport.DEFAULT_TIMEOUT


def test_table_is_keyed_by_real_command_ids():
    valid = {int(c) for c in CmdId}
    for key in CMD_TIMEOUTS_S:
        assert int(key) in valid, f"0x{int(key):02X} is not a CmdId"


def test_hat_get_status_is_covered():
    # Every MCP tool calls this through require_hat(), so a short timeout here
    # surfaces as "the whole link is wedged".
    assert CMD_TIMEOUTS_S[CmdId.HAT_GET_STATUS] >= 10.0


def test_transport_applies_the_table_without_the_caller_asking():
    # The lookup lives in the transport, which already knows the command id.
    # Keeping it out of the call signature means no caller and no test double
    # has to change, and existing call-args assertions keep working.
    t = USBTransport("COM_TEST")
    assert t._resolve_timeout(CmdId.DAQ_CONFIG) == CMD_TIMEOUTS_S[CmdId.DAQ_CONFIG]


def test_unlisted_command_uses_the_transport_default():
    t = USBTransport("COM_TEST")
    assert t._resolve_timeout(CmdId.PING) == t._timeout


def test_explicit_argument_beats_the_table():
    t = USBTransport("COM_TEST")
    assert t._resolve_timeout(CmdId.DAQ_CONFIG, 0.25) == 0.25


def test_send_command_reports_the_timeout_it_actually_used():
    t = USBTransport("COM_TEST")

    class _S:
        is_open = True
        def write(self, data): return len(data)
        def flush(self): pass

    t._serial = _S()
    t._running = True
    t.auto_reconnect = False
    import threading
    t._reader_thread = threading.Thread(target=lambda: __import__("time").sleep(2),
                                        daemon=True)
    t._reader_thread.start()

    with pytest.raises(TimeoutError, match="within 0.05s"):
        t.send_command(CmdId.PING, timeout=0.05)
