# DAQ HAT Display Protocol (DDP)

**Version:** 2 (`ddp-v2`)
**Transport:** UART 921600 8N1 (internal P4↔C6 link)
**Roles:** ESP32-P4 = Master / data source, ESP32-C6 = Display + input co-processor

---

## 1. Overview

The DAQ HAT carries two MCUs:

```
ESP32-P4 (application proc) ──internal UART 921600──▶ ESP32-C6 (display proc)
        ▲                                                 ├── ST7789 SPI panel (284×76 landscape)
        └──────── SET_CONFIG events ──────────────────────┴── 3 nav buttons (UP/DOWN/OK)
```

The **P4** acquires/manages measurements and pushes them to the **C6**, which
renders the on-board ST7789 display and hosts the settings/diagnostics menu.

Two data directions:

- **P4 → C6 (master push):** live measurement, full diagnostics snapshot, plus
  backlight/status/clear render commands. These follow the master-push model —
  every command gets an `RSP_OK`/`RSP_ERR`.
- **C6 → P4 (config events):** when the user edits a setting on-device (buttons),
  the C6 emits a `SET_CONFIG` event so the P4 applies it. A TX mutex serializes
  these against command responses on the half-duplex line.

The framing is identical to the RP2040 HAT control protocol
(see [`../HAT_Protocol.md`](../HAT_Protocol.md)) so the CRC routine and parser
are shared. The canonical C definitions live in
[`ESP32C6/src/ddp_proto.h`](ESP32C6/src/ddp_proto.h).

> **No P4 yet.** Until the P4 firmware exists, the C6 self-drives:
> - the **readout** runs a demo generator (`DDP_STATE_SIM`) sweeping V and I
>   across many decades to exercise the autoscaler;
> - the **Diagnostics** menu shows simulated sensor values.
>
> The moment a valid `SET_MEASUREMENT` arrives the readout switches to
> `DDP_STATE_LIVE` (falling back to SIM after 1 s of silence); a
> `SET_DIAGNOSTICS` within the last 2 s replaces the simulated diagnostics with
> live values.

---

## 2. Frame Format

```
┌──────┬─────┬─────┬────────────────────┬──────┐
│ SYNC │ LEN │ CMD │    PAYLOAD         │ CRC  │
│ 1B   │ 1B  │ 1B  │    0..32 B         │ 1B   │
└──────┴─────┴─────┴────────────────────┴──────┘
```

| Field | Size | Description |
|-------|------|-------------|
| SYNC | 1 | Frame start marker: **0xAA** |
| LEN | 1 | Payload length (0–32), excludes SYNC/LEN/CMD/CRC |
| CMD | 1 | Command/event (0x01–0x7F) or Response (0x80–0xFF) |
| PAYLOAD | 0–32 | Command-specific, **little-endian** |
| CRC | 1 | CRC-8 (poly 0x07, init 0x00) over CMD + PAYLOAD |

CRC-8 polynomial `0x07`, initial value `0x00` — same as the HAT protocol.

---

## 3. Commands (P4 → C6)

| CMD | Name | Payload | Response |
|-----|------|---------|----------|
| `0x01` | PING | — | `RSP_OK` |
| `0x02` | GET_INFO | — | `RSP_INFO` |
| `0x10` | SET_MEASUREMENT | `ddp_measurement_t` (9 B) | `RSP_OK` |
| `0x11` | SET_STATUS | `u8 state, ASCII label[…]` | `RSP_OK` |
| `0x12` | SET_BACKLIGHT | `u8 brightness` | `RSP_OK` |
| `0x13` | CLEAR | — | `RSP_OK` |
| `0x14` | SET_DIAGNOSTICS | `ddp_diag_t` (21 B) | `RSP_OK` |

### 0x10 SET_MEASUREMENT — `ddp_measurement_t` (9 B)

| Off | Field | Type | Notes |
|-----|-------|------|-------|
| 0 | `voltage_v` | f32 | volts (base SI) |
| 4 | `current_a` | f32 | amperes (base SI) |
| 8 | `flags` | u8 | see below |

The C6 autoscales for display (nV…MV, nA…A). Flags:

| Bit | Meaning |
|-----|---------|
| 0 | `V_VALID` — voltage field is live |
| 1 | `I_VALID` — current field is live |
| 2 | `V_OVERRANGE` |
| 3 | `I_OVERRANGE` |

Example — V = 3.3 V, I = 12.5 mA, both valid:

```
AA 09 10  66 66 53 40  00 80 4C 3D  03  <CRC>
        │  └ f32 3.30 ┘ └ f32 .0125┘ └flags
        └ CMD=SET_MEASUREMENT, LEN=9
```

### 0x14 SET_DIAGNOSTICS — `ddp_diag_t` (21 B)

Compact fixed-point so the whole snapshot fits in one frame. Push periodically
(≈1–2 Hz is enough; the C6 treats anything older than 2 s as stale and reverts
to simulated values). Drives the **Diagnostics** menu.

| Off | Field | Type | Unit | Menu row |
|-----|-------|------|------|----------|
| 0  | `adc_temp_c10`    | i16 | 0.1 °C | ADC Temp. |
| 2  | `vsrc_temp_c10`   | i16 | 0.1 °C | Voltage Src Temp. |
| 4  | `esp_current_ma`  | i16 | mA | ESP32 Current |
| 6  | `esp_input_ma`    | i16 | mA | ESP32 Input Curr. |
| 8  | `p4_free_stack_b` | u16 | bytes | P4 Free Stack |
| 10 | `p4_free_mem_kb`  | u16 | KB | P4 Free Mem. |
| 12 | `p4_tasks`        | u8  | count | P4 Tasks |
| 13 | `mb_temp_c10`     | i16 | 0.1 °C | Main Board Temp. |
| 15 | `mb_pd_mv`        | u16 | mV | Main Board PD V. |
| 17 | `mb_dvcc_mv`      | u16 | mV | Main Board DVCC V. |
| 19 | `mb_avcc_mv`      | u16 | mV | Main Board AVCC V. |

---

## 4. Events (C6 → P4)

| CMD | Name | Payload | Direction |
|-----|------|---------|-----------|
| `0x60` | SET_CONFIG | `ddp_config_t` (10 B) | C6 → P4 |

### 0x60 SET_CONFIG — `ddp_config_t` (10 B)

Emitted **unsolicited** by the C6 whenever the user changes a setting through
the on-screen menu, mirroring the on-device state so the P4 can apply it
(ranging, sample rate, DUT limits, etc.). Fire-and-forget — no response.

| Off | Field | Type | Notes |
|-----|-------|------|-------|
| 0 | `autoranging`     | u8  | 0/1 |
| 1 | `range_idx`       | u8  | 0=A, 1=mA, 2=uA |
| 2 | `sample_rate_idx` | u8  | 0..4 → 10k/50k/100k/250k/1M sps |
| 3 | `reserved`        | u8  | 0 |
| 4 | `dut_current_ma`  | u16 | 100..2500 |
| 6 | `dut_voltage_mv`  | u16 | 1800..20000 |
| 8 | `brightness_pct`  | u8  | 10..100 |
| 9 | `dark_mode`       | u8  | 0/1 |

The C6 also persists these to NVS, so they survive power cycles independently
of the P4.

---

## 5. Responses (C6 → P4)

| RSP | Name | Payload |
|-----|------|---------|
| `0x80` | RSP_OK | — |
| `0x82` | RSP_INFO | `u8 hat_type, u8 fw_major, u8 fw_minor, u8 proto_version` |
| `0xFF` | RSP_ERR | `u8 error_code` |

Other than `SET_CONFIG` events, the C6 only transmits in response to a command.

---

## 6. UI / Display System

The C6 firmware presents two top-level screens, toggled by the buttons:

### 6.1 Main readout
- **Header:** animated Pac-Man chasing a stream of lightning bolts (cached
  sprites blitted each frame — no per-frame vector math).
- **Two hero cards:** Voltage and Current, each with a baked **JetBrains Mono**
  number that autoscales with an SI prefix (`n µ m _ k M`).

### 6.2 Settings menu (Pebble-style carousel)
Pressing any button from the readout opens the menu. Navigation: **UP/DOWN**
move the selection (spring "gravity" snap), **OK** activates, **hold OK = Back**.
A **30 s** inactivity timer returns to the readout.

| Menu | Items |
|------|-------|
| **HAT Settings** | Autoranging (ON/OFF) · Range Setting (A/mA/uA, shown only when manual, with ⚠) · Sample Rate (10k…1M sps) · DUT Current Limit (bargraph 0.1–2.5 A) · DUT Voltage (bargraph 1.8–20 V) |
| **Screen Settings** | Brightness (bargraph 10–100%) · Dark Mode (light ⇄ neon-dark) |
| **Main Board Settings** | *(empty)* |
| **WiFi Settings** | *(empty)* |
| **Diagnostics** | 11 read-only rows fed by `SET_DIAGNOSTICS` (§3) |

Settings persist to NVS and are mirrored to the P4 via `SET_CONFIG` (§4).

### 6.3 Buttons (wired to the C6)

| Button | GPIO | Function |
|--------|------|----------|
| UP | IO5 | navigate up / increment |
| DOWN | IO6 | navigate down / decrement |
| OK | IO7 | activate (short) · Back (hold) |

Active-low with internal pull-ups; UP/DOWN auto-repeat when held.

---

## 7. Firmware source map (`Firmware/DAQ_HAT/ESP32C6/src/`)

| File | Role |
|------|------|
| `ddp_proto.h` | Wire definitions (canonical) |
| `ddp.c/.h` | UART slave: parse, respond, diagnostics cache, config TX |
| `menu.c/.h` | Carousel menu engine + tree + bargraph editor |
| `settings.c/.h` | Settings struct + NVS persistence |
| `theme.c/.h` | Runtime light / neon-dark palettes |
| `buttons.c/.h` | Debounced nav buttons (repeat + long-press) |
| `ui.c/.h` | Main readout (cards + animated header) |
| `gfx.c/.h` | Framebuffer primitives, sprites, fonts |
| `display.c/.h` | esp_lcd ST7789 driver + synchronous flush |

