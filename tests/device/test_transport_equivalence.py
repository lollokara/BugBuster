"""TR-11 gate (T3): the same operation over USB and over HTTP leaves the device
in the same state, and both transports read that state back identically.

Run with both transports, no DUT needed (channel function only):

    pytest tests/device/test_transport_equivalence.py --device-usb=COM6 \
        --device-http=192.168.3.51
"""

import time

import pytest

from bugbuster import ChannelFunction

pytestmark = pytest.mark.timeout(60)


def _fn(dev, ch=0):
    st = dev.get_status()
    return st["channels"][ch]["function"]


@pytest.mark.parametrize("writer", ["usb", "http"])
def test_channel_function_equivalent_across_transports(usb_device, http_device, writer):
    w = usb_device if writer == "usb" else http_device
    try:
        w.set_channel_function(0, ChannelFunction.VIN)
        time.sleep(0.3)
        assert _fn(usb_device) == _fn(http_device) == int(ChannelFunction.VIN)
    finally:
        w.set_channel_function(0, ChannelFunction.HIGH_IMP)
        time.sleep(0.3)
    assert _fn(usb_device) == _fn(http_device) == int(ChannelFunction.HIGH_IMP)


def test_device_info_equivalent_across_transports(usb_device, http_device):
    a, b = usb_device.get_device_info(), http_device.get_device_info()
    assert (a.spi_ok, a.silicon_rev, a.silicon_id0, a.silicon_id1) == \
           (b.spi_ok, b.silicon_rev, b.silicon_id0, b.silicon_id1)


# Read-only views of the same state. Values that move on their own (die
# temperature, ADC readings, uptime) are left out.
_RO_VIEWS = [
    ("get_faults", lambda r: (r["alert_status"], r["alert_mask"], r["supply_alert_mask"],
                              [(c["alert"], c["mask"]) for c in r["channels"]])),
    ("wifi_get_status", lambda r: (r["connected"], r["sta_ip"], r["ap_ip"], r["ap_mac"])),
    ("hat_get_status", lambda r: (r["detected"], r["type"], r["fw_version"])),
    ("get_status", lambda r: (r["spi_ok"], [(c["function"], c["adc_range"], c["adc_rate"], c["adc_mux"])
                                            for c in r["channels"]])),
]


@pytest.mark.parametrize("method,view", _RO_VIEWS, ids=[m for m, _ in _RO_VIEWS])
def test_read_views_equivalent_across_transports(usb_device, http_device, method, view):
    a = view(getattr(usb_device, method)())
    b = view(getattr(http_device, method)())
    assert a == b
