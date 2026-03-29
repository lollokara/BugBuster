# BugBuster

Open-source, four-channel analog/digital I/O debug and programming tool built around the **Analog Devices AD74416H** software-configurable I/O chip and an **ESP32-S3** microcontroller.

![License](https://img.shields.io/badge/license-MIT-blue)

---

## What It Does

BugBuster turns a single USB-C connection into a versatile bench instrument:

- **4-channel ADC** -- 24-bit, up to 4.8 kSPS per channel, multiple voltage/current ranges
- **4-channel DAC** -- 16-bit voltage (0-11V / bipolar) and current (0-25 mA) output
- **Waveform generator** -- Sine, square, triangle, sawtooth up to 100 Hz
- **Digital I/O** -- Configurable per-channel as logic input, loop-powered input, or digital output
- **32-switch MUX matrix** -- 4x ADGS2414D octal SPST switches for flexible signal routing
- **Adjustable power supplies** -- DS4424 IDAC tunes LTM8063/LTM8078 DCDC output voltages (3-15V)
- **USB Power Delivery** -- HUSB238 negotiates 5-20V from USB-C source
- **WiFi AP + STA** -- Built-in web UI, REST API, OTA firmware updates
- **Real-time oscilloscope** -- ADC streaming with backend throttling, BBSC binary recording

## Architecture

```
                    USB-C (power + data)
                          |
    +---------------------+----------------------+
    |              ESP32-S3 (dual-core)           |
    |                                             |
    |  Core 0: WiFi, HTTP, CLI, USB CDC           |
    |  Core 1: ADC polling, DAC, fault monitor    |
    |                                             |
    |  SPI bus ---- AD74416H (4-ch I/O)           |
    |       \------ ADGS2414D x4 (MUX matrix)    |
    |                                             |
    |  I2C bus ---- DS4424 (IDAC, voltage adjust) |
    |       \------ PCA9535 (GPIO expander)       |
    |        \----- HUSB238 (USB PD controller)   |
    +---------------------------------------------+
         |                    |
    USB CDC #0           WiFi AP/STA
    (BBP binary)         (HTTP REST)
         |                    |
    +----+--------------------+----+
    |      Tauri Desktop App       |
    |  Rust backend + Leptos WASM  |
    |  18 tabs, real-time scope    |
    +------------------------------+
```

## Repository Structure

```
BugBuster/
  Firmware/
    esp32_ad74416h/       ESP-IDF firmware (PlatformIO)
      src/                34 source files (drivers, protocol, webserver)
      data/               SPIFFS web UI (Alpine.js + Tailwind)
      partitions.csv      A/B OTA partition table
    BugBusterProtocol.md  BBP protocol specification (v1.5)
    FirmwareStructure.md  Firmware reference

  DesktopApp/
    BugBuster/            Tauri v2 + Leptos 0.7 desktop application
      src/                Leptos WASM frontend (18 tab modules)
      src-tauri/          Rust backend (transport, commands, state)
      styles.css          Glass UI theme

  PCB Material/           Altium Designer schematics + PCB layout
    ARCHITECTURE.md       Full system architecture document
    BugBuster.pdf         Schematic export

  Docs/                   Component datasheets (AD74416H, ADGS2414D, etc.)
  Libs/                   Altium component libraries
  Notebooks/              Jupyter notebooks (flash, build, setup)
  Scripts/                Test scripts (SPI clock sweep, etc.)
```

## Getting Started

### Prerequisites

| Tool | Version | Purpose |
|------|---------|---------|
| [PlatformIO](https://platformio.org/) | 6.x | Firmware build + flash |
| [Rust](https://rustup.rs/) | 1.75+ | Desktop app backend |
| [Trunk](https://trunkrs.dev/) | 0.21+ | WASM frontend build |
| [Node.js](https://nodejs.org/) | 18+ | Tauri CLI |

### Flash Firmware

```bash
cd Firmware/esp32_ad74416h

# Build and flash (ESP32-S3 in boot mode)
pio run --target upload

# Flash web UI assets
pio run --target uploadfs
```

### Build Desktop App

```bash
cd DesktopApp/BugBuster

# Development mode
cargo tauri dev

# Production build
cargo tauri build
```

### OTA Update (wireless)

After the initial USB flash, firmware can be updated wirelessly:

1. Connect the device to WiFi (Diagnostics tab)
2. Build new firmware: `pio run`
3. In the app: Diagnostics --> Firmware --> OTA Update --> select `firmware.bin`

## Communication

The device supports two transports:

| Transport | Protocol | Use Case |
|-----------|----------|----------|
| **USB CDC** | BBP (COBS + CRC-16) | Low-latency, streaming, full control |
| **WiFi HTTP** | REST API (JSON) | Remote access, OTA updates |

Both transports are abstracted behind a `Transport` trait -- the desktop app works identically over either.

See [BugBusterProtocol.md](Firmware/BugBusterProtocol.md) for the full protocol specification.

## Hardware

The PCB is designed in Altium Designer. Key ICs:

| IC | Function | Interface |
|----|----------|-----------|
| AD74416H | 4-ch software-configurable I/O | SPI (up to 20 MHz) |
| ADGS2414D x4 | 32-switch MUX matrix | SPI (address mode) |
| DS4424 | 4-ch IDAC for DCDC voltage adjust | I2C (0x10) |
| HUSB238 | USB-C PD sink controller | I2C (0x08) |
| PCA9535AHF | 16-bit GPIO expander | I2C (0x23) |
| LTM8063 x2 | Adjustable DCDC (3-15V, 2A) | Analog (FB pin) |
| LTM8078 | Level shifter DCDC | Analog (FB pin) |
| TPS1641x x4 | E-fuse / current limiters | GPIO enable |

Schematics and PCB layout are in [PCB Material/](PCB%20Material/).

## Desktop App Tabs

| Tab | Function |
|-----|----------|
| Overview | Status dashboard, SPI health, temperature |
| ADC | 4-channel ADC readings with range/rate/mux config |
| Diagnostics | Internal supplies, alerts, firmware info, WiFi, OTA |
| VDAC | Voltage DAC output control |
| IDAC | Current DAC output control |
| IIN | Current input monitoring |
| DIN / DOUT | Digital I/O control |
| Faults | Alert register viewer with per-channel detail |
| GPIO | AD74416H GPIO configuration |
| UART | UART bridge configuration |
| Scope | Real-time oscilloscope with recording |
| WaveGen | Waveform generator (sine, square, triangle, saw) |
| Signal Path | Interactive MUX switch matrix visualization |
| Voltages | DCDC voltage adjustment with calibration |
| Calibration | DS4424 IDAC calibration wizard |
| USB PD | USB Power Delivery status and PDO selection |
| IO Expander | PCA9535 GPIO expander control |

## Partition Table

The ESP32-S3 uses A/B OTA partitions for safe wireless updates:

| Partition | Type | Offset | Size | Purpose |
|-----------|------|--------|------|---------|
| nvs | data | 0x9000 | 20 KB | Settings, WiFi creds, calibration |
| otadata | data | 0xE000 | 8 KB | OTA boot selection |
| app0 | app | 0x10000 | 1.6 MB | Firmware slot A |
| app1 | app | 0x1B0000 | 1.6 MB | Firmware slot B |
| spiffs | data | 0x350000 | 704 KB | Web UI assets |

NVS data (WiFi credentials, calibration) is preserved across OTA updates.

## License

MIT -- see [LICENSE](LICENSE).
