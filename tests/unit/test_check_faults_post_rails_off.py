"""IO-19: check_faults_post must not report a lost power-good on a rail that is
switched off (it reads low because the rail is off, not because it is loaded).

A: every write_digital / configure_io response warned "VADJ1 power-good signal
lost" whenever VADJ1 was disabled. check_faults already ignores it."""
from __future__ import annotations

import pytest

from bugbuster_mcp.safety import check_faults_post
from tests.unit._mock_client import make_client_mock


def _bb(status: dict):
    bb = make_client_mock()
    bb.power_get_status.return_value = status
    return bb


@pytest.mark.xfail(strict=True, reason="IO-19")
def test_disabled_rails_do_not_warn():
    bb = _bb({"efuse_faults": [False] * 4, "vadj1_en": False, "vadj1_pg": False,
              "vadj2_en": False, "vadj2_pg": False})
    assert check_faults_post(bb) == []


def test_enabled_rail_without_power_good_still_warns():
    bb = _bb({"efuse_faults": [False] * 4, "vadj1_en": True, "vadj1_pg": False,
              "vadj2_en": True, "vadj2_pg": True})
    warnings = check_faults_post(bb)
    assert len(warnings) == 1 and "VADJ1" in warnings[0]


def test_tripped_efuse_warns():
    bb = _bb({"efuse_faults": [False, True, False, False], "vadj1_en": True, "vadj1_pg": True,
              "vadj2_en": True, "vadj2_pg": True})
    assert any("E-fuse 2" in w for w in check_faults_post(bb))
