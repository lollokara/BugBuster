"""
test_07_wavegen.py — Software waveform generator tests.

The firmware continuously updates the DAC to produce waveforms.
Supported types: SINE, SQUARE, TRIANGLE, SAWTOOTH.
Both VOLTAGE and CURRENT output modes are supported.

Note: stop_waveform() takes no arguments (stops globally).
"""

import time
import pytest
import bugbuster as bb
from bugbuster import ChannelFunction, WaveformType, OutputMode

pytestmark = [pytest.mark.timeout(15)]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _setup_vout_channel(device, ch=0):
    """Put channel in VOUT mode for voltage waveform testing."""
    device.set_channel_function(ch, ChannelFunction.HIGH_IMP)
    device.set_channel_function(ch, ChannelFunction.VOUT)


def _setup_iout_channel(device, ch=0):
    """Put channel in IOUT mode for current waveform testing."""
    device.set_channel_function(ch, ChannelFunction.HIGH_IMP)
    device.set_channel_function(ch, ChannelFunction.IOUT)


def _safe_stop(device, ch=0):
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
# Sine wave
# ---------------------------------------------------------------------------

def test_start_stop_wavegen_sine(device):
    """
    Start a 1 Hz sine wave on ch0 (VOUT, 2 V amplitude, 5 V offset),
    wait 100 ms, then stop.  Verifies no exception is raised.
    """
    _setup_vout_channel(device, 0)
    device.start_waveform(
        0, WaveformType.SINE,
        freq_hz=1000.0, amplitude=2.0, offset=5.0,
        mode=OutputMode.VOLTAGE,
    )
    time.sleep(0.1)
    device.stop_waveform()
    _safe_stop(device, 0)


# ---------------------------------------------------------------------------
# Square wave
# ---------------------------------------------------------------------------

def test_start_stop_wavegen_square(device):
    """
    Start a 100 Hz square wave on ch0 (VOUT, 3 V amplitude, 3 V offset).
    """
    _setup_vout_channel(device, 0)
    device.start_waveform(
        0, WaveformType.SQUARE,
        freq_hz=100.0, amplitude=3.0, offset=3.0,
    )
    time.sleep(0.1)
    device.stop_waveform()
    _safe_stop(device, 0)


# ---------------------------------------------------------------------------
# Triangle wave
# ---------------------------------------------------------------------------

def test_start_stop_wavegen_triangle(device):
    """
    Start a 50 Hz triangle wave on ch0 (VOUT, 4 V amplitude, 4 V offset).
    """
    _setup_vout_channel(device, 0)
    device.start_waveform(
        0, WaveformType.TRIANGLE,
        freq_hz=50.0, amplitude=4.0, offset=4.0,
    )
    time.sleep(0.1)
    device.stop_waveform()
    _safe_stop(device, 0)


# ---------------------------------------------------------------------------
# Sawtooth wave
# ---------------------------------------------------------------------------

def test_start_stop_wavegen_sawtooth(device):
    """
    Start a 200 Hz sawtooth wave on ch0 (VOUT, 1 V amplitude, 1 V offset).
    """
    _setup_vout_channel(device, 0)
    device.start_waveform(
        0, WaveformType.SAWTOOTH,
        freq_hz=200.0, amplitude=1.0, offset=1.0,
    )
    time.sleep(0.1)
    device.stop_waveform()
    _safe_stop(device, 0)


# ---------------------------------------------------------------------------
# Current mode (IOUT)
# ---------------------------------------------------------------------------

def test_wavegen_current_mode(device):
    """
    Start a sine wave in CURRENT mode on ch0 (IOUT, 4 mA amplitude, 12 mA offset).
    This simulates a 4–20 mA loop signal variation.
    """
    _setup_iout_channel(device, 0)
    device.start_waveform(
        0, WaveformType.SINE,
        freq_hz=1.0, amplitude=4.0, offset=12.0,
        mode=OutputMode.CURRENT,
    )
    time.sleep(0.1)
    device.stop_waveform()
    _safe_stop(device, 0)


# ---------------------------------------------------------------------------
# Frequency range
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("freq_hz", [1.0, 100.0, 1000.0, 10000.0])
def test_wavegen_frequency_range(device, freq_hz):
    """
    Try starting a sine wave at various frequencies (1, 100, 1000, 10000 Hz).
    Verifies the firmware accepts a wide frequency range without errors.
    """
    _setup_vout_channel(device, 0)
    device.start_waveform(
        0, WaveformType.SINE,
        freq_hz=freq_hz, amplitude=2.0, offset=5.0,
    )
    time.sleep(0.05)
    device.stop_waveform()
    _safe_stop(device, 0)


# ---------------------------------------------------------------------------
# Stop all channels
# ---------------------------------------------------------------------------

def test_wavegen_stop_all_channels(device):
    """
    Start a waveform on ch0, then stop it.
    Verifies the stop_waveform() call leaves no error state.
    """
    _setup_vout_channel(device, 0)
    device.start_waveform(
        0, WaveformType.SQUARE,
        freq_hz=500.0, amplitude=1.0, offset=2.0,
    )
    time.sleep(0.05)
    device.stop_waveform()

    # Verify device is still responsive after stop
    status = device.get_status()
    assert isinstance(status, dict), "Device should respond after stop_waveform()"
    _safe_stop(device, 0)


# ---------------------------------------------------------------------------
# Multiple waveform types on the same channel
# ---------------------------------------------------------------------------

def test_wavegen_switch_types(device):
    """
    Switch between different waveform types on the same channel.
    Verifies that starting a new waveform after stopping the previous one works.
    """
    _setup_vout_channel(device, 0)

    for wtype in [WaveformType.SINE, WaveformType.SQUARE, WaveformType.TRIANGLE]:
        device.start_waveform(0, wtype, freq_hz=100.0, amplitude=1.0, offset=2.0)
        time.sleep(0.05)
        device.stop_waveform()
        time.sleep(0.02)

    _safe_stop(device, 0)
