# BugBuster HAT Protocol Specification

**Version:** 1.1 (LA data-plane clarifications for `bb-hat-2.0`)
**Date:** 2026-04-17
**Transport:** UART 921600 8N1 (GPIO43 TX, GPIO44 RX)
**Roles:** BugBuster = Master, HAT = Slave

---

## 1. Overview

The HAT (Hardware Attached on Top) protocol defines communication between the BugBuster main board and expansion HAT boards connected via the HAT header. BugBuster is always the bus master — it initiates all transactions. The HAT responds to commands and may assert the shared interrupt line (GPIO15, open-drain) to request attention.

### 1.0 Scope — what this UART carries (and doesn't)

This UART protocol is the **HAT control plane**: configuration, status polling,
power management, pin routing, HVPAK access, and Logic Analyzer *arming*.
Frames are small (≤ 32-byte payload) and master-polled.

**High-bandwidth data does NOT flow over this UART.** The RP2040 HAT exposes
its own USB device, and the host talks to it directly for:

- **CMSIS-DAP v2 SWD debug** — vendor interface 1 (EP `0x04`/`0x85`). Host
  debug tools (OpenOCD, pyOCD, probe-rs, VS Code) connect straight to the
  RP2040 USB; neither BugBuster nor this UART is in the debug path.
- **Logic Analyzer streaming and one-shot readout** — vendor bulk interface 0
  (EP `0x06`/`0x87`). LA sample bytes stream from PIO 1 → DMA → USB FIFO →
  host libusb claim, completely bypassing this UART. See
  [`../Docs/LogicAnalyzer.md`](../Docs/LogicAnalyzer.md) for the packet
  format, ring buffer sizing, and rearm protocol.
- **Target UART bridge** — CDC interfaces 2/3 on the RP2040 USB.

So this HAT UART protocol sees `HAT_LA_CONFIG` / `HAT_LA_ARM` /
`HAT_LA_STREAM_START` / `HAT_LA_STOP` (tiny control frames) but never the
captured samples themselves. That split is what makes sustained 1 MHz / 4-ch
LA streaming possible — a 921600-baud UART could never carry 4 MB/s of raw
LA data.

### 1.1 Physical Interface

| Signal | GPIO | Direction | Description |
|--------|------|-----------|-------------|
| HAT_TX | GPIO43 (TXD0) | BugBuster → HAT | UART transmit |
| HAT_RX | GPIO44 (RXD0) | HAT → BugBuster | UART receive |
| HAT_DETECT | GPIO47 | BugBuster ADC input | Voltage divider for HAT identification |
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

Before UART communication begins, BugBuster identifies the attached HAT using an analog voltage divider on GPIO47.

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

BugBuster has a fixed 10kΩ pull-up to 3.3V. Each HAT type has a specific pull-down resistor (R_ID) creating a unique voltage:

```
V_detect = 3.3V × R_ID / (10kΩ + R_ID)
```

### 2.2 HAT Type Table

| HAT Type | R_ID | V_detect | ADC Range | ID Code |
|----------|------|----------|-----------|---------|
| No HAT | Open (∞) | ~3.3V | > 2.5V | 0x00 |
| SWD/GPIO | 10kΩ | ~1.65V | 1.2V – 2.1V | 0x01 |
| *(Reserved)* | 4.7kΩ | ~1.06V | 0.8V – 1.2V | 0x02 |
| *(Reserved)* | 22kΩ | ~2.27V | 2.1V – 2.5V | 0x03 |
| *(Reserved)* | 2.2kΩ | ~0.60V | 0.3V – 0.8V | 0x04 |

### 2.3 Detection Algorithm

1. Configure GPIO47 as ADC input (ADC1_CH6, 12-bit, 0–3.3V range)
2. Take 8 consecutive readings with 2ms spacing
3. Average the valid readings
4. Map voltage to HAT type using the threshold table above
5. If HAT detected (type ≠ NONE): proceed to UART initialization

---

## 3. Frame Format

All UART communication uses a fixed frame structure. Both commands (master → slave) and responses (slave → master) use the same format.

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

All multi-byte integers are **little-endian** (LSB first), consistent with the BugBuster Binary Protocol (BBP).

### 3.4 Maximum Frame Size

- Minimum frame: 4 bytes (SYNC + LEN=0 + CMD + CRC)
- Maximum frame: 36 bytes (SYNC + LEN=32 + CMD + 32 payload + CRC)
- Maximum payload: 32 bytes

---

## 4. Transaction Model

### 4.1 Command-Response

Every transaction is initiated by the master (BugBuster). The master sends a **command frame**, and the slave (HAT) must reply with a **response frame** within the specified timeout.

```
Master (BugBuster)          Slave (HAT)
       │                        │
       │── Command Frame ──────>│
       │                        │ (process)
       │<── Response Frame ─────│
       │                        │
```

### 4.2 Timeouts

| Scenario | Timeout | Action on Timeout |
|----------|---------|-------------------|
| PING | 200 ms | Mark HAT as disconnected |
| GET_INFO | 200 ms | Retry once, then mark disconnected |
| SET_PIN_CONFIG | 300 ms | Report config failure to host |
| GET_PIN_CONFIG | 200 ms | Use cached config |
| RESET | 500 ms | Report reset failure |

If the slave does not respond within the timeout, the master discards any partial frame data and may retry once.

### 4.3 Interrupt Line (GPIO15)

The HAT may assert the interrupt line (pull GPIO15 LOW) to signal that it has status to report. The master should then poll the HAT with a GET_INFO or GET_PIN_CONFIG command.

**Interrupt protocol:**
1. HAT pulls GPIO15 LOW (open-drain assert)
2. BugBuster detects falling edge
3. BugBuster sends appropriate query command
4. HAT responds and releases GPIO15 (return to high-Z)
5. External pull-up restores line HIGH

**Important:** The HAT must NEVER transmit on UART without being polled by the master. The interrupt line is the only mechanism for unsolicited signaling.

---

## 5. Command Reference

### 5.1 Commands (Master → Slave)

Commands use IDs in the range **0x01–0x7F**.

---

#### 0x01 PING

Connectivity check. The HAT should respond immediately with OK.

**Request payload:** Empty (LEN=0)

**Expected response:** RSP_OK (0x80), payload empty

**Timeout:** 200 ms

**Example frame:**
```
TX: AA 00 01 01
         │  │  └─ CRC8(0x01) = 0x01
         │  └──── CMD = PING
         └─────── LEN = 0
```

---

#### 0x02 GET_INFO

Request HAT identification and firmware version.

**Request payload:** Empty (LEN=0)

**Expected response:** RSP_INFO (0x82)

**Response payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | hat_type | u8 | HAT type code (matches detection table) |
| 1 | fw_major | u8 | Firmware version major |
| 2 | fw_minor | u8 | Firmware version minor |

**Timeout:** 200 ms

---

#### 0x03 SET_PIN_CONFIG

Configure EXP_EXT pin function assignments. Supports two modes:

**Mode A — Single pin** (LEN=2):

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | pin | u8 | Pin index: 0=EXP_EXT_1, 1=EXP_EXT_2, 2=EXP_EXT_3, 3=EXP_EXT_4 |
| 1 | function | u8 | Pin function code (see §6) |

**Mode B — All pins** (LEN=4):

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | ext1_func | u8 | EXP_EXT_1 function |
| 1 | ext2_func | u8 | EXP_EXT_2 function |
| 2 | ext3_func | u8 | EXP_EXT_3 function |
| 3 | ext4_func | u8 | EXP_EXT_4 function |

The HAT distinguishes Mode A from Mode B by the LEN field (2 vs 4).

**Expected response:** RSP_OK (0x80), payload empty

The HAT must apply the configuration **before** sending the OK response. This guarantees that when the master receives OK, the hardware routing is active.

**Timeout:** 300 ms

**Example (set EXP_EXT_1 to SWDIO):**
```
TX: AA 02 03 00 01 [CRC]
         │  │  │  │  └─ function = SWDIO (1)
         │  │  │  └──── pin = 0 (EXP_EXT_1)
         │  │  └─────── CMD = SET_PIN_CONFIG
         │  └────────── LEN = 2
         └───────────── SYNC
```

**Example (set all pins: SWD debug mode):**
```
TX: AA 04 03 01 02 03 04 [CRC]
         │  │  │  │  │  │  └─ EXT4 = TRACE2
         │  │  │  │  │  └──── EXT3 = TRACE1
         │  │  │  │  └─────── EXT2 = SWCLK
         │  │  │  └────────── EXT1 = SWDIO
         │  │  └───────────── CMD = SET_PIN_CONFIG
         │  └──────────────── LEN = 4
         └─────────────────── SYNC
```

---

#### 0x04 GET_PIN_CONFIG

Read the current pin configuration from the HAT.

**Request payload:** Empty (LEN=0)

**Expected response:** RSP_OK (0x80)

**Response payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | ext1_func | u8 | Current EXP_EXT_1 function |
| 1 | ext2_func | u8 | Current EXP_EXT_2 function |
| 2 | ext3_func | u8 | Current EXP_EXT_3 function |
| 3 | ext4_func | u8 | Current EXP_EXT_4 function |

**Timeout:** 200 ms

---

#### 0x05 RESET

Reset the HAT to its default state. All EXP_EXT pins are set to DISCONNECTED (0x00). The HAT should complete the reset before responding.

**Request payload:** Empty (LEN=0)

**Expected response:** RSP_OK (0x80), payload empty

**Timeout:** 500 ms

---

### 5.2 Responses (Slave → Master)

Response IDs use the range **0x80–0xFF**.

---

#### 0x80 RSP_OK

Command executed successfully. Payload depends on the command:

| In response to | Payload |
|----------------|---------|
| PING | Empty |
| SET_PIN_CONFIG | Empty |
| GET_PIN_CONFIG | 4 bytes: current pin functions |
| RESET | Empty |

---

#### 0x81 RSP_ERROR

Command failed. Contains an error code.

**Payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | error_code | u8 | Error code (see §5.3) |

---

#### 0x82 RSP_INFO

Response to GET_INFO command.

**Payload:**

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | hat_type | u8 | HAT type code |
| 1 | fw_major | u8 | Firmware major version |
| 2 | fw_minor | u8 | Firmware minor version |

---

### 5.3 Error Codes

| Code | Name | Description |
|------|------|-------------|
| 0x01 | INVALID_CMD | Unknown command ID |
| 0x02 | INVALID_PIN | Pin index out of range (must be 0–3) |
| 0x03 | INVALID_FUNC | Function code out of range |
| 0x04 | BUSY | HAT is busy (e.g., mid-reconfiguration) |
| 0x05 | CRC_ERROR | CRC check failed on received frame |
| 0x06 | FRAME_ERROR | Malformed frame (bad LEN, missing bytes) |

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

### 6.1 Recommended Presets

While any combination is valid, these presets cover common use cases:

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
1. Configure GPIO47 as ADC (ADC1_CH6, 12-bit, 0–3.3V)
2. Read 8 ADC samples, average → V_detect
3. Map V_detect to HAT type
4. If HAT type = NONE → done (no HAT)
5. Initialize UART0: 921600 8N1 on GPIO43/GPIO44
6. Configure GPIO15 as open-drain input with pull-up
7. Flush UART RX buffer
8. Send PING (0x01), wait 200ms for RSP_OK
9. If no response → retry once
10. If still no response → mark as "detected but not responding"
11. Send GET_INFO (0x02), parse type + firmware version
12. Send GET_PIN_CONFIG (0x04), cache current pin state
13. Report HAT status to host application
```

### 7.2 HAT Boot (Slave Side)

```
1. Initialize UART: 921600 8N1
2. Set R_ID pull-down resistor (hardware)
3. Configure GPIO15 as open-drain output, release HIGH
4. Set all EXP_EXT pins to DISCONNECTED (safe default)
5. Enter idle state: wait for UART commands
6. On each received byte: accumulate into frame buffer
7. On complete frame: validate CRC, execute command, send response
```

---

## 8. Interrupt Protocol (GPIO15)

GPIO15 is a shared open-drain line with external pull-up. Either side can assert it (pull LOW) to signal the other.

### 8.1 HAT → BugBuster (Attention Request)

The HAT asserts GPIO15 when it has a status change to report (e.g., target detected, power fault, configuration change initiated by a physical switch on the HAT).

**Sequence:**
1. HAT pulls GPIO15 LOW
2. BugBuster ISR detects falling edge
3. BugBuster sends GET_INFO or GET_PIN_CONFIG
4. HAT responds with current state
5. HAT releases GPIO15 (high-Z)

### 8.2 BugBuster → HAT (Wake)

BugBuster may pulse GPIO15 LOW briefly (1 ms) to wake a sleeping HAT before sending a UART command.

**Sequence:**
1. BugBuster pulls GPIO15 LOW for 1 ms
2. BugBuster releases GPIO15
3. BugBuster waits 5 ms for HAT to wake
4. BugBuster sends UART command

---

## 9. Timing Requirements

| Parameter | Value | Notes |
|-----------|-------|-------|
| Inter-frame gap (master) | ≥ 1 ms | Minimum time between command frames |
| Response latency (slave) | ≤ 50 ms | Time from last command byte to first response byte |
| Frame timeout | Per command | See §4.2 |
| IRQ assert duration (HAT) | ≤ 100 ms | HAT must release if not polled |
| IRQ wake pulse (BugBuster) | 1 ms | Minimum duration for HAT wake |
| Post-reset settle time | 100 ms | After RESET command, before next command |

---

## 10. Implementation Notes

### 10.1 HAT Firmware Guidelines

- The HAT must not transmit any UART data unless responding to a command
- All pin reconfigurations must be complete **before** sending RSP_OK
- If reconfiguration takes significant time (>50ms), send RSP_OK only after hardware is ready
- On CRC error, respond with RSP_ERROR + CRC_ERROR code
- On unknown command, respond with RSP_ERROR + INVALID_CMD code
- Implement a 500ms watchdog on the UART RX state machine to recover from partial frames

### 10.2 BugBuster Firmware Guidelines

- Always flush UART RX before sending a command (discard stale data)
- Use the SYNC byte (0xAA) to resynchronize after errors
- On timeout, assume HAT is busy or disconnected
- Periodically re-detect (ADC read) to handle hot-plug/removal
- Cache last-known pin config locally; only query HAT on connect or IRQ

### 10.3 Future Extensibility

- Command IDs 0x06–0x7F are reserved for future commands
- Response IDs 0x83–0xFF are reserved for future response types
- Pin function codes 0x09–0xFF are reserved for future HAT types
- HAT type codes 0x02–0xFE are reserved for future hardware variants
- The LEN field allows payloads up to 32 bytes; if larger payloads are needed,
  define a multi-frame transfer protocol using a new command

### 5.4 HVPAK Backend Commands (0x14–0x1F)

These commands are handled by the RP2040 HVPAK driver (`bb_hvpak.c`) and carry
fine-grained access to the Renesas GreenPAK (SLG47104 / SLG47115-E) mailbox.
The RP2040 is the capability authority: it validates every request against the
detected part profile before forwarding to the I2C mailbox.

| Cmd | Name | Payload (request) | Response |
|-----|------|-------------------|----------|
| 0x14 | HVPAK_GET_INFO | — | `part_id(u8)`, `identity(u8)`, `ready(u8)`, `last_error(u8)`, `factory_virgin(u8)` |
| 0x15 | HVPAK_SET_VOLTAGE | `voltage_mv(u16)` | `ok(u8)`, `actual_mv(u16)` |
| 0x16 | HVPAK_GET_VOLTAGE | — | `voltage_mv(u16)`, `preset_index(u8)` |
| 0x17 | HVPAK_GET_CAPS | — | `flags(u32)` — bitmask of `CAP_LUT2/3/4`, `CAP_BRIDGE`, `CAP_PWM0/1`, `CAP_ANALOG`, `CAP_ACMP1`, `CAP_REG_RW` |
| 0x18 | HVPAK_GET_LUT | `lut_index(u8)` | `truth_table(u16)` — 4-input LUT entry for specified index |
| 0x19 | HVPAK_SET_LUT | `lut_index(u8)`, `truth_table(u16)` | `ok(u8)` |
| 0x1A | HVPAK_GET_BRIDGE | — | bridge config struct: `output_mode[2](u8)`, `ocp_retry[2](u8)`, `predriver(u8)`, `full_bridge(u8)`, `control_sel(u8)`, `uvlo(u8)` |
| 0x1B | HVPAK_SET_BRIDGE | bridge config struct (same layout as GET) | `ok(u8)` |
| 0x1C | HVPAK_GET_ANALOG | — | analog config: `vref_mode(u8)`, `vref_power(u8)`, `acmp0_gain(u8)`, `acmp0_vref(u8)`, `cs_vref(u8)`, `cs_gain(u8)`, `cs_enable(u8)` |
| 0x1D | HVPAK_SET_ANALOG | analog config struct (same layout as GET) | `ok(u8)` |
| 0x1E | HVPAK_GET_PWM | `pwm_index(u8)` | `period_source(u8)`, `duty_source(u8)`, `deadband(u8)` |
| 0x1F | HVPAK_SET_PWM | `pwm_index(u8)`, PWM config struct | `ok(u8)` |

**Notes:**
- All responses include a trailing `error(u8)` byte; `0x00` = success.
- Unsafe registers (identity `0x48`, command `0x4C`, service `0xF5/0xF6`) are
  blocked and return `HVPAK_UNSAFE_REGISTER` error.
- `factory_virgin` in `HVPAK_GET_INFO` is set when the mailbox identity is absent
  and service registers `F5`, `FD`, `FE` all read `0x00` — indicates an
  unprovisioned GreenPAK that needs programming.

---

### 10.4 HVPAK Mailbox Contract

The unreleased HVPAK voltage-control path now assumes a programmed GreenPAK
mailbox image behind the RP2040 I2C bus:

- I2C address `0x48`
- register `0x48`: read-only OTP identity byte
- register `0x4C`: writable command byte
- identity values:
  - `0x04` = `SLG47104`
  - `0x15` = `SLG47115-E`
- preset voltages:
  - `1200`
  - `1800`
  - `2500`
  - `3300`
  - `5000` mV

`SET_IO_VOLTAGE` and `GET_IO_VOLTAGE` responses may now include HVPAK metadata
(`hvpak_part`, `hvpak_ready`, `hvpak_last_error`) so the host can tell whether
the programmed image matches the expected mailbox contract.

### 10.5 Advanced HVPAK Backend Commands

The HAT UART protocol now also carries typed HVPAK backend commands for:
- part info / capabilities
- LUT truth-table access
- bridge configuration
- analog/comparator/Vref configuration
- PWM configuration
- guarded raw register read/write

These commands live in the `0x14..0x1F` range on the UART-side HAT protocol and
are mirrored into BBP on the ESP32 side. The RP2040 remains the capability
authority and validates every request against the detected part profile.

Additional HAT-side HVPAK errors now distinguish:
- no device
- timeout
- unknown identity
- unsupported capability
- invalid index
- invalid argument
- unsafe register access

The HVPAK info surface may also flag a device as `factory_virgin` when:
- the custom mailbox identity is absent, and
- service registers `F5`, `FD`, and `FE` are readable and still equal `0x00`

This is intended as a provisioning check for blank/unprovisioned PAKs, not as
an absolute OTP-forensics claim.

---

## Appendix A: Complete Frame Examples

### A.1 PING Transaction

```
Master TX: AA 00 01 01
                │  │  └─ CRC8([01]) = 0x01
                │  └──── CMD = 0x01 (PING)
                └─────── LEN = 0

Slave  RX: (parse, validate CRC, process)

Slave  TX: AA 00 80 80
                │  │  └─ CRC8([80]) = 0x80
                │  └──── CMD = 0x80 (RSP_OK)
                └─────── LEN = 0
```

### A.2 GET_INFO Transaction

```
Master TX: AA 00 02 02
                │  │  └─ CRC8([02]) = 0x02
                │  └──── CMD = 0x02 (GET_INFO)
                └─────── LEN = 0

Slave  TX: AA 03 82 01 01 00 [CRC]
                │  │  │  │  │  └─ fw_minor = 0
                │  │  │  │  └──── fw_major = 1
                │  │  │  └─────── hat_type = 1 (SWD/GPIO)
                │  │  └────────── CMD = 0x82 (RSP_INFO)
                │  └───────────── LEN = 3
                └──────────────── SYNC

CRC = CRC8([82, 01, 01, 00]) = ...
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

CRC = CRC8([03, 01, 02, 03, 04]) = ...

Slave TX: AA 00 80 80
          (RSP_OK, config applied)
```

### A.4 Error Response

```
Slave TX: AA 01 81 03 [CRC]
               │  │  │
               │  │  └─ error_code = 0x03 (INVALID_FUNC)
               │  └──── CMD = 0x81 (RSP_ERROR)
               └─────── LEN = 1
```
