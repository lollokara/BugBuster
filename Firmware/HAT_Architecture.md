# BugBuster HAT — Extended Architecture & Feature Plan

**Date:** 2026-04-02
**HAT MCU:** RP2040 (Dual Cortex-M0+, 264KB SRAM, PIO, USB)
**Target:** BugBuster PCB mode only

---

## 1. Hardware Overview

```
┌─────────────────────────────────────────────────────────────┐
│                     BugBuster Main Board                     │
│                                                              │
│  ESP32-S3 ──UART0──> HAT_TX/RX ──> RP2040                  │
│            ──GPIO47─> HAT_DETECT   (ADC voltage divider)     │
│            <─GPIO15─> HAT_IRQ      (open-drain, shared)      │
│                                                              │
│  VADJ1 (3-15V) ─────────────────> HAT VADJ1_PASS            │
│  VADJ2 (3-15V) ─────────────────> HAT VADJ2_PASS            │
│                                                              │
│  EXP_EXT_1..4 (via MUX S4) ────> HAT I/O lines              │
└─────────────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────┐
│                        HAT Board                             │
│                                                              │
│  RP2040                                                      │
│  ├── UART (slave) ←── BugBuster command bus                  │
│  ├── PIO 0 ──────────── SWD engine (SWDIO/SWCLK)            │
│  ├── PIO 1 ──────────── Logic analyzer / signal capture      │
│  ├── GPIO ───────────── Connector enables, status LEDs       │
│  └── USB ────────────── (optional: direct host connection)    │
│                                                              │
│  Renesas HVPAK                                               │
│  ├── VCC_IO programmable ── sets I/O voltage (1.2V–5.5V)    │
│  └── EXP_EXT_1..4 ──────── level-translated to target V     │
│                                                              │
│  Connector A (Target 1)                                      │
│  ├── VADJ1_PASS power ── switched via EN_A                   │
│  ├── EXP_EXT_1, EXP_EXT_2 ── SWD or GPIO (level-shifted)   │
│  └── GND                                                     │
│                                                              │
│  Connector B (Target 2)                                      │
│  ├── VADJ2_PASS power ── switched via EN_B                   │
│  ├── EXP_EXT_3, EXP_EXT_4 ── SWD or GPIO (level-shifted)   │
│  └── GND                                                     │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. Feature Modules

### Module 1: Target Power Management (Priority: HIGH — implement first)

Controls power delivery to each target connector and configures I/O voltage levels.

**Capabilities:**
- Per-connector power enable/disable (EN_A, EN_B) via RP2040 GPIO
- Voltage pass-through from VADJ1 → Connector A, VADJ2 → Connector B
- BugBuster controls VADJ1/VADJ2 voltage via DS4424 IDAC (already implemented)
- HVPAK I/O voltage programming — sets the level translation voltage to match target
- Current monitoring (if shunt resistor present on HAT)
- Power sequencing: enable I/O voltage before target power, or vice versa
- Overcurrent detection via RP2040 ADC or comparator

**New HAT Protocol Commands:**

| CMD | Name | Payload | Description |
|-----|------|---------|-------------|
| 0x10 | SET_POWER | connector(u8), enable(u8) | Enable/disable connector power |
| 0x11 | GET_POWER_STATUS | — | Read power state, current, voltage |
| 0x12 | SET_IO_VOLTAGE | voltage_mv(u16) | Set HVPAK I/O level (mV) |
| 0x13 | GET_IO_VOLTAGE | — | Read current I/O voltage setting |
| 0x14 | SET_POWER_SEQUENCE | seq_config(u8) | Configure power-on sequence |

**BugBuster Side (ESP32):**
- New BBP commands: `HAT_SET_POWER` (0xCA), `HAT_GET_POWER_STATUS` (0xCB), `HAT_SET_IO_VOLTAGE` (0xCC)
- HTTP: `POST /api/hat/power`, `GET /api/hat/power`, `POST /api/hat/io_voltage`
- Desktop UI: Power controls in HAT tab with voltage readback
- Python: `bb.hat_set_power(connector, enable)`, `bb.hat_set_io_voltage(voltage_mv)`

---

### Module 2: SWD Debug Probe (Priority: HIGH — implement second)

Uses RP2040 PIO for high-performance SWD protocol implementation.

**Capabilities:**
- SWD-DP (Debug Port) protocol: JTAG-to-SWD switch sequence, line reset
- Read/write DP and AP registers
- Target detection (DPIDR read)
- Memory read/write via AHB-AP (MEM-AP)
- Flash programming (target-specific algorithms uploaded from host)
- Halt, resume, single-step
- Breakpoint set/clear (hardware breakpoints via FPB)
- Register read/write (R0-R15, xPSR, MSP, PSP)
- Core status query (halted, running, lockup, sleep)
- SWO/TRACE capture via separate PIO or UART

**RP2040 PIO Usage:**
- PIO 0, SM 0: SWD clock generation + data I/O (bidirectional SWDIO)
- PIO 0, SM 1: SWO capture (Manchester or UART mode)
- SWD clock: configurable 100 kHz – 10 MHz (PIO clock divider)

**New HAT Protocol Commands:**

| CMD | Name | Payload | Description |
|-----|------|---------|-------------|
| 0x20 | SWD_INIT | clock_khz(u16) | Initialize SWD, set clock speed |
| 0x21 | SWD_DETECT | — | Line reset + read DPIDR |
| 0x22 | SWD_READ_REG | ap_dp(u8), addr(u8) | Read DP/AP register |
| 0x23 | SWD_WRITE_REG | ap_dp(u8), addr(u8), value(u32) | Write DP/AP register |
| 0x24 | SWD_READ_MEM | addr(u32), len(u16) | Read target memory block |
| 0x25 | SWD_WRITE_MEM | addr(u32), data(N) | Write target memory block |
| 0x26 | SWD_HALT | — | Halt target core |
| 0x27 | SWD_RESUME | — | Resume target core |
| 0x28 | SWD_STEP | — | Single-step |
| 0x29 | SWD_GET_STATE | — | Read core state (halted, DHCSR) |
| 0x2A | SWD_READ_CORE_REG | reg(u8) | Read core register (R0-R15, etc.) |
| 0x2B | SWD_WRITE_CORE_REG | reg(u8), value(u32) | Write core register |
| 0x2C | SWD_FLASH_ALGO | algo_data(N) | Upload flash algorithm |
| 0x2D | SWD_FLASH_WRITE | addr(u32), data(N) | Program flash page |
| 0x2E | SWD_FLASH_ERASE | addr(u32), len(u32) | Erase flash sector(s) |

**BugBuster Side:**
- New BBP commands block 0xD5–0xDF for SWD operations
- HTTP: `POST /api/hat/swd/*` endpoints
- Desktop UI: New "Debug" tab with target status, register view, memory viewer
- Python: `bb.swd_detect()`, `bb.swd_read_mem(addr, len)`, `bb.swd_halt()`, etc.

**Note on large transfers:** Memory read/write may exceed the 32-byte HAT frame limit.
Use a chunked transfer protocol:
- Master sends READ_MEM with addr + total length
- HAT responds with multiple RSP_OK frames, each containing up to 28 bytes of data
- Sequence number in each response chunk for reassembly
- Or: define a streaming mode where HAT sends continuous data after initial response

---

### Module 3: Logic Analyzer / Signal Capture (Priority: MEDIUM — implement third)

Uses RP2040 PIO for high-speed digital signal capture on EXP_EXT lines.

**Capabilities:**
- Capture 1–4 digital channels simultaneously
- Sample rates: up to 100 MHz (PIO clock) for single channel
- Trigger: edge (rising/falling/both), pattern, immediate
- Buffer: RP2040 SRAM ring buffer (up to ~200KB usable)
- Stream to BugBuster via UART (limited by 115200 baud) or via USB (if connected)
- Pre-trigger and post-trigger capture
- Protocol decode assist (SPI, I2C, UART framing) on RP2040 side

**RP2040 PIO Usage:**
- PIO 1, SM 0-3: Input capture (one SM per channel for independent sampling)
- DMA: Continuous DMA from PIO FIFO to SRAM buffer
- Trigger: PIO IRQ or GPIO interrupt triggers capture stop

**New HAT Protocol Commands:**

| CMD | Name | Payload | Description |
|-----|------|---------|-------------|
| 0x30 | LA_CONFIG | channels(u8), rate_khz(u32), depth(u16) | Configure capture |
| 0x31 | LA_SET_TRIGGER | type(u8), channel(u8), edge(u8) | Set trigger condition |
| 0x32 | LA_ARM | — | Arm the trigger, start waiting |
| 0x33 | LA_FORCE_TRIGGER | — | Force immediate capture |
| 0x34 | LA_GET_STATUS | — | Capture state (idle/armed/triggered/done) |
| 0x35 | LA_READ_DATA | offset(u32), len(u16) | Read captured data chunk |
| 0x36 | LA_STOP | — | Abort capture |

**Bandwidth consideration:** At 115200 baud UART, max throughput is ~11 KB/s. For a 4-channel capture at 1 MHz, data rate is 500 KB/s — far exceeding UART bandwidth. Options:
1. Capture to RP2040 SRAM, then read back in chunks (offline analysis)
2. Use RP2040 USB for high-speed data transfer (12 Mbps = ~1.2 MB/s)
3. Compress on RP2040 (run-length encoding for digital signals)
4. Reduce sample rate for real-time streaming via UART

---

## 3. Protocol Extensions Summary

### 3.1 Command ID Space

| Range | Module | Description |
|-------|--------|-------------|
| 0x01–0x0F | Core | PING, GET_INFO, SET_PIN_CONFIG, GET_PIN_CONFIG, RESET |
| 0x10–0x1F | Power Management | Connector enable, voltage, current, sequencing |
| 0x20–0x2F | SWD Debug | SWD protocol, memory access, flash programming |
| 0x30–0x3F | Logic Analyzer | Capture config, trigger, data readout |
| 0x40–0x7F | *(Reserved)* | Future modules |

### 3.2 Response ID Space

| Range | Description |
|-------|-------------|
| 0x80 | RSP_OK (generic success) |
| 0x81 | RSP_ERROR |
| 0x82 | RSP_INFO |
| 0x83 | RSP_POWER_STATUS |
| 0x84 | RSP_SWD_DATA (memory/register read result) |
| 0x85 | RSP_LA_STATUS |
| 0x86 | RSP_LA_DATA |
| 0x87–0xFF | *(Reserved)* |

### 3.3 BBP Command Mapping (BugBuster ↔ Host)

| BBP ID | Name | HAT CMD | Module |
|--------|------|---------|--------|
| 0xC5 | HAT_GET_STATUS | 0x02 | Core |
| 0xC6 | HAT_SET_PIN | 0x03 | Core |
| 0xC7 | HAT_SET_ALL_PINS | 0x03 | Core |
| 0xC8 | HAT_RESET | 0x05 | Core |
| 0xC9 | HAT_DETECT | — | Core (ADC only) |
| 0xCA | HAT_SET_POWER | 0x10 | Power |
| 0xCB | HAT_GET_POWER_STATUS | 0x11 | Power |
| 0xCC | HAT_SET_IO_VOLTAGE | 0x12 | Power |
| 0xD5 | HAT_SWD_INIT | 0x20 | SWD |
| 0xD6 | HAT_SWD_DETECT | 0x21 | SWD |
| 0xD7 | HAT_SWD_READ_REG | 0x22 | SWD |
| 0xD8 | HAT_SWD_WRITE_REG | 0x23 | SWD |
| 0xD9 | HAT_SWD_READ_MEM | 0x24 | SWD |
| 0xDA | HAT_SWD_WRITE_MEM | 0x25 | SWD |
| 0xDB | HAT_SWD_HALT | 0x26 | SWD |
| 0xDC | HAT_SWD_RESUME | 0x27 | SWD |
| 0xDD | HAT_SWD_GET_STATE | 0x29 | SWD |

---

## 4. Implementation Roadmap

### Phase 1: Target Power Management (Simplest — foundation for all other features)

**RP2040 firmware:**
1. UART slave command handler (reuse HAT protocol framing from HAT_Protocol.md)
2. GPIO control for EN_A, EN_B (connector power enables)
3. ADC read for current monitoring (if shunt present)
4. I2C/SPI to HVPAK for I/O voltage programming
5. Power sequencing state machine

**BugBuster firmware:**
1. Extend `hat.cpp` with power commands (0x10–0x14 forwarding)
2. Add BBP commands 0xCA–0xCC
3. HTTP endpoints for power control
4. Desktop UI: power toggles + voltage readback in HAT tab
5. Python API: `hat_set_power()`, `hat_set_io_voltage()`

**Deliverable:** User can power targets at configurable voltage, enable/disable each connector, set I/O level translation voltage from the desktop app.

### Phase 2: SWD Debug Probe (Most complex — highest value)

**RP2040 firmware:**
1. PIO program for SWD bit-banging (SWDIO bidirectional + SWCLK)
2. SWD-DP protocol layer (line reset, JTAG→SWD, DPIDR read)
3. MEM-AP access layer (read/write 32-bit words via AHB-AP)
4. Target halt/resume/step via DHCSR
5. Core register read/write via DCRSR/DCRDR
6. Flash algorithm framework (upload + execute in target RAM)
7. Common flash drivers: STM32F0/F1/F4, nRF52, RP2040 (self-flash)

**BugBuster firmware:**
1. Extend HAT protocol with SWD command forwarding
2. Handle large memory transfers (chunked read/write)
3. BBP commands 0xD5–0xDD
4. HTTP endpoints for debug operations

**Desktop app:**
1. New "Debug" tab or section in HAT tab
2. Target status indicator (connected, halted, running)
3. Register viewer (R0-R15, xPSR, special registers)
4. Simple memory viewer (hex dump with address navigation)
5. Flash programming UI (file picker + progress bar)

**Python API:**
1. `bb.swd_detect()` → returns DPIDR, target info
2. `bb.swd_read_mem(addr, length)` → bytes
3. `bb.swd_write_mem(addr, data)` → bool
4. `bb.swd_halt()`, `bb.swd_resume()`, `bb.swd_step()`
5. `bb.swd_flash(file_path, addr)` → flashes binary/hex file

**Deliverable:** Full SWD debug probe controllable from desktop app and Python scripts. Flash programming for common ARM targets.

### Phase 3: Logic Analyzer (Builds on PIO expertise from Phase 2)

**RP2040 firmware:**
1. PIO capture programs (1–4 channels, configurable rate)
2. DMA ring buffer management
3. Edge/pattern trigger engine
4. Data compression (RLE for digital signals)
5. Chunked readout via UART command protocol

**BugBuster firmware:**
1. LA command forwarding through HAT protocol
2. Data buffering and streaming to host

**Desktop app:**
1. Waveform viewer (timeline with digital traces)
2. Trigger configuration UI
3. Zoom/pan/measure tools
4. Protocol decode overlays (optional, can be done host-side)

**Python API:**
1. `bb.la_capture(channels, rate_khz, duration_ms)`
2. `bb.la_get_data()` → numpy array or similar
3. Trigger configuration helpers

**Deliverable:** 1-4 channel digital logic analyzer integrated into BugBuster, configurable from the desktop app. Capture data viewable as waveforms.

---

## 5. RP2040 Firmware Architecture

```
┌─────────────────────────────────────────┐
│            RP2040 Main Loop              │
│                                          │
│  ┌──────────────┐  ┌─────────────────┐  │
│  │ Core 0       │  │ Core 1          │  │
│  │              │  │                 │  │
│  │ UART Handler │  │ PIO Engine      │  │
│  │ ├─ Parse Cmd │  │ ├─ SWD (PIO 0) │  │
│  │ ├─ Dispatch  │  │ ├─ LA  (PIO 1) │  │
│  │ ├─ Power Mgr │  │ └─ DMA buffers │  │
│  │ └─ Send Rsp  │  │                 │  │
│  └──────────────┘  └─────────────────┘  │
│                                          │
│  Shared:                                 │
│  ├─ Command mailbox (Core 0 → Core 1)   │
│  ├─ Response mailbox (Core 1 → Core 0)  │
│  ├─ SRAM capture buffer (~200KB)         │
│  └─ Power state + pin config             │
└─────────────────────────────────────────┘
```

**Core 0:** UART communication, power management, command routing
**Core 1:** PIO-intensive operations (SWD protocol, logic capture)
**Inter-core:** Mailbox with spin-lock for command/response passing

---

## 6. HVPAK Integration

The Renesas HVPAK provides programmable voltage level translation for the EXP_EXT lines.

**Configuration:**
- Input voltage: 3.3V (from BugBuster logic level)
- Output voltage: programmable 1.2V – 5.5V (matching target)
- Set via I2C or SPI from RP2040
- Must be configured BEFORE routing EXP_EXT signals to target

**Power-on sequence:**
1. Set HVPAK output voltage to match target (via SET_IO_VOLTAGE command)
2. Wait for HVPAK to stabilize (~1ms)
3. Enable connector power (EN_A or EN_B)
4. Wait for target power good
5. Configure EXP_EXT pin functions (SWD, GPIO, etc.)
6. Begin debug/capture operations

**Power-off sequence (reverse):**
1. Disconnect EXP_EXT pins (set to DISCONNECTED)
2. Disable connector power
3. Optionally power down HVPAK

---

## 7. Large Transfer Protocol Extension

For SWD memory access and logic analyzer data that exceeds the 32-byte HAT frame payload:

### 7.1 Chunked Read

```
Master: CMD_SWD_READ_MEM  [addr:u32, total_len:u16]
Slave:  RSP_OK            [chunk_count:u8]
Slave:  RSP_SWD_DATA      [seq:u8, data:28B]   ← chunk 0
Slave:  RSP_SWD_DATA      [seq:u8, data:28B]   ← chunk 1
...
Slave:  RSP_SWD_DATA      [seq:u8, data:remaining]  ← last chunk
```

- Each chunk has a 1-byte sequence number (0–255, wrapping)
- Max 28 data bytes per chunk (32 - 1 seq - 1 LEN - 1 CMD - 1 CRC)
- Master waits for all chunks within a timeout (total_len / 28 * 50ms + 200ms)

### 7.2 Chunked Write

```
Master: CMD_SWD_WRITE_MEM [addr:u32, total_len:u16, chunk_count:u8]
Master: (data frame)      [seq:u8, data:28B]   ← chunk 0
Master: (data frame)      [seq:u8, data:28B]   ← chunk 1
...
Master: (data frame)      [seq:u8, data:remaining]  ← last chunk
Slave:  RSP_OK            [bytes_written:u16]
```

- Master sends all chunks, then slave responds with total bytes written
- If CRC error on any chunk, slave responds RSP_ERROR immediately

---

## 8. Desktop App UI Extensions

### HAT Tab Sections (Progressive):

**Phase 1 additions:**
```
┌─────────────────────────────────────────┐
│ HAT Status          [SWD/GPIO] [v1.0]  │
│ ● Detected  ● Connected  IO: 3.3V      │
├─────────────────────────────────────────┤
│ Target Power                             │
│  Connector A: [ON/OFF]  VADJ1: 5.0V     │
│  Connector B: [ON/OFF]  VADJ2: 3.3V     │
│  I/O Voltage: [▼ 3.3V]  (HVPAK)        │
├─────────────────────────────────────────┤
│ Pin Configuration                        │
│  EXT_1: [▼ SWDIO ]  EXT_2: [▼ SWCLK ] │
│  EXT_3: [▼ TRACE1]  EXT_4: [▼ TRACE2] │
│  [SWD Debug] [GPIO Mode] [Disconnect]   │
└─────────────────────────────────────────┘
```

**Phase 2 additions:**
```
┌─────────────────────────────────────────┐
│ SWD Debug                                │
│  Target: STM32F411  DPIDR: 0x0BB11477   │
│  State: ● Halted   PC: 0x08001234       │
│                                          │
│  [Halt] [Resume] [Step] [Reset]          │
│                                          │
│  Registers:                              │
│  R0:  0x00000000   R8:  0x20001000      │
│  R1:  0x08001234   R9:  0x00000000      │
│  ...                                     │
│                                          │
│  Flash: [Select File] [Program] ████ 45% │
└─────────────────────────────────────────┘
```

**Phase 3 additions:**
```
┌─────────────────────────────────────────┐
│ Logic Analyzer                           │
│  Channels: [4]  Rate: [1 MHz]           │
│  Trigger: [▼ Ch1 Rising Edge]           │
│  [Arm] [Force] [Stop]                   │
│                                          │
│  ──CH1──┐   ┌──────┐   ┌──             │
│          └───┘      └───┘               │
│  ──CH2────────┐         ┌──────         │
│               └─────────┘               │
│  ──CH3──┐         ┌─┐       ┌──        │
│          └─────────┘ └───────┘          │
└─────────────────────────────────────────┘
```
