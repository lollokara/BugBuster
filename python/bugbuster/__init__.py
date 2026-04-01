"""
BugBuster Python Library
========================

Python control library for the BugBuster AD74416H industrial I/O device.
Supports both USB binary protocol (low-latency, full-speed streaming) and
HTTP REST API (WiFi, no streaming).

Quick start::

    from bugbuster import connect_usb, connect_http
    from bugbuster import ChannelFunction, AdcRate, WaveformType

    # ── USB ──────────────────────────────────────────────────────────
    with connect_usb("/dev/ttyACM0") as bb:
        bb.reset()
        bb.set_channel_function(0, ChannelFunction.VOUT)
        bb.set_dac_voltage(0, 3.3)
        result = bb.get_adc_value(1)
        print(f"Channel 1: {result.value:.4f} V")

    # ── HTTP (WiFi) ───────────────────────────────────────────────────
    with connect_http("192.168.4.1") as bb:
        bb.set_channel_function(0, ChannelFunction.IOUT)
        bb.set_dac_current(0, 12.0)   # 12 mA
"""

from .client    import BugBuster, connect_usb, connect_http
from .constants import (
    ChannelFunction,
    AdcRange, AdcRate, AdcMux,
    GpioMode,
    WaveformType, OutputMode,
    RtdCurrent,
    VoutRange,
    CurrentLimit,
    DoMode,
    AvddSelect,
    PowerControl,
    UsbPdVoltage,
    ErrorCode,
)
from .transport import USBTransport, HTTPTransport
from .hal import BugBusterHAL, PortMode

__all__ = [
    # Factory functions
    "connect_usb",
    "connect_http",

    # Main client
    "BugBuster",

    # HAL
    "BugBusterHAL",
    "PortMode",

    # Transports (for advanced use)
    "USBTransport",
    "HTTPTransport",

    # Enums
    "ChannelFunction",
    "AdcRange",
    "AdcRate",
    "AdcMux",
    "GpioMode",
    "WaveformType",
    "OutputMode",
    "RtdCurrent",
    "VoutRange",
    "CurrentLimit",
    "DoMode",
    "AvddSelect",
    "PowerControl",
    "UsbPdVoltage",
    "ErrorCode",
]

__version__ = "0.1.0"
