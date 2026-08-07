# Mainboard hardware

The main-board ICs, power topology, and ESP32-S3 pin assignments. Schematics and
layout live in Altium form under [`../PCB Material/`](../PCB%20Material/); this
page exists so a human or a model can reason about the board without opening
Altium.

For pin-level detail the canonical source is
`.mex/context/hardware-pinout.md`, which is verified directly against
`config.h`.

---

## 1. Key ICs

| IC | Function | Interface |
|---|---|---|
| **AD74416H** | 4-channel software-configurable I/O - 24-bit ADC, 16-bit DAC, per-channel range/mode | SPI (up to 20 MHz) |
| **ADGS2414D × 4** | 32-switch SPST analog MUX matrix, daisy-chained | SPI (shared bus) |
| **DS4424** | 4-channel IDAC - tunes LTM8063 / LTM8078 feedback networks | I²C `0x10` |
| **HUSB238** | USB-C PD sink controller (5–20 V negotiation) | I²C `0x08` |
| **PCA9535AHF** | 16-bit GPIO expander - power enables, e-fuse control, status LEDs | I²C `0x23` |
| **LTM8063 × 2** | Adjustable step-down DCDC (3–15 V, 2 A) for VADJ1 / VADJ2 (U4, U6) | Analog (FB pin) |
| **LTM8078** | Dual silent switcher; its Out2 feedback node sets VLOGIC (U3) | Analog (FB pin) |
| **TPS74601** | Adjustable 1 A LDO producing `3V3_ADJ` for the AD74416H (U5) | Enable via PCA9535 |
| **TPS16410 × 4** | E-fuse / current limiter per output port (U12, U14, U18, U19) | GPIO enable |
| **TXS0108E × 2** | 8-bit bidirectional level shifters for the 12 digital IOs (U13, U15) | OE via GPIO |

---

## 2. Power topology

```
USB-C
  │
  ▼
HUSB238 ──(PD negotiation, default 20 V)──▶ VBUS_PD
                                              │
                                              ├─▶ LTM8063 #1 (U4) ─▶ VADJ1 (3–15 V, DS4424 OUT1)
                                              │                     │
                                              │                     ├─▶ TPS16410 U12 ─▶ Port 1 (IO 1-3)
                                              │                     └─▶ TPS16410 U14 ─▶ Port 2 (IO 4-6)
                                              │
                                              ├─▶ LTM8063 #2 (U6) ─▶ VADJ2 (3–15 V, DS4424 OUT2)
                                              │                     │
                                              │                     ├─▶ TPS16410 U18 ─▶ Port 3 (IO 7-9)
                                              │                     └─▶ TPS16410 U19 ─▶ Port 4 (IO 10-12)
                                              │
                                              └─▶ LTM8078 (U3) ─▶ VLOGIC (1.8–5 V, DS4424 OUT0)
                                                                    │
                                                                    └─▶ TXS0108E × 2 (all 12 DIOs)
```

`3V3_ADJ`, the AD74416H analog supply, comes from the TPS74601 LDO (U5) and is
separate from VLOGIC. The two are easy to confuse: **VLOGIC is trimmed through
the LTM8078 Out2 feedback node, not the TPS74601.**

- **VADJ1 / VADJ2** are independently adjustable under AI control
  (`set_supply_voltage(rail, voltage)`), gated by the board-profile rail lock
  if a profile is active (see [`board-profiles.md`](board-profiles.md)).
- **VLOGIC** is set once at MCP server startup via `--vlogic` and is not
  re-writable during a session.  This is the single most-often-enforced safety
  invariant in `bugbuster_mcp/safety.py`.
- **E-fuses** auto-arm when any output function is configured. A trip causes
  an `ALERT_EVT` (BBP `0xE1`) with the offending port index, surfaced to the AI
  as a fault warning on the next tool response.

---

## 3. IO architecture

The board has **12 physical IOs** organised into 2 power domains of 2 IO
blocks of 3 IOs each:

```
Block 1 (VADJ1, 3-15 V)              Block 2 (VADJ2, 3-15 V)
  IO_Block 1  [E-fuse 1, MUX U10]      IO_Block 3  [E-fuse 3, MUX U17]
    IO 1 - digital only                  IO 7 - digital only
    IO 2 - digital only                  IO 8 - digital only
    IO 3 - analog + digital + HAT        IO 9 - analog + digital + HAT
  IO_Block 2  [E-fuse 2, MUX U11]      IO_Block 4  [E-fuse 4, MUX U16]
    IO 4 - digital only                  IO 10 - digital only
    IO 5 - digital only                  IO 11 - digital only
    IO 6 - analog + digital + HAT        IO 12 - analog + digital + HAT
```

Each IO is routed through an ADGS2414D octal SPST switch - functions are
**mutually exclusive** (exactly one active path at a time).  VLOGIC controls
the logic level for all digital IOs through TXS0108E level shifters.

Analog-capable IOs (3, 6, 9, 12) can act as voltage input, current input,
voltage output, current source, RTD excitation probe, or be routed to the HAT
connector for SWD / LA.

### User-facing C/D remap

Operators, scripts, MCP tools, and UI surfaces always address the analog
channels in natural order `A/B/C/D`:

| Label | IO_Block | Analog IO | Public ADC channel | MUX device | ESP32 GPIO | E-fuse |
|-------|----------|-----------|--------------------|------------|------------|--------|
| A     | 1        | IO3       | 0                  | U10 / 0    | GPIO1      | EFUSE1 |
| B     | 2        | IO6       | 1                  | U11 / 1    | GPIO5      | EFUSE2 |
| C     | 3        | IO9       | 2                  | U17 / 2    | GPIO10     | EFUSE3 |
| D     | 4        | IO12      | 3                  | U16 / 3    | GPIO13     | EFUSE4 |

The PCB swaps the AD74416H C/D register wiring, so firmware translates public
channel C to AD74416H physical D and public channel D to AD74416H physical C in
`tasks_logical_to_physical()`. Do not apply that ADC-register swap in host code,
docs, or UI labels. MUX devices, ESP32 GPIOs, e-fuses, and IO_Block ownership
stay with the connector row above.

The selftest U23 path is an internal exception: it is wired to AD74416H physical
Channel D and claims the logical owner of that physical register while measuring.

---

## 4. ESP32-S3 pin assignments

### SPI bus (shared AD74416H + ADGS2414D MUX)

| Signal | GPIO | Notes |
|---|---|---|
| SCLK | 16 | 10 MHz default, up to 20 MHz |
| MOSI (SDI) | 17 | To AD74416H |
| MISO (SDO) | 18 | From AD74416H |
| CS (SYNC) | 40 | AD74416H chip select, active-low |
| MUX_CS | 21 | ADGS2414D daisy-chain chip select (5 devices) |
| LSHIFT_OE | 14 | Level-shifter output enable (gates all 12 DIOs) |

### AD74416H control lines

| Signal | GPIO | Notes |
|---|---|---|
| RESET | 45 | Active-low hardware reset |
| ADC_RDY | 38 | Open-drain - ADC conversion ready |
| ALERT | 39 | Open-drain - fault output |

### I²C bus (shared DS4424 / HUSB238 / PCA9535)

| Signal | GPIO | Notes |
|---|---|---|
| SDA | 42 | 400 kHz Fast Mode |
| SCL | 41 | |
| PCA9535 INT | 3 | PCAL9535A interrupt output |

### HAT expansion header

| Signal | GPIO | Notes |
|---|---|---|
| HAT_TX (UART0 → RP2040) | 43 | 921600 8N1 |
| HAT_RX (RP2040 → ESP32) | 44 | 921600 8N1 |
| HAT_DETECT | 47 | **Digital strap** - HIGH=no HAT, LOW=HAT present (NOT ADC) |
| HAT_IRQ | 15 | Shared open-drain, pulled by RP2040 GPIO8 |

---

## 5. FreeRTOS task layout (ESP32-S3)

The four instrument tasks are created in `tasks.cpp` and all pinned to **Core 1**:

| Task | Core | Priority | Stack | Purpose |
|---|:---:|:---:|---|---|
| `adcPoll` | 1 | 3 | `TASK_STACK_ADCPOLL` | Read ADC results, scope buckets, DSP streaming |
| `faultMon` | 1 | 4 | `TASK_STACK_FAULTMON` | Alert and fault status, DIN counters, diagnostics |
| `cmdProc` | 1 | 2 | `TASK_STACK_CMDPROC` | BBP command dispatch |
| `wavegen` | 1 | 3 | `TASK_STACK_WAVEGEN` | Waveform output (sine / square / triangle / sawtooth) |
| `httpd` | 0 | - | - | ESP-IDF HTTP server: 155 `/api/*` paths, SSE, admin auth |
| `mpTask` | 1 | - | - | MicroPython VM (1 MB GC heap in PSRAM) |
| `wifiWorker` | 0 | - | - | WiFi STA connect and reconnect, mDNS |

Core 0 runs USB, WiFi and HTTP; Core 1 runs the real-time analog pipeline and
MicroPython. That split is what lets the scope and ADC streaming path keep up
while the HTTP interface is being hammered.

Stack sizes are `TASK_STACK_*` constants rather than literals, because
`tests/unit/` parses them out of the firmware source to check the memory budget.
I²C devices (PCA9535, HUSB238, DS4424) are polled on demand by the BBP, HTTP and
CLI handlers - there is no background I²C task.

---

## 6. See also

- [`logic-analyzer.md`](logic-analyzer.md) - RP2040 HAT LA engine and vendor-bulk
  streaming.
- [`board-profiles.md`](board-profiles.md) - pin-map / rail-lock schema that
  sits on top of this hardware.
- [`../Firmware/bbp-protocol.md`](../Firmware/bbp-protocol.md) - wire
  format the host uses to drive all of the above.
- [`../Firmware/la-hat-architecture.md`](../Firmware/la-hat-architecture.md) - HAT
  board design (RP2040, debugprobe fork).
