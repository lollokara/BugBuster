"""Behaviour of the simulated DAQ HAT control plane and the ADC DSP stream.

These exercise tests/mock/handlers/daq.py and the DSP half of
tests/mock/handlers/streaming.py through the real client API, so the wire
layouts in python/bugbuster/daq_config.py are round-tripped rather than
assumed.
"""

import threading
import time

import pytest

import bugbuster as bb
from bugbuster.constants import CmdId
from bugbuster.daq_config import (
    DaqAction,
    DaqCalMode,
    DaqCalPhase,
    DaqCalPrompt,
    DaqCfgOp,
    DaqKey,
    DaqTrigEdge,
    DaqTrigLogic,
    DaqTrigRole,
    DaqTrigSource,
    DaqType,
)
from bugbuster.transport.usb import DeviceError
from tests.mock import SimulatedDevice, SimulatedUSBTransport


@pytest.fixture
def client():
    device = SimulatedDevice()
    transport = SimulatedUSBTransport(device, hat=True)
    c = bb.BugBuster(transport)
    c.connect()
    yield c
    c.disconnect()


# ---------------------------------------------------------------------------
# DAQ_CONFIG registry
# ---------------------------------------------------------------------------

def test_set_then_get_returns_the_written_value(client):
    client.daq.set(DaqKey.DUT_VOLTAGE_MV, 5000)
    assert client.daq.get(DaqKey.DUT_VOLTAGE_MV) == 5000


def test_get_all_pages_and_returns_every_public_key(client):
    """The P4 pages GET_ALL replies to fit the 32-byte HAT frame, so the host
    paging loop must run more than once. A single-page simulator would let a
    broken loop pass, so assert the paging actually happens rather than only
    that the merged result looks right."""
    first = client._usb_cmd(CmdId.DAQ_CONFIG, bytes([DaqCfgOp.GET_ALL, 0, 0]))
    assert first[0] != 0xFF, (
        "the whole registry fit in one reply — this test would then pass even "
        "with the host paging loop broken")

    all_settings = client.daq.get_all()
    assert int(DaqKey.DUT_VOLTAGE_MV) in all_settings
    assert int(DaqKey.AUTORANGING) in all_settings
    # Only reachable on a later page than the first.
    assert int(DaqKey.DEVICE_LABEL) in all_settings


def test_get_all_withholds_the_wifi_password_unless_asked(client):
    assert int(DaqKey.WIFI_PASSWORD) not in client.daq.get_all()
    assert int(DaqKey.WIFI_PASSWORD) in client.daq.get_all(include_secret=True)


def test_get_all_reflects_a_prior_set(client):
    client.daq.set(DaqKey.BRIGHTNESS_PCT, 42)
    assert client.daq.get_all()[int(DaqKey.BRIGHTNESS_PCT)] == 42


def test_unknown_key_is_rejected_rather_than_silently_stored(client):
    # SET of key 0xDEAD, type U8, length 1, value 0 — hand-built because the
    # typed client API refuses to encode a key it does not know.
    payload = bytes([DaqCfgOp.SET, 0xAD, 0xDE, int(DaqType.U8), 0x01, 0x00])
    with pytest.raises(DeviceError):
        client._usb_cmd(CmdId.DAQ_CONFIG, payload)


def test_schema_reports_bounds_for_a_bounded_key(client):
    schema = client.daq.schema(DaqKey.BRIGHTNESS_PCT)
    assert schema["key"] == int(DaqKey.BRIGHTNESS_PCT)
    assert schema["min"] == 0 and schema["max"] == 100
    assert schema["label"]


def test_factory_reset_action_restores_defaults(client):
    client.daq.set(DaqKey.BRIGHTNESS_PCT, 5)
    client.daq.action(DaqAction.FACTORY_RESET)
    assert client.daq.get(DaqKey.BRIGHTNESS_PCT) == 80


# ---------------------------------------------------------------------------
# DAQ_CAL state machine
# ---------------------------------------------------------------------------

def test_calibration_blocks_on_an_operator_prompt_before_running(client):
    client.daq.cal_start(DaqCalMode.VOLTAGE)
    status = client.daq.cal_status()
    assert status["phase"] == DaqCalPhase.PROMPT
    assert status["prompt"] == DaqCalPrompt.DISCONNECT_LOAD


def test_current_calibration_asks_for_a_shorted_output(client):
    client.daq.cal_start(DaqCalMode.CURRENT)
    assert client.daq.cal_status()["prompt"] == DaqCalPrompt.SHORT_OUTPUT


def test_ack_before_a_prompt_is_rejected(client):
    with pytest.raises(DeviceError):
        client.daq.cal_ack()


def test_calibration_converges_to_success_after_ack(client):
    client.daq.cal_start(DaqCalMode.VOLTAGE)
    client.daq.cal_ack()
    for _ in range(10):
        status = client.daq.cal_status()
        if status["phase"] == DaqCalPhase.SUCCESS:
            break
    assert status["phase"] == DaqCalPhase.SUCCESS
    assert status["vcount"] > 0


def test_abort_returns_the_machine_to_idle(client):
    client.daq.cal_start(DaqCalMode.VOLTAGE)
    client.daq.cal_abort()
    assert client.daq.cal_status()["phase"] == DaqCalPhase.IDLE


# ---------------------------------------------------------------------------
# DAQ_MEASURE
# ---------------------------------------------------------------------------

def test_measure_reads_zero_while_the_source_is_disabled(client):
    client.daq.set(DaqKey.SOURCE_ENABLE, False)
    m = client.daq.measure()
    assert m["voltage_v"] == 0.0
    assert m["source_enabled"] is False


def test_measure_follows_the_configured_setpoint(client):
    client.daq.set(DaqKey.SOURCE_ENABLE, True)
    client.daq.set(DaqKey.DUT_VOLTAGE_MV, 5000)
    m = client.daq.measure()
    assert m["voltage_v"] == pytest.approx(5.0)
    assert m["power_w"] == pytest.approx(m["voltage_v"] * m["current_a"])


def test_measure_clamps_current_at_the_configured_limit(client):
    client.daq.set(DaqKey.SOURCE_ENABLE, True)
    client.daq.set(DaqKey.DUT_VOLTAGE_MV, 15000)
    client.daq.set(DaqKey.DUT_ILIMIT_MA, 10)
    assert client.daq.measure()["current_a"] == pytest.approx(0.010)


# ---------------------------------------------------------------------------
# DAQ_TRIG
# ---------------------------------------------------------------------------

def test_trigger_io_config_round_trips(client):
    client.daq_trigger.set_io(3, DaqTrigRole.FLAG, DaqTrigEdge.ANY,
                              DaqTrigSource.ANALOG, threshold_v=2.5)
    cfg = client.daq_trigger.get_io(3)
    assert cfg["role"] == DaqTrigRole.FLAG
    assert cfg["edge"] == DaqTrigEdge.ANY
    assert cfg["source"] == DaqTrigSource.ANALOG
    assert cfg["threshold_v"] == pytest.approx(2.5)


def test_analog_source_is_rejected_on_a_digital_only_io(client):
    with pytest.raises(ValueError):
        client.daq_trigger.set_io(1, DaqTrigRole.TRIGGER,
                                  source=DaqTrigSource.ANALOG)


def test_arm_and_logic_are_visible_in_status(client):
    client.daq_trigger.set_logic(DaqTrigLogic.AND)
    client.daq_trigger.arm(True, pre_samples=12500)
    status = client.daq_trigger.status()
    assert status["logic"] == DaqTrigLogic.AND
    assert status["armed"] is True


def test_get_all_returns_twelve_io_configs(client):
    client.daq_trigger.set_io(6, DaqTrigRole.TRIGGER)
    everything = client.daq_trigger.get_all()
    assert len(everything["ios"]) == 12
    assert everything["ios"][5]["role"] == DaqTrigRole.TRIGGER


def test_out_of_range_io_is_rejected(client):
    with pytest.raises(ValueError):
        client.daq_trigger.get_io(13)


# ---------------------------------------------------------------------------
# ADC DSP stream
# ---------------------------------------------------------------------------

def test_dsp_stream_delivers_self_consistent_windows(client):
    got = []
    done = threading.Event()

    def on_window(win):
        got.append(win)
        if len(got) >= 2:
            done.set()

    client.start_adc_dsp_stream(channel=1, callback=on_window)
    try:
        assert done.wait(timeout=3.0), "no ADC_DSP_EVT delivered"
    finally:
        client.stop_adc_dsp_stream()

    win = got[0]
    assert win.channel == 1
    assert win.min_v < win.mean_v < win.max_v
    # RMS of a sine about a positive offset always exceeds the mean.
    assert win.rms_v > win.mean_v
    assert win.fft_peaks, "expected FFT peaks"
    # The fundamental must dominate its harmonics.
    assert win.fft_peaks[0][1] >= win.fft_peaks[-1][1]


def test_dsp_stream_timestamps_advance(client):
    got = []
    done = threading.Event()

    def on_window(win):
        got.append(win)
        if len(got) >= 3:
            done.set()

    client.start_adc_dsp_stream(channel=0, callback=on_window)
    try:
        assert done.wait(timeout=3.0)
    finally:
        client.stop_adc_dsp_stream()

    stamps = [w.timestamp_us for w in got[:3]]
    assert stamps == sorted(stamps) and stamps[0] != stamps[-1]


def test_a_high_spike_threshold_suppresses_spikes(client):
    got = []
    done = threading.Event()

    client.start_adc_dsp_stream(
        channel=0, spike_threshold=99.0,
        callback=lambda w: (got.append(w), done.set()))
    try:
        assert done.wait(timeout=3.0)
    finally:
        client.stop_adc_dsp_stream()

    assert got[0].spikes == []


def test_stopping_the_dsp_stream_ends_delivery(client):
    got = []
    client.start_adc_dsp_stream(channel=0, callback=got.append)
    time.sleep(0.15)
    client.stop_adc_dsp_stream()
    settled = len(got)
    time.sleep(0.2)
    assert len(got) == settled, "windows still arriving after stop"


def test_disconnect_stops_a_running_dsp_stream():
    """A leaked stream thread outlives the test that started it and corrupts
    whichever test runs next."""
    device = SimulatedDevice()
    transport = SimulatedUSBTransport(device, hat=True)
    c = bb.BugBuster(transport)
    c.connect()
    c.start_adc_dsp_stream(channel=0, callback=lambda w: None)
    c.disconnect()
    assert device.dsp_stream is None
