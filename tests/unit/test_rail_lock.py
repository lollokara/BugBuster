"""
Unit tests for board-profile rail-lock enforcement.

The MCP safety layer must refuse to change a voltage rail (VADJ1, VADJ2, VLOGIC)
when the active board profile declares it ``locked: true``.  Profiles use the
nested schema documented in ``Docs/board_profiles.md``::

    {
      "name": "...",
      "vlogic": {"value": 3.3, "locked": true},
      "vadj1":  {"value": 3.3, "locked": true},
      "vadj2":  {"value": 5.0, "locked": false}
    }

Runs without hardware.
"""

import unittest
from unittest.mock import patch

from bugbuster_mcp import safety


class RailLockTests(unittest.TestCase):
    """validate_vadj_voltage / validate_vlogic honour the active profile lock."""

    def _with_profile(self, profile):
        return patch(
            "bugbuster_mcp.session.get_active_board_profile",
            return_value=profile,
        )

    # -- VADJ1 ----------------------------------------------------------------

    def test_vadj1_locked_blocks_mismatched_voltage(self):
        profile = {
            "name": "stm32f4_discovery",
            "vadj1": {"value": 3.3, "locked": True},
        }
        with self._with_profile(profile):
            with self.assertRaises(ValueError) as ctx:
                safety.validate_vadj_voltage(5.0, index=1, confirm=False)
            self.assertIn("locked", str(ctx.exception))
            self.assertIn("stm32f4_discovery", str(ctx.exception))

    def test_vadj1_locked_allows_matching_voltage(self):
        profile = {
            "name": "stm32f4_discovery",
            "vadj1": {"value": 3.3, "locked": True},
        }
        with self._with_profile(profile):
            # No exception when the requested voltage matches the locked value.
            safety.validate_vadj_voltage(3.3, index=1, confirm=False)

    def test_vadj1_unlocked_allows_change(self):
        profile = {
            "name": "dev_board",
            "vadj1": {"value": 3.3, "locked": False},
        }
        with self._with_profile(profile):
            safety.validate_vadj_voltage(5.0, index=1, confirm=False)

    # -- VADJ2 ----------------------------------------------------------------

    def test_vadj2_lock_checked_independently(self):
        profile = {
            "name": "mixed",
            "vadj1": {"value": 3.3, "locked": False},
            "vadj2": {"value": 5.0, "locked": True},
        }
        with self._with_profile(profile):
            # VADJ1 change allowed, VADJ2 change blocked.
            safety.validate_vadj_voltage(5.0, index=1, confirm=False)
            with self.assertRaises(ValueError):
                safety.validate_vadj_voltage(3.3, index=2, confirm=False)

    # -- VLOGIC ---------------------------------------------------------------

    def test_vlogic_locked_blocks_change(self):
        profile = {
            "name": "vlogic_locked",
            "vlogic": {"value": 3.3, "locked": True},
        }
        with self._with_profile(profile):
            with self.assertRaises(ValueError) as ctx:
                safety.validate_vlogic(1.8)
            self.assertIn("VLOGIC is locked", str(ctx.exception))

    def test_vlogic_locked_allows_matching_voltage(self):
        profile = {
            "name": "vlogic_locked",
            "vlogic": {"value": 3.3, "locked": True},
        }
        with self._with_profile(profile):
            safety.validate_vlogic(3.3)

    # -- No profile active ----------------------------------------------------

    def test_no_profile_falls_through_to_hardware_limits(self):
        with self._with_profile(None):
            # Should still enforce hardware min/max, but not raise "locked".
            safety.validate_vadj_voltage(3.3, index=1, confirm=False)
            safety.validate_vlogic(3.3)


if __name__ == "__main__":
    unittest.main()
