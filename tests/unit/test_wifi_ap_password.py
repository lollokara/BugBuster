"""PLT-02: wifi_set_ap_password must report the firmware status correctly.

handler_wifi_set_ap_password (cmd_wifi.cpp) replies 0x00 = applied+persisted,
0x01 = applied but not persisted, 0x02 = failed. The client returned
bool(resp[0]), i.e. success read as False and failure as True.
"""

from bugbuster import BugBuster
from tests.mock import SimulatedDevice, SimulatedUSBTransport

PW = "correct-horse-battery-staple"


def _bb(status):
    dev = SimulatedDevice()
    dev.wifi_ap_password_persist_result = status
    return BugBuster(SimulatedUSBTransport(dev))


def test_applied_and_persisted_is_success():
    r = _bb(0x00).wifi_set_ap_password(PW)
    assert bool(r) is True and r.persisted is True


def test_applied_not_persisted_is_success_but_flagged():
    r = _bb(0x01).wifi_set_ap_password(PW)
    assert bool(r) is True and r.persisted is False


def test_failure_is_failure():
    r = _bb(0x02).wifi_set_ap_password(PW)
    assert bool(r) is False
