"""AN-01: over HTTP, the 8 mA current limit must actually reach the device.

The firmware handler reads only "limit8mA" and treats a missing key as 25 mA,
so a client that sends any other key gets a silent no-op. The HTTP simulator
route mirrors that handler.
"""

from bugbuster import BugBuster
from bugbuster.constants import CurrentLimit
from tests.mock import SimulatedDevice, SimulatedHTTPTransport, SimulatedUSBTransport


def test_http_current_limit_8ma_is_applied():
    dev = SimulatedDevice()
    bb = BugBuster(SimulatedHTTPTransport(dev))
    bb.set_current_limit(1, CurrentLimit.MA_8)
    assert dev.channels[1]["current_limit"] == int(CurrentLimit.MA_8)


def test_usb_current_limit_8ma_is_applied():
    """Control: the USB path already works, so the A failure above is the
    HTTP key and not the simulator."""
    dev = SimulatedDevice()
    bb = BugBuster(SimulatedUSBTransport(dev))
    bb.set_current_limit(1, CurrentLimit.MA_8)
    assert dev.channels[1]["current_limit"] == int(CurrentLimit.MA_8)
