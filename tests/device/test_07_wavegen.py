"""
test_07_wavegen.py — Software waveform generator tests.

The firmware continuously updates the DAC to produce waveforms.
Supported types: SINE, SQUARE, TRIANGLE, SAWTOOTH.
Both VOLTAGE and CURRENT output modes are supported.

Channel assignment (breadboard):
  Ch0 (A): 330 Ω resistor  → IOUT (current) tests
  Ch1 (B): not connected   → VOUT (voltage) tests

Note: stop_waveform() takes no arguments (stops globally).
"""

import time
import pytest
from bugbuster import ChannelFunction, WaveformType, OutputMode
from conftest import assert_no_faults

pytestmark = [pytest.mark.timeout(15)]

# Channel assignments matching physical breadboard setup
CH_VOUT = 1  # Channel B — unconnected, safe for voltage output
CH_IOUT = 0  # Channel A — 330 Ω load, suitable for current output


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _setup_vout_channel(device, ch=CH_VOUT):
    """Put channel in VOUT mode for voltage waveform testing."""
    device.set_channel_function(ch, ChannelFunction.HIGH_IMP)
    device.set_channel_function(ch, ChannelFunction.VOUT)


def _setup_iout_channel(device, ch=CH_IOUT):
    """Put channel in IOUT mode for current waveform testing."""
    device.set_channel_function(ch, ChannelFunction.HIGH_IMP)
    device.set_channel_function(ch, ChannelFunction.IOUT)


def _safe_stop(device, ch):
    """Stop waveform and put channel to HIGH_IMP."""
    try:
        device.stop_waveform()
    except Exception:
        pass
    try:
        device.set_channel_function(ch, ChannelFunction.HIGH_IMP)
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Sine wave (VOUT on ch1)
# ---------------------------------------------------------------------------

def test_start_stop_wavegen_sine(device):
    """
    Start a 10 Hz sine wave on ch1 (VOUT, 2 V amplitude, 5 V offset),
    wait 100 ms, then stop.  Verifies no exception is raised.
    """
    _setup_vout_channel(device)
    device.start_waveform(
        CH_VOUT, WaveformType.SINE,
        freq_hz=10.0, amplitude=2.0, offset=5.0,
        mode=OutputMode.VOLTAGE,
    )
    time.sleep(0.1)
    device.stop_waveform()
    _safe_stop(device, CH_VOUT)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Square wave (VOUT on ch1)
# ---------------------------------------------------------------------------

def test_start_stop_wavegen_square(device):
    """
    Start a 100 Hz square wave on ch1 (VOUT, 3 V amplitude, 3 V offset).
    """
    _setup_vout_channel(device)
    device.start_waveform(
        CH_VOUT, WaveformType.SQUARE,
        freq_hz=100.0, amplitude=3.0, offset=3.0,
    )
    time.sleep(0.1)
    device.stop_waveform()
    _safe_stop(device, CH_VOUT)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Triangle wave (VOUT on ch1)
# ---------------------------------------------------------------------------

def test_start_stop_wavegen_triangle(device):
    """
    Start a 50 Hz triangle wave on ch1 (VOUT, 4 V amplitude, 4 V offset).
    """
    _setup_vout_channel(device)
    device.start_waveform(
        CH_VOUT, WaveformType.TRIANGLE,
        freq_hz=50.0, amplitude=4.0, offset=4.0,
    )
    time.sleep(0.1)
    device.stop_waveform()
    _safe_stop(device, CH_VOUT)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Sawtooth wave (VOUT on ch1)
# ---------------------------------------------------------------------------

def test_start_stop_wavegen_sawtooth(device):
    """
    Start a 20 Hz sawtooth wave on ch1 (VOUT, 1 V amplitude, 1 V offset).
    """
    _setup_vout_channel(device)
    device.start_waveform(
        CH_VOUT, WaveformType.SAWTOOTH,
        freq_hz=20.0, amplitude=1.0, offset=1.0,
    )
    time.sleep(0.1)
    device.stop_waveform()
    _safe_stop(device, CH_VOUT)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Current mode (IOUT on ch0, 330 Ω load)
# ---------------------------------------------------------------------------

def test_wavegen_current_mode(device):
    """
    Start a sine wave in CURRENT mode on ch0 (IOUT, 4 mA amplitude, 12 mA offset).
    This simulates a 4–20 mA loop signal variation through the 330 Ω load.
    Peak compliance voltage: 16 mA × 330 Ω = 5.28 V (well within AVDD).
    """
    _setup_iout_channel(device)
    device.start_waveform(
        CH_IOUT, WaveformType.SINE,
        freq_hz=1.0, amplitude=4.0, offset=12.0,
        mode=OutputMode.CURRENT,
    )
    time.sleep(0.1)
    device.stop_waveform()
    _safe_stop(device, CH_IOUT)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Frequency range (VOUT on ch1)
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("freq_hz", [0.5, 1.0, 10.0, 50.0, 100.0])
def test_wavegen_frequency_range(device, freq_hz):
    """
    Try starting a sine wave at various frequencies within the firmware's
    supported range (0.1–100 Hz).  Verifies no INVALID_PARAM error.
    """
    _setup_vout_channel(device)
    device.start_waveform(
        CH_VOUT, WaveformType.SINE,
        freq_hz=freq_hz, amplitude=2.0, offset=5.0,
    )
    time.sleep(0.05)
    device.stop_waveform()
    _safe_stop(device, CH_VOUT)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Stop all channels (VOUT on ch1)
# ---------------------------------------------------------------------------

def test_wavegen_stop_all_channels(device):
    """
    Start a waveform on ch1, then stop it.
    Verifies the stop_waveform() call leaves no error state.
    """
    _setup_vout_channel(device)
    device.start_waveform(
        CH_VOUT, WaveformType.SQUARE,
        freq_hz=50.0, amplitude=1.0, offset=2.0,
    )
    time.sleep(0.05)
    device.stop_waveform()

    # Verify device is still responsive after stop
    status = device.get_status()
    assert isinstance(status, dict), "Device should respond after stop_waveform()"
    _safe_stop(device, CH_VOUT)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Multiple waveform types on the same channel (VOUT on ch1)
# ---------------------------------------------------------------------------

def test_wavegen_switch_types(device):
    """
    Switch between different waveform types on the same channel.
    Verifies that starting a new waveform after stopping the previous one works.
    """
    _setup_vout_channel(device)

    for wtype in [WaveformType.SINE, WaveformType.SQUARE, WaveformType.TRIANGLE]:
        device.start_waveform(CH_VOUT, wtype, freq_hz=100.0, amplitude=1.0, offset=2.0)
        time.sleep(0.05)
        device.stop_waveform()
        time.sleep(0.02)

    _safe_stop(device, CH_VOUT)
    assert_no_faults(device)
