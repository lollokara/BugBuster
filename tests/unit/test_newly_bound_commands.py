"""The eight BBP commands that had no client method until 2026-08-06.

Each was defined in bbp.h, enumerated in constants.py and implemented in the
firmware, but unreachable from Python -- found by
tests/unit/test_bbp_command_parity.py. These tests pin the wire encoding
against the payload layouts documented in the firmware handlers, so a future
refactor cannot quietly change one side.
"""

import struct

import pytest

import bugbuster as bb
from bugbuster.constants import AvddSelect, CmdId, DoMode
from bugbuster.protocol import ProtocolError
from tests.mock import SimulatedDevice, SimulatedHTTPTransport, SimulatedUSBTransport


class _Recorder:
    """Captures the raw (cmd_id, payload) a client method puts on the wire."""

    def __init__(self, device):
        self.device = device
        self.sent = []
        self._real = device.dispatch

        def spy(cmd_id, payload):
            self.sent.append((int(cmd_id), bytes(payload)))
            return self._real(cmd_id, payload)

        device.dispatch = spy

    def payload_for(self, cmd_id):
        for cid, p in self.sent:
            if cid == int(cmd_id):
                return p
        raise AssertionError(
            f"0x{int(cmd_id):02X} was never sent; saw "
            f"{[hex(c) for c, _ in self.sent]}")


@pytest.fixture
def usb():
    device = SimulatedDevice()
    client = bb.BugBuster(SimulatedUSBTransport(device, hat=True))
    client.connect()
    rec = _Recorder(device)
    yield client, device, rec
    client.disconnect()


@pytest.fixture
def http():
    device = SimulatedDevice()
    client = bb.BugBuster(SimulatedHTTPTransport(device, hat=True))
    client.connect()
    yield client, device


# ---------------------------------------------------------------------------
# GET_DIAGNOSTICS (0x04) — resp: 4 x (u8 slot, u8 source, u16 raw, f32 value)
# ---------------------------------------------------------------------------

def test_get_diagnostics_returns_four_slots(usb):
    client, _, _ = usb
    diags = client.get_diagnostics()
    assert len(diags) == 4
    assert [d["slot"] for d in diags] == [0, 1, 2, 3]
    for d in diags:
        assert set(d) == {"slot", "source", "raw", "value"}


def test_get_diagnostics_sends_no_payload(usb):
    client, _, rec = usb
    client.get_diagnostics()
    assert rec.payload_for(CmdId.GET_DIAGNOSTICS) == b""


def test_get_diagnostics_rejects_a_truncated_reply(usb):
    client, device, _ = usb
    device.register_handler(CmdId.GET_DIAGNOSTICS, lambda p: b"\x00" * 8)
    with pytest.raises(ProtocolError, match="too short"):
        client.get_diagnostics()


# ---------------------------------------------------------------------------
# SET_DO_CONFIG (0x16) — payload: u8 ch, u8 mode, bool srcSelGpio, u8 t1, u8 t2
# ---------------------------------------------------------------------------

def test_set_do_config_encodes_all_five_fields(usb):
    client, _, rec = usb
    client.set_do_config(2, DoMode.PUSH_PULL, src_sel_gpio=True, t1=7, t2=9)
    assert rec.payload_for(CmdId.SET_DO_CONFIG) == struct.pack(
        "<BBBBB", 2, int(DoMode.PUSH_PULL), 1, 7, 9)


def test_set_do_config_defaults_are_off(usb):
    client, _, rec = usb
    client.set_do_config(0, DoMode.HIGH_SIDE)
    assert rec.payload_for(CmdId.SET_DO_CONFIG) == struct.pack(
        "<BBBBB", 0, int(DoMode.HIGH_SIDE), 0, 0, 0)


def test_set_do_config_is_five_bytes(usb):
    """The firmware rejects anything shorter than 5 bytes."""
    client, _, rec = usb
    client.set_do_config(1, DoMode.LOW_SIDE)
    assert len(rec.payload_for(CmdId.SET_DO_CONFIG)) == 5


def test_set_do_config_over_http(http):
    client, device = http
    client.set_do_config(1, DoMode.LOW_SIDE, t1=3, t2=4)
    assert device.channels[1]["do_mode"] == int(DoMode.LOW_SIDE)


# ---------------------------------------------------------------------------
# SET_AVDD_SELECT (0x1A) — payload: u8 ch, u8 sel
# ---------------------------------------------------------------------------

def test_set_avdd_select_encodes_channel_and_rail(usb):
    client, _, rec = usb
    client.set_avdd_select(3, AvddSelect.HI)
    assert rec.payload_for(CmdId.SET_AVDD_SELECT) == struct.pack(
        "<BB", 3, int(AvddSelect.HI))


def test_set_avdd_select_low_rail(usb):
    client, _, rec = usb
    client.set_avdd_select(0, AvddSelect.LO)
    assert rec.payload_for(CmdId.SET_AVDD_SELECT) == b"\x00\x00"


def test_set_avdd_select_updates_simulated_state(usb):
    client, device, _ = usb
    client.set_avdd_select(2, AvddSelect.HI)
    assert device.channels[2]["avdd_select"] == int(AvddSelect.HI)


# ---------------------------------------------------------------------------
# IDAC_CAL_ADD_POINT (0xA4) — payload: u8 ch, i8 code, f32 measured_v
# ---------------------------------------------------------------------------

def test_idac_cal_add_point_encodes_signed_code_and_float(usb):
    client, _, rec = usb
    client.idac_cal_add_point(1, -40, 2.75)
    assert rec.payload_for(CmdId.IDAC_CAL_ADD_POINT) == struct.pack(
        "<Bbf", 1, -40, 2.75)


def test_idac_cal_add_point_payload_is_six_bytes(usb):
    """The firmware rejects anything shorter than 6 bytes."""
    client, _, rec = usb
    client.idac_cal_add_point(0, 10, 1.0)
    assert len(rec.payload_for(CmdId.IDAC_CAL_ADD_POINT)) == 6


def test_idac_cal_add_point_returns_the_running_count(usb):
    client, _, _ = usb
    r = client.idac_cal_add_point(0, 10, 1.0)
    assert set(r) == {"channel", "count", "valid"}
    assert r["channel"] == 0


@pytest.mark.parametrize("code", [-128, 128, 200, -300])
def test_idac_cal_add_point_rejects_out_of_range_codes(usb, code):
    """The wire field is int8; struct.pack would raise a confusing error."""
    client, _, _ = usb
    with pytest.raises(ValueError, match="-127"):
        client.idac_cal_add_point(0, code, 1.0)


# ---------------------------------------------------------------------------
# IDAC_CAL_CLEAR (0xA5) — payload: u8 ch
# ---------------------------------------------------------------------------

def test_idac_cal_clear_encodes_the_channel(usb):
    client, _, rec = usb
    client.idac_cal_clear(2)
    assert rec.payload_for(CmdId.IDAC_CAL_CLEAR) == b"\x02"


def test_idac_cal_clear_over_http(http):
    client, device = http
    client.idac_cal_clear(1)
    assert device.idac[1]["calibrated"] is False


# ---------------------------------------------------------------------------
# IDAC_CALIBRATE (0xA3) — payload: u8 ch, u8 step, u16 settle_ms, u8 adc_ch
# ---------------------------------------------------------------------------

def test_idac_calibrate_encodes_step_and_settle(usb):
    client, _, rec = usb
    client.idac_calibrate(1, adc_channel=2, step=4, settle_ms=250)
    assert rec.payload_for(CmdId.IDAC_CALIBRATE) == struct.pack(
        "<BBHB", 1, 4, 250, 2)


def test_idac_calibrate_payload_is_five_bytes(usb):
    """The firmware rejects anything shorter than 5 bytes."""
    client, _, rec = usb
    client.idac_calibrate(0, adc_channel=0)
    assert len(rec.payload_for(CmdId.IDAC_CALIBRATE)) == 5


def test_idac_calibrate_returns_the_point_count(usb):
    client, _, _ = usb
    assert isinstance(client.idac_calibrate(0, adc_channel=0), int)


def test_idac_calibrate_is_usb_only(http):
    client, _ = http
    with pytest.raises(NotImplementedError):
        client.idac_calibrate(0, adc_channel=0)


# ---------------------------------------------------------------------------
# PCA_SET_PORT (0xB2) — payload: u8 port, u8 val
# ---------------------------------------------------------------------------

def test_power_set_port_encodes_port_and_value(usb):
    client, _, rec = usb
    client.power_set_port(1, 0xA5, confirm=True)
    assert rec.payload_for(CmdId.PCA_SET_PORT) == b"\x01\xa5"


def test_power_set_port_echoes_back(usb):
    client, _, _ = usb
    assert client.power_set_port(0, 0x8F, confirm=True) == (0, 0x8F)


@pytest.mark.parametrize("port", [2, -1, 255])
def test_power_set_port_rejects_an_invalid_port(usb, port):
    client, _, _ = usb
    with pytest.raises(ValueError, match="port must be 0 or 1"):
        client.power_set_port(port, 0, confirm=True)


def test_power_set_port_is_usb_only(http):
    """The firmware registers no /api/ioexp/port route."""
    client, _ = http
    with pytest.raises(NotImplementedError):
        client.power_set_port(0, 0, confirm=True)


# ---------------------------------------------------------------------------
# USBPD_GO (0xC2) — payload: u8 cmd
# ---------------------------------------------------------------------------

def test_usbpd_go_encodes_the_command_byte(usb):
    client, _, rec = usb
    client.usbpd_go(0x01)
    assert rec.payload_for(CmdId.USBPD_GO) == b"\x01"


def test_usbpd_go_echoes_the_command(usb):
    client, _, _ = usb
    assert client.usbpd_go(0x07) == 0x07


def test_usbpd_go_masks_to_one_byte(usb):
    client, _, rec = usb
    client.usbpd_go(0x1FF)
    assert len(rec.payload_for(CmdId.USBPD_GO)) == 1


def test_usbpd_go_is_usb_only(http):
    """The firmware registers no /api/usbpd/go route."""
    client, _ = http
    with pytest.raises(NotImplementedError):
        client.usbpd_go(1)
