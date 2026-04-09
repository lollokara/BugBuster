"""
Unit tests for the HAT-presence guard on Logic Analyzer / SWD entry points.

Runs WITHOUT hardware — the BugBuster client is constructed with a mocked
USB transport so we can drive `hat_get_status()` to return either a
detected or non-detected HAT, then exercise the guarded entry points.

The contract under test (per project state in `.mex/ROUTER.md`):
    "Logic analyzer and SWD are HAT-only — calls without HAT must fail
    clearly."
"""

import struct
import unittest
from unittest.mock import MagicMock

from bugbuster import BugBuster, HatNotPresentError, HatPinFunctionError
from bugbuster.transport.usb import USBTransport


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _hat_status_payload(*, detected: bool) -> bytes:
    """
    Build the raw bytes that `hat_get_status()` expects from the firmware.

    Layout (USB path in client.py):
        u8 detected, u8 connected, u8 type, f32 detect_voltage,
        u8 fw_major, u8 fw_minor, u8 config_confirmed, 4 x u8 pins
    """
    return (
        bytes([1 if detected else 0])         # detected
        + bytes([1 if detected else 0])       # connected
        + bytes([0])                          # hat_type
        + struct.pack('<f', 3.3 if detected else 0.0)  # detect_voltage
        + bytes([1, 0])                       # fw_major, fw_minor
        + bytes([1 if detected else 0])       # config_confirmed
        + bytes([0, 0, 0, 0])                 # pin_config (4 pins)
    )


def _make_client(*, hat_detected: bool) -> tuple[BugBuster, MagicMock]:
    """
    Build a BugBuster client whose USB transport is a MagicMock.

    `send_command` returns a HAT status payload reflecting `hat_detected`
    on `HAT_GET_STATUS`, and an empty bytes object for everything else
    (which is fine because the guard short-circuits before any other
    command is dispatched in the negative case).
    """
    mock_transport = MagicMock(spec=USBTransport)

    def send_command(cmd_id, payload=b''):
        # 0x70 = HAT_GET_STATUS in CmdId. Use the imported enum to be safe.
        from bugbuster.constants import CmdId
        if cmd_id == CmdId.HAT_GET_STATUS:
            return _hat_status_payload(detected=hat_detected)
        return b''

    mock_transport.send_command.side_effect = send_command

    client = BugBuster(mock_transport)
    return client, mock_transport


# ---------------------------------------------------------------------------
# Guard behavior — HAT NOT present
# ---------------------------------------------------------------------------

class TestHatGuardRaisesWhenAbsent(unittest.TestCase):
    """When no HAT is detected, every LA + SWD entry point must raise."""

    def setUp(self):
        self.client, self.transport = _make_client(hat_detected=False)

    def _assert_raises_clearly(self, fn, *args, **kwargs):
        with self.assertRaises(HatNotPresentError) as ctx:
            fn(*args, **kwargs)
        msg = str(ctx.exception)
        # Error message must mention the HAT and how to recover.
        self.assertIn("HAT", msg)
        self.assertIn("hat_detect", msg)

    def test_swd_setup_raises(self):
        self._assert_raises_clearly(self.client.hat_setup_swd, 3300, 0)

    def test_la_configure_raises(self):
        self._assert_raises_clearly(self.client.hat_la_configure, 4, 1_000_000, 1000)

    def test_la_set_trigger_raises(self):
        self._assert_raises_clearly(self.client.hat_la_set_trigger, 1, 0)

    def test_la_arm_raises(self):
        self._assert_raises_clearly(self.client.hat_la_arm)

    def test_la_force_raises(self):
        self._assert_raises_clearly(self.client.hat_la_force)

    def test_la_stop_raises(self):
        self._assert_raises_clearly(self.client.hat_la_stop)

    def test_la_get_status_raises(self):
        self._assert_raises_clearly(self.client.hat_la_get_status)

    def test_la_read_all_raises(self):
        self._assert_raises_clearly(self.client.hat_la_read_all)

    def test_no_la_or_swd_command_dispatched_when_hat_absent(self):
        """The guard must short-circuit before any LA / SWD command hits the bus."""
        from bugbuster.constants import CmdId

        guarded_cmds = {
            CmdId.HAT_SETUP_SWD,
            CmdId.HAT_LA_CONFIG,
            CmdId.HAT_LA_TRIGGER,
            CmdId.HAT_LA_ARM,
            CmdId.HAT_LA_FORCE,
            CmdId.HAT_LA_STOP,
            CmdId.HAT_LA_STATUS,
            CmdId.HAT_LA_READ,
        }

        for fn, args in [
            (self.client.hat_setup_swd, (3300, 0)),
            (self.client.hat_la_configure, (4, 1_000_000, 1000)),
            (self.client.hat_la_set_trigger, (1, 0)),
            (self.client.hat_la_arm, ()),
            (self.client.hat_la_force, ()),
            (self.client.hat_la_stop, ()),
            (self.client.hat_la_get_status, ()),
            (self.client.hat_la_read_all, ()),
        ]:
            with self.assertRaises(HatNotPresentError):
                fn(*args)

        dispatched = [c.args[0] for c in self.transport.send_command.call_args_list]
        for cmd in dispatched:
            self.assertNotIn(
                cmd, guarded_cmds,
                f"Guard failed: {cmd!r} was dispatched even though HAT is absent",
            )


# ---------------------------------------------------------------------------
# Guard behavior — HAT present
# ---------------------------------------------------------------------------

class TestHatGuardAllowsWhenPresent(unittest.TestCase):
    """When the HAT is detected, the guard must be transparent."""

    def setUp(self):
        self.client, self.transport = _make_client(hat_detected=True)

    def test_swd_setup_dispatches(self):
        from bugbuster.constants import CmdId
        self.client.hat_setup_swd(3300, 0)
        cmds = [c.args[0] for c in self.transport.send_command.call_args_list]
        self.assertIn(CmdId.HAT_SETUP_SWD, cmds)

    def test_la_configure_dispatches(self):
        from bugbuster.constants import CmdId
        self.client.hat_la_configure(4, 1_000_000, 1000)
        cmds = [c.args[0] for c in self.transport.send_command.call_args_list]
        self.assertIn(CmdId.HAT_LA_CONFIG, cmds)


# ---------------------------------------------------------------------------
# Cache invalidation — hat_detect() must reset the cached presence
# ---------------------------------------------------------------------------

class TestHatGuardCache(unittest.TestCase):
    """The cache should make repeat checks free, and hat_detect() should
    reset it so a newly attached HAT is picked up."""

    def test_cache_avoids_repeat_status_query(self):
        from bugbuster.constants import CmdId
        client, transport = _make_client(hat_detected=True)

        client.hat_la_arm()
        client.hat_la_arm()
        client.hat_la_arm()

        status_calls = [
            c for c in transport.send_command.call_args_list
            if c.args[0] == CmdId.HAT_GET_STATUS
        ]
        self.assertEqual(
            len(status_calls), 1,
            "Guard should cache HAT presence after the first probe",
        )

    def test_hat_detect_resets_cache_so_new_state_takes_effect(self):
        # Start with HAT absent — guard caches False, calls raise.
        client, transport = _make_client(hat_detected=False)
        with self.assertRaises(HatNotPresentError):
            client.hat_la_arm()

        # Now simulate the user plugging in the HAT and re-detecting.
        # We swap the side_effect to return a "present" payload, then
        # call hat_detect() which itself returns the present result and
        # must reset the cache via that return value.
        from bugbuster.constants import CmdId

        def send_command_present(cmd_id, payload=b''):
            if cmd_id == CmdId.HAT_GET_STATUS:
                return _hat_status_payload(detected=True)
            if cmd_id == CmdId.HAT_DETECT:
                # Layout for hat_detect: detected,u8 type, f32 v, u8 connected
                return bytes([1, 0]) + struct.pack('<f', 3.3) + bytes([1])
            return b''

        transport.send_command.side_effect = send_command_present

        result = client.hat_detect()
        self.assertTrue(result["detected"])

        # Now the guard should let the call through.
        try:
            client.hat_la_arm()
        except HatNotPresentError as exc:
            self.fail(f"Guard did not refresh after hat_detect(): {exc}")


# ---------------------------------------------------------------------------
# hat_set_pin — reserved SWD/TRACE slots must raise HatPinFunctionError
# ---------------------------------------------------------------------------

class TestHatSetPinRejectsReservedFunctionCodes(unittest.TestCase):
    """
    Numeric slots 1..4 (formerly SWDIO/SWCLK/TRACE1/TRACE2) are reserved
    for wire-protocol compatibility. After the 2026-04-09 cleanup, SWD
    lives on a dedicated 3-pin connector and is enabled via
    hat_setup_swd() — hat_set_pin() must refuse the old function codes.
    """

    def setUp(self):
        self.client, self.transport = _make_client(hat_detected=True)

    def _assert_reserved_rejected(self, func_code: int):
        with self.assertRaises(HatPinFunctionError) as ctx:
            self.client.hat_set_pin(0, func_code)
        msg = str(ctx.exception)
        # Error message must be actionable.
        self.assertIn("reserved", msg.lower())
        self.assertIn("hat_setup_swd", msg)

    def test_rejects_slot_1_swdio(self):
        self._assert_reserved_rejected(1)

    def test_rejects_slot_2_swclk(self):
        self._assert_reserved_rejected(2)

    def test_rejects_slot_3_trace1(self):
        self._assert_reserved_rejected(3)

    def test_rejects_slot_4_trace2(self):
        self._assert_reserved_rejected(4)

    def test_accepts_disconnected_and_gpio(self):
        """Slots 0 and 5..8 must still work — only 1..4 are reserved."""
        for func_code in (0, 5, 6, 7, 8):
            try:
                self.client.hat_set_pin(0, func_code)
            except HatPinFunctionError as exc:
                self.fail(
                    f"hat_set_pin(0, {func_code}) raised HatPinFunctionError "
                    f"but slot {func_code} is NOT reserved: {exc}"
                )

    def test_reserved_rejected_before_transport_call(self):
        """
        Guard must short-circuit before any USB command is dispatched.
        """
        from bugbuster.constants import CmdId

        with self.assertRaises(HatPinFunctionError):
            self.client.hat_set_pin(0, 1)  # formerly SWDIO

        dispatched = [c.args[0] for c in self.transport.send_command.call_args_list]
        self.assertNotIn(
            CmdId.HAT_SET_PIN, dispatched,
            "Guard failed: HAT_SET_PIN was dispatched even though the "
            "function code was reserved",
        )


# ---------------------------------------------------------------------------
# hat_setup_swd — must not call hat_set_pin (EXP_EXT is no longer touched)
# ---------------------------------------------------------------------------

class TestHatSetupSwdDoesNotTouchExpExt(unittest.TestCase):
    """
    After the 2026-04-09 cleanup, hat_setup_swd() only configures target
    voltage and power — it does NOT assign any EXP_EXT pin. This test
    confirms the new behavior at the wire level without any hardware.
    """

    def test_hat_setup_swd_dispatches_no_set_pin_commands(self):
        from bugbuster.constants import CmdId

        client, transport = _make_client(hat_detected=True)
        client.hat_setup_swd(target_voltage_mv=3300, connector=0)

        dispatched = [c.args[0] for c in transport.send_command.call_args_list]
        self.assertIn(
            CmdId.HAT_SETUP_SWD, dispatched,
            "hat_setup_swd() should dispatch HAT_SETUP_SWD",
        )
        self.assertNotIn(
            CmdId.HAT_SET_PIN, dispatched,
            "hat_setup_swd() must NOT dispatch HAT_SET_PIN — SWD no longer "
            "routes through EXP_EXT pins (dedicated 3-pin connector).",
        )


if __name__ == "__main__":
    unittest.main()
