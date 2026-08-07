# bugbuster - Python control library

Drive BugBuster hardware from Python: the AD74416H quad-channel software-configurable
IO front end, the MUX matrix, adjustable supplies, digital IOs, external I²C/SPI
buses, and both expansion HATs.

Two APIs over the same connection:

- **`dev`** - the low-level client. One method per hardware operation. Use it
  when you know the hardware.
- **`dev.hal`** - Arduino-style `configure()` / `read()` / `write()` on physical
  IO numbers. It handles MUX routing, power sequencing, level shifters and
  e-fuses for you. Use it for everything else.

## Install

```bash
cd python
pip install -e .
```

`pyusb` is needed only for logic-analyzer USB streaming:
`pip install pyusb`.

## Connect

```python
from bugbuster import connect_usb, ChannelFunction

with connect_usb("/dev/cu.usbmodem1234561") as dev:
    dev.set_channel_function(0, ChannelFunction.VOUT)
    dev.set_dac_voltage(0, 5.0)
    print(f"{dev.get_adc_value(1).value:.4f} V")
```

Over WiFi instead:

```python
from bugbuster import connect_http

with connect_http("192.168.4.1") as dev:
    dev.set_channel_function(0, ChannelFunction.IOUT)
    dev.set_dac_current(0, 12.0)   # mA
```

Both return a `BugBuster` client and work as context managers.

### What HTTP can't do

| Capability | USB | HTTP |
|---|:---:|:---:|
| Channel IO (VOUT, IOUT, VIN, IIN, RTD, DIN) | yes | yes |
| GPIO, MUX, power management, WiFi, waveform generator | yes | yes |
| IDAC, self-test, diagnostics | yes | yes |
| ADC / scope streaming | yes | - |
| Logic analyzer (`hat_la_*`) | yes | - |
| HAT power and IO voltage | yes | - |
| UART bridge config | yes | - |
| Direct register access | yes | - |

Methods unavailable over HTTP raise `NotImplementedError`, and say so in their
docstring.

## The HAL

```python
from bugbuster import connect_usb, PortMode

with connect_usb("/dev/cu.usbmodem1234561") as dev:
    hal = dev.hal
    hal.begin(supply_voltage=12.0, vlogic=3.3)

    hal.configure(3, PortMode.ANALOG_OUT)      # analog: IO 3, 6, 9, 12 only
    hal.write_voltage(3, 5.0)

    hal.configure(6, PortMode.ANALOG_IN)
    print(f"IO 6: {hal.read_voltage(6):.4f} V")

    hal.configure(2, PortMode.DIGITAL_OUT)     # digital: any of the 12 IOs
    hal.write_digital(2, True)

    hal.set_voltage(rail=1, voltage=10.0)      # VADJ1 → IO 1–6
    hal.set_vlogic(3.3)

    hal.shutdown()
```

| Method | Purpose |
|---|---|
| `begin(supply_voltage, vlogic)` | Power-up sequence |
| `configure(io, PortMode.X)` | Set IO mode |
| `write_voltage` / `read_voltage` | Analog out / in |
| `write_current` / `read_current` | 4–20 mA out / in |
| `write_digital` / `read_digital` | Digital out / in |
| `read_resistance` / `read_temperature_pt100` | RTD |
| `set_voltage(rail, v)` / `set_vlogic(v)` | Supply rails |
| `set_serial(tx, rx)` | Route the UART bridge to two IOs |
| `shutdown()` | Safe power-down |

`PortMode`: `DISABLED`, `ANALOG_IN`, `ANALOG_OUT`, `CURRENT_IN`, `CURRENT_OUT`,
`DIGITAL_IN`, `DIGITAL_OUT`, `DIGITAL_IN_LOW`, `DIGITAL_OUT_LOW`, `RTD`, `HART`,
`HAT`.

## External I²C / SPI

`dev.bus` takes physical IO numbers and resolves power, MUX routing, VLOGIC,
level shifters, e-fuses and ESP32 GPIO assignment for you.

```python
with connect_usb("/dev/cu.usbmodem1234561") as dev:
    scan = dev.bus.i2c_scan(sda=1, scl=2, io_voltage=3.3, supply_voltage=3.3)
    print(scan["addresses"])

    dev.bus.setup_spi(sck=1, mosi=2, miso=4, cs=5,
                      io_voltage=3.3, supply_voltage=3.3)
    print(dev.bus.spi_jedec_id())
```

Full guide: [Docs/external-bus.md](../Docs/external-bus.md)

## Logic analyzer

Requires the Logic Analyzer HAT and USB. Streaming runs over the RP2040's own
vendor-bulk endpoint, so throughput is independent of the BBP control link.

```python
with connect_usb("/dev/cu.usbmodem1234561") as dev:
    dev.hat_la_configure(channels=4, rate_hz=1_000_000,
                         depth=100_000, rle_enabled=True)
    data = dev.hat_la_stream_usb_cycle(duration_s=0.5)   # needs pyusb
    print(f"{len(data[0])} samples on CH0")
```

Details, routing options and throughput limits:
[Docs/logic-analyzer.md](../Docs/logic-analyzer.md)

## Hardware model

```
BLOCK 1 - VADJ1 (3–15 V)          BLOCK 2 - VADJ2 (3–15 V)
├── IO_Block 1 · EFUSE1           ├── IO_Block 3 · EFUSE3
│   IO 1, IO 2 digital            │   IO 7, IO 8 digital
│   IO 3 analog / HAT             │   IO 9 analog / HAT
└── IO_Block 2 · EFUSE2           └── IO_Block 4 · EFUSE4
    IO 4, IO 5 digital                IO 10, IO 11 digital
    IO 6 analog / HAT                 IO 12 analog / HAT
```

| IO | Type | Routing options |
|---|---|---|
| 3, 6, 9, 12 | analog-capable | ESP GPIO (high/low drive) · AD74416H channel · HAT passthrough |
| 1, 2, 4, 5, 7, 8, 10, 11 | digital only | ESP GPIO (high drive) · ESP GPIO (low drive) |

| Rail | Range | Covers | Set with |
|---|---|---|---|
| VADJ1 | 3–15 V | IO 1–6 | `hal.set_voltage(1, v)` |
| VADJ2 | 3–15 V | IO 7–12 | `hal.set_voltage(2, v)` |
| VLOGIC | 1.8–5.0 V | all 12 IOs | `hal.set_vlogic(v)` |

VADJ1 and VADJ2 are buck rails: they cannot regulate above the negotiated USB-C
input voltage. Select a high-enough USB-PD profile first, or the library warns
and clamps.

| Subsystem | IC | Role |
|---|---|---|
| Analog IO | AD74416H | 4-ch configurable: voltage/current in and out, RTD, digital, HART |
| MUX matrix | ADGS2414D ×4 | 32 SPST switches routing signals to the 12 physical IOs |
| Adjustable supplies | LTM8063 ×2 | VADJ1 / VADJ2, trimmed by DS4424 OUT1 / OUT2 |
| Logic level | LTM8078 | VLOGIC 1.8–5 V, trimmed by DS4424 OUT0 |
| USB-PD | HUSB238 | Negotiates 5–20 V from a USB-C source |
| GPIO expander | PCA9535 | Power enables, e-fuse control, fault monitoring |
| Level shifters | TXS0108E ×2 | Shift the digital IOs to VLOGIC |
| Serial bridge | ESP32 UART | Configurable UART routed to any two IOs via the MUX |

## API surface

Import from the package root:

```python
from bugbuster import (
    connect_usb, connect_http,          # factories
    BugBuster,                          # client
    BugBusterHAL, PortMode,             # HAL
    BugBusterBusManager,                # external I²C/SPI
    discover, discover_mdns,            # device discovery
    OTAClient,                          # firmware update
    ChannelFunction, AdcRange, AdcRate, AdcMux, GpioMode,
    WaveformType, OutputMode, RtdCurrent, VoutRange,
    CurrentLimit, DoMode, AvddSelect, PowerControl,
    UsbPdVoltage, ErrorCode,
    HatNotPresentError, HatPinFunctionError,
)
```

Client method groups (see docstrings in
[bugbuster/client.py](bugbuster/client.py) for signatures):

| Group | Methods |
|---|---|
| Channels | `set_channel_function`, `set_dac_voltage`, `set_dac_current`, `get_adc_value` |
| Digital IO | `dio_configure`, `dio_read`, `dio_write` |
| MUX | `mux_get`, `mux_set_all`, `mux_set_switch` |
| Power | `power_set`, `power_get_status`, `idac_set_voltage`, `idac_get_status` |
| UART bridge | `get_uart_config`, `set_uart_config`, `get_uart_pins` |
| USB-PD | `usbpd_get_status`, `usbpd_select_voltage` |
| Waveform | `start_waveform`, `stop_waveform` |
| ADC streaming (USB) | `start_adc_stream`, `stop_adc_stream`, `on_scope_data` |
| Logic analyzer (USB) | `hat_la_configure`, `hat_la_set_route`, `hat_la_set_trigger`, `hat_la_arm`, `hat_la_force`, `hat_la_stream_start`, `hat_la_stream_usb_cycle`, `hat_la_stop`, `hat_la_read_all`, `hat_la_decode`, `hat_la_get_status` |

HAT-only calls raise `HatNotPresentError` when no HAT is attached.

## Examples

Runnable scripts in [examples/](examples/):

| File | Covers |
|---|---|
| `01_hello_device.py` | Connect, ping, read device info and status |
| `02_analog_io.py` | Voltage and current in/out, RTD measurement |
| `03_adc_streaming.py` | High-speed ADC streaming (USB only) |
| `04_waveform_and_mux.py` | Waveform generator plus MUX routing |
| `05_hal_basics.py` | HAL tutorial - all 12 IO modes, supplies, serial bridge |
| `06_power_management.py` | USB-PD, IDAC voltage control, e-fuse, power sequencing |
| `07_digital_io.py` | ESP32 GPIO digital read/write over USB and HTTP |

## Protocol

The USB binary protocol is [BBP v10](../Firmware/bbp-protocol.md). The HTTP REST
API mirrors it - every binary command has an equivalent endpoint.

## License

AGPL-3.0, same as the rest of the project. See [LICENSE](../LICENSE).
