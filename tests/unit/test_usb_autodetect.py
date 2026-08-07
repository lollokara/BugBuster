"""Unit tests for USB serial auto-detection of the BugBuster mainboard."""
from unittest.mock import patch

from bugbuster.discovery import (
    USB_PID_MAINBOARD, USB_VID_ESPRESSIF, UsbPort, _interface_index,
    find_usb_port, list_usb_ports,
)


class _FakePort:
    def __init__(self, device, vid=None, pid=None, location="", description="",
                 serial_number=""):
        self.device = device
        self.vid = vid
        self.pid = pid
        self.location = location
        self.description = description
        self.serial_number = serial_number


def _fake_comports(ports):
    return patch("serial.tools.list_ports.comports", return_value=ports)


BB = dict(vid=USB_VID_ESPRESSIF, pid=USB_PID_MAINBOARD)


def test_interface_index_parsing():
    assert _interface_index("1-4.1:x.0") == 0
    assert _interface_index("1-4.1:x.2") == 2
    assert _interface_index("") is None
    assert _interface_index("no-dots") is None


def test_list_usb_ports_filters_non_bugbuster_by_default():
    ports = [_FakePort("COM3"), _FakePort("COM6", location="1-4.1:x.0", **BB)]
    with _fake_comports(ports):
        assert [p.device for p in list_usb_ports()] == ["COM6"]
        assert {p.device for p in list_usb_ports(all_ports=True)} == {"COM3", "COM6"}


def test_cdc0_ranks_before_cdc1():
    # The console interface enumerates first here; ranking must still pick CDC0.
    ports = [_FakePort("COM5", location="1-4.1:x.2", **BB),
             _FakePort("COM6", location="1-4.1:x.0", **BB)]
    with _fake_comports(ports):
        assert [p.device for p in list_usb_ports()] == ["COM6", "COM5"]


def test_unknown_interface_sorts_last_not_first():
    ports = [_FakePort("COM9", **BB),                            # no location
             _FakePort("COM6", location="1-4.1:x.0", **BB)]
    with _fake_comports(ports):
        assert [p.device for p in list_usb_ports()] == ["COM6", "COM9"]


def test_non_bugbuster_sorts_after_bugbuster():
    ports = [_FakePort("COM3"), _FakePort("COM6", location="1-4.1:x.0", **BB)]
    with _fake_comports(ports):
        assert [p.device for p in list_usb_ports(all_ports=True)] == ["COM6", "COM3"]


def test_is_bugbuster_requires_both_ids():
    assert UsbPort("COM6", vid=USB_VID_ESPRESSIF, pid=USB_PID_MAINBOARD).is_bugbuster
    assert not UsbPort("COM6", vid=USB_VID_ESPRESSIF, pid=0x4001).is_bugbuster
    assert not UsbPort("COM6").is_bugbuster


def test_find_usb_port_without_probe_takes_top_candidate():
    ports = [_FakePort("COM5", location="1-4.1:x.2", **BB),
             _FakePort("COM6", location="1-4.1:x.0", **BB)]
    with _fake_comports(ports):
        assert find_usb_port(probe=False) == "COM6"


def test_find_usb_port_returns_none_when_nothing_present():
    with _fake_comports([_FakePort("COM3")]):
        assert find_usb_port(probe=False) is None


def test_find_usb_port_probe_falls_through_to_the_port_that_answers():
    ports = [_FakePort("COM6", location="1-4.1:x.0", **BB),
             _FakePort("COM5", location="1-4.1:x.2", **BB)]
    tried = []

    class _Transport:
        def __init__(self, device, timeout=2.0):
            self.device = device
            tried.append(device)

        def connect(self):
            if self.device == "COM6":
                raise OSError("port busy")

        def disconnect(self):
            pass

    with _fake_comports(ports), \
            patch("bugbuster.transport.usb.USBTransport", _Transport):
        assert find_usb_port() == "COM5"
    assert tried == ["COM6", "COM5"]


def test_find_usb_port_probe_returns_none_when_no_port_answers():
    ports = [_FakePort("COM6", location="1-4.1:x.0", **BB)]

    class _Transport:
        def __init__(self, device, timeout=2.0):
            pass

        def connect(self):
            raise OSError("nothing there")

    with _fake_comports(ports), \
            patch("bugbuster.transport.usb.USBTransport", _Transport):
        assert find_usb_port() is None
