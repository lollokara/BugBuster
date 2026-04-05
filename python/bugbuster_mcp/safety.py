"""
BugBuster MCP — Safety validation helpers.

All dangerous parameter checks live here so tool handlers stay clean.
Raises ValueError with human-readable messages that the AI can act on.
"""

from __future__ import annotations
from .config import (
    MAX_VADJ_VOLTAGE, MIN_VADJ_VOLTAGE,
    MAX_VLOGIC, MIN_VLOGIC,
    MAX_DAC_VOLTAGE_UNIPOLAR, MAX_DAC_VOLTAGE_BIPOLAR,
    MAX_DAC_CURRENT_MA, MAX_DAC_CURRENT_MA_SAFE,
    VADJ_CONFIRM_THRESHOLD, ANALOG_IOS, ALL_IOS,
)


# ---------------------------------------------------------------------------
# IO validation
# ---------------------------------------------------------------------------

def require_valid_io(io: int) -> None:
    if io not in ALL_IOS:
        raise ValueError(
            f"IO {io} is not valid. BugBuster has IOs 1-12."
        )


def require_analog_io(io: int, operation: str = "this operation") -> None:
    require_valid_io(io)
    if io not in ANALOG_IOS:
        raise ValueError(
            f"IO {io} does not support {operation}. "
            f"Analog modes (voltage, current, RTD) are only available on IOs 1, 4, 7, and 10."
        )


def require_io_mode(hal, io: int, expected_modes, operation: str) -> None:
    """
    Verify the IO is in one of the expected PortMode values.
    hal must be an initialized BugBusterHAL.
    """
    from bugbuster.hal import PortMode
    current = hal._io_mode.get(io)
    if isinstance(expected_modes, (list, tuple, set, frozenset)):
        modes = set(expected_modes)
    else:
        modes = {expected_modes}

    if current not in modes:
        current_name = current.name if current is not None else "UNCONFIGURED"
        needed_names = ", ".join(m.name for m in modes)
        raise ValueError(
            f"IO {io} is in mode {current_name}, but {operation} requires mode {needed_names}. "
            f"Call configure_io first."
        )


def require_hal_initialized(hal) -> None:
    if not hal._powered_up:
        raise RuntimeError(
            "HAL is not initialized. This should not happen — "
            "try calling reset_device to reinitialize."
        )


# ---------------------------------------------------------------------------
# Voltage / current limits
# ---------------------------------------------------------------------------

def validate_vadj_voltage(voltage: float, confirm: bool = False) -> None:
    if voltage < MIN_VADJ_VOLTAGE:
        raise ValueError(
            f"Requested voltage {voltage:.2f} V is below the minimum {MIN_VADJ_VOLTAGE} V. "
            f"The adjustable supplies support {MIN_VADJ_VOLTAGE}-{MAX_VADJ_VOLTAGE} V."
        )
    if voltage > MAX_VADJ_VOLTAGE:
        raise ValueError(
            f"Requested voltage {voltage:.2f} V exceeds the hardware maximum of {MAX_VADJ_VOLTAGE} V."
        )
    if voltage > VADJ_CONFIRM_THRESHOLD and not confirm:
        raise ValueError(
            f"Requested voltage {voltage:.2f} V is above {VADJ_CONFIRM_THRESHOLD} V. "
            f"To proceed, pass confirm=True to acknowledge the higher voltage."
        )


def validate_vlogic(voltage: float) -> None:
    if not (MIN_VLOGIC <= voltage <= MAX_VLOGIC):
        raise ValueError(
            f"VLOGIC {voltage:.2f} V is outside the valid range "
            f"{MIN_VLOGIC}-{MAX_VLOGIC} V (TPS74601 limits)."
        )


def validate_dac_voltage(voltage: float, bipolar: bool = False) -> None:
    if bipolar:
        if abs(voltage) > MAX_DAC_VOLTAGE_BIPOLAR:
            raise ValueError(
                f"DAC voltage {voltage:.3f} V exceeds ±{MAX_DAC_VOLTAGE_BIPOLAR} V bipolar limit."
            )
    else:
        if not (0.0 <= voltage <= MAX_DAC_VOLTAGE_UNIPOLAR):
            raise ValueError(
                f"DAC voltage {voltage:.3f} V is outside the 0-{MAX_DAC_VOLTAGE_UNIPOLAR} V range. "
                f"Use bipolar=True to enable ±12 V mode."
            )


def validate_dac_current(current_ma: float, allow_full_range: bool = False) -> None:
    limit = MAX_DAC_CURRENT_MA if allow_full_range else MAX_DAC_CURRENT_MA_SAFE
    if not (0.0 <= current_ma <= limit):
        raise ValueError(
            f"Current {current_ma:.2f} mA is outside 0-{limit} mA. "
            + (
                ""
                if allow_full_range else
                f"Pass allow_full_range=True to use up to {MAX_DAC_CURRENT_MA} mA."
            )
        )


# ---------------------------------------------------------------------------
# HAT presence
# ---------------------------------------------------------------------------

def require_hat(bb) -> None:
    """
    Raise an informative error if the HAT expansion board is not detected.
    """
    try:
        status = bb.hat_get_status()
        if not status.get("detected"):
            raise RuntimeError(
                "No HAT expansion board detected. "
                "SWD debug probe and logic analyzer require the RP2040 HAT. "
                "Check that the HAT is physically connected."
            )
    except NotImplementedError:
        raise RuntimeError(
            "HAT commands require USB transport. "
            "Connect via USB (--transport usb) for HAT/SWD/logic-analyzer access."
        )


# ---------------------------------------------------------------------------
# Fault check — call after any output-driving operation
# ---------------------------------------------------------------------------

def check_faults_post(bb) -> list[str]:
    """
    Return a list of human-readable fault warnings (empty if clean).
    Does NOT raise — callers include warnings in the response.
    """
    warnings = []
    try:
        status = bb.power_get_status()
        efuse_faults = status.get("efuse_faults", [])
        for i, tripped in enumerate(efuse_faults):
            if tripped:
                warnings.append(
                    f"E-fuse {i + 1} has tripped (overcurrent on IO_Block {i + 1}). "
                    f"The output has been disabled. Check the connected device and reduce load."
                )
        if not status.get("vadj1_pg", True):
            warnings.append("VADJ1 power-good signal lost. Supply 1 may be overloaded.")
        if not status.get("vadj2_pg", True):
            warnings.append("VADJ2 power-good signal lost. Supply 2 may be overloaded.")
    except Exception:
        pass
    return warnings
