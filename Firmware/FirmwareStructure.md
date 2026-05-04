## PROJECT STRUCTURE
```
Firmware/
├── ESP32/                   # ESP32-S3 main controller firmware
│   ├── platformio.ini                # PlatformIO configuration
│   ├── CMakeLists.txt                # ESP-IDF build configuration
│   ├── partitions.csv                # Flash partition table
│   ├── sdkconfig.defaults            # IDF SDK defaults
│   ├── sdkconfig.esp32s3             # ESP32-S3 specific configuration
│   ├── README.md                     # ESP32 firmware documentation
│   ├── web/                          # Vite/Preact web UI build output (served from SPIFFS)
│   │   ├── index.html                # Entry point built by `cd web && npm run build`
│   │   └── ...                       # Bundled JS/CSS assets
│   ├── components/
│   │   └── micropython/              # MicroPython IDF component (frozen modules: bb_helpers, bb_devices, bb_logging; quicksetup; bus/ext_bus)
│   │       ├── manifest.py           # Freeze list — points to python/firmware_modules/
│   │       ├── mpconfigport.h        # Port configuration
│   │       └── micropython/          # Upstream MicroPython submodule
│   └── src/
│       ├── main.cpp                  # Entry point & init sequence
│       ├── config.h                  # Pin definitions & constants
│       ├── ad74416h.h/.cpp           # High-level AD74416H HAL (all features)
│       ├── ad74416h_regs.h           # Register map, enums, bit fields
│       ├── ad74416h_spi.h/.cpp       # Low-level SPI driver (CRC-8)
│       ├── tasks.h/.cpp              # FreeRTOS task management & command processor
│       ├── bbp.h/.cpp                # Binary protocol (COBS, SPSC ring buffer, streaming)
│       ├── hat.h/.cpp                # HAT board detection & UART command forwarding
│       ├── adgs2414d.h/.cpp          # ADGS2414D MUX driver (SPI, daisy-chain, break-before-make)
│       ├── webserver.h/.cpp          # HTTP API server (20+ endpoints)
│       ├── serial_io.h/.cpp          # USB CDC interface (TinyUSB)
│       ├── usb_cdc.h/.cpp            # USB composite device (CLI + UART bridge)
│       ├── cli.h/.cpp                # Serial CLI menu interface
│       ├── wifi_manager.h/.cpp       # WiFi AP+STA management
│       ├── uart_bridge.h/.cpp        # UART-to-USB bridge (for external devices)
│       └── idf_component.yml         # Component dependencies
├── RP2040/                           # HAT expansion board firmware (debugprobe fork)
│   ├── CMakeLists.txt                # Pico SDK build
│   ├── README.md                     # RP2040 firmware documentation
│   ├── src/                          # BugBuster HAT extensions
│   │   ├── bb_main.c                 # Command task, UART dispatcher, IRQ signaling
│   │   ├── bb_config.h               # Pin definitions, protocol constants
│   │   ├── bb_protocol.c/h           # HAT UART framing (CRC-8, frame timeout)
│   │   ├── bb_power.c/h              # Connector power, current sense, fault detection
│   │   ├── bb_hvpak.c/h              # HVPAK I2C voltage control (STUB)
│   │   ├── bb_pins.c/h               # EXP_EXT pin routing
│   │   ├── bb_swd.c/h                # SWD status queries (target detect STUB)
│   │   ├── bb_la.c/h                 # Logic analyzer (PIO 1, DMA with IRQ completion)
│   │   ├── bb_la.pio                 # Capture PIO programs (1/2/4 channel)
│   │   ├── bb_la_trigger.pio         # Trigger PIO programs (edge/level)
│   │   ├── bb_la_rle.c/h             # RLE compression for LA data
│   │   └── bb_la_usb.c/h             # USB vendor bulk endpoint for LA streaming
│   └── lib/debugprobe/               # Upstream debugprobe (unmodified)
├── BugBusterProtocol.md              # BBP binary protocol specification
├── FirmwareStructure.md              # This file
├── HAT_Architecture.md               # HAT expansion board design document
└── HAT_Protocol.md                   # HAT UART protocol specification
```
---
## 1. ESP32 VARIANT & HARDWARE
**Device:** ESP32-S3 (DFRobot FireBeetle 2)
- **Flash:** 4 MB
- **Framework:** ESP-IDF (native, no Arduino)
- **SPI Clock:** 1 MHz (safe, max 20 MHz supported)
**Pin Configuration:**
| Function        | GPIO Pin | Type |
|-----------------|----------|------|
| **SPI MISO**    | GPIO_NUM_8 (SDO)   | Input  |
| **SPI MOSI**    | GPIO_NUM_9 (SDI)   | Output |
| **SPI CS**      | GPIO_NUM_10 (SYNC) | Output |
| **SPI Clock**   | GPIO_NUM_11 (SCLK) | Output |
| **Reset**       | GPIO_NUM_5         | Output (active-low, high=operating) |
| **ADC Ready**   | GPIO_NUM_6         | Input (open-drain, active-low) |
| **Alert**       | GPIO_NUM_7         | Input (open-drain, active-low) |
**AD74416H Device Address:** 0x00 (AD0=AD1=GND)
---
## 2. ADC CONFIGURATION & DATA ACQUISITION
### ADC Architecture
- **24-bit ADC** (resolution: ADC_FULL_SCALE = 16,777,216)
- **4 channels** (A, B, C, D)
- **Multiplexer + Range + Rate per channel**
- **4 diagnostic slots** (default: temperature, AVDD_HI, DVCC, AVCC)
### ADC Ranges (AdcRange enum)
| Code | Name | Range | V_offset | V_span | Use Case |
|------|------|-------|----------|--------|----------|
| 0 | ADC_RNG_0_12V | 0 to +12 V | 0.0 V | 12.0 V | Standard voltage |
| 1 | ADC_RNG_NEG12_12V | -12 to +12 V | -12.0 V | 24.0 V | Bipolar voltage |
| 2 | ADC_RNG_NEG0_3125_0_3125V | -312.5 to +312.5 mV | -0.3125 V | 0.625 V | Precision current sense |
| 3 | ADC_RNG_NEG0_3125_0V | -312.5 to 0 mV | -0.3125 V | 0.3125 V | Low-side current |
| 4 | ADC_RNG_0_0_3125V | 0 to +312.5 mV | 0.0 V | 0.3125 V | 0+ current sense |
| 5 | ADC_RNG_0_0_625V | 0 to +625 mV | 0.0 V | 0.625 V | 0+ wider range |
| 6 | ADC_RNG_NEG104MV_104MV | -104 to +104 mV | -0.104 V | 0.208 V | High precision sense |
| 7 | ADC_RNG_NEG2_5_2_5V | -2.5 to +2.5 V | -2.5 V | 5.0 V | Bipolar mid-range |
### ADC Sample Rates (AdcRate enum)
| Code | Name | Rate | Rejection |
|------|------|------|-----------|
| 0 | ADC_RATE_10SPS_H | 10 SPS | High (50/60 Hz) |
| 1 | ADC_RATE_20SPS | 20 SPS | Standard |
| 3 | ADC_RATE_20SPS_H | 20 SPS | High |
| 4 | ADC_RATE_200SPS_H1 | 200 SPS | High variant 1 |
| 6 | ADC_RATE_200SPS_H | 200 SPS | High variant 2 |
| 8 | ADC_RATE_1_2KSPS | 1.2 kSPS | Standard |
| 9 | ADC_RATE_1_2KSPS_H | 1.2 kSPS | High |
| 12 | ADC_RATE_4_8KSPS | 4.8 kSPS | Standard |
| 13 | ADC_RATE_9_6KSPS | 9.6 kSPS | Standard |
### ADC Multiplexer Options (AdcConvMux enum)
| Code | Name | Description |
|------|------|-------------|
| 0 | ADC_MUX_LF_TO_AGND | LF terminal to AGND |
| 1 | ADC_MUX_HF_TO_LF | HF to LF differential |
| 2 | ADC_MUX_VSENSEN_TO_AGND | VSENSE- to AGND |
| 3 | ADC_MUX_LF_TO_VSENSEN | LF to VSENSE- (4-wire) |
| 4 | ADC_MUX_AGND_TO_AGND | Self-test/offset |
### ADC Conversion Mode
- **Continuous conversion** (default: enabled, non-blocking)
- Hardware-set defaults when channel function is configured
- Default rate: **20 SPS** (configurable per channel)
- **Default sense resistor:** 12.0 Ω (configurable in DAC/ADC conversion)
### Data Acquisition Task (taskAdcPoll)
- **Core:** Core 1
- **Priority:** 3
- **Dynamic poll interval:** 1-100 ms based on fastest channel rate
- **Scope buffering:** 10 ms buckets, min/max/avg per channel
- **Bucket ring buffer:** 256 buckets (2.56 seconds @ 10ms/bucket)
---
## 3. DAC CONFIGURATION & OUTPUT
### DAC Architecture
- **16-bit DAC per channel** (resolution: DAC_FULL_SCALE = 65535)
- **Voltage output**
- **Current output**
### DAC Output Ranges
**Voltage Output (VOUT):**
- **Unipolar:** 0 to +12 V (span 12.0 V)
- **Bipolar:** -12 to +12 V (span 24.0 V, offset 12.0 V)
**Current Output (IOUT):**
- **Full range:** 0 to 25 mA (span 25.0 mA)
- **Limited:** 0 to 8 mA (configurable via `setCurrentLimit()`)
### DAC Conversion Formulas
```c
// Unipolar voltage (0..12V)
code = (voltage / 12.0) * 65535
// Bipolar voltage (-12..12V)
code = ((voltage + 12.0) / 24.0) * 65535
// Current (0..25 mA)
code = (current_mA / 25.0) * 65535
```
### DAC Output Configuration Options
- **VOUT Range:** Unipolar (0..12V) or Bipolar (-12..12V)
- **Current Limit:** 8 mA or 25 mA (full)
- **AVDD Source Selection:** 4 options (0-3)
- **Slew Rate:** Configurable step/rate (voltage ramping)
---
## 4. CHANNEL FUNCTIONS (ChannelFunction enum)
All channels support dynamic switching with safe transition sequence (HIGH_IMP → target):
| Code | Function | Type | ADC Input | DAC Output |
|------|----------|------|-----------|-----------|
| 0 | CH_FUNC_HIGH_IMP | Safe state | — | — |
| 1 | CH_FUNC_VOUT | Voltage out | Monitor | ✓ (0-12V unipolar) |
| 2 | CH_FUNC_IOUT | Current out | Compliance | ✓ (0-25 mA) |
| 3 | CH_FUNC_VIN | Voltage in | ✓ | — |
| 4 | CH_FUNC_IIN_EXT_PWR | Current in (ext) | ✓ | — |
| 5 | CH_FUNC_IIN_LOOP_PWR | Current in (loop) | ✓ | — |
| 7 | CH_FUNC_RES_MEAS | RTD/resistance | ✓ | — |
| 8 | CH_FUNC_DIN_LOGIC | Digital input | Comparator | — |
| 9 | CH_FUNC_DIN_LOOP | Digital input (loop) | Comparator | — |
| 10 | CH_FUNC_IOUT_HART | Current HART out | Compliance | ✓ (0-25 mA) |
| 11 | CH_FUNC_IIN_EXT_PWR_HART | Current in HART (ext) | ✓ | — |
| 12 | CH_FUNC_IIN_LOOP_PWR_HART | Current in HART (loop) | ✓ | — |
### Channel Switching Sequence (Safety-Critical)
1. Force channel to HIGH_IMP (safe state)
2. Wait 300 µs (or 4200 µs for IOUT_HART)
3. Set DAC code to 0
4. Set desired function
5. Wait settling time (300 µs or 4200 µs for HART)
---
## 5. DIGITAL I/O
### Digital Input (DIN) Configuration
Per-channel setup with:
- **Comparator threshold:** 7-bit code (COMP_THRESH[6:0])
- **Threshold mode:** Fixed or programmable
- **Debounce time:** 5-bit code (configurable intervals)
- **Current sink:** 5-bit code (DIN_SINK[4:0])
- **Sink range:** Low/High (DIN_SINK_RANGE)
- **Open-circuit detection:** Enable/disable
- **Short-circuit detection:** Enable/disable
- **Counter:** 32-bit event counter per DIN channel
### Digital Output (DO) Configuration
- **Mode:** High-side or low-side drive (DO_MODE[1:0])
- **Source:** SPI register or GPIO input (DO_SRC_SEL)
- **Timing:** T1[3:0], T2[7:0] parameters
- **Control:** `setDoData()` to drive output ON/OFF (when SPI-sourced)
### GPIO (6 pins: A–F)
- **Modes:** HIGH_IMP, OUTPUT, INPUT, DIN_OUT, DO_EXT
- **Pull-down:** Configurable weak pull-down (GP_WK_PD_EN)
- **Output:** Logic high/low control
- **Input:** Read logic state
---
## 6. WEB SERVER / HTTP INTERFACE
**Server:** ESP-IDF native `httpd` (port 80)
**Response Format:** JSON
**CORS:** Enabled (Access-Control-Allow-Origin: *)
### HTTP Endpoints
#### **GET Endpoints** (Read-only, fast)
| Endpoint | Purpose | Response |
|----------|---------|----------|
| `GET /` | Serve `web/index.html` from SPIFFS | HTML dashboard (Vite/Preact build) |
| `GET /api/status` | Full device state snapshot | All channels, faults, GPIO, diag |
| `GET /api/channel/?/adc` | Channel ADC raw + value | adcRaw, adcValue, range, rate, mux |
| `GET /api/channel/?/dac/readback` | DAC active code readback | activeCode |
| `GET /api/faults` | Fault status (global + per-ch) | alertStatus, supplyAlertStatus, masks |
| `GET /api/gpio` | All 6 GPIO states | mode, output, input, pulldown |
| `GET /api/diagnostics` | 4 diagnostic slots | slot, source, rawCode, value, unit |
| `GET /api/device/info` | Silicon ID & revision | siliconRev, siliconId0/1, spiOk |
| `GET /api/scope?since=<seq>` | Downsampled scope data (ring buffer) | seq, samples (t, ch_avg, ch_min/max) |
| `GET /api/uart/config` | UART bridge configuration | bridges[], id, uartNum, pins, baudrate |
| `GET /api/uart/pins` | Available UART pins | available[] pin list |
#### **POST Endpoints** (Configuration/Control, enqueued)
| Endpoint | Purpose | JSON Body | Response |
|----------|---------|-----------|----------|
| `POST /api/channel/?/function` | Set channel function | `{"function": <code>}` | `{"ok": true, ...}` |
| `POST /api/channel/?/dac` | Set DAC output | `{"code": <code>}` OR `{"voltage": <float>, "bipolar": <bool>}` OR `{"current_mA": <float>}` | `{"ok": true, ...}` |
| `POST /api/channel/?/adc/config` | Configure ADC | `{"mux": <code>, "range": <code>, "rate": <code>}` | `{"ok": true, ...}` |
| `POST /api/channel/?/din/config` | Configure digital input | `{"thresh": <uint8>, "threshMode": <bool>, "debounce": <uint8>, "sink": <uint8>, "sinkRange": <bool>, "ocDet": <bool>, "scDet": <bool>}` | `{"ok": true, ...}` |
| `POST /api/channel/?/do/config` | Configure digital output | `{"mode": <uint8>, "srcSelGpio": <bool>, "t1": <uint8>, "t2": <uint8>}` | `{"ok": true, ...}` |
| `POST /api/channel/?/do/set` | Drive DO output | `{"on": <bool>}` | `{"ok": true, "on": <bool>}` |
| `POST /api/channel/?/vout/range` | Set voltage range | `{"bipolar": <bool>}` | `{"ok": true, "bipolar": <bool>}` |
| `POST /api/channel/?/ilimit` | Set current limit | `{"limit8mA": <bool>}` | `{"ok": true, "limit8mA": <bool>}` |
| `POST /api/channel/?/avdd` | Select AVDD source | `{"select": <0-3>}` | `{"ok": true, "select": <int>}` |
| `POST /api/device/reset` | Reset all to HIGH_IMP + clear alerts | (empty body) | `{"ok": true}` |
| `POST /api/faults/clear` | Clear all fault status bits | (empty body) | `{"ok": true}` |
| `POST /api/faults/clear/?` | Clear per-channel fault | (empty body) | `{"ok": true, "channel": <int>}` |
| `POST /api/faults/mask` | Set global alert masks | `{"alertMask": <uint16>, "supplyMask": <uint16>}` | `{"ok": true}` |
| `POST /api/faults/mask/?` | Set per-channel alert mask | `{"mask": <uint16>}` | `{"ok": true, ...}` |
| `POST /api/diagnostics/config` | Configure diagnostic slot | `{"slot": <0-3>, "source": <0-13>}` | `{"ok": true, "slot": <int>, "source": <int>}` |
| `POST /api/gpio/?/config` | Configure GPIO mode | `{"mode": <0-4>, "pulldown": <bool>}` | `{"ok": true, ...}` |
| `POST /api/gpio/?/set` | Set GPIO output | `{"value": <bool>}` | `{"ok": true, "value": <bool>}` |
| `POST /api/uart/?/config` | Configure UART bridge | `{"uartNum": <0-2>, "txPin": <int>, "rxPin": <int>, "baudrate": <300-3M>, "dataBits": <5-8>, "parity": <0-2>, "stopBits": <0-2>, "enabled": <bool>}` | `{"ok": true, "id": <int>}` |
#### **OPTIONS /api/*** (CORS Preflight)
- Returns 204 with CORS headers
### Data Streaming Method
**HTTP Polling-based scope data:**
- Client sends `GET /api/scope?since=<seq>` with last received sequence
- Server returns new buckets since that sequence (ring buffer)
- Buckets: timestamp_ms, 4× channel avg, 4× channel min, 4× channel max
- Bucket interval: 10 ms
- No WebSocket or SSE
---
## 7. USB/SERIAL CONFIGURATION
### USB CDC (TinyUSB Composite Device)
**2 CDC Ports:**
1. **CDC #0:** CLI console (serial I/O for terminal menu)
2. **CDC #1:** UART bridge (transparent pass-through to external UART)
### Serial I/O (USB CDC #0 - CLI)
- **Baud rate:** N/A (USB, logical)
- **Functions:**
  - `serial_init()` – Initialize USB CDC
  - `serial_available()` – Check bytes available
  - `serial_read()` – Read single byte
  - `serial_print()` / `serial_println()` – Send string
  - `serial_printf()` – Printf-style output
### UART Bridge (USB CDC #1)
**Purpose:** Transparent bridge between USB host and external UART device
- **Configurable UART:** UART0, UART1, or UART2
- **Configurable pins:** Via web API (`/api/uart/0/config`)
- **Baud rate:** 300 bps to 3 Mbps (configurable)
- **Data bits:** 5-8
- **Parity:** None, Odd, Even
- **Stop bits:** 1, 1.5, 2
**Web API Configuration:**
```json
POST /api/uart/0/config
{
  "uartNum": 1,
  "txPin": 17,
  "rxPin": 18,
  "baudrate": 9600,
  "dataBits": 8,
  "parity": 0,
  "stopBits": 0,
  "enabled": true
}
```
---
## 8. WIFI CONFIGURATION
### WiFi Modes
- **AP (Access Point):**
  - SSID: `BugBuster`
  - Password: `bugbuster123`
  - IP: 192.168.4.1 (default)
  - Channel: 1
  - Max connections: 4
- **STA (Station):** Requires credentials from web UI or config
### WiFi Manager Functions
- `wifi_init(ap_ssid, ap_pass, sta_ssid, sta_pass)` – Initialize AP+STA
- `wifi_connect(ssid, pass)` – Switch STA to new network
- `wifi_is_connected()` – Check STA connection
- `wifi_get_sta_ip()` – Get STA IP address
- `wifi_get_ap_ip()` – Get AP IP address
- `wifi_get_ap_mac()` – Get AP MAC
- `wifi_get_sta_ssid()` – Get connected SSID
- `wifi_get_rssi()` – Get signal strength (dBm)
---
## 9. FREERTOS TASK STRUCTURE
### Task 1: ADC Poll (taskAdcPoll)
- **Core:** Core 1
- **Priority:** 3
- **Interval:** Dynamic 1-100 ms (based on fastest ADC rate)
- **Purpose:**
  - Read all non-HIGH_IMP channel ADC results
  - Accumulate into 10 ms scope buckets (min/max/avg)
  - Update g_deviceState under mutex
- **Scope buffering:** Ring of 256 buckets, 10 ms each
### Task 2: Fault Monitor (taskFaultMonitor)
- **Core:** Core 1
- **Priority:** 4
- **Interval:** 200 ms (5 Hz)
- **Purpose:**
  - Read alert status (global + per-channel)
  - Read DIN counters (for DIN_LOGIC/DIN_LOOP channels)
  - Read GPIO input states
  - Read diagnostics every 5th iteration (~1 sec)
  - Update g_deviceState under mutex
### Task 3: Command Processor (taskCommandProcessor)
- **Core:** Core 1
- **Priority:** 2 (higher = runs first)
- **Blocking:** Waits on command queue
- **Purpose:**
  - Dequeue commands from webserver/CLI
  - Execute device operations (function switch, DAC set, config, etc.)
  - Update g_deviceState under mutex
### Task 4: Main Loop (mainLoopTask)
- **Core:** Core 0
- **Priority:** 1
- **Interval:** 20 ms (50 Hz)
- **Purpose:**
  - Process CLI commands (non-blocking)
  - Print heartbeat every 30 seconds
### Global State
- **g_deviceState:** Shared DeviceState struct (mutex-protected)
- **g_stateMutex:** Binary semaphore (100 ms timeout typical)
- **g_cmdQueue:** Command queue (100 ms timeout on send)
---
## 10. COMMAND STRUCTURE & PROTOCOL
### Command Types (Enqueued via sendCommand())
```c
enum CommandType {
    CMD_SET_CHANNEL_FUNC,
    CMD_SET_DAC_CODE,
    CMD_SET_DAC_VOLTAGE,
    CMD_SET_DAC_CURRENT,
    CMD_ADC_CONFIG,
    CMD_DIN_CONFIG,
    CMD_DO_CONFIG,
    CMD_DO_SET,
    CMD_CLEAR_ALERTS,
    CMD_CLEAR_CHANNEL_ALERT,
    CMD_SET_ALERT_MASK,
    CMD_SET_CH_ALERT_MASK,
    CMD_SET_SUPPLY_ALERT_MASK,
    CMD_SET_VOUT_RANGE,
    CMD_SET_CURRENT_LIMIT,
    CMD_DIAG_CONFIG,
    CMD_SET_AVDD_SELECT,
    CMD_GPIO_CONFIG,
    CMD_GPIO_SET,
};
```
### SPI Protocol
- **Frame format:** 40-bit, MSB first, SPI Mode 2 (CPOL=1, CPHA=0)
- **Frame structure:**
  - Byte 0: [D39:D38]=00 (write), [D37:D36]=dev_addr, [D35:D32]=reserved
  - Byte 1: Register address
  - Bytes 2-3: Data (16-bit, MSB first)
  - Byte 4: CRC-8 (polynomial 0x07, init 0x00)
- **CRC-8:** Computed over bytes 0-3, appended as byte 4
- **CS behavior:** SYNC pin active-low (pulled low during frame, released after)
---
## 11. APPLICATION FLOW & INITIALIZATION
### Boot Sequence (app_main)
1. Initialize USB CDC (TinyUSB), wait 500 ms
2. Initialize serial I/O (CLI over USB #0)
3. Set RESET pin HIGH
4. Configure ALERT and ADC_RDY pins (input pullup)
5. Initialize WiFi (AP+STA mode)
6. Mount SPIFFS (web files)
7. Initialize AD74416H:
   - Hardware reset pulse
   - SPI verification via SCRATCH register
   - Clear all alerts
   - Enable internal reference
8. Set up diagnostics
9. Start ADC continuous mode (diag only, channels disabled)
10. Initialize FreeRTOS tasks
11. Start UART bridge
12. Start HTTP web server (port 80)
13. Initialize CLI
14. Create main loop task
15. Heartbeat output every 30 seconds
### State Machine
- **Device:** AD74416H (single instance, global)
- **SPI Driver:** Single instance, thread-safe via mutex
- **HTTP Server:** Single instance, handles all API requests
- **Command Queue:** Single queue, non-blocking enqueue, blocking dequeue
---
## 12. DIAGNOSTIC SOURCES (4 Slots)
| Source Code | Name | Transfer Function | Unit |
|-------------|------|-------------------|------|
| 0 | AGND | V = code / 65536 * 2.5 V | V |
| 1 | TEMP | T = code / 512 - 273.15 °C | °C |
| 2 | DVCC | V = code / 65536 * 2.5 V | V |
| 3 | AVCC | V = code / 65536 * 2.5 V | V |
| 4 | LDO1V8 | V = code / 65536 * 2.5 V | V |
| 5 | AVDD_HI | V = code / 65536 * 2.5 V | V |
| 6 | AVDD_LO | V = code / 65536 * 2.5 V | V |
| 7 | AVSS | V = code / 65536 * 2.5 V | V |
| 8 | LVIN | V = code / 65536 * 2.5 V | V |
| 9 | DO_VDD | V = code / 65536 * 2.5 V | V |
| 10 | VSENSEP | V = code / 65536 * 2.5 V | V |
| 11 | VSENSEN | V = code / 65536 * 2.5 V | V |
| 12 | DO_CURRENT | V = code / 65536 * 2.5 V | V |
| 13 | AVDD | V = code / 65536 * 2.5 V | V |
**Default setup:** Slot0=TEMP, Slot1=AVDD_HI, Slot2=DVCC, Slot3=AVCC
---
## 13. SERIAL CLI INTERFACE
Available commands (non-blocking, menu-driven):
- `help`, `h`, `?` – Display help
- `status`, `s` – Show current state
- `rreg <addr>` – Read register
- `wreg <addr> <value>` – Write register
- `func <ch> <func>` – Set channel function
- `adc <ch>` – Read ADC (raw + converted)
- `diag` – Show diagnostics
- `dac <ch> <code>` – Set DAC code
- `din` – Show DIN status
- `do <ch> <on|off>` – Set digital output
- `faults` – Show fault status
- `cf` – Clear all faults
- `temp` – Show die temperature
- `reset` – Hardware reset
- `scratch <value>` – SCRATCH register test
- `sweep <ch> <step> <delay>` – Voltage/current sweep
- `diag_cfg <slot> <source>` – Configure diagnostic
- `diag_read` – Read diagnostics
- `ilimit <ch> <0|1>` – Set current limit
- `vrange <ch> <0|1>` – Set voltage range
- `avdd <ch> <0-3>` – Set AVDD select
- `silicon` – Show silicon ID/revision
- `regs` – Dump all registers
- `gpio <gpio> <mode>` – Configure GPIO
- `wifi <ssid> <pass>` – Connect WiFi
---
## 14. KEY PERFORMANCE CHARACTERISTICS
| Metric | Value |
|--------|-------|
| **ADC Resolution** | 24-bit (16.7M codes) |
| **ADC Max Rate** | 9.6 kSPS |
| **DAC Resolution** | 16-bit (65,536 codes) |
| **DAC Max Output** | 25 mA or 12 V |
| **SPI Clock** | 1 MHz (verified safe) |
| **Scope Buffer** | 256 × 10 ms buckets (2.56 s) |
| **Fault Monitor Rate** | 200 ms (5 Hz) |
| **Typical HTTP Latency** | <100 ms (command queue) |
| **Mutex Timeout** | 50-100 ms typical |
| **WiFi Mode** | AP+STA (simultaneous) |
| **UART Bridge Rates** | 300 bps – 3 Mbps |
| **Temperature Range** | Die temp via DIAG0 |
---
## 15. DESIGN PATTERNS & SAFETY FEATURES
- **Mutex-protected state:** All shared device state protected by g_stateMutex
- **Safe channel switching:** 300 µs settling delays, HIGH_IMP intermediate state
- **Command queue:** Non-blocking from HTTP/CLI, blocking processor task
- **SPI error handling:** CRC-8 verification on all transfers, retry logic
- **SPIFFS fallback:** Auto-format if corrupted
- **Heartbeat monitoring:** 30-second uptime heartbeat via serial
- **Alert masking:** Global + per-channel + supply alert masks
- **ADC data latching:** UPR register read latches lower 16 bits automatically