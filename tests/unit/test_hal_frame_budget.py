"""TR-6 / MCP-31: `configure_io` cost 12-14 BBP frames on an analog IO. Every
mutating call auto-wrapped its own IO_CLAIM + IO_RELEASE, and the HAL wrote
the MUX twice (DISABLED, then the target).

B: one lease around the whole configure, and a single MUX write - the
firmware set-all is break-before-make safe on its own (IO-13). Target from the
plan: <= 7 frames. Simulator frame counter.
"""

import pytest

import bugbuster as bb
from bugbuster.constants import CmdId
from bugbuster.hal import BugBusterHAL, PortMode
from tests.mock import SimulatedDevice, SimulatedUSBTransport


def _hal():
    c = bb.BugBuster(SimulatedUSBTransport(SimulatedDevice()))
    c.connect()
    hal = BugBusterHAL(c)
    hal.begin()
    sent = []
    orig = c._t.send_command

    def counting(cmd, payload=b"", timeout=None):
        sent.append(int(cmd))
        return orig(cmd, payload)

    c._t.send_command = counting
    return hal, sent


@pytest.mark.xfail(strict=True, reason="TR-6")
@pytest.mark.parametrize("mode", [PortMode.ANALOG_IN, PortMode.ANALOG_OUT])
def test_configure_io_frame_budget(mode):
    hal, sent = _hal()
    hal.configure(3, mode)
    assert len(sent) <= 7, [CmdId(s).name for s in sent]
    assert sent.count(int(CmdId.IO_CLAIM)) <= 1
    assert sent.count(int(CmdId.MUX_SET_ALL)) == 1


def test_configure_still_routes_and_records_the_mode():
    hal, _ = _hal()
    hal.configure(3, PortMode.ANALOG_IN)
    assert hal._io_mode[3] == PortMode.ANALOG_IN
