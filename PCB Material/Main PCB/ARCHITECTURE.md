# BugBuster Architecture

Reference document for the BugBuster hardware test instrument. Last updated 2026-03-30.

---

## 1. System Overview

BugBuster is an open-source, four-channel analog/digital I/O instrument built around the Analog Devices AD74416H software-configurable I/O chip. It provides voltage/current input and output, digital I/O, resistance measurement, waveform generation, and a configurable MUX switch matrix -- all controllable from a desktop application or an embedded web interface.

The system comprises three subsystems:

1. **ESP32-S3 Firmware** -- FreeRTOS application (ESP-IDF) running on a DFRobot FireBeetle 2. Drives all hardware over SPI and I2C, serves an HTTP REST API and a binary protocol over USB CDC.
2. **Tauri Desktop App (Rust backend)** -- Manages USB (BBP binary protocol) and HTTP (REST) connections to the device, exposes Tauri commands, and polls device state at 5 Hz.
3. **Leptos Frontend (WASM)** -- Single-page application compiled to WebAssembly via Trunk, rendered inside the Tauri webview. Provides tabbed UI for every hardware subsystem.

Communication between host and device uses either:
- **USB CDC #0** with the BugBuster Binary Protocol (BBP): COBS-framed, CRC-16 protected, sub-ms latency, streaming capable.
- **HTTP REST API** over WiFi (AP at 192.168.4.1, optional STA): JSON request/response, polling-based scope data.

Both transports are abstracted behind a `Transport` trait in the Rust backend so the frontend is transport-agnostic.

---

## 2. Hardware Architecture

### 2.1 Core Components

| Component | Role | Bus | Address |
|-----------|------|-----|---------|
| **ESP32-S3** (DFRobot FireBeetle 2) | Main MCU, 4 MB flash | -- | -- |
| **AD74416H** | 4-ch software-configurable I/O (24-bit ADC, 16-bit DAC) | SPI (10 MHz default, up to 20 MHz) | Dev addr 0x00 |
| **ADGS2414D x4** | Octal SPST analog switch matrix (32 switches total) | SPI (shared bus, separate CS) | Daisy-chain |
| **DS4424** | 4-ch I2C current DAC (adjusts LTM8063/LTM8078 output voltages) | I2C | 0x10 |
| **HUSB238** | USB-C PD sink controller (negotiates 5-20V from USB-C) | I2C | 0x08 |
| **PCA9535AHF** | 16-bit I2C GPIO expander (power enables, e-fuse control, status) | I2C | 0x23 |

### 2.2 Pin Assignments

**SPI Bus (AD74416H + ADGS2414D share MISO/MOSI/SCLK):**

| Signal | GPIO | Notes |
|--------|------|-------|
| SDO (MISO) | 8 | From AD74416H |
| SDI (MOSI) | 9 | To AD74416H |
| SYNC (CS) | 10 | AD74416H chip select (active-low) |
| SCLK | 11 | SPI clock, 10 MHz (configurable up to 20 MHz) |
| MUX_CS | 12 | ADGS2414D chip select |
| LSHIFT_OE | 14 | Level shifter output enable (TXS0108E) |

**AD74416H Control:**

| Signal | GPIO | Notes |
|--------|------|-------|
| RESET | 5 | Active-low hardware reset (held HIGH during operation) |
| ADC_RDY | 6 | Open-drain, active-low -- ADC conversion ready |
| ALERT | 7 | Open-drain, active-low -- fault/alert output |

**I2C Bus (shared: DS4424, HUSB238, PCA9535):**

| Signal | GPIO | Notes |
|--------|------|-------|
| SDA | 1 | Standard-mode (100 kHz) |
| SCL | 4 | Standard-mode (100 kHz) |

### 2.3 Bus Topology

```
ESP32-S3
  |
  +--[SPI @ 10 MHz]-+--[CS GPIO10]--> AD74416H (4-ch I/O)
  |                  |
  |                  +--[CS GPIO12]--> ADGS2414D x4 (daisy-chain, via level shifter)
  |
  +--[I2C @ 100 kHz]--+--[0x10]--> DS4424 (IDAC, adjusts regulator voltages)
  |                    +--[0x08]--> HUSB238 (USB-PD status/control)
  |                    +--[0x23]--> PCA9535 (power enables, e-fuse control)
  |
  +--[USB CDC #0]--> Host (CLI / BBP binary protocol)
  +--[USB CDC #1]--> Host (UART bridge passthrough)
  +--[WiFi AP+STA]--> HTTP REST API (port 80)
```

**SPI Bus Sharing:** The AD74416H and ADGS2414D share MISO/MOSI/SCLK with separate chip selects. The ADC poll task (which continuously reads the AD74416H) yields the SPI bus on request via `g_spi_bus_request` / `g_spi_bus_granted` flags to allow MUX operations.

### 2.4 Power Topology

- **HUSB238** negotiates USB-C PD (default 20V)
- **LTM8063 x2** step-down regulators produce V_ADJ1 (feeds ports P1+P2) and V_ADJ2 (feeds ports P3+P4), adjustable via DS4424 current injection into feedback network
- **TPS1641x x4** e-fuses protect each output port (P1-P4), enabled/monitored via PCA9535
- **PCA9535** controls: VADJ1_EN, VADJ2_EN, EN_15V (analog supply), EN_MUX, EN_USB_HUB, EFUSE_EN_1-4
- **PCA9535** monitors: LOGIC_PG, VADJ1_PG, VADJ2_PG, EFUSE_FLT_1-4

---

## 3. Firmware Architecture

**Location:** `Firmware/ESP32/`
**Framework:** ESP-IDF (native, no Arduino), built with PlatformIO
**Board:** DFRobot FireBeetle 2 ESP32-S3 (`espressif32@6.9.0`)

### 3.1 Boot Sequence (`app_main` in main.cpp)

1. Initialize USB CDC composite device (TinyUSB: CLI + UART bridge)
2. Wait 500 ms for USB enumeration, initialize serial I/O
3. Set RESET pin HIGH, configure ALERT and ADC_RDY as input with pull-up
4. Initialize WiFi (AP mode: "BugBuster" + optional STA)
5. Mount SPIFFS (web UI files)
6. Initialize AD74416H: hardware reset, SPI verify via SCRATCH register, enable internal reference
7. Set up diagnostics (4 slots: temperature, AVDD_HI, DVCC, AVCC)
8. Start ADC in continuous mode (diagnostics only, all channels HIGH_IMP)
9. Create FreeRTOS tasks (ADC poll, fault monitor, command processor)
10. Initialize ADGS2414D MUX matrix (level shifter enable, daisy-chain mode)
11. Initialize I2C bus and probe devices: DS4424, HUSB238, PCA9535
12. Start UART bridge (CDC #1)
13. Start HTTP web server (port 80)
14. Initialize CLI + BBP handlers
15. Create main loop task on Core 0

### 3.2 FreeRTOS Tasks

| Task | Core | Priority | Period | Purpose |
|------|------|----------|--------|---------|
| **taskAdcPoll** | 1 | 3 | 1-100 ms (dynamic) | Read ADC results, convert to engineering units, accumulate scope buckets, push to BBP stream ring buffer |
| **taskFaultMonitor** | 1 | 4 | 200 ms | Read alert/fault status, DIN counters, GPIO inputs, diagnostics (every 5th cycle ~1s), SPI health check |
| **taskCommandProcessor** | 1 | 2 | Blocking (queue) | Dequeue Command structs, execute hardware operations (channel function set, DAC write, config changes), update state |
| **taskI2cPoll** | 1 | 1 | 500 ms | Poll DS4424/HUSB238/PCA9535 status, update `g_deviceState` I2C fields |
| **taskWavegen** | 1 | 3 | ~10 ms | Generate waveform samples (sine/square/triangle/sawtooth), write DAC codes at target frequency |
| **mainLoopTask** | 0 | 1 | 2-20 ms | Process CLI input, detect BBP handshake, run BBP binary protocol, send heartbeat every 30s |

### 3.3 Shared State

All tasks communicate through `g_deviceState` (a `DeviceState` struct), protected by `g_stateMutex` (FreeRTOS binary semaphore, typical 10-50 ms timeout). The command queue `g_cmdQueue` (FreeRTOS queue) decouples command sources (CLI, BBP, HTTP) from hardware execution.

### 3.4 SPI Bus Sharing

The AD74416H and ADGS2414D share the SPI bus with different chip selects. Since the ADC poll task runs continuously, MUX operations request the bus via atomic flags:

```
MUX driver sets g_spi_bus_request = true
ADC task sees request, sets g_spi_bus_granted = true, pauses
MUX driver performs SPI transaction, clears g_spi_bus_request
ADC task resumes polling
```

---

## 4. Communication Protocol (BBP)

**Full specification:** `Firmware/BugBusterProtocol.md`

### 4.1 Framing

Each message is COBS-encoded (Consistent Overhead Byte Stuffing) then terminated by a 0x00 delimiter. The raw message format before COBS encoding:

```
[type:1][seq:2 LE][cmd_id:1][payload:0..1018][crc16:2 LE]
```

- **type:** 0x01=CMD (host->device), 0x02=RSP, 0x03=EVT (unsolicited), 0x04=ERR
- **seq:** 16-bit sequence number (little-endian), response echoes the request's seq
- **crc16:** CRC-16/CCITT (poly 0x1021, init 0xFFFF) over all bytes before CRC

### 4.2 Handshake (CLI -> Binary Mode)

Host sends raw bytes `0xBB 0x42 0x55 0x47` ("BUG" with 0xBB prefix). Device responds with 8 bytes: `[magic:4][proto_ver:1][fw_major:1][fw_minor:1][fw_patch:1]`. After the response, CDC #0 switches from text CLI to COBS-framed binary. The DISCONNECT command (0xFF) returns to CLI mode.

### 4.3 Command Groups

| Range | Group | Key Commands |
|-------|-------|-------------|
| 0x01-0x04 | Status/Info | GET_STATUS, GET_DEVICE_INFO, GET_FAULTS, GET_DIAGNOSTICS |
| 0x10-0x1C | Channel Config | SET_CH_FUNC, SET_DAC_CODE, SET_DAC_VOLTAGE, SET_ADC_CONFIG |
| 0x20-0x23 | Faults | CLEAR_ALL_ALERTS, CLEAR_CH_ALERT, SET_ALERT_MASK |
| 0x30 | Diagnostics | SET_DIAG_CONFIG |
| 0x40-0x42 | GPIO | GET_GPIO_STATUS, SET_GPIO_CONFIG, SET_GPIO_VALUE |
| 0x50-0x52 | UART | GET_UART_CONFIG, SET_UART_CONFIG, GET_UART_PINS |
| 0x60-0x63 | Streaming | START_ADC_STREAM, STOP_ADC_STREAM, START_SCOPE_STREAM, STOP_SCOPE_STREAM |
| 0x90-0x92 | MUX Matrix | MUX_SET_ALL, MUX_GET_ALL, MUX_SET_SWITCH |
| 0xA0-0xA6 | DS4424 IDAC | IDAC_GET_STATUS, IDAC_SET_CODE, IDAC_SET_VOLTAGE, calibration commands |
| 0xB0-0xB2 | PCA9535 IO | PCA_GET_STATUS, PCA_SET_CONTROL, PCA_SET_PORT |
| 0xC0-0xC2 | HUSB238 PD | USBPD_GET_STATUS, USBPD_SELECT_PDO, USBPD_GO |
| 0xD0-0xD1 | Waveform Gen | START_WAVEGEN, STOP_WAVEGEN |
| 0x70-0x72 | System | DEVICE_RESET, REG_READ, REG_WRITE |
| 0xFE-0xFF | Utility | PING, DISCONNECT |

### 4.4 Streaming Events (Device -> Host)

| Event ID | Purpose |
|----------|---------|
| 0x80 | ADC_DATA -- raw 24-bit samples per channel at hardware rate |
| 0x81 | SCOPE_DATA -- downsampled min/max/avg buckets (10 ms interval) |
| 0x82 | ALERT -- fault/alert status change |
| 0x83 | DIN -- digital input state change |

ADC streaming uses a lock-free ring buffer (`BbpAdcStreamBuf`, 256 entries) written by the ADC poll task (Core 1) and read by the BBP processor (Core 0).

---

## 5. Desktop App Architecture

**Location:** `DesktopApp/BugBuster/`

### 5.1 Tauri Backend (Rust)

**Location:** `DesktopApp/BugBuster/src-tauri/src/`

The backend is a standard Tauri 2.x application. Key modules:

- **`lib.rs`** -- Entry point. Registers all Tauri commands and manages `ConnectionManager` as application state.
- **`connection_manager.rs`** -- Central hub. Owns the active `Transport`, starts a polling loop (200 ms), and emits `device-state` and `connection-status` Tauri events to the frontend.
- **`transport.rs`** -- `Transport` trait with `send_command`, `get_status`, `is_connected`, `disconnect`. Both USB and HTTP implement this trait.
- **`usb_transport.rs`** -- Opens a serial port, performs BBP handshake, spawns a reader thread that decodes COBS frames and dispatches responses/events. Uses `oneshot` channels to match request sequence numbers to response futures.
- **`http_transport.rs`** -- Uses `reqwest` to call the firmware's REST API. Maps BBP command IDs to HTTP endpoints. Fallback transport when USB is unavailable.
- **`bbp.rs`** -- Pure-data BBP codec: COBS encode/decode, CRC-16, `Message` build/parse, `FrameAccumulator`, `PayloadWriter`/`PayloadReader`. No I/O.
- **`state.rs`** -- Serde-serializable types mirroring firmware state: `DeviceState`, `ChannelState`, `DiagState`, `GpioState`, `IdacState`, `UsbPdState`, `IoExpState`, `ConnectionStatus`, `DiscoveredDevice`, `ScopeBucket`, `AdcStreamBatch`.
- **`commands.rs`** -- Tauri command handlers (`#[tauri::command]`). Each handler calls `ConnectionManager::send_command` with the appropriate BBP command ID and payload. Also handles file dialog, recording (BBSC binary + CSV), and export.
- **`discovery.rs`** -- Device discovery: enumerates USB serial ports (filters by Espressif VID `0x303A`), probes each with a BBP handshake, also scans `http://192.168.4.1` for the REST API.
- **`wavegen.rs`** -- Waveform generator: spawns a tokio task that computes sine/square/triangle/sawtooth samples at 100 updates/sec and sends `SET_DAC_VOLTAGE` commands.

**Dependencies:** `tauri` 2.x, `serialport` 4.x, `tokio`, `reqwest`, `serde`, `anyhow`, `async-trait`, `csv`, `chrono`.

### 5.2 Leptos Frontend (WASM)

**Location:** `DesktopApp/BugBuster/src/`

Built with **Leptos 0.7** (CSR mode), compiled to WASM via Trunk, served at `http://localhost:1420`.

- **`main.rs`** -- Mounts the `App` component.
- **`app.rs`** -- Root component. Manages signals for connection state, device state, and active tab. Sets up Tauri event listeners (`device-state`, `connection-status`). Renders the header bar, connection panel (when disconnected), tab bar, and active tab content.
- **`tauri_bridge.rs`** -- FFI bindings to `window.__TAURI__.core.invoke` and `window.__TAURI__.event.listen`. Defines frontend-side types (`DeviceState`, `ChannelState`, etc.) and helper functions for invoking Tauri commands (`send_set_channel_function`, `send_idac_code`, `send_mux_set_switch`, etc.). Also contains ADC/channel/GPIO option constants.
- **`tabs/mod.rs`** -- Re-exports all tab modules.
- **`components/mod.rs`** -- Shared UI components: `controls.rs` (buttons, sliders, selects), `display.rs` (value displays, indicators), `layout.rs` (card, grid helpers).

**Tab modules** (`src/tabs/`):

| Tab | File | Purpose |
|-----|------|---------|
| Overview | `overview.rs` | System status dashboard: SPI health, temperature, all channel summaries |
| ADC | `adc.rs` | Per-channel ADC configuration (mux, range, rate) and live readings |
| Diagnostics | `diag.rs` | 4 diagnostic slot configuration and values (temp, supply voltages) |
| VDAC | `vdac.rs` | Voltage/current DAC output control (code, voltage, bipolar/unipolar) |
| IDAC | `idac.rs` | DS4424 current DAC control, voltage targets, calibration display |
| IIN | `iin.rs` | Current input channel configuration and monitoring |
| DIN | `din.rs` | Digital input configuration (threshold, debounce, counter) |
| DOUT | `dout.rs` | Digital output configuration and control |
| Faults | `faults.rs` | Alert/fault status display and clear controls |
| GPIO | `gpio.rs` | GPIO mode configuration and state display (pins A-F) |
| UART | `uart.rs` | UART bridge configuration (baud, pins, data format) |
| Scope | `scope.rs` | Oscilloscope-style time-domain plot using canvas 2D (scope bucket data) |
| WaveGen | `wavegen.rs` | Waveform generator controls (shape, frequency, amplitude, offset) |
| Signal Path | `signal_path.rs` | MUX switch matrix visualization and control |
| Voltages | `voltages.rs` | PCA9535 status, e-fuse control, HUSB238 USB-PD status |
| Calibration | `calibration.rs` | DS4424 IDAC calibration workflow (add points, save to NVS) |

### 5.3 State Flow

```
Firmware                        Tauri Backend                    Leptos Frontend
--------                        -------------                    ---------------
g_deviceState                   ConnectionManager                Leptos signals
(mutex-protected)               polls GET_STATUS @ 5 Hz         (reactive)
      |                               |                              |
      +--- BBP/HTTP response -------->+                              |
                                      +--- emit("device-state") --->+
                                      |                              |
                                      +<-- invoke("set_dac_voltage")+
                                      |                              |
      +<-- BBP CMD / HTTP POST -------+                              |
```

---

## 6. File Index

### 6.1 Firmware (`Firmware/ESP32/src/`)

| File | Description |
|------|-------------|
| `main.cpp` | Entry point (`app_main`): boot sequence, hardware init, task creation |
| `config.h` | Pin definitions, I2C/SPI constants, timing, GPIO helper macros |
| `ad74416h.h/.cpp` | High-level AD74416H HAL: channel functions, DAC/ADC, DIN/DO, GPIO, diagnostics |
| `ad74416h_regs.h` | Complete AD74416H register map, bit field masks, enums (ChannelFunction, AdcRange, etc.) |
| `ad74416h_spi.h/.cpp` | Low-level SPI driver: 40-bit frame format, CRC-8, register read/write |
| `adgs2414d.h/.cpp` | ADGS2414D MUX switch matrix driver: daisy-chain init, set/get switches, dead-time protection |
| `tasks.h/.cpp` | FreeRTOS tasks (ADC poll, fault monitor, command processor, I2C poll, wavegen), DeviceState struct, command queue |
| `bbp.h/.cpp` | BugBuster Binary Protocol: COBS codec, CRC-16, handshake, command dispatch, ADC stream ring buffer |
| `cli.h/.cpp` | Serial CLI: text command parser (help, status, rreg, wreg, func, dac, etc.) |
| `webserver.h/.cpp` | HTTP REST API server: 20+ endpoints for status, channels, faults, GPIO, UART, scope, MUX, IDAC, PD |
| `serial_io.h/.cpp` | USB CDC #0 serial abstraction: read/write/printf over TinyUSB |
| `usb_cdc.h/.cpp` | TinyUSB composite device init: 2x CDC (CLI + UART bridge) |
| `uart_bridge.h/.cpp` | UART-to-USB bridge: bidirectional passthrough between CDC #1 and hardware UART |
| `wifi_manager.h/.cpp` | WiFi AP+STA management: init, connect, status, IP/MAC queries |

| `i2c_bus.h/.cpp` | Shared I2C master bus driver with mutex protection |
| `ds4424.h/.cpp` | DS4424 4-ch IDAC driver: code/voltage set, calibration (NVS save/load), formula-based and interpolated conversion |
| `husb238.h/.cpp` | HUSB238 USB-PD sink controller: status read, PDO selection, re-negotiation |
| `pca9535.h/.cpp` | PCA9535 16-bit GPIO expander: power enables, e-fuse control, status monitoring |

### 6.2 Tauri Backend (`DesktopApp/BugBuster/src-tauri/src/`)

| File | Description |
|------|-------------|
| `main.rs` | Binary entry point (calls `lib::run()`) |
| `lib.rs` | Tauri builder: registers all command handlers and manages ConnectionManager |
| `bbp.rs` | BBP codec: COBS encode/decode, CRC-16, Message parse/build, FrameAccumulator, payload helpers |
| `state.rs` | Shared types: DeviceState, ChannelState, IdacState, UsbPdState, IoExpState, ConnectionStatus, etc. |
| `transport.rs` | Transport trait: `send_command`, `get_status`, `is_connected`, `disconnect` |
| `usb_transport.rs` | USB CDC transport: serial port open, BBP handshake, reader thread, sequence-matched responses |
| `http_transport.rs` | HTTP REST transport: reqwest client, JSON request/response, maps commands to REST endpoints |
| `connection_manager.rs` | Owns transport, polls status at 5 Hz, emits Tauri events, manages connect/disconnect lifecycle |
| `commands.rs` | Tauri command handlers: discovery, connection, channel config, MUX, IDAC, PCA, UART, recording, export |
| `discovery.rs` | Device discovery: USB port enumeration (Espressif VID filter + BBP probe), HTTP scan |
| `wavegen.rs` | Waveform generator: tokio task computing sine/square/triangle/sawtooth, drives DAC at 100 Hz |

### 6.3 Leptos Frontend (`DesktopApp/BugBuster/src/`)

| File | Description |
|------|-------------|
| `main.rs` | WASM entry point, mounts App component |
| `app.rs` | Root component: connection panel, tab bar, tab routing, event listeners |
| `tauri_bridge.rs` | Tauri JS FFI (invoke/listen), frontend state types, command helpers, constants |
| `components/controls.rs` | Reusable UI controls (buttons, sliders, selects) |
| `components/display.rs` | Value display widgets, status indicators |
| `components/layout.rs` | Card/grid layout helpers |
| `tabs/overview.rs` | System overview dashboard |
| `tabs/adc.rs` | ADC configuration and live readings |
| `tabs/diag.rs` | Diagnostic slot configuration and display |
| `tabs/vdac.rs` | Voltage/current DAC output controls |
| `tabs/idac.rs` | DS4424 IDAC controls and calibration display |
| `tabs/iin.rs` | Current input monitoring |
| `tabs/din.rs` | Digital input configuration |
| `tabs/dout.rs` | Digital output control |
| `tabs/faults.rs` | Fault/alert status and clear |
| `tabs/gpio.rs` | GPIO mode configuration (pins A-F) |
| `tabs/uart.rs` | UART bridge configuration |
| `tabs/scope.rs` | Oscilloscope canvas plot |
| `tabs/wavegen.rs` | Waveform generator UI |
| `tabs/signal_path.rs` | MUX switch matrix visualization |
| `tabs/voltages.rs` | Power topology: PCA9535 status, e-fuses, USB-PD |
| `tabs/calibration.rs` | IDAC calibration workflow |

### 6.4 Documentation & Config

| File | Description |
|------|-------------|
| `Firmware/BugBusterProtocol.md` | Full BBP protocol specification (framing, handshake, commands, streaming) |
| `Firmware/FirmwareStructure.md` | Firmware reference: ADC/DAC config, channel functions, HTTP endpoints, task structure |
| `Firmware/ESP32/platformio.ini` | PlatformIO build config (ESP-IDF, ESP32-S3) |
| `Firmware/ESP32/partitions.csv` | Flash partition table (app + SPIFFS) |
| `Firmware/ESP32/data/index.html` | Embedded web dashboard (served via SPIFFS) |
| `DesktopApp/BugBuster/Cargo.toml` | Frontend workspace Cargo config (Leptos 0.7, wasm-bindgen) |
| `DesktopApp/BugBuster/src-tauri/Cargo.toml` | Backend Cargo config (Tauri 2, serialport, tokio, reqwest) |
| `DesktopApp/BugBuster/Trunk.toml` | Trunk build config for WASM (serves on port 1420) |
| `DesktopApp/BugBuster/index.html` | HTML shell for Trunk/Tauri webview |
| `DesktopApp/BugBuster/styles.css` | Global CSS styles (dark theme) |
| `Docs/*.pdf` | Component datasheets (AD74416H, ADGS2414D, DS4424, HUSB238, PCA9535, TPS1641, LTM8063, ESP32-S3) |
| `*.SchDoc` | Altium Designer schematic sheets (Analog, ESP32, IOs, Power, USB) |
| `BugBusterMain.PcbDoc` | Altium PCB layout |

---

## 7. Key Data Flows

### 7.1 ADC Reading -> Scope Display

```
AD74416H (SPI)
    |
    v
taskAdcPoll reads CONV_RESULT registers (1-100 ms poll rate)
    |
    +---> convertAdcCode() -> engineering value (V or mA)
    |
    +---> g_deviceState.channels[ch].adcRawCode / adcValue (mutex)
    |
    +---> ScopeBuffer accumulator: min/max/sum per 10ms bucket
    |         When bucket elapses -> commit to ring buffer (256 slots)
    |
    +---> BBP ADC stream ring buffer (if streaming active, lock-free)
    |
    v
ConnectionManager polls GET_STATUS (BBP cmd 0x01) every 200 ms
    |
    v
DeviceState emitted via Tauri event "device-state"
    |
    v
ScopeTab: requests scope data (SCOPE_STREAM or HTTP /api/scope?since=N)
    -> renders min/max/avg on HTML5 Canvas via CanvasRenderingContext2d
```

### 7.2 MUX Switch Toggle

```
User clicks switch in Signal Path tab
    |
    v
send_mux_set_switch(device, switch_num, state) via tauri_bridge
    |
    v
commands::mux_set_switch -> ConnectionManager::send_command(MUX_SET_SWITCH, payload)
    |
    v
BBP command 0x92: [device:1][switch:1][state:1]
    |
    v
Firmware bbp handler: adgs_set_switch_safe(device, switch, closed)
    +---> Request SPI bus from ADC task (g_spi_bus_request)
    +---> Open all switches in same group (dead time protection)
    +---> Wait 100 ms (ADGS_DEAD_TIME_MS)
    +---> Set new switch state via daisy-chain SPI write
    +---> Update g_deviceState.muxState[device]
    +---> Release SPI bus
```

### 7.3 IDAC Voltage Setting with Calibration

```
User sets target voltage in IDAC tab
    |
    v
send_idac_voltage(channel, voltage) via tauri_bridge
    |
    v
BBP command 0xA2: [channel:1][voltage:f32]
    |
    v
Firmware: ds4424_set_voltage(ch, volts)
    +---> ds4424_voltage_to_code(ch, volts):
    |       if calibration valid -> piecewise linear interpolation of cal_points
    |       else -> formula: code = (V_midpoint - V_target) / step_mV * 1000
    +---> ds4424_set_code(ch, computed_code) -> I2C write to DS4424 register
    +---> Update g_deviceState.idac.state[ch]
```

**Calibration flow:** The desktop app drives a sweep (IDAC_CAL_ADD_POINT commands), measuring actual voltage via the AD74416H ADC at each step, building a code-to-voltage lookup table. This is saved to ESP32 NVS via IDAC_CAL_SAVE and loaded at boot.

### 7.4 Waveform Generation

**Firmware-side wavegen (when available):**
```
BBP command START_WAVEGEN: [channel:1][waveform:1][freq_hz:f32][amplitude:f32][offset:f32][mode:1]
    |
    v
taskWavegen spawned: generates samples using sin/square/triangle/sawtooth math
    +---> Computes DAC code per sample based on waveform phase
    +---> Writes DAC register directly via SPI (bypasses command queue for low latency)
    +---> Runs at calculated step rate to achieve target frequency
```

**Desktop-side wavegen (fallback):**
```
WavegenState::start() spawns tokio task at 100 updates/sec
    +---> Computes waveform sample at current phase
    +---> Sends SET_DAC_VOLTAGE command over BBP
    +---> Limited to ~50 Hz max waveform frequency due to command latency
```

---

## 8. Build & Flash

### 8.1 Firmware (PlatformIO + ESP-IDF)

**Prerequisites:** PlatformIO CLI or IDE, ESP-IDF toolchain (auto-installed by PlatformIO).

```bash
cd Firmware/ESP32

# Build
pio run -e esp32s3

# Flash firmware
pio run -e esp32s3 -t upload

# Flash SPIFFS (web UI)
pio run -e esp32s3 -t uploadfs

# Serial monitor
pio device monitor -b 115200
```

The upload port is configured in `platformio.ini` (default COM10 on Windows; adjust for your system or let PlatformIO auto-detect).

### 8.2 Desktop App (Tauri + Trunk)

**Prerequisites:** Rust toolchain, `wasm32-unknown-unknown` target, Trunk, Tauri CLI.

```bash
# Install tools (one-time)
rustup target add wasm32-unknown-unknown
cargo install trunk
cargo install tauri-cli

# Run in development mode (hot-reload)
cd DesktopApp/BugBuster
cargo tauri dev

# Build release binary
cargo tauri build
```

`cargo tauri dev` starts Trunk on port 1420 (compiles Leptos frontend to WASM) and launches the Tauri window pointing at it. The release build bundles everything into a native application.

### 8.3 WiFi Access

After flashing, the ESP32 creates a WiFi AP:
- **SSID:** BugBuster
- **Password:** bugbuster123
- **IP:** 192.168.4.1
- **Web UI:** http://192.168.4.1 (served from SPIFFS)
- **REST API:** http://192.168.4.1/api/status (and other endpoints)

The desktop app discovers the device automatically via USB (preferred) or WiFi.
