"""
Unit tests for MCP IO ownership tools (io_claim, io_release,
io_owner_status, io_force_release) and the _verify_lease_covers helper.
"""

import unittest
from unittest.mock import MagicMock, patch

from bugbuster_mcp import session
from bugbuster_mcp.tools.io_owner import (
    register, _active_leases, _leases_lock, _verify_lease_covers,
)


class DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(func):
            self.tools[func.__name__] = func
            return func
        return decorator


def _clear_leases():
    """Clear the server-side lease dict between tests."""
    with _leases_lock:
        _active_leases.clear()


class TestIoClaim(unittest.TestCase):
    def setUp(self):
        session.configure(transport="usb", port="/dev/null", vlogic=3.3)
        self.mcp = DummyMCP()
        register(self.mcp)
        _clear_leases()

    def _fake_bb(self):
        bb = MagicMock()
        bb.io_claim.return_value = None
        bb.io_release.return_value = None
        bb.io_owner_status.return_value = [
            {"slot": i, "owner_kind": "NONE", "session_id": 0,
             "lease_until_ms": 0, "purpose_tag": 0}
            for i in range(16)
        ]
        return bb

    def test_io_claim_returns_handle(self):
        fake_bb = self._fake_bb()
        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            handle = self.mcp.tools["io_claim"]([0, 1], lease_seconds=10.0)
        self.assertIsInstance(handle, str)
        self.assertEqual(len(handle), 32)  # uuid4 hex
        fake_bb.io_claim.assert_called_once_with([0, 1], 10.0, "")

    def test_io_claim_stores_slots_in_lease_dict(self):
        fake_bb = self._fake_bb()
        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            handle = self.mcp.tools["io_claim"]([5, 6])
        with _leases_lock:
            self.assertIn(handle, _active_leases)
            self.assertEqual(_active_leases[handle], [5, 6])

    def test_io_claim_rejects_empty_slots(self):
        with self.assertRaises(ValueError):
            self.mcp.tools["io_claim"]([])

    def test_io_claim_rejects_invalid_slot(self):
        fake_bb = self._fake_bb()
        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            with self.assertRaises(ValueError):
                self.mcp.tools["io_claim"]([16])  # out of range

    def test_io_claim_rejects_negative_slot(self):
        fake_bb = self._fake_bb()
        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            with self.assertRaises(ValueError):
                self.mcp.tools["io_claim"]([-1])

    def test_io_claim_with_purpose(self):
        fake_bb = self._fake_bb()
        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            self.mcp.tools["io_claim"]([0], purpose="voltage sweep")
        fake_bb.io_claim.assert_called_once_with([0], 30.0, "voltage sweep")


class TestIoRelease(unittest.TestCase):
    def setUp(self):
        session.configure(transport="usb", port="/dev/null", vlogic=3.3)
        self.mcp = DummyMCP()
        register(self.mcp)
        _clear_leases()

    def test_io_release_removes_handle(self):
        fake_bb = MagicMock()
        fake_bb.io_claim.return_value = None
        fake_bb.io_release.return_value = None

        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            handle = self.mcp.tools["io_claim"]([2, 3])
            result = self.mcp.tools["io_release"](handle)

        self.assertTrue(result["success"])
        self.assertEqual(result["slots_released"], [2, 3])
        with _leases_lock:
            self.assertNotIn(handle, _active_leases)
        fake_bb.io_release.assert_called_once_with([2, 3])

    def test_io_release_unknown_handle_raises(self):
        with self.assertRaises(ValueError):
            self.mcp.tools["io_release"]("nonexistent_handle_xyz")

    def test_io_release_double_release_raises(self):
        fake_bb = MagicMock()
        fake_bb.io_claim.return_value = None
        fake_bb.io_release.return_value = None

        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            handle = self.mcp.tools["io_claim"]([0])
            self.mcp.tools["io_release"](handle)
            with self.assertRaises(ValueError):
                self.mcp.tools["io_release"](handle)


class TestIoOwnerStatus(unittest.TestCase):
    def setUp(self):
        session.configure(transport="usb", port="/dev/null", vlogic=3.3)
        self.mcp = DummyMCP()
        register(self.mcp)
        _clear_leases()

    def test_io_owner_status_returns_16_entries(self):
        expected = [
            {"slot": i, "owner_kind": "NONE", "session_id": 0,
             "lease_until_ms": 0, "purpose_tag": 0}
            for i in range(16)
        ]
        fake_bb = MagicMock()
        fake_bb.io_owner_status.return_value = expected

        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            result = self.mcp.tools["io_owner_status"]()

        self.assertEqual(len(result), 16)
        self.assertEqual(result[0]["slot"], 0)
        self.assertEqual(result[0]["owner_kind"], "NONE")
        fake_bb.io_owner_status.assert_called_once()


class TestIoForceRelease(unittest.TestCase):
    def setUp(self):
        session.configure(transport="usb", port="/dev/null", vlogic=3.3,
                          admin_token="secret-admin")
        self.mcp = DummyMCP()
        register(self.mcp)
        _clear_leases()

    def tearDown(self):
        # Reset admin token in session
        session.configure(transport="usb", port="/dev/null", vlogic=3.3)

    def test_force_release_uses_server_token(self):
        fake_bb = MagicMock()
        fake_bb.io_force_release.return_value = None

        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            result = self.mcp.tools["io_force_release"](slot=3)

        self.assertTrue(result["success"])
        self.assertEqual(result["slot"], 3)
        fake_bb.io_force_release.assert_called_once_with(3, "secret-admin")

    def test_force_release_uses_caller_token_when_supplied(self):
        fake_bb = MagicMock()
        fake_bb.io_force_release.return_value = None

        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            result = self.mcp.tools["io_force_release"](
                slot=5, admin_token="caller-token"
            )

        self.assertTrue(result["success"])
        fake_bb.io_force_release.assert_called_once_with(5, "caller-token")

    def test_force_release_rejects_without_any_token(self):
        session.configure(transport="usb", port="/dev/null", vlogic=3.3,
                          admin_token=None)
        fake_bb = MagicMock()
        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            with self.assertRaises(PermissionError) as ctx:
                self.mcp.tools["io_force_release"](slot=0)
        # Token must NOT appear in error message
        self.assertNotIn("secret-admin", str(ctx.exception))

    def test_force_release_error_does_not_leak_token(self):
        fake_bb = MagicMock()
        fake_bb.io_force_release.side_effect = PermissionError("bad token")

        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            with self.assertRaises(PermissionError) as ctx:
                self.mcp.tools["io_force_release"](slot=0)
        self.assertNotIn("secret-admin", str(ctx.exception))

    def test_force_release_invalid_slot_raises(self):
        with self.assertRaises(ValueError):
            self.mcp.tools["io_force_release"](slot=16)

    def test_force_release_minus_one_clears_all_leases(self):
        fake_bb = MagicMock()
        fake_bb.io_claim.return_value = None
        fake_bb.io_force_release.return_value = None

        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            handle = self.mcp.tools["io_claim"]([1, 2])
            self.mcp.tools["io_force_release"](slot=-1)

        with _leases_lock:
            self.assertNotIn(handle, _active_leases)

    def test_force_release_slot_evicts_matching_lease(self):
        fake_bb = MagicMock()
        fake_bb.io_claim.return_value = None
        fake_bb.io_force_release.return_value = None

        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            handle = self.mcp.tools["io_claim"]([7, 8])
            self.mcp.tools["io_force_release"](slot=7)

        # Lease for handle should be evicted because slot 7 was in it
        with _leases_lock:
            self.assertNotIn(handle, _active_leases)


class TestVerifyLeaseCovershHelper(unittest.TestCase):
    """Tests for the _verify_lease_covers shared helper."""

    def setUp(self):
        _clear_leases()

    def tearDown(self):
        _clear_leases()

    def _plant_lease(self, slots: list[int]) -> str:
        """Insert a fake lease directly into _active_leases and return its handle."""
        import uuid
        handle = uuid.uuid4().hex
        with _leases_lock:
            _active_leases[handle] = list(slots)
        return handle

    # --- unknown handle ---

    def test_unknown_handle_raises_value_error(self):
        with self.assertRaises(ValueError) as ctx:
            _verify_lease_covers("totally-unknown-handle", [0])
        self.assertIn("Unknown lease handle", str(ctx.exception))

    # --- valid handle but wrong slot ---

    def test_valid_handle_missing_slot_raises(self):
        handle = self._plant_lease([0, 1, 2])
        with self.assertRaises(ValueError) as ctx:
            _verify_lease_covers(handle, [0, 5])  # slot 5 not in lease
        self.assertIn("does not cover slot", str(ctx.exception))
        self.assertIn("5", str(ctx.exception))

    # --- valid handle covering the required slots ---

    def test_valid_handle_covering_slots_does_not_raise(self):
        handle = self._plant_lease([0, 1, 2, 3])
        # Should not raise
        _verify_lease_covers(handle, [0, 1, 2, 3])

    def test_valid_handle_partial_coverage_ok(self):
        handle = self._plant_lease([0, 1, 2, 3, 4])
        _verify_lease_covers(handle, [1, 3])  # subset — OK

    # --- no auto-claim when lease_handle is provided to a tool ---

    def test_write_digital_with_valid_lease_does_not_call_io_claim(self):
        """When lease_handle is valid and covers the slot, the tool must not
        trigger an additional io_claim call on bb."""
        from bugbuster_mcp.tools.digital import register as reg_digital
        mcp = DummyMCP()
        reg_digital(mcp)

        # Plant lease covering IO5 → slot 4
        handle = self._plant_lease([4])

        fake_bb = MagicMock()
        fake_bb.io_claim.return_value = None

        fake_hal = MagicMock()
        from bugbuster.hal import PortMode
        fake_hal._io_mode = {5: PortMode.DIGITAL_OUT}
        fake_hal.write_digital.return_value = None
        fake_bb.io_owner_status.return_value = []

        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb), \
             patch("bugbuster_mcp.session.get_hal", return_value=fake_hal):
            mcp.tools["write_digital"](io=5, state=True, lease_handle=handle)

        # The tool must NOT have called bb.io_claim (no auto-claim on valid lease)
        fake_bb.io_claim.assert_not_called()

    # --- None lease_handle auto-claims (existing behaviour preserved) ---

    def test_write_digital_without_lease_handle_does_not_raise(self):
        """When lease_handle is None, the tool proceeds normally (auto-claim path)."""
        from bugbuster_mcp.tools.digital import register as reg_digital
        mcp = DummyMCP()
        reg_digital(mcp)

        fake_bb = MagicMock()
        fake_bb.io_claim.return_value = None

        fake_hal = MagicMock()
        from bugbuster.hal import PortMode
        fake_hal._io_mode = {3: PortMode.DIGITAL_OUT}
        fake_hal.write_digital.return_value = None
        fake_bb.io_owner_status.return_value = []

        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb), \
             patch("bugbuster_mcp.session.get_hal", return_value=fake_hal):
            result = mcp.tools["write_digital"](io=3, state=False, lease_handle=None)

        self.assertTrue(result["success"])


if __name__ == "__main__":
    unittest.main()
