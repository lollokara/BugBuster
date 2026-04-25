# BugBuster — Hardware design

Deep dive on the main-board ICs, power topology, and ESP32-S3 pin assignments.
Schematics and PCB layout live in Altium form under
[`../PCB Material/`](../PCB%20Material/); this page exists so AI and humans can
reason about the board without opening Altium.

---

## 1. Key ICs

| IC | Function | Interface |
|---|---|---|
| **AD74416H** | 4-channel software-configurable I/O — 24-bit ADC, 16-bit DAC, per-channel range/mode | SPI (up to 20 MHz) |
| **ADGS2414D × 4** | 32-switch SPST analog MUX matrix, daisy-chained | SPI (shared bus) |
| **DS4424** | 4-channel IDAC — tunes LTM8063 / LTM8078 feedback networks | I²C `0x10` |
| **HUSB238** | USB-C PD sink controller (5–20 V negotiation) | I²C `0x08` |
| **PCA9535AHF** | 16-bit GPIO expander — power enables, e-fuse control, status LEDs | I²C `0x23` |
| **LTM8063 × 2** | Adjustable step-down DCDC (3–15 V, 2 A) for VADJ1 / VADJ2 | Analog (FB pin) |
| **LTM8078** | Level-shifter DCDC for VLOGIC | Analog (FB pin) |
| **TPS1641x × 4** | E-fuse / current limiters per output port | GPIO enable |
| **TXS0108E × 4** | Bidirectional level shifters for 12 digital IOs | OE via GPIO |

---

## 2. Power topology

```
USB-C
  │
  ▼
HUSB238 ──(PD negotiation, default 20 V)──▶ VBUS_PD
                                              │
                                              ├─▶ LTM8063 #1 ──▶ VADJ1 (3–15 V, DS4424-tuned)
                                              │                     │
                                              │                     ├─▶ TPS1641x #1 ──▶ Port 1 (IO 1-3)
                                              │                     └─▶ TPS1641x #2 ──▶ Port 2 (IO 4-6)
                                              │
                                              ├─▶ LTM8063 #2 ──▶ VADJ2 (3–15 V, DS4424-tuned)
                                              │                     │
                                              │                     ├─▶ TPS1641x #3 ──▶ Port 3 (IO 7-9)
                                              │                     └─▶ TPS1641x #4 ──▶ Port 4 (IO 10-12)
                                              │
                                              └─▶ LTM8078 ──▶ VLOGIC (1.8–5 V, fixed at startup)
                                                                    │
                                                                    └─▶ TXS0108E × 4 (level-shift all 12 DIOs)
```

- **VADJ1 / VADJ2** are independently adjustable under AI control
  (`set_supply_voltage(rail, voltage)`), gated by the board-profile rail lock
  if a profile is active (see [`board_profiles.md`](board_profiles.md)).
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
  IO_Block 1  [E-fuse 1, MUX U10]      IO_Block 3  [E-fuse 4, MUX U17]
    IO 1  — digital only                  IO 7  — digital only
    IO 2  — digital only                  IO 8  — digital only
    IO 3  — analog + digital + HAT        IO 9  — analog + digital + HAT
  IO_Block 2  [E-fuse 2, MUX U11]      IO_Block 4  [E-fuse 3, MUX U16]
    IO 4  — digital only                  IO 10 — digital only
    IO 5  — digital only                  IO 11 — digital only
    IO 6  — analog + digital + HAT        IO 12 — analog + digital + HAT
```

Each IO is routed through an ADGS2414D octal SPST switch — functions are
**mutually exclusive** (exactly one active path at a time).  VLOGIC controls
the logic level for all digital IOs through TXS0108E level shifters.

Analog-capable IOs (3, 6, 9, 12) can act as voltage input, current input,
voltage output, current source, RTD excitation probe, or be routed to the HAT
connector for SWD / LA.

---

## 4. ESP32-S3 pin assignments

### SPI bus (shared AD74416H + ADGS2414D MUX)

| Signal | GPIO | Notes |
|---|---|---|
| MISO (SDO) | 8 | From AD74416H |
| MOSI (SDI) | 9 | To AD74416H |
| CS (SYNC) | 10 | AD74416H chip select, active-low |
| SCLK | 11 | 10 MHz default, up to 20 MHz |
| MUX_CS | 12 | ADGS2414D chip select |
| LSHIFT_OE | 14 | Level-shifter output enable (gates all 12 DIOs) |

### AD74416H control lines

| Signal | GPIO | Notes |
|---|---|---|
| RESET | 5 | Active-low hardware reset |
| ADC_RDY | 6 | Open-drain — ADC conversion ready |
| ALERT | 7 | Open-drain — fault output |

### I²C bus (shared DS4424 / HUSB238 / PCA9535)

| Signal | GPIO |
|---|---|
| SDA | 1 |
| SCL | 4 |

### HAT expansion header

| Signal | GPIO | Notes |
|---|---|---|
| HAT_TX (UART0 → RP2040) | 43 | 921600 8N1 |
| HAT_RX (RP2040 → ESP32) | 44 | 921600 8N1 |
| HAT_DETECT | 47 | ADC1_CH6 voltage divider (see `HAT_Protocol.md` §2) |
| HAT_IRQ | 15 | Shared open-drain, pulled by RP2040 GPIO8 |

---

## 5. FreeRTOS task layout (ESP32-S3)

| Task | Core | Priority | Purpose |
|---|---|---|---|
| `taskAdcPoll` | 1 | 3 | Read ADC results, convert to engineering units, accumulate scope buckets |
| `taskFaultMonitor` | 1 | 4 | Alert / fault status, DIN counters, GPIO, supply diagnostics |
| `taskCommandProcessor` | 1 | 2 | Dequeue and execute hardware commands (channel func, DAC, config) |
| `taskI2cPoll` | 1 | 1 | Poll DS4424 / HUSB238 / PCA9535 state |
| `taskWavegen` | 1 | 3 | Generate waveform samples, write DAC codes at target frequency |
| `mainLoopTask` | 0 | 1 | CLI input, BBP handshake, binary protocol, heartbeat |

Core 0 is dedicated to USB + Wi-Fi + BBP framing; Core 1 runs the real-time
analog pipeline.  This split is why the scope / ADC streaming path can keep up
even while the user hammers the HTTP REST interface concurrently.

---

## 6. See also

- [`LogicAnalyzer.md`](LogicAnalyzer.md) — RP2040 HAT LA engine and vendor-bulk
  streaming.
- [`board_profiles.md`](board_profiles.md) — pin-map / rail-lock schema that
  sits on top of this hardware.
- [`../Firmware/BugBusterProtocol.md`](../Firmware/BugBusterProtocol.md) — wire
  format the host uses to drive all of the above.
- [`../Firmware/HAT_Architecture.md`](../Firmware/HAT_Architecture.md) — HAT
  board design (RP2040, debugprobe fork, HVPAK).
