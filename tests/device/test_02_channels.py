"""
test_02_channels.py — AD74416H channel configuration and I/O tests.

Covers: VOUT, IOUT, VIN, IIN_EXT, IIN_LOOP, DIN, DO, RTD, ADC config,
DAC readback, voltage/current range, current limit, and channel independence.
"""

import time
import pytest
import bugbuster as bb
from conftest import assert_no_faults
from bugbuster import (
    ChannelFunction,
    AdcRange, AdcRate, AdcMux,
    VoutRange, CurrentLimit,
    RtdCurrent,
)

pytestmark = [pytest.mark.timeout(10)]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _safe_high_imp(device, *channels):
    """Put specified channels to HIGH_IMP; ignore errors during teardown."""
    for ch in channels:
        try:
            device.set_channel_function(ch, ChannelFunction.HIGH_IMP)
        except Exception:
            pass


# ---------------------------------------------------------------------------
# HIGH_IMP
# ---------------------------------------------------------------------------

def test_set_high_imp(device):
    """
    Set all 4 channels to HIGH_IMP mode and verify the status reflects it.
    HIGH_IMP is the safe default state (function code = 0).
    """
    for ch in range(4):
        device.set_channel_function(ch, ChannelFunction.HIGH_IMP)

    status = device.get_status()
    for i, ch_status in enumerate(status["channels"]):
        assert ch_status.get("function") == int(ChannelFunction.HIGH_IMP), (
            f"Channel {i} function should be HIGH_IMP after set, got {ch_status.get('function')}"
        )

    assert_no_faults(device)


# ---------------------------------------------------------------------------
# VOUT
# ---------------------------------------------------------------------------

def test_vout_set_and_readback(device):
    """
    Configure ch0 to VOUT, set 5.0 V, and verify get_dac_readback() returns a non-zero code.
    The DAC code for 5.0 V on a 0–12 V range should be approximately 27307.
    """
    device.set_channel_function(0, ChannelFunction.HIGH_IMP)
    device.set_channel_function(0, ChannelFunction.VOUT)
    device.set_vout_range(0, VoutRange.UNIPOLAR)
    device.set_dac_voltage(0, 5.0)
    time.sleep(0.05)

    code = device.get_dac_readback(0)
    assert isinstance(code, int), f"DAC readback must be int, got {type(code)}"
    assert code != 0, "DAC readback code should be non-zero after setting 5.0 V"
    assert 0 < code <= 0xFFFF, f"DAC code out of range: {code:#06x}"

    _safe_high_imp(device, 0)
    assert_no_faults(device)


def test_vout_range_unipolar(device):
    """Set ch0 to VOUT with UNIPOLAR range (0–12 V). Should not raise."""
    device.set_channel_function(0, ChannelFunction.HIGH_IMP)
    device.set_channel_function(0, ChannelFunction.VOUT)
    device.set_vout_range(0, VoutRange.UNIPOLAR)
    # 3.3 V is well within unipolar range
    device.set_dac_voltage(0, 3.3)
    time.sleep(0.05)
    code = device.get_dac_readback(0)
    assert code > 0, "DAC code should be > 0 for 3.3 V unipolar"
    _safe_high_imp(device, 0)
    assert_no_faults(device)


def test_vout_range_bipolar(device):
    """Set ch0 to VOUT with BIPOLAR range (±12 V). Should not raise."""
    device.set_channel_function(0, ChannelFunction.HIGH_IMP)
    device.set_channel_function(0, ChannelFunction.VOUT)
    device.set_vout_range(0, VoutRange.BIPOLAR)
    device.set_dac_voltage(0, 0.0, bipolar=True)  # mid-scale
    time.sleep(0.05)
    code = device.get_dac_readback(0)
    assert code is not None
    _safe_high_imp(device, 0)
    assert_no_faults(device)


def test_set_dac_code_raw(device):
    """
    Write a raw 16-bit DAC code (0x8000 = mid-scale) to ch0 in VOUT mode.
    Verifies the raw code path works independently of voltage conversion.
    """
    device.set_channel_function(0, ChannelFunction.HIGH_IMP)
    device.set_channel_function(0, ChannelFunction.VOUT)
    device.set_dac_code(0, 0x8000)
    time.sleep(0.05)
    code = device.get_dac_readback(0)
    assert code is not None
    assert 0 <= code <= 0xFFFF, f"DAC readback out of range: {code}"
    _safe_high_imp(device, 0)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# IOUT
# ---------------------------------------------------------------------------

def test_iout_set_and_readback(device):
    """
    Configure ch0 to IOUT and set 12.0 mA (mid-scale for a 4–20 mA loop).
    Verifies the command does not raise.
    """
    device.set_channel_function(0, ChannelFunction.HIGH_IMP)
    device.set_channel_function(0, ChannelFunction.IOUT)
    device.set_dac_current(0, 12.0)
    time.sleep(0.05)
    _safe_high_imp(device, 0)
    assert_no_faults(device)


def test_current_limit_25ma(device):
    """
    Set current limit to MA_25 (full range) on ch0 in IOUT mode.
    Should not raise.
    """
    device.set_channel_function(0, ChannelFunction.HIGH_IMP)
    device.set_channel_function(0, ChannelFunction.IOUT)
    device.set_current_limit(0, CurrentLimit.MA_25)
    time.sleep(0.02)
    _safe_high_imp(device, 0)
    assert_no_faults(device)


def test_current_limit_8ma(device):
    """
    Set current limit to MA_8 (restricted range) on ch0 in IOUT mode.
    Should not raise.
    """
    device.set_channel_function(0, ChannelFunction.HIGH_IMP)
    device.set_channel_function(0, ChannelFunction.IOUT)
    device.set_current_limit(0, CurrentLimit.MA_8)
    time.sleep(0.02)
    _safe_high_imp(device, 0)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# VIN
# ---------------------------------------------------------------------------

def test_vin_configure(device):
    """
    Configure ch1 to VIN with AdcRange.V_0_12 and default mux.
    Should not raise.
    """
    device.set_channel_function(1, ChannelFunction.HIGH_IMP)
    device.set_channel_function(1, ChannelFunction.VIN)
    device.set_adc_config(
        1,
        mux=AdcMux.LF_TO_AGND,
        range_=AdcRange.V_0_12,
        rate=AdcRate.SPS_200_H,
    )
    _safe_high_imp(device, 1)
    assert_no_faults(device)


def test_adc_config_all_rates(device):
    """
    Cycle through all AdcRate values on ch1 in VIN mode.
    Verifies each rate setting is accepted without error.
    """
    device.set_channel_function(1, ChannelFunction.HIGH_IMP)
    device.set_channel_function(1, ChannelFunction.VIN)

    for rate in AdcRate:
        device.set_adc_config(1, rate=rate)

    _safe_high_imp(device, 1)
    assert_no_faults(device)


def test_adc_config_all_ranges(device):
    """
    Cycle through all AdcRange values on ch1 in VIN mode.
    Verifies each range setting is accepted without error.
    """
    device.set_channel_function(1, ChannelFunction.HIGH_IMP)
    device.set_channel_function(1, ChannelFunction.VIN)

    for rng in AdcRange:
        device.set_adc_config(1, range_=rng)

    _safe_high_imp(device, 1)
    assert_no_faults(device)


def test_get_adc_value(device):
    """
    Configure ch1 to VIN and call get_adc_value().
    Verifies the result is an AdcResult namedtuple with a float 'value' field.
    """
    device.set_channel_function(1, ChannelFunction.HIGH_IMP)
    device.set_channel_function(1, ChannelFunction.VIN)
    device.set_adc_config(1, range_=AdcRange.V_0_12, rate=AdcRate.SPS_200_H)
    time.sleep(0.2)  # wait for ADC to settle

    result = device.get_adc_value(1)

    assert hasattr(result, "value"), "AdcResult missing 'value' field"
    assert isinstance(result.value, float), f"AdcResult.value must be float, got {type(result.value)}"
    assert hasattr(result, "raw"), "AdcResult missing 'raw' field"
    assert hasattr(result, "range"), "AdcResult missing 'range' field"

    _safe_high_imp(device, 1)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# IIN modes
# ---------------------------------------------------------------------------

def test_iin_ext_configure(device):
    """Configure ch2 to IIN_EXT_PWR (externally powered 4–20 mA). Should not raise."""
    device.set_channel_function(2, ChannelFunction.HIGH_IMP)
    device.set_channel_function(2, ChannelFunction.IIN_EXT_PWR)
    time.sleep(0.05)
    _safe_high_imp(device, 2)
    assert_no_faults(device)


def test_iin_loop_configure(device):
    """Configure ch2 to IIN_LOOP_PWR (loop-powered 4–20 mA). Should not raise."""
    device.set_channel_function(2, ChannelFunction.HIGH_IMP)
    device.set_channel_function(2, ChannelFunction.IIN_LOOP_PWR)
    time.sleep(0.05)
    _safe_high_imp(device, 2)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# DIN
# ---------------------------------------------------------------------------

def test_din_logic_configure(device):
    """
    Configure ch3 to DIN_LOGIC and apply default din_config settings.
    Should not raise.
    """
    device.set_channel_function(3, ChannelFunction.HIGH_IMP)
    device.set_channel_function(3, ChannelFunction.DIN_LOGIC)
    device.set_din_config(3, threshold=64, debounce=5)
    _safe_high_imp(device, 3)
    assert_no_faults(device)


def test_din_logic_validate_threshold_range(device):
    """
    set_din_config with threshold=300 (>255) should raise ValueError.
    The valid range is 0–255.
    """
    device.set_channel_function(3, ChannelFunction.HIGH_IMP)
    device.set_channel_function(3, ChannelFunction.DIN_LOGIC)

    with pytest.raises(ValueError, match="threshold"):
        device.set_din_config(3, threshold=300)

    _safe_high_imp(device, 3)
    assert_no_faults(device)


def test_din_debounce_validate_range(device):
    """
    set_din_config with debounce=50 (>31) should raise ValueError.
    The valid range is 0–31.
    """
    device.set_channel_function(3, ChannelFunction.HIGH_IMP)
    device.set_channel_function(3, ChannelFunction.DIN_LOGIC)

    with pytest.raises(ValueError, match="debounce"):
        device.set_din_config(3, threshold=64, debounce=50)

    _safe_high_imp(device, 3)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# RTD
# ---------------------------------------------------------------------------

def test_rtd_configure(device):
    """
    Configure ch0 to RES_MEAS (RTD) mode with 500 µA excitation.
    Should not raise.
    """
    device.set_channel_function(0, ChannelFunction.HIGH_IMP)
    device.set_channel_function(0, ChannelFunction.RES_MEAS)
    device.set_rtd_config(0, current=RtdCurrent.UA_500)
    time.sleep(0.05)
    _safe_high_imp(device, 0)
    assert_no_faults(device)


def test_rtd_configure_1ma(device):
    """Configure ch0 to RES_MEAS with 1 mA excitation (default for PT100)."""
    device.set_channel_function(0, ChannelFunction.HIGH_IMP)
    device.set_channel_function(0, ChannelFunction.RES_MEAS)
    device.set_rtd_config(0, current=RtdCurrent.MA_1)
    time.sleep(0.05)
    _safe_high_imp(device, 0)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# DO
# ---------------------------------------------------------------------------

def test_do_configure(device):
    """
    Configure ch3 to DIN_LOGIC and toggle the digital output state.
    Verifies set_digital_output() works without raising.
    """
    device.set_channel_function(3, ChannelFunction.HIGH_IMP)
    device.set_channel_function(3, ChannelFunction.DIN_LOGIC)
    device.set_digital_output(3, True)
    time.sleep(0.02)
    device.set_digital_output(3, False)
    _safe_high_imp(device, 3)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Safe channel switching
# ---------------------------------------------------------------------------

def test_channel_switch_via_high_imp(device):
    """
    Perform a safe VOUT → HIGH_IMP → IOUT transition on ch0.
    Always pass through HIGH_IMP when switching between incompatible functions
    to avoid voltage/current glitches.
    """
    # Step 1: VOUT
    device.set_channel_function(0, ChannelFunction.HIGH_IMP)
    device.set_channel_function(0, ChannelFunction.VOUT)
    device.set_dac_voltage(0, 3.0)
    time.sleep(0.05)

    # Step 2: HIGH_IMP (safe intermediate state)
    device.set_channel_function(0, ChannelFunction.HIGH_IMP)
    time.sleep(0.02)

    # Step 3: IOUT
    device.set_channel_function(0, ChannelFunction.IOUT)
    device.set_dac_current(0, 4.0)
    time.sleep(0.05)

    _safe_high_imp(device, 0)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# All channels independent
# ---------------------------------------------------------------------------

def test_all_channels_independent(device):
    """
    Configure all 4 channels differently simultaneously and verify status
    reflects each channel's individual function setting.
    """
    # Set up all channels
    device.set_channel_function(0, ChannelFunction.HIGH_IMP)
    device.set_channel_function(0, ChannelFunction.VOUT)
    device.set_channel_function(1, ChannelFunction.HIGH_IMP)
    device.set_channel_function(1, ChannelFunction.VIN)
    device.set_channel_function(2, ChannelFunction.HIGH_IMP)
    device.set_channel_function(2, ChannelFunction.IIN_EXT_PWR)
    device.set_channel_function(3, ChannelFunction.HIGH_IMP)
    device.set_channel_function(3, ChannelFunction.DIN_LOGIC)
    time.sleep(0.1)

    status = device.get_status()
    funcs = {ch["id"]: ch["function"] for ch in status["channels"]}

    assert funcs.get(0) == int(ChannelFunction.VOUT), f"ch0 should be VOUT, got {funcs.get(0)}"
    assert funcs.get(1) == int(ChannelFunction.VIN), f"ch1 should be VIN, got {funcs.get(1)}"
    assert funcs.get(2) == int(ChannelFunction.IIN_EXT_PWR), f"ch2 should be IIN_EXT_PWR, got {funcs.get(2)}"
    assert funcs.get(3) == int(ChannelFunction.DIN_LOGIC), f"ch3 should be DIN_LOGIC, got {funcs.get(3)}"

    _safe_high_imp(device, 0, 1, 2, 3)
    assert_no_faults(device)
