"""
Unit tests for BBP CRC/timeout retry logic in BugBuster._usb_cmd (V2-H).

Uses the _usb_pre_send_hook injection point to simulate TimeoutError on the
first send_command call.  The second attempt should succeed transparently.
If both attempts time out, the original exception must be re-raised.
"""

import pytest
import bugbuster as bb
from tests.mock import SimulatedDevice, SimulatedUSBTransport


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _usb_client():
    device = SimulatedDevice()
    transport = SimulatedUSBTransport(device)
    client = bb.BugBuster(transport)
    client.connect()
    return client, device


# ---------------------------------------------------------------------------
# Hook injection
# ---------------------------------------------------------------------------

class TestPreSendHook:
    def test_hook_fires_once(self):
        """_usb_pre_send_hook is cleared after first invocation."""
        client, _ = _usb_client()
        fired = []
        client._usb_pre_send_hook = lambda: fired.append(1)

        # ping does NOT go through _usb_cmd in the same way, so call
        # script_status which is a simple USB read
        client.script_status()

        assert len(fired) == 1
        # Hook must be cleared after firing
        assert client._usb_pre_send_hook is None
        client.disconnect()

    def test_hook_cleared_on_success(self):
        """Hook is set to None even when the command succeeds."""
        client, _ = _usb_client()
        client._usb_pre_send_hook = lambda: None  # no-op hook
        client.script_status()
        assert client._usb_pre_send_hook is None
        client.disconnect()


# ---------------------------------------------------------------------------
# TimeoutError on first attempt — retry succeeds
# ---------------------------------------------------------------------------

class TestTimeoutRetry:
    def test_first_timeout_retried(self):
        """
        When the pre-send hook raises TimeoutError, the command is retried.
        The second attempt (no hook) should succeed and return a valid result.
        """
        client, device = _usb_client()

        def _raise_once():
            raise TimeoutError("simulated timeout")

        client._usb_pre_send_hook = _raise_once

        # Should NOT raise — retry must succeed
        result = client.script_status()
        assert result.script_id == 0  # fresh device
        client.disconnect()

    def test_retry_does_not_corrupt_state(self):
        """After a retried timeout the device state is unchanged."""
        client, device = _usb_client()
        device.script_running = True

        client._usb_pre_send_hook = lambda: (_ for _ in ()).throw(TimeoutError("t"))

        status = client.script_status()
        assert status.is_running is True
        client.disconnect()


# ---------------------------------------------------------------------------
# TimeoutError on both attempts — original error re-raised
# ---------------------------------------------------------------------------

class TestDoubleTimeout:
    def test_double_timeout_raises(self):
        """
        When both attempts time out, the original TimeoutError is re-raised.
        """
        client, _ = _usb_client()

        call_count = [0]

        original_send = client._t.send_command

        def _always_timeout(cmd_id, payload=b''):
            call_count[0] += 1
            raise TimeoutError("always times out")

        client._t.send_command = _always_timeout

        with pytest.raises(TimeoutError):
            client.script_status()

        # Must have been attempted exactly twice (original + 1 retry)
        assert call_count[0] == 2
        client.disconnect()

    def test_double_timeout_raises_original(self):
        """The re-raised exception is the original TimeoutError instance."""
        client, _ = _usb_client()

        # send_command always raises the same sentinel (simulates both
        # attempts timing out with matching message)
        sentinel = TimeoutError("original error text")

        def _always(cmd_id, payload=b''):
            raise sentinel

        client._t.send_command = _always

        with pytest.raises(TimeoutError, match="original error text"):
            client.script_status()

        client.disconnect()
