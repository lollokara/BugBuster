"""The Python USB transport warns on a BBP protocol mismatch, like the desktop
app's connection manager does. A: it only logged the device's version."""
from __future__ import annotations

import inspect
import logging

import pytest

from bugbuster.protocol import BBP_PROTO_VERSION
from bugbuster.transport import usb


@pytest.mark.xfail(strict=True, reason="PROTO-CHECK")
def test_mismatch_is_reported(caplog):
    with caplog.at_level(logging.WARNING, logger=usb.__name__):
        msg = usb.check_proto_version(BBP_PROTO_VERSION - 1)
    assert msg and f"device v{BBP_PROTO_VERSION - 1}" in msg and f"library v{BBP_PROTO_VERSION}" in msg
    assert msg in caplog.text


@pytest.mark.xfail(strict=True, reason="PROTO-CHECK")
def test_match_is_silent(caplog):
    with caplog.at_level(logging.WARNING, logger=usb.__name__):
        assert usb.check_proto_version(BBP_PROTO_VERSION) is None
    assert caplog.text == ""


@pytest.mark.xfail(strict=True, reason="PROTO-CHECK")
def test_connect_runs_the_check():
    assert "check_proto_version(self.proto_version)" in inspect.getsource(usb.USBTransport.connect)
