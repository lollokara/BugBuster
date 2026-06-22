﻿# BugBuster HAT â€” Extended Architecture & Feature Plan

**Date:** 2026-04-02
**HAT MCU:** RP2040 (Dual Cortex-M0+, 264KB SRAM, PIO, USB)
**SWD Base:** Fork of [raspberrypi/debugprobe](https://github.com/raspberrypi/debugprobe) (CMSIS-DAP)
**Target:** BugBuster PCB mode only

---

## 1. Hardware Overview

```
â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
â”‚                     BugBuster Main Board                     â”‚
â”‚                                                              â”‚
â”‚  ESP32-S3 â”€â”€UART0â”€â”€> HAT_TX/RX â”€â”€> RP2040                  â”‚
â”‚            â”€â”€GPIO47â”€> HAT_DETECT   (ADC voltage divider)     â”‚
â”‚            <â”€GPIO15â”€> HAT_IRQ      (open-drain, shared)      â”‚
â”‚                                                              â”‚
â”‚  VADJ1 (3-15V) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€> HAT VADJ1_PASS            â”‚
â”‚  VADJ2 (3-15V) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€> HAT VADJ2_PASS            â”‚
â”‚                                                              â”‚
â”‚  EXP_EXT_1..4 (via MUX S4) â”€â”€â”€â”€> HAT I/O lines              â”‚
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
                         â”‚
                         â–¼
â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
â”‚                        HAT Board                             â”‚
â”‚                                                              â”‚
â”‚  RP2040 (debugprobe fork + BugBuster extensions)             â”‚
â”‚  â”œâ”€â”€ USB IF 0 â”€â”€â”€â”€â”€â”€â”€ Vendor bulk â€” LA data plane           â”‚
â”‚  â”‚                     (EP 0x06 OUT / 0x87 IN)               â”‚
â”‚  â”œâ”€â”€ USB IF 1 â”€â”€â”€â”€â”€â”€â”€ CMSIS-DAP v2 (HID/vendor)              â”‚
â”‚  â”œâ”€â”€ USB IF 2/3 â”€â”€â”€â”€â”€â”€ CDC UART bridge to target             â”‚
â”‚  â”œâ”€â”€ PIO 0 â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ SWD engine (from debugprobe)         â”‚
â”‚  â”œâ”€â”€ PIO 1 â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ Logic analyzer capture (4 SMs)       â”‚
â”‚  â”œâ”€â”€ UART0 (slave) â”€â”€â”€â”€ BugBuster management bus             â”‚
â”‚  â””â”€â”€ GPIO â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ Connector enables, LEDs       â”‚
â”‚                                                              â”‚
â”‚  â”‚                                                              â”‚
â”‚  Connector A (Target 1)                                      â”‚
â”‚  â”œâ”€â”€ VADJ1_PASS power â”€â”€ switched via EN_A                   â”‚
â”‚  â”œâ”€â”€ EXP_EXT_1 (SWDIO), EXP_EXT_2 (SWCLK) â”€â”€    â”‚
â”‚  â””â”€â”€ GND                                                     â”‚
â”‚                                                              â”‚
â”‚  Connector B (Target 2)                                      â”‚
â”‚  â”œâ”€â”€ VADJ2_PASS power â”€â”€ switched via EN_B                   â”‚
â”‚  â”œâ”€â”€ EXP_EXT_3, EXP_EXT_4 â”€â”€ GPIO/Trace          â”‚
â”‚  â””â”€â”€ GND                                                     â”‚
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
```

### 1.1 Host USB topology (HAT present)

The HAT enumerates **three** USB interfaces alongside the ESP32's CDC ports.
When the HAT is installed the host sees two independent USB devices â€” one
per MCU â€” and uses each for different classes of traffic:

```mermaid
flowchart LR
  HOST["Host application<br/>(desktop / MCP / OpenOCD)"]
  ESP32["ESP32-S3<br/>USB CDC #0"]
  RP["RP2040<br/>USB composite"]

  HOST -->|"BBP v4 â€” control plane<br/>(commands, status, events)"| ESP32
  HOST -->|"Vendor bulk IF0<br/>EP 0x06 / 0x87<br/>LA streaming & readout"| RP
  HOST -->|"CMSIS-DAP v2 IF1<br/>EP 0x04 / 0x85"| RP
  HOST -->|"CDC bridge IF2/3<br/>target UART"| RP

  ESP32 -->|"HAT UART 921600 8N1<br/>config / status"| RP
```

**Key property:** LA data and SWD debug traffic do NOT traverse the ESP32 or
the HAT UART. The ESP32 only configures and arms the LA (via
`HAT_LA_CONFIG` / `HAT_LA_ARM` / `HAT_LA_STREAM_START` over the HAT UART);
sample data flows directly from the RP2040's vendor-bulk endpoint to the
host. See [`../Docs/LogicAnalyzer.md`](../Docs/LogicAnalyzer.md) for the
full packet format and rearm protocol.

---

## 2. SWD Architecture â€” debugprobe Integration

### 2.1 Why Fork debugprobe (Not Reimplement)

The Raspberry Pi debugprobe project provides:
- **PIO-accelerated SWD** â€” hardware-timed SWCLK/SWDIO via PIO state machines (`probe.pio`)
- **CMSIS-DAP v1 + v2** â€” industry-standard debug protocol over USB
- **CDC UART bridge** â€” USB serial for target UART
- **SWO trace** â€” Serial Wire Output capture
- **FreeRTOS** â€” task-based architecture with proper USB handling
- **Pico SDK 2.0** â€” mature build system (CMake)
- **Wide tool support** â€” works with OpenOCD, pyOCD, probe-rs, VS Code, etc.

Reimplementing SWD from scratch would duplicate thousands of lines of tested PIO code,
CMSIS-DAP protocol handling, and USB descriptors. Instead, we fork and extend.

### 2.2 debugprobe Internals (Key Components)

| Component | File | Purpose |
|-----------|------|---------|
| SWD PIO engine | `probe.pio`, `probe.c` | PIO programs for SWD bit timing |
| SWD protocol | `sw_dp_pio.c` | SWD request/response, parity, ACK handling |
| CMSIS-DAP | `DAP.c`, `DAP_vendor.c` | USB command processing (standard + vendor) |
| USB stack | `usb_descriptors.c`, TinyUSB | CMSIS-DAP HID/Bulk + CDC endpoints |
| UART bridge | `cdc_uart.c` | USB CDC â†” target UART |
| Main loop | `main.c` | FreeRTOS tasks, LED control |

**PIO usage:**
- PIO 0: SWD engine (SWCLK output, SWDIO bidirectional)
- PIO 1: Available for our logic analyzer

**USB endpoints (as shipped on `bb-hat-2.0`):**
- EP `0x06/0x87`: **BB_LA vendor bulk** â€” Logic Analyzer streaming / readout (interface 0)
- EP `0x04/0x85`: CMSIS-DAP commands (interface 1, vendor class per DAP v2 spec)
- EP `0x02/0x83`: CDC UART data (target bridge)
- EP `0x81`: CDC notification

The LA interface is deliberately placed on interface **0** so TinyUSB's
built-in vendor driver claims it; the custom DAP driver
(`lib/debugprobe/src/tusb_edpt_handler.c`) claims interface 1 only. Prior to
2026-04 the indices were swapped and TinyUSB/DAP fought for ownership.

### 2.3 Integration Strategy

**We do NOT modify the SWD/CMSIS-DAP core.** Our additions run alongside:

```
â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
â”‚              RP2040 Firmware                  â”‚
â”‚                                              â”‚
â”‚  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”  â”‚
â”‚  â”‚  debugprobe core (unmodified)          â”‚  â”‚
â”‚  â”‚  â”œâ”€â”€ DAP task (CMSIS-DAP over USB)     â”‚  â”‚
â”‚  â”‚  â”œâ”€â”€ SWD PIO engine (PIO 0)           â”‚  â”‚
â”‚  â”‚  â”œâ”€â”€ CDC UART bridge                   â”‚  â”‚
â”‚  â”‚  â””â”€â”€ SWO capture                       â”‚  â”‚
â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜  â”‚
â”‚                                              â”‚
â”‚  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”  â”‚
â”‚  â”‚  BugBuster extensions (new code)       â”‚  â”‚
â”‚  â”‚  â”œâ”€â”€ UART0 command handler task        â”‚  â”‚
â”‚  â”‚  â”‚   â”œâ”€â”€ Power management (EN_A/B)     â”‚  â”‚
â”‚  â”‚  â”‚   â”œâ”€â”€ HVPAK I/O voltage control     â”‚  â”‚
â”‚  â”‚  â”‚   â”œâ”€â”€ Pin configuration routing     â”‚  â”‚
â”‚  â”‚  â”‚   â”œâ”€â”€ Logic analyzer control        â”‚  â”‚
â”‚  â”‚  â”‚   â””â”€â”€ Status/detect reporting       â”‚  â”‚
â”‚  â”‚  â”œâ”€â”€ Logic analyzer PIO (PIO 1)        â”‚  â”‚
â”‚  â”‚  â””â”€â”€ IRQ signaling (GPIO8â†’GPIO15)      â”‚  â”‚
â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜  â”‚
â”‚                                              â”‚
â”‚  Shared: GPIO, power state, pin config       â”‚
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
```

**Key principle:** The host computer talks to the SWD probe directly via USB CMSIS-DAP.
BugBuster's role is **management** â€” power control, voltage setup, pin routing, and
coordination. BugBuster does NOT proxy SWD commands through UART.

### 2.4 Host-Side Debug Workflow

```
Desktop App / Python Script
    â”‚
    â”œâ”€â”€[USB CMSIS-DAP]â”€â”€> RP2040 debugprobe â”€â”€> Target SWD
    â”‚                     (direct, no proxy)
    â”‚
    â””â”€â”€[BugBuster BBP/HTTP]â”€â”€> ESP32-S3 â”€â”€[UART]â”€â”€> RP2040 BugBuster task
                               (management only: power, voltage, pin config)
```

1. User selects target voltage in BugBuster desktop app â†’ ESP32 sets VADJ via DS4424
2. User clicks "Enable Target Power" â†’ ESP32 sends HAT_SET_POWER via UART â†’ RP2040 toggles EN_A
3. User clicks "Configure SWD" â†’ ESP32 sends HAT_SET_ALL_PINS â†’ RP2040 routes SWDIO/SWCLK
4. User opens OpenOCD / pyOCD / VS Code â†’ connects to RP2040 USB CMSIS-DAP directly
5. Debug session runs over USB â€” BugBuster is not in the debug data path
6. BugBuster monitors target power, can emergency-disconnect if fault detected

### 2.5 What BugBuster Manages (via UART to RP2040)

| Function | BugBuster's Role |
|----------|-----------------|
| SWD protocol | **None** â€” handled by debugprobe over USB |
| Target power | Enable/disable connectors, set VADJ voltage |
| I/O voltage | Set IO voltage via RP2040 |
| Pin routing | Configure which EXP_EXT goes where |
| Target detect | RP2040 can report if SWD target responds |
| Emergency stop | BugBuster can cut power on fault |
| Status display | Show debug probe state in BugBuster UI |

### 2.6 debugprobe Fork Modifications

Minimal changes to the debugprobe codebase:

1. **`main.c`** â€” Add a new FreeRTOS task: `bugbuster_cmd_task` on Core 0
   - Initializes UART0 for BugBuster command bus
   - Runs the HAT protocol frame parser
   - Dispatches commands to power/pin/LA handlers
   - Priority: lower than DAP task (debug is real-time critical)

2. **`CMakeLists.txt`** â€” Add our source files:
   - `bugbuster_hat.c` â€” command handler, power control
   - `bugbuster_hvpak.c` â€” HVPAK I2C/SPI driver
   - `bugbuster_la.c` â€” logic analyzer PIO control
   - `bugbuster_protocol.c` â€” frame parser/builder (CRC-8, sync)

3. **`board_bugbuster_hat_config.h`** â€” New board config:
   - Pin assignments (UART0 TX/RX, EN_A, EN_B, IRQ)
   - SWD pins (matching HAT PCB routing)
   - LED pins

4. **`DAP_config.h`** â€” Adjust SWD pin assignments to match HAT PCB layout

5. **No changes to:** `probe.c`, `probe.pio`, `sw_dp_pio.c`, `DAP.c`, `SWO.c`

---

## 3. Feature Modules

### Module 1: Target Power Management (Implement First)

Controls power delivery to each target connector and configures I/O voltage levels.

**Capabilities:**
- Per-connector power enable/disable (EN_A, EN_B) via RP2040 GPIO
- Voltage pass-through from VADJ1 â†’ Connector A, VADJ2 â†’ Connector B
- BugBuster controls VADJ1/VADJ2 voltage via DS4424 IDAC (already implemented)
- IO voltage programming — sets the level translation voltage to match target
- Power sequencing: set I/O voltage â†’ enable connector â†’ route pins
- Overcurrent detection via RP2040 ADC (if shunt resistor present)

**HAT Protocol Commands (RP2040 side):**

| CMD | Name | Payload | Description |
|-----|------|---------|-------------|
| 0x10 | SET_POWER | connector(u8), enable(u8) | Enable/disable connector power |
| 0x11 | GET_POWER_STATUS | â€” | Read power state for both connectors |
| 0x12 | SET_IO_VOLTAGE | voltage_mv(u16) | Set IO level (mV) |
| 0x13 | GET_IO_VOLTAGE | â€” | Read current I/O voltage setting |

**BugBuster BBP Commands (ESP32 side):**

| BBP ID | Name | Forwards to HAT CMD |
|--------|------|---------------------|
| 0xCA | HAT_SET_POWER | 0x10 |
| 0xCB | HAT_GET_POWER_STATUS | 0x11 |
| 0xCC | HAT_SET_IO_VOLTAGE | 0x12 |

**Power-On Sequence:**
1. BugBuster sets VADJ1/VADJ2 to desired voltage (DS4424)
2. BugBuster sends `HAT_SET_IO_VOLTAGE` → RP2040 programs IO voltage
3. Wait for voltage stabilization (~1ms)
4. BugBuster sends `HAT_SET_POWER(connector=A, enable=1)` â†’ RP2040 asserts EN_A
5. Wait for target power good
6. BugBuster sends `HAT_SET_ALL_PINS` â†’ RP2040 routes EXP_EXT lines
7. Target is now powered and debug/GPIO connections are live

---

### Module 2: SWD Debug Integration (Implement Second)

**The SWD protocol itself is handled entirely by debugprobe over USB CMSIS-DAP.**
BugBuster's role is management and coordination.

**What we add to the RP2040 BugBuster task:**

| CMD | Name | Payload | Description |
|-----|------|---------|-------------|
| 0x20 | GET_DAP_STATUS | â€” | Is debugprobe USB connected? Target detected? |
| 0x21 | GET_TARGET_INFO | â€” | DPIDR if target connected, SWD clock speed |
| 0x22 | SET_SWD_CLOCK | freq_khz(u16) | Adjust SWD clock (calls probe_set_swclk_freq) |

**What the host debug tools do directly (not through BugBuster):**
- OpenOCD, pyOCD, probe-rs connect to RP2040 USB CMSIS-DAP endpoint
- All SWD transactions (read/write DP/AP, memory, flash) go over USB
- BugBuster is not in this path â€” zero latency overhead

**What BugBuster Desktop App shows:**
- Target power status (from Module 1)
- Whether a debug tool is connected to the CMSIS-DAP port
- Target DPIDR (queried via UART from RP2040)
- Quick-setup wizard: "Click to configure SWD Debug" â†’
  1. Set VADJ to target voltage
  2. Set IO voltage
  3. Enable connector power
  4. Route EXP_EXT_1=SWDIO, EXP_EXT_2=SWCLK
  5. Show "Ready â€” connect with OpenOCD/VS Code"

**Python API convenience:**
```python
bb.hat_setup_swd(target_voltage=3.3)  # One-call setup:
# 1. Sets VADJ, 2. Sets IO voltage, 3. Enables power, 4. Routes SWD pins
# Returns: {"dpidr": "0x0BB11477", "ready": True}
```

---

### Module 3: Logic Analyzer (Implement Third)

Uses RP2040 **PIO 1** (PIO 0 is reserved for debugprobe SWD). Can operate simultaneously
with an active debug session.

**Capabilities:**
- 1â€“4 channel digital capture on EXP_EXT_3/EXP_EXT_4 (or all 4 if SWD not in use)
- Sample rates: configurable via PIO clock divider
  - 4 channels: up to 25 MHz each
  - 2 channels: up to 50 MHz each
  - 1 channel: up to 100 MHz
- Trigger: edge (rising/falling/both), level, pattern match
- Capture buffer: RP2040 SRAM ring buffer (~200KB)
- DMA: continuous PIO FIFO â†’ SRAM transfer
- Readout: chunked transfer via BugBuster UART (offline), or future RP2040 USB endpoint

**PIO 1 Allocation:**
- SM 0: Channel capture (1â€“4 channels multiplexed)
- SM 1: Trigger detection (optional hardware trigger)
- DMA channels: 2 (double-buffered ping-pong)

**HAT Protocol Commands:**

| CMD | Name | Payload | Description |
|-----|------|---------|-------------|
| 0x30 | LA_CONFIG | channels(u8), rate_khz(u32), depth_samples(u16) | Configure capture |
| 0x31 | LA_SET_TRIGGER | type(u8), channel(u8), edge(u8) | Set trigger condition |
| 0x32 | LA_ARM | â€” | Arm trigger, start waiting |
| 0x33 | LA_FORCE_TRIGGER | â€” | Force immediate capture |
| 0x34 | LA_GET_STATUS | â€” | State: idle/armed/capturing/done + samples captured |
| 0x35 | LA_READ_DATA | offset(u32), len(u16) | Read captured data chunk |
| 0x36 | LA_STOP | â€” | Abort capture |

**Bandwidth â€” chosen path (implemented in `bb-hat-2.0`):**

The HAT UART (even at 921600 baud â‰ˆ 92 KB/s) cannot carry sustained LA data
above a few hundred kHz across 4 channels. The RP2040's LA data path is
therefore a **dedicated USB vendor-bulk interface** (IF0, EP `0x06` OUT /
`0x87` IN) that the host claims directly via libusb. This keeps LA streaming
off the HAT UART and off the ESP32 entirely.

- **Interface 0** is claimed by TinyUSB's built-in vendor driver
  (`BB_LA_VENDOR_ITF = 0`); interface 1 is the CMSIS-DAP vendor interface
  claimed by the custom DAP driver.
- **Packet layout:** 4-byte header (`[type:u8][seq:u8][payload_len:u8][info:u8]`)
  plus 0â€“60 bytes of payload, so a full 64-byte FS USB bulk packet is one
  frame.
- **Packet types:** `PKT_START`, `PKT_DATA` (raw or RLE), `PKT_STOP`
  (with stop-reason info byte), `PKT_ERROR`.
- **Ring buffer:** 8 slots Ã— 2432 bytes â‰ˆ 19.5 KB on the RP2040 â€” sized so
  the host has ~5 ms of slack at 1 MHz / 4 ch before DMA overrun.
- **Throughput ceiling:** ~1.1 MB/s sustained (USB 2.0 Full-Speed).
- **Consecutive-run rearm:** `HAT_LA_STOP` performs a STOP-first preflight,
  SIE endpoint reset, then `tud_vendor_n_fifo_clear` â€” see
  [`../Docs/LogicAnalyzer.md`](../Docs/LogicAnalyzer.md) Â§6.1.

RLE compression (typical 10:1 on digital signals) is still used to trade CPU
for bandwidth inside a single run, but is now about fitting noisy signals
into the link rather than fitting any signal at all.

One-shot readout (e.g. `HAT_CMD_LA_USB_SEND` for offline capture-then-dump)
uses the same vendor-bulk endpoint with a simpler length-prefixed format.

**Simultaneous SWD + LA:**

PIO 0 handles SWD (debugprobe), PIO 1 handles LA (our code). They operate independently.
The logic analyzer can capture EXP_EXT_3/EXP_EXT_4 (GPIO/trace) while EXP_EXT_1/EXP_EXT_2
are used for SWD. This enables:
- Capture SWO trace on EXP_EXT_3 while debugging via SWD on EXP_EXT_1/2
- Monitor target GPIO signals during a debug session
- Trigger LA capture on a GPIO event, then halt target via SWD

---

## 4. RP2040 Firmware Architecture

```
â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
â”‚                   RP2040 Firmware                      â”‚
â”‚                                                        â”‚
â”‚  FreeRTOS Tasks:                                       â”‚
â”‚  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”  â”‚
â”‚  â”‚ Core 0                                           â”‚  â”‚
â”‚  â”‚ â”œâ”€â”€ dap_task (debugprobe) â€” CMSIS-DAP commands   â”‚  â”‚
â”‚  â”‚ â”œâ”€â”€ cdc_task (debugprobe) â€” USB UART bridge      â”‚  â”‚
â”‚  â”‚ â””â”€â”€ bb_cmd_task (NEW) â€” BugBuster UART handler   â”‚  â”‚
â”‚  â”‚     â”œâ”€â”€ Frame parser (0xAA sync + CRC-8)         â”‚  â”‚
â”‚  â”‚     â”œâ”€â”€ Power manager (EN_A, EN_B, sequencing)   â”‚  â”‚
â”‚  â”‚     â”œâ”€â”€ IO voltage control (DS4424 DAC)     â”‚  â”‚
â”‚  â”‚     â”œâ”€â”€ Pin config (EXP_EXT routing state)        â”‚  â”‚
â”‚  â”‚     â””â”€â”€ LA control (start/stop/readout)           â”‚  â”‚
â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜  â”‚
â”‚  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”  â”‚
â”‚  â”‚ Core 1 (optional, for CPU-intensive LA)          â”‚  â”‚
â”‚  â”‚ â””â”€â”€ LA DMA management + compression              â”‚  â”‚
â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜  â”‚
â”‚                                                        â”‚
â”‚  PIO 0: SWD engine (debugprobe, untouched)             â”‚
â”‚  PIO 1: Logic analyzer capture (BugBuster extension)   â”‚
â”‚                                                        â”‚
â”‚  USB: CMSIS-DAP + CDC (debugprobe) + optional LA bulk  â”‚
â”‚  UART0: BugBuster command bus (115200, slave)           â”‚
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
```

---

## 5. Command ID Space (HAT UART Protocol)

| Range | Module | Status |
|-------|--------|--------|
| 0x01â€“0x0F | Core (PING, INFO, PIN_CONFIG, RESET) | Implemented |
| 0x10â€“0x1F | Power Management (connectors, IO voltage) | Phase 1 |
| 0x20â€“0x2F | SWD Management (status, clock config) | Phase 2 |
| 0x30â€“0x3F | Logic Analyzer (config, trigger, readout) | Phase 3 |
| 0x40â€“0x7F | *(Reserved for future modules)* | â€” |

---

## 6. BBP Command Mapping (BugBuster ESP32 â†” Host)

| BBP ID | Name | HAT CMD | Module |
|--------|------|---------|--------|
| 0xC5 | HAT_GET_STATUS | 0x02 | Core |
| 0xC6 | HAT_SET_PIN | 0x03 | Core |
| 0xC7 | HAT_SET_ALL_PINS | 0x03 | Core |
| 0xC8 | HAT_RESET | 0x05 | Core |
| 0xC9 | HAT_DETECT | â€” | Core (ADC) |
| 0xCA | HAT_SET_POWER | 0x10 | Power |
| 0xCB | HAT_GET_POWER_STATUS | 0x11 | Power |
| 0xCC | HAT_SET_IO_VOLTAGE | 0x12 | Power |
| 0xCD | HAT_GET_DAP_STATUS | 0x20 | SWD Mgmt |
| 0xCE | HAT_SET_SWD_CLOCK | 0x22 | SWD Mgmt |
| 0xCF | HAT_LA_CONFIG | 0x30 | Logic Analyzer |
| 0xD5 | HAT_LA_ARM | 0x32 | Logic Analyzer |
| 0xD6 | HAT_LA_STATUS | 0x34 | Logic Analyzer |
| 0xD7 | HAT_LA_READ_DATA | 0x35 | Logic Analyzer |
| 0xD8 | HAT_LA_STOP | 0x36 | Logic Analyzer |
| 0xD9 | HAT_LA_STOP (alt) | 0x36 | Logic Analyzer |
| 0xDA | HAT_LA_TRIGGER | â€” | Logic Analyzer |

---


## 8. Implementation Roadmap

### Phase 1: Target Power Management + debugprobe fork setup

**RP2040 side:**
1. Fork debugprobe, create `board_bugbuster_hat_config.h`
2. Add `bb_cmd_task` FreeRTOS task to `main.c`
3. Implement HAT protocol frame parser (`bugbuster_protocol.c`)
4. Implement power control: GPIO for EN_A, EN_B
5. Test: UART commands control power, CMSIS-DAP still works over USB

**BugBuster ESP32 side:**
1. Add BBP commands 0xCAâ€“0xCC with UART forwarding
2. HTTP endpoints: `POST /api/hat/power`, `POST /api/hat/io_voltage`
3. Desktop UI: power toggles + I/O voltage selector in HAT tab
4. Python: `hat_set_power()`, `hat_set_io_voltage()`

**Deliverable:** User powers targets at correct voltage from BugBuster app. CMSIS-DAP
debug works over USB simultaneously. Pin configuration routes SWD/GPIO signals.

### Phase 2: SWD Management Layer

**RP2040 side:**
1. Add HAT commands 0x20â€“0x22 (query DAP status, target DPIDR, set SWD clock)
2. Read debugprobe internal state to report connection/target status

**BugBuster side:**
1. Add BBP commands 0xCDâ€“0xCE
2. Desktop UI: "SWD Debug" section showing target status + quick-setup wizard
3. Python: `hat_setup_swd(voltage)` convenience function

**Deliverable:** One-click SWD setup from BugBuster app. Status reporting. Debug via USB.

### Phase 3: Logic Analyzer

**RP2040 side:**
1. PIO 1 capture programs (`bugbuster_la.pio`)
2. DMA double-buffer management
3. Trigger engine (edge/pattern)
4. HAT commands 0x30â€“0x36
5. SRAM buffer management and chunked readout

**BugBuster side:**
1. BBP commands 0xCF, 0xD5â€“0xD8
2. HTTP endpoints for LA control and data
3. Desktop UI: waveform viewer with trigger config
4. Python: `hat_la_capture()`, `hat_la_get_data()`

**Deliverable:** 1â€“4 channel logic capture, viewable in BugBuster app. Can run
simultaneously with SWD debug (PIO 0 = SWD, PIO 1 = LA).

---

## 9. Desktop App UI Vision

### HAT Tab Layout (All Phases Complete)

```
â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
â”‚ HAT Status          [SWD/GPIO HAT v1.0]  [Refresh]          â”‚
â”‚ â— Detected  â— Connected  â— DAP Active  IO: 3.3V             â”‚
â”œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¤
â”‚ Target Power                                                  â”‚
â”‚  Connector A: [ON /OFF]  VADJ1: 3.3V   I: 45mA              â”‚
â”‚  Connector B: [ON /OFF]  VADJ2: 5.0V   I: 12mA              â”‚
â”‚  I/O Voltage: [â–¼ 3.3V â–¼]  (IO level translation)        â”‚
â”‚  [Power Sequence: IO first â–¼]                                â”‚
â”œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¤
â”‚ Pin Configuration                                             â”‚
â”‚  EXT_1: [â–¼ SWDIO  â–¼]   EXT_2: [â–¼ SWCLK  â–¼]                â”‚
â”‚  EXT_3: [â–¼ TRACE1 â–¼]   EXT_4: [â–¼ GPIO4  â–¼]                â”‚
â”‚  [SWD Debug] [GPIO Mode] [SWD+SWO] [Disconnect All]         â”‚
â”œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¤
â”‚ SWD Debug                                                     â”‚
â”‚  Target: STM32F411  DPIDR: 0x0BB11477                        â”‚
â”‚  CMSIS-DAP: â— Connected (OpenOCD)  SWD Clock: 4 MHz         â”‚
â”‚  [Quick Setup: 3.3V SWD] [Set Clock â–¼]                      â”‚
â”œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¤
â”‚ Logic Analyzer                                                â”‚
â”‚  Channels: [â–¼ 2 â–¼]  Rate: [â–¼ 1 MHz â–¼]  Depth: 200K samples â”‚
â”‚  Trigger: [â–¼ Ch3 Rising Edge â–¼]                              â”‚
â”‚  Status: â— Armed (waiting for trigger)                        â”‚
â”‚  [Arm] [Force] [Stop] [Download Data]                        â”‚
â”‚                                                               â”‚
â”‚  â”€â”€CH3â”€â”€â”   â”Œâ”€â”€â”€â”€â”€â”€â”   â”Œâ”€â”€â”€â”€â”€â”€â”€â”€                            â”‚
â”‚          â””â”€â”€â”€â”˜      â””â”€â”€â”€â”˜                                     â”‚
â”‚  â”€â”€CH4â”€â”€â”€â”€â”€â”€â”€â”€â”         â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€                          â”‚
â”‚               â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜                                     â”‚
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
```
