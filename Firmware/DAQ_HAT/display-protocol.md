# DAQ HAT display protocol (DDP)

The link between the two MCUs on the Power Profiler Pro HAT. The ESP32-P4 is
master and data source; the ESP32-C6 renders the ST7789 panel and owns the
on-device menu.

**Wire version 9** · UART 921600 8N1 · framing identical to the
[HAT UART protocol](../hat-uart-protocol.md), so the CRC code is shared.

The canonical definitions are in
[common/ddp_proto.h](common/ddp_proto.h) - one header, included by **both**
firmwares, so the two chips cannot drift. This document explains the shape;
the header is the source of truth for every field.

```
ESP32-P4 (acquire + DSP) ──UART 921600──▶ ESP32-C6 ──▶ ST7789 284×76 panel
        ▲                                     │
        └────── CONFIG_SET / CONFIG_ACTION ───┘        (3 nav buttons on the P4)
```

Traffic goes both ways:

- **P4 → C6** - live measurement, diagnostics snapshots, button-event relay,
  config pushes, LED and backlight commands. Master-push: every command gets an
  `RSP_OK` or `RSP_ERR`.
- **C6 → P4** - unsolicited setting changes and one-shot actions when the user
  edits something on-device. A TX mutex serialises these against command
  responses on the half-duplex line.

## Frame format

```
┌──────┬─────┬─────┬──────────────┬──────┐
│ SYNC │ LEN │ CMD │   PAYLOAD    │ CRC  │
│  1B  │ 1B  │ 1B  │   0..240 B   │  1B  │
└──────┴─────┴─────┴──────────────┴──────┘
```

| Field | Size | Meaning |
|---|---|---|
| SYNC | 1 | `0xAA` |
| LEN | 1 | Payload length, 0–240 |
| CMD | 1 | Command `0x01–0x5F`, event `0x60–0x7F`, response `0x80–0xFF` |
| PAYLOAD | 0–240 | Command-specific, **little-endian** |
| CRC | 1 | CRC-8 (poly `0x07`, init `0x00`) over CMD + PAYLOAD |

## Commands - P4 → C6

| CMD | Name | Payload |
|---|---|---|
| `0x01` | `PING` | - |
| `0x02` | `GET_INFO` | - → `RSP_INFO` |
| `0x10` | `SET_MEASUREMENT` | `ddp_measurement_t` (9 B) |
| `0x11` | `SET_STATUS` | `u8 state`, ASCII label ≤30 B |
| `0x12` | `SET_BACKLIGHT` | `u8 brightness` |
| `0x13` | `CLEAR` | - |
| `0x14` | `SET_DIAGNOSTICS` | `ddp_diag_t` - full onboard-device snapshot |
| `0x15` | `BUTTON_EVENT` | `u8` bitmask - the buttons are wired to the P4 and relayed |
| `0x16` | `CONFIG_PUSH` | TLV batch - current or changed settings |
| `0x18` | `SET_CH_LEDS` | 4× `u8` channel colour codes |
| `0x19` | `MB_REQUEST` | Mainboard tunnel request (see below) |
| `0x1A` | `MB_RESPONSE` | Mainboard tunnel result |
| `0x1B` | `CAL_CTRL` | SMU calibration wizard control |
| `0x1C` | `CAL_STATUS` | SMU calibration wizard status |
| `0x1D` | `WIFI_STREAM_MODE` | `u8 enable` - SDIO handover notice |

### `0x10 SET_MEASUREMENT`

`float voltage_v`, `float current_a`, `u8 flags`. The C6 autoscales for display
(nV…MV, nA…A), so both values are plain base-SI.

Flag bits: `V_VALID` (0), `I_VALID` (1), `V_OVERRANGE` (2), `I_OVERRANGE` (3),
`SRC_ON` (4). Bits 5–6 carry the live current range (`HI` 51 Ω / `MID` 2 Ω /
`LO` 50 mΩ / unknown) so the home screen can show a range badge without a
second frame.

```
AA 09 10  66 66 53 40  00 80 4C 3D  03  <CRC>
        │  └ f32 3.30V ┘└ f32 12.5mA┘ └ flags
        └ CMD=SET_MEASUREMENT, LEN=9
```

### `0x14 SET_DIAGNOSTICS`

A full board snapshot in compact fixed point, pushed at roughly 1 Hz: board and
ADAQ die temperatures, the P4 and relayed S3 die temperatures, fused I/V/P in
micro-units, SMU monitor currents, measured `V_DUT`, USB-PD and VADJ rails
relayed from the S3, and P4 runtime stats.

A `valid` bitmask says which sources are fresh; the C6 renders `--` for anything
not flagged. Temperatures use `0x7FFF` as an unreadable sentinel. The C6 reads
its own heap, die temperature and uptime locally rather than receiving them.

Anything older than 2 s is treated as stale.

### `0x1D WIFI_STREAM_MODE`

The P4 warns the C6 before handing the shared SDIO link to ESP-Hosted for iOS
streaming, so the C6 stops refreshing the display and shows a static screen
instead of fighting the radio stack for bus and CPU time. The C6 acks either
way.

## Events - C6 → P4

| CMD | Name | Payload |
|---|---|---|
| `0x60` | `SET_CONFIG` | `ddp_config_t` (10 B) - legacy fixed snapshot, deprecated |
| `0x61` | `CONFIG_SET` | TLV batch - the preferred key-addressed path |
| `0x62` | `CONFIG_ACTION` | `u8 action_id` - stateless one-shot |

Settings are key-addressed TLVs defined in `daq_config_registry.h`. The P4 is
the authoritative store: the C6 emits `CONFIG_SET`, the P4 applies it and
re-broadcasts via `CONFIG_PUSH`. `CONFIG_ACTION` covers one-shots with no state,
such as energy or charge reset.

## Responses - C6 → P4

| RSP | Name | Payload |
|---|---|---|
| `0x80` | `RSP_OK` | - |
| `0x82` | `RSP_INFO` | `u8 hat_type, fw_major, fw_minor, proto_version` |
| `0xFF` | `RSP_ERR` | `u8 error_code` |

Apart from the `0x60–0x62` events, the C6 only transmits in response to a
command.

## Mainboard tunnel

The C6 has no path to the ESP32-S3, so its Main Board menus tunnel:
**C6 → P4 (`MB_REQUEST`) → S3 (HAT link) → back as `MB_RESPONSE`**. The P4
caches the request; the S3 polls for it *only while the P4 is not streaming to
the PC*, and a deferred request returns status `BUSY`. Requests are issued only
while the relevant menu is open, so the HAT link is idle the rest of the time.

| Req | Name | Purpose |
|---|---|---|
| `0x01` | `MB_POWER` | Read rail setpoints, e-fuse enable/fault, rail enable/power-good |
| `0x02` | `MB_SET_RAIL` | Set VLOGIC / VADJ1 / VADJ2 in mV |
| `0x03` | `MB_SET_EFUSE` | Enable or disable e-fuse 1–4 |
| `0x04` | `MB_SCRIPTS` | List MicroPython scripts and engine status |
| `0x05` | `MB_SCRIPT_RUN` | Run a script by name |
| `0x06` | `MB_SCRIPT_STOP` | Stop the running script |
| `0x07` | `MB_SET_RAIL_EN` | Toggle level-shifter OE, VADJ1_EN, VADJ2_EN |
| `0x08` | `MB_FWINFO` | Installed versions for all four MCUs + GitHub release list |
| `0x09` | `MB_FW_APPLY` | Apply a release by index to a target mask |

### Firmware screen

Only the S3 has a radio, so it is the only chip that can reach GitHub.
`MB_FWINFO` is answered from a **cached** snapshot that the S3 refreshes on a
worker task - an HTTPS release query must never run inside the tunnel poll,
which has to answer within the HAT link timeout.

Releases are identified by **index** into the S3's release list, not by tag
string, because that is what `update_manager_apply_release_index()` consumes.
Tags are display-only. The per-MCU target bits must match `update_target_t` in
`Firmware/ESP32/src/update/update_manager.h`.

## SMU calibration wizard

`CAL_CTRL` / `CAL_STATUS`, C6 ↔ P4 directly with no S3 involvement. The P4 runs
the calibration on a background task (`ESP32P4/src/cal/smu_cal`); the C6 starts
a mode, polls status while the wizard screen is open, acknowledges the operator
prompt, or aborts.

Three modes: **voltage** (sweep V_FB, output disconnected), **current** (sweep
I_FB, output shorted), **baseline** (open-circuit offset per range). Current
calibration needs a USB-PD contract of at least 20 V / 3 A and aborts with a
`NO_PD` flag otherwise.

`ddp_cal_status_t` must stay byte-identical to `smu_cal_status_t`.

## On-device UI

**Main readout** - an animated header plus two hero cards, voltage and current,
with baked JetBrains Mono numerals that autoscale with an SI prefix.

**Settings menu** - a Pebble-style carousel. UP/DOWN move the selection with a
spring snap, OK activates, holding OK goes back, and 30 s of inactivity returns
to the readout.

| Menu | Items |
|---|---|
| HAT Settings | DUT Supply · Autoranging · Range · Super Resolution · Sample Rate · Filter · Decimation · DUT Current Limit · DUT Voltage · FFT (on/length/window/source) · Calibration · Reset Energy · Reset Charge · Factory Reset |
| Screen Settings | Brightness · Dark Mode · LED Mode · LED Color · LED Brightness |
| Main Board Settings | Power (VLOGIC / VADJ1 / VADJ2 / E-Fuse 1–4) · Scripts |
| WiFi Settings | Status and connection control |
| Diagnostics | Read-only sensor rows fed by `SET_DIAGNOSTICS`, with a per-sensor sparkline detail view |
| Firmware | Installed-vs-available table for all four MCUs → release picker → confirm |

The buttons (UP / DOWN / OK-Back) are wired to the **P4** and relayed to the C6
over `BUTTON_EVENT`. Settings persist to C6 NVS so they survive a power cycle
independently of the P4.

With no valid `SET_MEASUREMENT` for 1 s the readout falls back to
`DDP_STATE_SIM`, a local demo generator that sweeps V and I across many decades
to exercise the autoscaler.

## Source map

| File | Role |
|---|---|
| [common/ddp_proto.h](common/ddp_proto.h) | Wire definitions - canonical, shared by both chips |
| `ESP32C6/src/ddp.c` | UART slave: parse, respond, diagnostics cache, config TX |
| `ESP32C6/src/menu.c` | Carousel menu engine, tree, bargraph editor |
| `ESP32C6/src/settings.c` | Settings struct and NVS persistence |
| `ESP32C6/src/ui.c` | Main readout - cards and animated header |
| `ESP32C6/src/gfx.c` | Framebuffer primitives, sprites, fonts |
| `ESP32C6/src/display.c` | esp_lcd ST7789 driver and synchronous flush |
| `ESP32C6/src/buttons.c` | Debounced navigation with repeat and long-press |
| `ESP32C6/src/theme.c` | Runtime light / neon-dark palettes |
