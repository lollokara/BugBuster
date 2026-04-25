"""
BugBuster Python Library
========================

Python control library for the BugBuster industrial I/O board.
Supports both USB binary protocol (BBP, low-latency, streaming) and
HTTP REST API (WiFi, no streaming).

Two levels of API
-----------------
**Low-level client** — direct access to all AD74416H functions, MUX
switches, IDAC, PCA9535, GPIO, and UART bridge::

    from bugbuster import connect_usb, ChannelFunction, AdcRate

    with connect_usb("/dev/cu.usbmodem1234561") as bb:
        bb.set_channel_function(0, ChannelFunction.VOUT)
        bb.set_dac_voltage(0, 3.3)
        result = bb.get_adc_value(1)
        print(f"Channel 1: {result.value:.4f} V")

**HAL (Hardware Abstraction Layer)** — Arduino-style port API that
hides the MUX, power sequencing, and register-level configuration
behind simple ``configure() / read() / write()`` calls::

    from bugbuster import connect_usb, PortMode

    with connect_usb("/dev/cu.usbmodem1234561") as bb:
        hal = bb.hal
        hal.begin(supply_voltage=12.0, vlogic=3.3)

        hal.configure(1, PortMode.ANALOG_OUT)
        hal.write_voltage(1, 5.0)

        hal.configure(4, PortMode.ANALOG_IN)
        print(f"IO 4: {hal.read_voltage(4):.4f} V")

        hal.configure(2, PortMode.DIGITAL_OUT)
        hal.write_digital(2, True)

        hal.set_voltage(rail=1, voltage=10.0)
        hal.shutdown()

Hardware overview
-----------------
12 IOs in 2 Blocks x 2 IO_Blocks x 3 IOs:

- **IO 1, 4, 7, 10** — analog-capable (ADC, DAC, 4-20 mA, RTD, HART, HAT)
- **IO 2, 3, 5, 6, 8, 9, 11, 12** — digital only (high/low drive GPIO)
- **VADJ1** (3-15 V) powers IO 1-6; **VADJ2** powers IO 7-12
- **VLOGIC** (1.8-5 V) sets the logic level for all digital IOs

Examples
--------
See the ``examples/`` directory:

- ``01_hello_device.py``  — connect, ping, read status
- ``02_analog_io.py``     — voltage/current/RTD I/O
- ``03_adc_streaming.py`` — high-speed ADC data streaming (USB only)
- ``04_waveform_and_mux.py`` — waveform generator + MUX routing
- ``05_hal_basics.py``    — HAL tutorial with all 12 modes
- ``06_power_management.py`` — USB PD, IDAC, e-fuse, power sequencing
- ``07_digital_io.py``    — ESP32 GPIO digital read/write (USB + HTTP)
"""

from .client    import (
    BugBuster,
    HatNotPresentError,
    HatPinFunctionError,
    connect_usb,
    connect_http,
)
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
from .bus import BugBusterBusManager, BusPlanError, BusRouteEntry, BusRoutePlan

__all__ = [
    # Factory functions
    "connect_usb",
    "connect_http",

    # Main client
    "BugBuster",

    # Errors
    "HatNotPresentError",
    "HatPinFunctionError",

    # HAL
    "BugBusterHAL",
    "PortMode",
    "BugBusterBusManager",
    "BusPlanError",
    "BusRouteEntry",
    "BusRoutePlan",

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
