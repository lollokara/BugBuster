<p align="center">
  <img src="DesktopApp/BugBuster/src-tauri/icons/master_1024.png" width="120" alt="BugBuster logo"/>
</p>

<h1 align="center">BugBuster</h1>

<p align="center">
  Open-source, four-channel analog/digital I/O debug and programming tool<br/>
  built around the <strong>Analog Devices AD74416H</strong> and an <strong>ESP32-S3</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/license-MIT-blue" alt="License"/>
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-lightgrey" alt="Platform"/>
  <img src="https://img.shields.io/badge/firmware-ESP--IDF%20%2B%20PlatformIO-red" alt="Firmware"/>
  <img src="https://img.shields.io/badge/desktop-Tauri%20v2%20%2B%20Leptos%200.7-orange" alt="Desktop"/>
  <img src="https://img.shields.io/badge/hardware-Altium%20Designer-blue" alt="Hardware"/>
  <img src="https://img.shields.io/badge/python-3.10%2B-yellow" alt="Python"/>
</p>

---

<p align="center">
  <img src="Docs/Images/ScreenPCB.png" alt="BugBuster PCB" width="700"/>
</p>

---

## Table of Contents

- [What It Does](#what-it-does)
- [Screenshots](#screenshots)
- [Architecture](#architecture)
- [Hardware](#hardware)
- [Python Library](#python-library)
- [Repository Structure](#repository-structure)
- [Getting Started](#getting-started)
- [Communication](#communication)
- [Desktop App](#desktop-app)
- [Partition Table](#partition-table)
- [License](#license)

---

## What It Does

BugBuster turns a single USB-C connection into a versatile bench instrument. One board replaces a collection of lab equipment for embedded systems testing and field instrument prototyping:

| Capability | Details |
|---|---|
| **4-ch ADC** | 24-bit, up to 4.8 kSPS per channel, multiple voltage/current ranges |
| **4-ch DAC** | 16-bit voltage (0–11 V / bipolar) and current (0–25 mA) output |
| **Waveform generator** | Sine, square, triangle, sawtooth up to 100 Hz |
| **12 Digital IOs** | ESP32 GPIO-based, level-shifted to configurable VLOGIC (1.8–5 V), MUX-routed |
| **Resistance measurement** | 2/3/4-wire RTD measurement with configurable 125 µA / 250 µA excitation |
| **32-switch MUX matrix** | 4× ADGS2414D octal SPST switches for flexible signal routing |
| **Adjustable power supplies** | DS4424 IDAC tunes LTM8063/LTM8078 DCDC output voltages (3–15 V) |
| **USB Power Delivery** | HUSB238 negotiates 5–20 V from USB-C source |
| **WiFi AP + STA** | Built-in web UI, REST API, OTA firmware updates |
| **Real-time oscilloscope** | ADC streaming with scope bucket aggregation, BBSC binary recording |

---

## Screenshots

<table>
  <tr>
    <td align="center">
      <img src="Docs/Images/ScreenHome.png" alt="Device discovery" width="420"/>
      <br/><sub><b>Device Discovery</b> — auto-detects BugBuster over USB or WiFi</sub>
    </td>
    <td align="center">
      <img src="Docs/Images/ScreenHome2.png" alt="Overview tab" width="420"/>
      <br/><sub><b>Overview</b> — live 4-channel readings, SPI health, temperature</sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <img src="Docs/Images/ScreenHome3.png" alt="Waveform generator" width="420"/>
      <br/><sub><b>Waveform Generator</b> — sine, square, triangle, sawtooth with live preview</sub>
    </td>
    <td align="center">
      <img src="Docs/Images/ScreenHome4.png" alt="Signal path / MUX matrix" width="420"/>
      <br/><sub><b>Signal Path</b> — interactive 32-switch MUX matrix visualization</sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <img src="Docs/Images/ScreenHome5.png" alt="Voltages tab" width="420"/>
      <br/><sub><b>Voltages</b> — DCDC voltage adjustment with calibration curves</sub>
    </td>
    <td align="center">
      <img src="Docs/Images/ScreenHome6.png" alt="UART bridge" width="420"/>
      <br/><sub><b>UART Bridge</b> — configurable baud, pins, data format passthrough</sub>
    </td>
  </tr>
  <tr>
    <td align="center" colspan="2">
      <img src="Docs/Images/ScreenHome7.png" alt="Diagnostics tab" width="420"/>
      <br/><sub><b>Diagnostics</b> — internal supply monitoring, fault alerts, OTA firmware update</sub>
    </td>
  </tr>
</table>

---

## Architecture

```mermaid
flowchart TB
  USB["USB-C\n(power + data)"] --> ESP32

  subgraph ESP32["ESP32-S3 (dual-core)"]
    direction LR
    C0["Core 0\nWiFi · HTTP · CLI\nUSB CDC · BBP"]
    C1["Core 1\nADC polling · DAC\nFault monitor"]
  end

  subgraph SPI["SPI Bus"]
    ADC["AD74416H\n4-ch Software-\nConfigurable I/O"]
    MUX["ADGS2414D ×4\n32-Switch MUX"]
  end

  subgraph I2C["I2C Bus"]
    IDAC["DS4424\nIDAC"]
    PCA["PCA9535\nGPIO Expander"]
    PD["HUSB238\nUSB PD"]
  end

  ESP32 --> SPI
  ESP32 --> I2C

  ESP32 -->|"USB CDC #0\n(BBP binary)"| APP["Tauri Desktop App\nRust + Leptos WASM\n18 tabs · real-time scope"]
  ESP32 -->|"WiFi AP/STA\n(HTTP REST)"| APP
  ESP32 -->|"Python Library\nUSB + HTTP"| PY["Python Client\n+ HAL"]
```

### FreeRTOS Task Layout

| Task | Core | Priority | Purpose |
|---|---|---|---|
| `taskAdcPoll` | 1 | 3 | Read ADC results, convert to engineering units, accumulate scope buckets |
| `taskFaultMonitor` | 1 | 4 | Alert/fault status, DIN counters, GPIO, supply diagnostics |
| `taskCommandProcessor` | 1 | 2 | Dequeue and execute hardware commands (channel func, DAC, config) |
| `taskI2cPoll` | 1 | 1 | Poll DS4424 / HUSB238 / PCA9535 state |
| `taskWavegen` | 1 | 3 | Generate waveform samples, write DAC codes at target frequency |
| `mainLoopTask` | 0 | 1 | CLI input, BBP handshake, binary protocol, heartbeat |

---

## Hardware

The PCB is designed in **Altium Designer**. Schematics and layout are in [`PCB Material/`](PCB%20Material/).

### Key ICs

| IC | Function | Interface |
|---|---|---|
| **AD74416H** | 4-ch software-configurable I/O — 24-bit ADC, 16-bit DAC | SPI (up to 20 MHz) |
| **ADGS2414D ×4** | 32-switch SPST analog MUX matrix | SPI (daisy-chain via level shifter) |
| **DS4424** | 4-ch IDAC — adjusts LTM8063/LTM8078 feedback network | I2C (0x10) |
| **HUSB238** | USB-C PD sink controller (5–20 V negotiation) | I2C (0x08) |
| **PCA9535AHF** | 16-bit GPIO expander — power enables, e-fuse control | I2C (0x23) |
| **LTM8063 ×2** | Adjustable step-down DCDC (3–15 V, 2 A) | Analog (FB pin) |
| **LTM8078** | Level-shifter DCDC | Analog (FB pin) |
| **TPS1641x ×4** | E-fuse / current limiters per output port | GPIO enable |

### Pin Assignments (ESP32-S3)

**SPI Bus:**

| Signal | GPIO | Notes |
|---|---|---|
| MISO (SDO) | 8 | From AD74416H |
| MOSI (SDI) | 9 | To AD74416H |
| CS (SYNC) | 10 | AD74416H chip select, active-low |
| SCLK | 11 | 10 MHz default, up to 20 MHz |
| MUX_CS | 12 | ADGS2414D chip select |
| LSHIFT_OE | 14 | Level-shifter output enable |

**AD74416H Control:**

| Signal | GPIO | Notes |
|---|---|---|
| RESET | 5 | Active-low hardware reset |
| ADC_RDY | 6 | Open-drain — ADC conversion ready |
| ALERT | 7 | Open-drain — fault output |

**I2C Bus (shared):**

| Signal | GPIO |
|---|---|
| SDA | 1 |
| SCL | 4 |

### Power Topology

```
USB-C → HUSB238 (PD negotiation, default 20 V)
    └── LTM8063 ×2 → V_ADJ1 / V_ADJ2 (3–15 V, DS4424-tuned)
              └── TPS1641x ×4 → P1..P4 output ports (e-fuse protected)
```

PCA9535 controls all power enables (`VADJ1_EN`, `VADJ2_EN`, `EN_15V`, `EFUSE_EN_1-4`) and monitors power-good / fault signals.

### IO Architecture

The board has **12 physical IOs** organized into 2 Blocks, each with 2 IO_Blocks of 3 IOs:

```mermaid
block-beta
  columns 2

  block:BLOCK1["BLOCK 1 — VADJ1 (3–15 V)"]:2
    columns 2
    block:IB1["IO_Block 1 · EFUSE1 · MUX U10"]
      IO1["IO 1 ⚡ analog / HAT · Ch A"]
      IO2["IO 2 · digital"]
      IO3["IO 3 · digital"]
    end
    block:IB2["IO_Block 2 · EFUSE2 · MUX U11"]
      IO4["IO 4 ⚡ analog / HAT · Ch B"]
      IO5["IO 5 · digital"]
      IO6["IO 6 · digital"]
    end
  end

  block:BLOCK2["BLOCK 2 — VADJ2 (3–15 V)"]:2
    columns 2
    block:IB3["IO_Block 3 · EFUSE3 · MUX U16"]
      IO7["IO 7 ⚡ analog / HAT · Ch C"]
      IO8["IO 8 · digital"]
      IO9["IO 9 · digital"]
    end
    block:IB4["IO_Block 4 · EFUSE4 · MUX U17"]
      IO10["IO 10 ⚡ analog / HAT · Ch D"]
      IO11["IO 11 · digital"]
      IO12["IO 12 · digital"]
    end
  end
```

Each IO is routed through one ADGS2414D octal switch — options are **MUX-exclusive** (one function at a time):

| IO | Capabilities | MUX Options |
|----|-------------|-------------|
| **1, 4, 7, 10** | Analog + Digital | ESP GPIO (high/low drive) · AD74416H channel · HAT passthrough |
| **2, 3, 5, 6, 8, 9, 11, 12** | Digital only | ESP GPIO (high drive) · ESP GPIO (low drive) |

**VLOGIC** (1.8–5 V, controlled by IDAC ch 0 via TPS74601) sets the logic level for all digital IOs through TXS0108E level shifters.

---

## Python Library

A full-featured Python control library lives in [`python/`](python/). It supports both USB and HTTP transports and provides two API levels:

### Low-Level Client — direct hardware access

```python
import bugbuster as bb
from bugbuster import ChannelFunction

with bb.connect_usb("/dev/cu.usbmodem1234561") as dev:
    dev.set_channel_function(0, ChannelFunction.VOUT)
    dev.set_dac_voltage(0, 5.0)
    print(dev.get_adc_value(1))
```

### HAL — Arduino-style port API

```python
from bugbuster import PortMode

with bb.connect_usb("/dev/cu.usbmodem1234561") as dev:
    hal = dev.hal
    hal.begin(supply_voltage=12.0, vlogic=3.3)

    hal.configure(1, PortMode.ANALOG_OUT)
    hal.write_voltage(1, 5.0)

    hal.configure(2, PortMode.DIGITAL_OUT)
    hal.write_digital(2, True)

    hal.set_voltage(rail=1, voltage=10.0)
    hal.shutdown()
```

### Digital IO — direct ESP32 GPIO control

```python
with bb.connect_usb("/dev/cu.usbmodem1234561") as dev:
    dev.dio_configure(1, 2)     # IO 1 → output
    dev.dio_write(1, True)      # HIGH
    dev.dio_configure(2, 1)     # IO 2 → input
    print(dev.dio_read(2))      # read level
```

See [`python/README.md`](python/README.md) for installation, full API reference, and 7 annotated examples.

---

## Repository Structure

```
BugBuster/
├── Firmware/
│   ├── esp32_ad74416h/         ESP-IDF firmware (PlatformIO)
│   │   ├── src/                34 source files (drivers, protocol, webserver)
│   │   ├── data/               SPIFFS web UI (Alpine.js + Tailwind)
│   │   └── partitions.csv      A/B OTA partition table
│   ├── BugBusterProtocol.md    BBP protocol specification (v1.5)
│   └── FirmwareStructure.md    Firmware reference
│
├── DesktopApp/
│   └── BugBuster/              Tauri v2 + Leptos 0.7 desktop application
│       ├── src/                Leptos WASM frontend (18 tab modules)
│       ├── src-tauri/          Rust backend (transport, commands, state)
│       └── styles.css          Glass UI theme
│
├── python/
│   ├── bugbuster/              Python control library (USB + HTTP)
│   │   ├── client.py           High-level BugBuster client
│   │   ├── hal.py              Hardware Abstraction Layer (12-IO port API)
│   │   ├── constants.py        Enums and protocol constants
│   │   ├── transport/          USB binary + HTTP REST transports
│   │   └── protocol.py         COBS/CRC codec
│   ├── examples/               7 annotated example scripts
│   └── README.md               Python library documentation
│
├── PCB Material/               Altium Designer schematics + PCB layout
│   ├── ARCHITECTURE.md         Full system architecture document
│   └── BugBuster.pdf           Schematic export
│
├── Docs/                       Component datasheets + screenshots
├── Libs/                       Altium component libraries
├── Notebooks/                  Jupyter notebooks (flash, build, setup)
└── Scripts/                    Test scripts (SPI clock sweep, etc.)
```

---

## Getting Started

### Prerequisites

| Tool | Version | Purpose |
|---|---|---|
| [PlatformIO](https://platformio.org/) | 6.x | Firmware build + flash |
| [Rust](https://rustup.rs/) | 1.75+ | Desktop app backend |
| [Trunk](https://trunkrs.dev/) | 0.21+ | WASM frontend build |
| [Node.js](https://nodejs.org/) | 18+ | Tauri CLI |

### 1. Flash Firmware

```bash
cd Firmware/esp32_ad74416h

# Build and flash (ESP32-S3 must be in boot mode)
pio run -e esp32s3 -t upload

# Flash web UI assets (SPIFFS)
pio run -e esp32s3 -t uploadfs

# Serial monitor
pio device monitor -b 115200
```

> The upload port is set in `platformio.ini` (default `COM10` on Windows). Adjust to your system or remove it to let PlatformIO auto-detect.

### 2. Build Desktop App

```bash
# One-time toolchain setup
rustup target add wasm32-unknown-unknown
cargo install trunk
cargo install tauri-cli

cd DesktopApp/BugBuster

# Development mode (hot-reload)
cargo tauri dev

# Release build
cargo tauri build
```

### 3. Python Library

```bash
cd python
pip install pyserial requests
pip install -e .

# Run an example
cd examples
python 07_digital_io.py
```

### 4. WiFi Access

After flashing, the device broadcasts a WiFi AP:

| Setting | Value |
|---|---|
| SSID | `BugBuster` |
| Password | `bugbuster123` |
| IP | `192.168.4.1` |
| Web UI | `http://192.168.4.1` |
| REST API | `http://192.168.4.1/api/status` |

The desktop app auto-discovers the device over USB (preferred) or WiFi.

### 5. OTA Firmware Update

After the initial USB flash, updates can be pushed wirelessly:

1. Connect the device to WiFi (Diagnostics tab)
2. Build new firmware: `pio run -e esp32s3`
3. In the app: **Diagnostics → Firmware → OTA Update** → select `firmware.bin`

---

## Communication

The device supports two transports, both abstracted behind a `Transport` trait so the desktop app works identically over either:

| Transport | Protocol | Latency | Use Case |
|---|---|---|---|
| **USB CDC** | BBP (COBS + CRC-16) | < 1 ms | Low-latency, streaming, full control |
| **WiFi HTTP** | REST API (JSON) | ~10 ms | Remote access, OTA updates |

### BBP Frame Format

```
[COBS-encoded content][0x00 frame delimiter]

Raw pre-COBS layout:
  [msg_type: 1 B][seq: 2 B LE][cmd_id: 1 B][payload: 0–N B][CRC16-CCITT: 2 B LE]
```

- **Handshake:** host sends `0xBB 0x42 0x55 0x47`; device responds with magic + firmware version
- **COBS** removes all `0x00` bytes from the payload; `0x00` is the exclusive frame delimiter
- **CRC-16/CCITT** (poly `0x1021`, init `0xFFFF`) covers all bytes before the CRC field

### BBP Command Groups

| Range | Group |
|---|---|
| `0x01–0x04` | Status / Info |
| `0x10–0x1C` | Channel Config (function, DAC, ADC) |
| `0x20–0x23` | Fault management |
| `0x40–0x42` | GPIO (AD74416H pins A–F) |
| `0x43–0x46` | Digital IO (12 ESP32 GPIOs) |
| `0x50–0x52` | UART bridge |
| `0x60–0x63` | ADC + scope streaming |
| `0x90–0x92` | MUX matrix |
| `0xA0–0xA6` | DS4424 IDAC + calibration |
| `0xB0–0xB2` | PCA9535 I/O expander |
| `0xC0–0xC2` | USB Power Delivery |
| `0xD0–0xD1` | Waveform generator |
| `0xFE–0xFF` | Ping / Disconnect |

See [BugBusterProtocol.md](Firmware/BugBusterProtocol.md) for the full specification.

---

## Desktop App

The desktop app is built with **Tauri v2** (Rust backend) and **Leptos 0.7** (WASM frontend). Device state is polled at 5 Hz over BBP/HTTP and pushed to the UI via Tauri events.

### Tabs

| Tab | Function |
|---|---|
| **Overview** | Status dashboard — SPI health, temperature, all channel summaries |
| **ADC** | 4-channel ADC readings with range / rate / mux config |
| **Diagnostics** | Internal supplies, alerts, firmware info, WiFi, OTA |
| **VDAC** | Voltage DAC output control (unipolar / bipolar) |
| **IDAC** | Current DAC output control |
| **IIN** | Current input monitoring (4–20 mA loop) |
| **DIN / DOUT** | Digital I/O configuration and control |
| **Faults** | Alert register viewer with per-channel detail |
| **GPIO** | AD74416H GPIO configuration (pins A–F) |
| **UART** | UART bridge configuration (baud, pins, format) |
| **Scope** | Real-time oscilloscope with BBSC binary + CSV recording |
| **WaveGen** | Waveform generator (sine, square, triangle, sawtooth) |
| **Signal Path** | Interactive MUX switch matrix visualization |
| **Voltages** | DCDC voltage adjustment with calibration |
| **Calibration** | DS4424 IDAC calibration wizard (NVS-persisted) |
| **USB PD** | USB Power Delivery status and PDO selection |
| **IO Expander** | PCA9535 GPIO expander control and fault monitoring |

### State Flow

```
Firmware                    Tauri Backend                 Leptos Frontend
--------                    -------------                 ---------------
g_deviceState               ConnectionManager             Leptos signals
(FreeRTOS mutex)            polls GET_STATUS @ 5 Hz       (reactive / WASM)
      |                           |                              |
      +--- BBP/HTTP response ---->+                              |
                                  +--- emit("device-state") --->+
                                  +<-- invoke("set_dac_voltage")-+
                                  |                              |
      +<-- BBP CMD / HTTP POST ---+                              |
```

---

## Partition Table

The ESP32-S3 uses A/B OTA partitions for safe wireless updates. NVS data (WiFi credentials, DS4424 calibration) is preserved across OTA updates.

| Partition | Type | Offset | Size | Purpose |
|---|---|---|---|---|
| nvs | data | `0x9000` | 20 KB | Settings, WiFi creds, calibration |
| otadata | data | `0xE000` | 8 KB | OTA boot slot selection |
| app0 | app | `0x10000` | 1.6 MB | Firmware slot A |
| app1 | app | `0x1B0000` | 1.6 MB | Firmware slot B |
| spiffs | data | `0x350000` | 704 KB | Web UI assets |

---

## License

MIT — see [LICENSE](LICENSE).
