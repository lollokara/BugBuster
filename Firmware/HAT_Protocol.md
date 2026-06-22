# BugBuster HAT Protocol Specification

**Version:** 2.0 (`bb-hat-3.3`)
**Date:** 2026-06-15
**Transport:** UART 921600 8N1 (GPIO43 TX, GPIO44 RX)
**Roles:** BugBuster = Master, HAT = Slave

---

## 1. Overview

The HAT (Hardware Attached on Top) protocol defines communication between the
BugBuster main board and expansion HAT boards connected via the HAT header.
BugBuster is always the bus master — it initiates all transactions.

### 1.0 Scope — what this UART carries (and doesn't)

This UART protocol is the **HAT control plane**: configuration, status polling,
power management, pin routing, and Logic Analyzer arming.
Frames are small (≤ 32-byte payload) and master-polled.

**High-bandwidth data does NOT flow over this UART.** The RP2040 HAT exposes
its own USB device, and the host talks to it directly for:

- **CMSIS-DAP v2 SWD debug** — vendor interface 1 (EP `0x04`/`0x85`). Host
  debug tools (OpenOCD, pyOCD, probe-rs, VS Code) connect straight to the
  RP2040 USB; neither BugBuster nor this UART is in the debug path.
- **Logic Analyzer streaming and one-shot readout** — vendor bulk interface 0
  (EP `0x06`/`0x87`). LA sample bytes stream from PIO → DMA → USB FIFO →
  host libusb claim, completely bypassing this UART. See
  [`../Docs/LogicAnalyzer.md`](../Docs/LogicAnalyzer.md) for the packet
  format, ring buffer sizing, and rearm protocol.
- **Target UART bridge** — CDC interfaces 2/3 on the RP2040 USB.

So this HAT UART protocol sees `LA_CONFIG` / `LA_ARM` / `LA_STREAM_START` /
`LA_STOP` (tiny control frames) but never the captured samples themselves.
That split is what makes sustained 1 MHz / 4-ch LA streaming possible — a
921600-baud UART could never carry 4 MB/s of raw LA data.

### 1.1 Physical Interface

| Signal | GPIO | Direction | Description |
|--------|------|-----------|-------------|
| HAT_TX | GPIO43 (TXD0) | BugBuster → HAT | UART transmit |
| HAT_RX | GPIO44 (RXD0) | HAT → BugBuster | UART receive |
| HAT_DETECT | GPIO47 | BugBuster input | HAT presence detect |
| HAT_IRQ | GPIO15 | Open-drain, bidirectional | Shared interrupt (active LOW) |
| EXP_EXT_1 | Via MUX S4 | Configurable | Expansion I/O line 1 |
| EXP_EXT_2 | Via MUX S4 | Configurable | Expansion I/O line 2 |
| EXP_EXT_3 | Via MUX S4 | Configurable | Expansion I/O line 3 |
| EXP_EXT_4 | Via MUX S4 | Configurable | Expansion I/O line 4 |

### 1.2 UART Configuration

| Parameter | Value |
|-----------|-------|
| Baud rate | 921600 |
| Data bits | 8 |
| Parity | None |
| Stop bits | 1 |
| Flow control | None |

### 1.3 Design Goals

- Simple, low-overhead framing suitable for microcontroller implementation
- CRC-8 integrity check on every frame
- Deterministic response times (all commands have timeouts)
- Extensible command space for future HAT types
- Master-slave architecture — HAT never transmits unsolicited data on UART (uses IRQ pin instead)

---

## 2. HAT Detection

Before UART communication begins, BugBuster identifies whether a HAT is
present using GPIO47.

### 2.1 Detection Circuit

```
BugBuster                    HAT Board
    │                            │
    ├── 10kΩ ── 3.3V             │
    │                            │
GPIO47 ────────────────────── DETECT_PIN
    │                            │
    │                 ├── R_ID ── GND
    │                            │
```

BugBuster has a fixed 10kΩ pull-up to 3.3V. HAT boards pull the line LOW
via R_ID.

### 2.2 HAT Detection Modes

| Board Revision | Method | No HAT | HAT Present |
|----------------|--------|--------|-------------|
| Breadboard (`bb-hat-1.x/2.x`) | ADC voltage threshold | > 2.5 V | 1.2 V – 2.1 V |
| PCB (`bb-hat-3.0+`) | **Digital input** | GPIO47 = HIGH | GPIO47 = LOW |

On PCB revision (`bb-hat-3.0`), the ADC-based multi-type detection is
disabled. GPIO47 is sampled as a simple logic input: HIGH means no HAT,
LOW means HAT present. The HAT type is determined after UART connection
via `GET_INFO` (0x02) and `GET_CAPS` (0x06).

### 2.3 HAT Type Table (legacy ADC path)

| HAT Type | R_ID | V_detect | ADC Range | ID Code |
|----------|------|----------|-----------|---------|
| No HAT | Open (∞) | ~3.3V | > 2.5V | 0x00 |
| SWD/GPIO | 10kΩ | ~1.65V | 1.2V – 2.1V | 0x01 |
| *(Reserved)* | 4.7kΩ | ~1.06V | 0.8V – 1.2V | 0x02 |
| *(Reserved)* | 22kΩ | ~2.27V | 2.1V – 2.5V | 0x03 |
| *(Reserved)* | 2.2kΩ | ~0.60V | 0.3V – 0.8V | 0x04 |

### 2.4 Boot Detection Algorithm

**PCB mode (`bb-hat-3.0`):**
1. Configure GPIO47 as digital input with internal pull-up
2. Sample GPIO47 once after 10 ms settle
3. LOW → HAT present, proceed to UART initialization
4. HIGH → no HAT, stop

**Breadboard mode (legacy):**
1. Configure GPIO47 as ADC input (12-bit, 0–3.3V)
2. Take 8 readings with 2 ms spacing, average valid readings
3. Map voltage to HAT type using §2.3 table
4. HAT type ≠ NONE → proceed to UART initialization

---

## 3. Frame Format

All UART communication uses a fixed frame structure. Both commands
(master → slave) and responses (slave → master) share the same format.

### 3.1 Frame Structure

```
┌──────┬─────┬─────┬────────────────────┬──────┐
│ SYNC │ LEN │ CMD │    PAYLOAD         │ CRC  │
│ 1B   │ 1B  │ 1B  │    0..32 B         │ 1B   │
└──────┴─────┴─────┴────────────────────┴──────┘
```

| Field | Size | Description |
|-------|------|-------------|
| SYNC | 1 byte | Frame start marker: **0xAA** |
| LEN | 1 byte | Payload length in bytes (0–32). Excludes SYNC, LEN, CMD, and CRC. |
| CMD | 1 byte | Command ID (0x01–0x7F) or Response ID (0x80–0xFF) |
| PAYLOAD | 0–32 bytes | Command-specific data. May be empty (LEN=0). |
| CRC | 1 byte | CRC-8 over CMD + PAYLOAD bytes |

### 3.2 CRC-8 Calculation

- **Polynomial:** 0x07 (x^8 + x^2 + x + 1) — same as AD74416H SPI CRC
- **Initial value:** 0x00
- **Input:** CMD byte + all PAYLOAD bytes
- **Does NOT include** SYNC or LEN in the CRC computation

**Reference implementation (C):**

```c
uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1);
        }
    }
    return crc;
}
```

### 3.3 Byte Order

All multi-byte integers are **little-endian** (LSB first), consistent with BBP.

### 3.4 Maximum Frame Size

- Minimum frame: 4 bytes (SYNC + LEN=0 + CMD + CRC)
- Maximum frame: 36 bytes (SYNC + LEN=32 + CMD + 32 payload + CRC)
- Maximum payload: 32 bytes

---

## 4. Transaction Model

### 4.1 Command-Response

Every transaction is initiated by the master (BugBuster).

```
Master (BugBuster)          Slave (HAT)
       │                        │
       │── Command Frame ──────>│
       │                        │ (process)
       │<── Response Frame ─────│
       │                        │
```

### 4.2 Timeouts

| Command | Timeout | Action on Timeout |
|---------|---------|-------------------|
| PING | 200 ms | Mark HAT as disconnected |
| GET_INFO / GET_CAPS / GET_PIN_CONFIG | 200 ms | Retry once, then mark disconnected |
| SET_PIN_CONFIG / SET_LED_STATE | 300 ms | Report failure |
| SET_POWER / SET_IO_VOLTAGE / SET_LEVEL_SHIFT / SET_RAIL_VOLTAGE | 300 ms | Report failure |
| GET_RAIL_STATUS / SET_RAIL_ENABLE | 500 ms | Report failure |
| CALIBRATE_START / CALIBRATE_STATUS / CALIBRATE_IMPORT | 500 ms | Report failure |
| LA_CONFIG / LA_SET_TRIGGER | 500 ms | Report failure |
| LA_ARM / LA_FORCE / LA_SET_ROUTE / LA_LOG_ENABLE | 200 ms | Report failure |
| LA_STOP / LA_STREAM_START | 2000 ms | Report failure |
| LA_USB_RESET | 500 ms | Report failure |
| RESET | 500 ms | Report reset failure |

### 4.3 Interrupt Line (GPIO15)

The HAT may assert GPIO15 LOW to signal that it has status to report.
The master should then poll with `GET_INFO` or `GET_RAIL_STATUS`.

**Interrupt protocol:**
1. HAT pulls GPIO15 LOW (open-drain assert)
2. BugBuster detects falling edge
3. BugBuster sends appropriate query command
4. HAT responds and releases GPIO15 (high-Z)
5. External pull-up restores line HIGH

**The HAT must NEVER transmit on UART without being polled.**

---

## 5. Command Reference

### 5.1 Commands (Master → Slave)

Command IDs use the range **0x01–0x7F**.

---

### Group 1 — Discovery & Configuration (0x01–0x06)

#### 0x01 PING

Connectivity check. HAT responds immediately.

**Request payload:** Empty (LEN=0)
**Response:** RSP_OK (0x80), empty payload
**Timeout:** 200 ms

**Example frame:**
```
TX: AA 00 01 01
         │  │  └─ CRC8([0x01]) = 0x01
         │  └──── CMD = PING
         └─────── LEN = 0
```

---

#### 0x02 GET_INFO

Request HAT identification and firmware version.

**Request payload:** Empty

**Response:** RSP_INFO (0x82)

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | hat_type | u8 | HAT type code (matches detection table) |
| 1 | fw_major | u8 | Firmware version major |
| 2 | fw_minor | u8 | Firmware version minor |

**Timeout:** 200 ms

---

#### 0x03 SET_PIN_CONFIG

Configure EXP_EXT pin function assignments.

**Mode A — Single pin** (LEN=2):

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | pin | u8 | 0=EXP_EXT_1 … 3=EXP_EXT_4 |
| 1 | function | u8 | Pin function code (see §6) |

**Mode B — All pins** (LEN=4):

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | ext1_func | u8 | EXP_EXT_1 function |
| 1 | ext2_func | u8 | EXP_EXT_2 function |
| 2 | ext3_func | u8 | EXP_EXT_3 function |
| 3 | ext4_func | u8 | EXP_EXT_4 function |

**Response:** RSP_OK (0x80), empty payload
**Timeout:** 300 ms

---

#### 0x04 GET_PIN_CONFIG

Read current EXP_EXT pin assignments.

**Request payload:** Empty

**Response:** RSP_OK (0x80)

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | ext1_func | u8 | Current EXP_EXT_1 function |
| 1 | ext2_func | u8 | Current EXP_EXT_2 function |
| 2 | ext3_func | u8 | Current EXP_EXT_3 function |
| 3 | ext4_func | u8 | Current EXP_EXT_4 function |

**Timeout:** 200 ms

---

#### 0x05 RESET

Reset HAT to default state. All EXP_EXT pins set to DISCONNECTED (0x00).

**Request payload:** Empty
**Response:** RSP_OK (0x80), empty payload
**Timeout:** 500 ms

---

#### 0x06 GET_CAPS

Query HAT hardware capabilities and firmware version. Must be called after
`GET_INFO` to determine which command groups are available.

**Request payload:** Empty

**Response:** RSP_CAPS (0x87)

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | hw_revision | u8 | PCB hardware revision (currently 2) |
| 1–4 | flags | u32 | Capability bitmask (LE) |
| 5 | rail_count | u8 | Number of adjustable rails (3 on `bb-hat-3.0`) |
| 6 | led_count | u8 | Number of WS2812B LEDs |
| 7 | shifted_io_count | u8 | Number of high-speed shifted IO pins |
| 8 | la_routes | u8 | Available LA route bitmask (bit0=low-speed, bit1=high-speed) |
| 9 | fw_major | u8 | Firmware major version |
| 10 | fw_minor | u8 | Firmware minor version |

**Capability flags (`flags` field):**

| Bit | Constant | Description |
|-----|----------|-------------|
| 0 | `HAT_CAP_RAILS` | Adjustable supply rails (VADJ3/VADJ4/3V3_ADJ) |
| 1 | `HAT_CAP_LEDS` | WS2812B LED strip present |
| 2 | `HAT_CAP_LA_LOW_SPEED` | Low-speed LA route via EXP_EXT pins |
| 3 | `HAT_CAP_LA_HIGH_SPEED` | High-speed LA route via direct PIO GPIOs |
| 4 | `HAT_CAP_SHIFTED_IO` | Level-shifted IO bank present |

`bb-hat-3.0` reports `flags = 0x17` (bits 0, 1, 2, 4) and `la_routes = 0x03`.

**Timeout:** 200 ms

---

### Group 2 — Power & IO Voltage (0x10–0x13)

#### 0x10 SET_POWER

Enable or disable power to a HAT connector (Target 1 or Target 2).

**Request payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | connector | u8 | 0=Connector A (Target 1), 1=Connector B (Target 2) |
| 1 | enable | bool | true=enable power, false=disable |

**Response:** RSP_POWER_STATUS (0x83)

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | a_enabled | bool | Connector A power state |
| 1–4 | a_current_ma | f32 | Connector A measured current (mA) |
| 5 | a_fault | bool | Connector A overcurrent fault |
| 6 | b_enabled | bool | Connector B power state |
| 7–10 | b_current_ma | f32 | Connector B measured current (mA) |
| 11 | b_fault | bool | Connector B overcurrent fault |

**Timeout:** 300 ms

---

#### 0x11 GET_POWER_STATUS

Read power status for both connectors.

**Request payload:** Empty
**Response:** RSP_POWER_STATUS (0x83) — same layout as SET_POWER response
**Timeout:** 300 ms

---

#### 0x12 SET_IO_VOLTAGE

Set the IO voltage level.

**Request payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0–1 | voltage_mv | u16 | Target preset voltage in mV (LE) |

**Supported preset voltages:** `1200`, `1800`, `2500`, `3300`, `5000`

**Response:** RSP_OK (0x80) + `[actual_mv:u16]`

**Timeout:** 300 ms

---

#### 0x13 GET_IO_VOLTAGE

Read the currently applied IO voltage.

**Request payload:** Empty

**Response:** RSP_OK (0x80) + `[voltage_mv:u16, preset_index:u8]`

**Timeout:** 200 ms

---

### Group 3 — DAP / SWD Debug (0x20–0x22)

### Group 4 — DAP / SWD Debug (0x20–0x22)

#### 0x20 GET_DAP_STATUS

Query CMSIS-DAP USB connection and SWD target detection state.

**Request payload:** Empty

**Response:** RSP_DAP_STATUS (0x84)

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | dap_connected | bool | USB CMSIS-DAP host is connected |
| 1 | target_detect | bool | SWD target responding |
| 2–5 | target_dpidr | u32 | Target DPIDR value (LE; 0 if no target) |

**Timeout:** 200 ms

---

#### 0x21 GET_TARGET_INFO

Extended SWD target information.

**Request payload:** Empty

**Response:** RSP_OK (0x80) + `[dpidr:u32, swd_clock_khz:u16]`

**Timeout:** 200 ms

---

#### 0x22 SET_SWD_CLOCK

Adjust the SWD clock frequency.

**Request payload:** `[clock_khz:u16]`

**Response:** RSP_OK (0x80), empty

**Timeout:** 300 ms

---

### Group 5 — Logic Analyzer (0x30–0x3B)

All LA commands operate on the RP2040 PIO-based logic analyzer. The actual
sample data is transported over the RP2040 USB vendor-bulk endpoint, not this
UART. See [`../Docs/LogicAnalyzer.md`](../Docs/LogicAnalyzer.md).

**GPIO pins (bb-hat-3.0):** CH0=GPIO2, CH1=GPIO3, CH2=GPIO4, CH3=GPIO5

---

#### 0x30 LA_CONFIG

Configure capture parameters.

**Request payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | channels | u8 | Number of channels: 1, 2, or 4 |
| 1–4 | rate_hz | u32 | Desired sample rate in Hz (LE) |
| 5–8 | depth | u32 | Sample depth (0 = maximum for config, LE) |

Note: RLE enable is implied by the LA streaming mode; the HAT manages RLE
internally. `depth=0` selects the maximum for the channel/rate combination.

**Response:** RSP_OK (0x80), empty

**Timeout:** 500 ms

---

#### 0x31 LA_SET_TRIGGER

Set the trigger condition for the next capture.

**Request payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | type | u8 | Trigger type (see table below) |
| 1 | channel | u8 | Channel index (0–3) |

| Type | Name | Description |
|------|------|-------------|
| 0 | NONE | No trigger — capture starts immediately on arm |
| 1 | RISING | Rising edge on specified channel |
| 2 | FALLING | Falling edge on specified channel |
| 3 | BOTH | Any edge on specified channel |
| 4 | HIGH | Level high on specified channel |
| 5 | LOW | Level low on specified channel |

**Response:** RSP_OK (0x80), empty
**Timeout:** 500 ms

---

#### 0x32 LA_ARM

Arm the logic analyzer. Capture starts when trigger fires (or immediately
if trigger type is NONE).

**Request payload:** Empty
**Response:** RSP_OK (0x80) + `[state:u8]` (1 = ARMED)
**Timeout:** 200 ms

---

#### 0x33 LA_FORCE

Force-trigger the logic analyzer (bypasses trigger condition).

**Request payload:** Empty
**Response:** RSP_OK (0x80) + `[state:u8]` (2 = CAPTURING)
**Timeout:** 200 ms

---

#### 0x34 LA_GET_STATUS

Get current capture and streaming status.

**Request payload:** Empty

**Response:** RSP_LA_STATUS (0x85)

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | state | u8 | Capture state (see table below) |
| 1 | channels | u8 | Configured channels |
| 2–5 | samples_captured | u32 | Samples captured so far (LE) |
| 6–9 | total_samples | u32 | Target depth (LE) |
| 10–13 | actual_rate_hz | u32 | Actual sample rate (LE) |
| 14 | usb_connected | bool | RP2040 USB cable attached |
| 15 | usb_mounted | bool | RP2040 USB host configured |
| 16 | stream_stop_reason | u8 | Reason for last stream stop |
| 17–20 | overrun_count | u32 | DMA overruns detected (LE) |
| 21–24 | short_writes | u32 | Short USB writes (LE) |
| 25 | usb_rearm_pending | bool | USB rearm sequence in progress |
| 26 | usb_rearm_req_count | u8 | Rearm requests sent |
| 27 | usb_rearm_ack_count | u8 | Rearm ACKs received |

**Capture state codes:**

| Value | Name | Description |
|-------|------|-------------|
| 0 | IDLE | Not configured or stopped |
| 1 | ARMED | Waiting for trigger |
| 2 | CAPTURING | Trigger fired, DMA active |
| 3 | DONE | Capture complete, data ready |
| 4 | STREAMING | Continuous double-buffered mode |
| 5 | ERROR | Capture failed |

**Timeout:** 500 ms

---

#### 0x35 LA_READ_DATA

Read a chunk of captured data over UART (≤ 28 bytes per call). For large
captures, use the RP2040 USB bulk endpoint instead.

**Request payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0–3 | offset | u32 | Byte offset into capture buffer (LE) |
| 4–5 | length | u16 | Bytes to read (1–28, LE) |

**Response:** RSP_LA_DATA (0x86) + raw capture bytes (up to 28 bytes)
**Timeout:** 200 ms

---

#### 0x36 LA_STOP

Stop an active capture or disarm a waiting trigger.

**Request payload:** Empty
**Response:** RSP_OK (0x80), empty
**Timeout:** 2000 ms

---

#### 0x37 LA_STREAM_START

Start continuous streaming mode over the RP2040 USB vendor-bulk endpoint.
LA_CONFIG and LA_ARM must be called first.

**Request payload:** Empty
**Response:** RSP_OK (0x80), empty
**Timeout:** 2000 ms

---

#### 0x39 LA_LOG_ENABLE

Enable or disable verbose LA debug log relay from the RP2040 to the host
via the `BBP_EVT_LA_LOG` (0xEC) event stream.

**Request payload:** `[enable:bool]`
**Response:** RSP_OK (0x80), empty
**Timeout:** 200 ms

---

#### 0x3A LA_USB_RESET

Force reinitialize the RP2040 USB vendor-bulk endpoint. Use to recover from
a stuck streaming state.

**Request payload:** Empty
**Response:** RSP_OK (0x80), empty
**Timeout:** 500 ms

---

#### 0x3B LA_SET_ROUTE

Select the LA signal route.

**Request payload:** `[route:u8]`

| Value | Name | Description |
|-------|------|-------------|
| 0 | LOW_SPEED | Via EXP_EXT pins — shared with SWD, slower |
| 1 | HIGH_SPEED | Direct PIO GPIOs (GPIO2–GPIO5) — default for `bb-hat-3.0` |

**Response:** RSP_OK (0x80) + `[route:u8]` (applied route)
**Timeout:** 200 ms

---

### Group 6 — Rails, LEDs & Calibration (0x40–0x48)

These commands control the `bb-hat-3.0` PCB features: three adjustable
supply rails, a WS2812B LED strip, DS4424-based calibrated voltage control,
and the level-shifted IO bank. Requires `HAT_CAP_RAILS | HAT_CAP_LEDS |
HAT_CAP_SHIFTED_IO` flags in GET_CAPS response.

**Rail IDs:**

| ID | Name | Voltage Range | Notes |
|----|------|---------------|-------|
| 0 | 3V3_ADJ | 1.2 – 5.5 V | VLOGIC/level-shifter reference; set  SET_IO_VOLTAGE |
| 1 | VADJ3 | 1.8 – 36 V | Connector 1 HV rail (DS4424 + LTM8083) |
| 2 | VADJ4 | 1.8 – 36 V | Connector 2 / SWD rail (DS4424 + LTM8083) |

**Rail status codes (`status` field):**

| Code | Name |
|------|------|
| 0 | OK |
| 1 | OVERCURRENT_FAULT |
| 2 | CAL_INVALID — voltage set/enable rejected; calibration not valid |
| 3 | PG_FAIL |

---

#### 0x40 GET_RAIL_STATUS

Read voltage and current telemetry for all three rails.

**Request payload:** Empty

**Response:** RSP_RAIL_STATUS (0x88)

```
Offset  Field       Type   Description
0       count       u8     Number of rails (always 3)

Per rail (7 bytes, repeated count times):
+0      rail_id     u8     Rail ID (0=3V3_ADJ, 1=VADJ3, 2=VADJ4)
+1      enabled     bool   Rail is powered on
+2–3    voltage_mv  u16    Measured voltage in mV (LE)
+4–5    current_ma  u16    Current draw in mA (LE; ISMON minus no-load baseline)
+6      status      u8     Status code (see table above)

Total: 1 + 3 × 7 = 22 bytes
```

**Timeout:** 500 ms

---

#### 0x41 SET_RAIL_ENABLE

Enable or disable a supply rail.

**Request payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | rail_id | u8 | Rail ID (0–2) |
| 1 | enable | bool | true = enable, false = disable |

**Response:** RSP_RAIL_STATUS (0x88) — same format as GET_RAIL_STATUS

Returns `HAT_ERR_CAL_INVALID` (0x05) if the rail has no valid calibration
and `enable=true` is requested for VADJ3 or VADJ4.

**Timeout:** 500 ms

---

#### 0x42 SET_LED_STATE

Set the color of a single WS2812B LED. The HAT has 9 LEDs (indices 0–8).
LED 0 is typically a HAT heartbeat indicator; LEDs 2–8 map to connectors
and IO blocks.

**Request payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | led_index | u8 | LED index (0–8) |
| 1 | color_code | u8 | Color code (see table) |

**Color codes:**

| Code | Color | Semantic meaning |
|------|-------|-----------------|
| 0 | Off | No supply, no IO |
| 1 | Red | EFUSE fault / error |
| 2 | Green | Supply present + IO/MUX configured |
| 3 | Blue | Supply present, no IO configured |
| 4 | Yellow | IO configured, no supply |

**Response:** RSP_OK (0x80), empty
**Timeout:** 300 ms

---

#### 0x43 CALIBRATE_START

Start an automatic DS4424 calibration sweep for VADJ3 or VADJ4. The sweep
collects measured-voltage vs DAC-code pairs across the full 1.8–36 V range
into a candidate table. The operation runs asynchronously; poll with
CALIBRATE_STATUS (0x44) until `state = 2 (SUCCESS)` or `3 (FAILED)`.

**Request payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | rail_id | u8 | 1 = VADJ3, 2 = VADJ4 (rail 0 not supported) |

**Response:** RSP_OK (0x80) + `[status:u8]` where `status=1` means sweep started.

Returns RSP_ERROR + `BUSY` if a sweep is already running, or `CAL_INVALID` if the
level-shifter OE is active (U3 must be disabled before calibrating).

**Timeout:** 500 ms

---

#### 0x44 CALIBRATE_STATUS

Poll calibration sweep progress and retrieve results.

**Request payload:** Empty

**Response:** RSP_CALIBRATE_STATUS (0x8A)

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | state | u8 | 0=idle, 1=running, 2=success, 3=failed |
| 1 | progress | u8 | Completion 0–100 % |
| 2 | rail_id | u8 | Rail being calibrated |
| 3 | last_error | u8 | Error detail code |
| 4 | persist_state | u8 | 0=candidate only, 1=validated and saved to flash |
| 5 | stage | u8 | Internal sweep stage (diagnostic) |
| 6 | point | u8 | Points collected so far |
| 7 | code | i8 | Last DAC code written (signed) |
| 8–11 | measured_mv | i32 | Last measured voltage in mV (LE) |
| 12–15 | min_mv | i32 | Minimum measured voltage in mV |
| 16–19 | max_mv | i32 | Maximum measured voltage in mV |
| 20–23 | max_gap_mv | i32 | Largest voltage gap between consecutive points |
| 24–27 | max_error_mv | i32 | Maximum per-point deviation vs linear fit |
| 28–29 | validation_flags | u16 | Validation check bitmask |

**`validation_flags` bits:**

| Bit | Meaning |
|-----|---------|
| 0 | Insufficient points collected |
| 1 | Coverage gap — setpoint outside measured range |
| 2 | Max measured-voltage gap too large |
| 3 | Invalid (NaN/inf) measurement in table |
| 4 | Non-monotonic trend detected |

**Timeout:** 500 ms

---

#### 0x45 CALIBRATE_IMPORT

Import a pre-built calibration table (e.g., produced by an external host-side
sweep). Each point is `[code:i8, measured_mv:i32 LE]` = 5 bytes.

**Request payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | rail_id | u8 | 1 = VADJ3, 2 = VADJ4 |
| 1 | count | u8 | Number of calibration points (max 30 for UART frame budget) |
| 2.. | points | u8[] | Packed points: `[i8 code][i32 measured_mv]` per point |

**Response:** RSP_OK (0x80), empty
**Timeout:** 500 ms

---

#### 0x46 SET_IO_BANK

Set direction, pull-up, pull-down, and output value for the 8-pin high-speed
IO bank (GPIO6–GPIO9 on `bb-hat-3.0`). Each byte is a bitmask; bit 0 = IO
bank pin 0 (lowest GPIO index).

**Request payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | dirs | u8 | Direction bitmask: 1 = output, 0 = input |
| 1 | ups | u8 | Pull-up enable bitmask |
| 2 | dns | u8 | Pull-down enable bitmask |
| 3 | vals | u8 | Output value bitmask (only applied to output-configured pins) |

**Response:** RSP_OK (0x80), empty
**Timeout:** 300 ms

---

#### 0x47 SET_LEVEL_SHIFT

Set the level-shifter output-enable and direction control.

**Request payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | oe | bool | Output-enable: true = level-shifter active, false = high-Z |
| 1 | dir | bool | Direction: true = A→B (ESP32 → target), false = B→A (target → ESP32) |

**Response:** RSP_OK (0x80) + `[oe:bool, dir:bool]` (applied values)
**Timeout:** 300 ms

---

#### 0x48 SET_RAIL_VOLTAGE

Set the target voltage for VADJ3 or VADJ4. The DS4424 calibration table is
used to translate the requested millivolt value to a DAC code. Fails closed
with `CAL_INVALID` if no valid calibration is present.

**Request payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | rail_id | u8 | 1 = VADJ3, 2 = VADJ4 (rail 0 uses SET_IO_VOLTAGE instead) |
| 1–2 | voltage_mv | u16 | Target voltage in mV (LE, 1800–36000) |

**Response:** RSP_RAIL_STATUS (0x88) — same format as GET_RAIL_STATUS

Returns `HAT_ERR_CAL_INVALID` if calibration is absent or the requested
voltage is outside the calibrated range by more than 200 mV.

**Timeout:** 300 ms

---

### 5.2 Responses (Slave → Master)

Response IDs use the range **0x80–0xFF**.

| Code | Name | In response to | Payload summary |
|------|------|----------------|-----------------|
| 0x80 | RSP_OK | Most commands | Empty or command-specific |
| 0x81 | RSP_ERROR | Any | `[error_code:u8]` |
| 0x82 | RSP_INFO | GET_INFO (0x02) | `[hat_type, fw_major, fw_minor]` |
| 0x83 | RSP_POWER_STATUS | SET_POWER, GET_POWER_STATUS | `[a_en, a_ma_f32, a_fault, b_en, b_ma_f32, b_fault]` |
| 0x84 | RSP_DAP_STATUS | GET_DAP_STATUS (0x20) | `[dap_connected, target_detect, dpidr:u32]` |
| 0x85 | RSP_LA_STATUS | LA_GET_STATUS (0x34) | 28-byte LA status payload (see §5.1 Group 5) |
| 0x86 | RSP_LA_DATA | LA_READ_DATA (0x35) | Raw captured bytes (≤ 28) |
| 0x87 | RSP_CAPS | GET_CAPS (0x06) | 12-byte capabilities payload (see §5.1 Group 1) |
| 0x88 | RSP_RAIL_STATUS | GET_RAIL_STATUS, SET_RAIL_ENABLE, SET_RAIL_VOLTAGE | `[count:u8, rail×7]` |
| 0x89 | RSP_LA_LOG | Unsolicited | Log line bytes (≤ 32 chars) — forwarded to host via `BBP_EVT_LA_LOG` (0xEC) |
| 0x8A | RSP_CALIBRATE_STATUS | CALIBRATE_STATUS (0x44) | 30-byte calibration status payload (see §5.1 Group 6) |

### 5.3 Error Codes

| Code | Name | Description |
|------|------|-------------|
| 0x01 | INVALID_CMD | Unknown command ID |
| 0x02 | INVALID_PIN | Pin index out of range (must be 0–3) |
| 0x03 | INVALID_FUNC | Function code out of range |
| 0x04 | BUSY | HAT is busy (e.g., calibration sweep running) |
| 0x05 | CAL_INVALID | No valid calibration; voltage set/enable rejected |
| 0x06 | CRC_ERROR | CRC check failed on received frame |
| 0x07 | FRAME_ERROR | Malformed frame (bad LEN, missing bytes) |
| 0x08 | UNSAFE_REGISTER | Register access blocked |
| 0x09 | INVALID_ARG | Parameter out of allowed range |

---

## 6. Pin Function Codes

Each EXP_EXT pin is assigned a function using a single byte:

| Code | Name | Description | Typical Use |
|------|------|-------------|-------------|
| 0x00 | DISCONNECTED | Pin not routed / high-impedance | Default after reset |
| 0x01 | SWDIO | Serial Wire Debug data I/O | ARM Cortex-M debug |
| 0x02 | SWCLK | Serial Wire Debug clock | ARM Cortex-M debug |
| 0x03 | TRACE1 | Trace data line 1 / SWO | ARM SWV/ITM trace output |
| 0x04 | TRACE2 | Trace data line 2 | Extended trace |
| 0x05 | GPIO1 | General-purpose I/O 1 | Logic analyzer, trigger |
| 0x06 | GPIO2 | General-purpose I/O 2 | Logic analyzer, trigger |
| 0x07 | GPIO3 | General-purpose I/O 3 | Logic analyzer, trigger |
| 0x08 | GPIO4 | General-purpose I/O 4 | Logic analyzer, trigger |
| 0x09–0xFF | *(Reserved)* | For future HAT types | — |

Codes 0x01–0x08 are **reserved** when SWD pin slots 1–4 are in use; sending
them returns `INVALID_FUNC`.

### 6.1 Recommended Presets

**SWD Debug:**
```
EXP_EXT_1 = SWDIO  (0x01)
EXP_EXT_2 = SWCLK  (0x02)
EXP_EXT_3 = TRACE1 (0x03)
EXP_EXT_4 = TRACE2 (0x04)
```

**SWD + SWO (3-pin debug with trace):**
```
EXP_EXT_1 = SWDIO  (0x01)
EXP_EXT_2 = SWCLK  (0x02)
EXP_EXT_3 = TRACE1 (0x03)
EXP_EXT_4 = GPIO4  (0x08)
```

**GPIO Mode (logic analyzer / general I/O):**
```
EXP_EXT_1 = GPIO1  (0x05)
EXP_EXT_2 = GPIO2  (0x06)
EXP_EXT_3 = GPIO3  (0x07)
EXP_EXT_4 = GPIO4  (0x08)
```

---

## 7. Initialization Sequence

### 7.1 BugBuster Boot (Master Side)

```
1.  Detect HAT (see §2.4)
2.  If no HAT → stop
3.  Initialize UART: 921600 8N1 on GPIO43/GPIO44
4.  Configure GPIO15 as open-drain input with pull-up
5.  Flush UART RX buffer
6.  Send PING (0x01), wait 200 ms for RSP_OK
7.  If no response → retry once; if still silent → mark "detected but not responding"
8.  Send GET_INFO (0x02), parse hat_type + fw_major/minor
9.  Send GET_CAPS (0x06), parse capability flags and resource counts
10. Send GET_PIN_CONFIG (0x04), cache current pin state
11. Send GET_RAIL_STATUS (0x40), cache rail states
12. Report HAT status to host application
```

### 7.2 HAT Boot (Slave Side)

```
1.  Initialize UART: 921600 8N1
2.  Set R_ID pull-down (PCB: GPIO47 low via board trace)
3.  Configure GPIO15 as open-drain output, release HIGH
4.  Set all EXP_EXT pins to DISCONNECTED (safe default)
5.  Initialize WS2812B LEDs to Off
6.  Initialize DS4424 IDAC; load calibration from flash if present
7.  Enter idle state: wait for UART commands
```

---

## 8. Interrupt Protocol (GPIO15)

GPIO15 is a shared open-drain line with external pull-up.

### 8.1 HAT → BugBuster (Attention Request)

HAT asserts GPIO15 when it has a status change to report (rail fault,
DAP connection state change, overcurrent trip).

1. HAT pulls GPIO15 LOW
2. BugBuster ISR detects falling edge
3. BugBuster sends GET_RAIL_STATUS (0x40) or GET_INFO (0x02)
4. HAT responds with current state
5. HAT releases GPIO15 (high-Z)

### 8.2 BugBuster → HAT (Wake)

BugBuster may pulse GPIO15 LOW briefly (1 ms) to wake a sleeping HAT.

1. BugBuster pulls GPIO15 LOW for 1 ms
2. BugBuster releases GPIO15
3. BugBuster waits 5 ms for HAT to wake
4. BugBuster sends UART command

---

## 9. Timing Requirements

| Parameter | Value | Notes |
|-----------|-------|-------|
| Inter-frame gap (master) | ≥ 1 ms | Minimum between command frames |
| Response latency (slave) | ≤ 50 ms | First response byte from last command byte |
| IRQ assert duration (HAT) | ≤ 100 ms | HAT must release if not polled |
| IRQ wake pulse (BugBuster) | 1 ms | Minimum for HAT wake |
| Post-reset settle time | 100 ms | After RESET command before next command |

---

## 10. Implementation Notes

### 10.1 HAT Firmware Guidelines

- The HAT must not transmit UART data unless responding to a command
- All pin reconfigurations must be complete **before** sending RSP_OK
- On CRC error, respond with RSP_ERROR + CRC_ERROR code (0x06)
- On unknown command, respond with RSP_ERROR + INVALID_CMD code (0x01)
- Implement a 500 ms watchdog on the UART RX state machine
- CALIBRATE_START on rail 0 (3V3_ADJ) returns INVALID_ARG — only VADJ3/VADJ4 calibrate

### 10.2 BugBuster Firmware Guidelines

- Always flush UART RX before sending a command (discard stale data)
- Use the SYNC byte (0xAA) to resynchronize after errors
- On timeout, assume HAT is busy or disconnected
- Periodically poll GET_RAIL_STATUS to update LED state and supply telemetry
- Cache GET_CAPS result across the session; only re-query on reconnect

### 10.3 Future Extensibility

- Command IDs 0x07–0x0F and 0x49–0x7F are reserved for future commands
- Response IDs 0x8B–0xFF are reserved for future response types
- Pin function codes 0x09–0xFF are reserved for future HAT types
- HAT type codes 0x02–0xFE are reserved for future hardware variants
- The LEN field allows payloads up to 32 bytes; multi-frame transfers require
  a new command definition if larger payloads are needed

---

## Appendix A: Complete Frame Examples

### A.1 PING Transaction

```
Master TX: AA 00 01 01
                │  │  └─ CRC8([0x01]) = 0x01
                │  └──── CMD = 0x01 (PING)
                └─────── LEN = 0

Slave  TX: AA 00 80 80
                │  │  └─ CRC8([0x80]) = 0x80
                │  └──── CMD = 0x80 (RSP_OK)
                └─────── LEN = 0
```

### A.2 GET_INFO Transaction

```
Master TX: AA 00 02 02

Slave  TX: AA 03 82 01 03 00 [CRC]
                │  │  │  │  │  └─ fw_minor = 0
                │  │  │  │  └──── fw_major = 3
                │  │  │  └─────── hat_type = 1 (SWD/GPIO)
                │  │  └────────── CMD = 0x82 (RSP_INFO)
                │  └───────────── LEN = 3
                └──────────────── SYNC
```

### A.3 SET_PIN_CONFIG — SWD Debug Preset

```
Master TX: AA 04 03 01 02 03 04 [CRC]
                │  │  │  │  │  │
                │  │  │  │  │  └─ EXT4 = TRACE2 (0x04)
                │  │  │  │  └──── EXT3 = TRACE1 (0x03)
                │  │  │  └─────── EXT2 = SWCLK  (0x02)
                │  │  └────────── EXT1 = SWDIO  (0x01)
                │  └───────────── CMD = 0x03 (SET_PIN_CONFIG)
                └──────────────── LEN = 4

Slave TX: AA 00 80 80   (RSP_OK)
```

### A.4 CALIBRATE_START for VADJ3

```
Master TX: AA 01 43 01 [CRC]
                │  │  │  └─ rail_id = 1 (VADJ3)
                │  │  └──── CMD = 0x43 (CALIBRATE_START)
                │  └─────── LEN = 1
                └────────── SYNC

Slave  TX: AA 01 80 01 [CRC]
                │  │  │  └─ status = 1 (sweep started)
                │  │  └──── CMD = 0x80 (RSP_OK)
                │  └─────── LEN = 1
                └────────── SYNC
```

### A.5 Error Response

```
Slave TX: AA 01 81 05 [CRC]
               │  │  │
               │  │  └─ error_code = 0x05 (CAL_INVALID)
               │  └──── CMD = 0x81 (RSP_ERROR)
               └─────── LEN = 1
```
