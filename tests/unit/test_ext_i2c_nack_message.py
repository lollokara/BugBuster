"""BUS-017 (host half): a NACK on the external I2C bus reaches the caller as an
I2C NACK naming the address, not "Device error SPI_FAIL".

The firmware maps a NACK to CMD_ERR_HARDWARE, which BBP carries as
BBP_ERR_SPI_FAIL (the only hardware-fault code on the wire). A stuck bus stays
TIMEOUT and is not rewritten."""
from __future__ import annotations

from unittest.mock import MagicMock

import pytest

import bugbuster as bb
from bugbuster.transport.usb import DeviceError

SPI_FAIL, TIMEOUT = 0x04, 0x11


def _client(code: int) -> bb.BugBuster:
    c = bb.BugBuster.__new__(bb.BugBuster)
    c._usb = True
    c._usb_cmd = MagicMock(side_effect=DeviceError(code, 7))
    return c


CALLS = [
    lambda c: c.ext_i2c_write(0x50, [0]),
    lambda c: c.ext_i2c_read(0x50, 1),
    lambda c: c.ext_i2c_write_read(0x50, [0], 1),
]


@pytest.mark.parametrize("call", CALLS)
def test_nack_names_the_address(call):
    with pytest.raises(DeviceError, match=r"NACK.*0x50") as exc:
        call(_client(SPI_FAIL))
    assert exc.value.code == SPI_FAIL


@pytest.mark.parametrize("call", CALLS)
def test_timeout_is_left_alone(call):
    with pytest.raises(DeviceError, match="TIMEOUT"):
        call(_client(TIMEOUT))
