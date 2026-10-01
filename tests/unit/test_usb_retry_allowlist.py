"""TR-7: `_usb_cmd` retried EVERY opcode once after a timeout, so a timed-out
I2C/SPI write, SCRIPT_EVAL or a power command could execute twice on the
device (the first one usually did run - only its reply was late).

B: only an allow-list of idempotent reads (PING, GET_*, *_STATUS, *_LIST,
*_INFO, *_GET, excluding EXT_* bus transactions) is retried.
"""

import pytest

import bugbuster as bb
from bugbuster.constants import CmdId
from tests.mock import SimulatedDevice, SimulatedUSBTransport


def _client_counting():
    client = bb.BugBuster(SimulatedUSBTransport(SimulatedDevice()))
    client.connect()
    calls = []

    def send(cmd, payload=b"", timeout=None):
        calls.append(cmd)
        raise TimeoutError("simulated")

    client._t.send_command = send
    return client, calls


@pytest.mark.parametrize("cmd", [CmdId.EXT_I2C_WRITE, CmdId.EXT_SPI_TRANSFER,
                                 CmdId.SCRIPT_EVAL, CmdId.PCA_SET_PORT])
def test_side_effecting_commands_are_not_resent(cmd):
    client, calls = _client_counting()
    with pytest.raises(TimeoutError):
        client._usb_cmd(cmd, b"\x00")
    assert calls == [cmd]


@pytest.mark.parametrize("cmd", [CmdId.GET_STATUS, CmdId.PING, CmdId.MUX_GET_ALL,
                                 CmdId.IO_OWNER_STATUS])
def test_idempotent_reads_are_retried_once(cmd):
    client, calls = _client_counting()
    with pytest.raises(TimeoutError):
        client._usb_cmd(cmd)
    assert calls == [cmd, cmd]
