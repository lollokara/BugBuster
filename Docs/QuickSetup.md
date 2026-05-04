# Quick Setup — NVS-backed Preset System

Quick Setup lets you snapshot the entire board state (analog channels, DAC
outputs, IDAC, power rails, GPIO, MUX) into one of four persistent slots and
restore it with a single call. Slots survive firmware resets and power cycles
because they are stored in the ESP32-S3 NVS (Non-Volatile Storage) partition.

---

## Overview

| Property | Value |
|---|---|
| Slot count | 4 (`qs_slot_0` … `qs_slot_3`) |
| Max payload per slot | 1000 bytes (JSON, unformatted) |
| Persistence | NVS — survives power cycle and firmware reset |
| NVS namespace | `quicksetup` |
| Auth required | `X-BugBuster-Admin-Token` header on all mutating endpoints |

A snapshot captures:

- **Analog channels** (0–3) — function, ADC mux/range/rate, DAC code, bipolar flag
- **IDAC** (DS4424, channels 0–2) — raw DAC code (−127 … 127)
- **PCA9535 power rails** — VADJ1, VADJ2, 15 V rail, USB-hub enable, logic-IO enable, e-fuses 1–4
- **GPIO** (12 digital IOs) — mode (disabled / input / output) + output level
- **MUX switch matrix** (ADGS2414D, 8 API devices) — per-device switch state

---

## HTTP API

All mutating endpoints (`POST`, `DELETE`) require the admin token:

```
X-BugBuster-Admin-Token: <token>
```

Retrieve the token over USB with BBP command `0x74 GET_ADMIN_TOKEN` or from
the desktop app's Settings tab.

### GET /api/quicksetup

List all four slots. Returns metadata only — not the full JSON payload.

**Response 200**

```json
{
  "slots": [
    { "index": 0, "occupied": true,  "name": "Current Setup", "ts": 1714900000, "size": 312, "summaryHash": 42 },
    { "index": 1, "occupied": false, "name": "Slot 1",        "ts": 0,          "size": 0,   "summaryHash": 0  },
    { "index": 2, "occupied": false, "name": "Slot 2",        "ts": 0,          "size": 0,   "summaryHash": 0  },
    { "index": 3, "occupied": false, "name": "Slot 3",        "ts": 0,          "size": 0,   "summaryHash": 0  }
  ]
}
```

Fields:

| Field | Type | Description |
|---|---|---|
| `index` | 0–3 | Slot number |
| `occupied` | bool | `true` if a snapshot is stored |
| `name` | string | `"name"` field from the stored JSON (default: `"Current Setup"`) |
| `ts` | uint32 | Unix timestamp (seconds) at save time, or 0 |
| `size` | uint16 | Stored payload size in bytes |
| `summaryHash` | uint8 | FNV-1a hash byte — quick dirty-check |

---

### GET /api/quicksetup/{slot}

Return the full JSON payload for slot `{slot}` (0–3).

**Response 200** — raw JSON blob (Content-Type: `application/json`).

**Response 404** — slot is empty.

**Response 400** — slot number out of range.

---

### POST /api/quicksetup/{slot}

**Snapshot the current board state and store it in `{slot}`.**
No request body is required; the firmware reads live hardware state.

Requires `X-BugBuster-Admin-Token`.

**Response 200** — the newly saved JSON snapshot.

**Response 500** — save failed or snapshot exceeded 1000 bytes.

---

### POST /api/quicksetup/{slot}/apply

**Restore a previously saved slot to hardware.**

Requires `X-BugBuster-Admin-Token`. Returns `409` (with a `failed` array) if
any sub-operation could not be applied; the rest of the state is still applied.

**Response 200**

```json
{ "ok": true, "applied": true }
```

**Response 404** — slot is empty.

**Response 409** — partial failure

```json
{
  "ok": false,
  "applied": false,
  "failed": ["adc_ch2", "gpio3"]
}
```

The `failed` array contains up to 8 component names. The rest of the snapshot
was applied before the failure was detected — check the specific components
listed.

---

### DELETE /api/quicksetup/{slot}

Erase a slot from NVS.

Requires `X-BugBuster-Admin-Token`.

**Response 200**

```json
{ "ok": true, "deleted": true }
```

`"deleted": false` means the slot was already empty (not an error).

---

## BBP Binary Commands

All Quick Setup commands share the `0xF_` opcode range on the BBP v4 wire
protocol (USB CDC #0). See [`Firmware/BugBusterProtocol.md`](../Firmware/BugBusterProtocol.md)
for full frame format.

| Opcode | Name | Direction | Description |
|---|---|---|---|
| `0xF0` | `QS_LIST` | Host → Device | List all 4 slots; returns compact metadata array |
| `0xF1` | `QS_GET` | Host → Device | Get full JSON payload for one slot |
| `0xF2` | `QS_SAVE` | Host → Device | Snapshot current state into slot (1-byte slot payload) |
| `0xF3` | `QS_APPLY` | Host → Device | Apply slot to hardware (1-byte slot payload) |
| `0xF4` | `QS_DELETE` | Host → Device | Delete slot (1-byte slot payload) |

---

## Snapshot JSON Shape

This is the JSON the firmware generates and stores. You do not normally need to
construct it manually — use `POST /api/quicksetup/{slot}` to capture live state.

```json
{
  "ts": 1714900000,
  "name": "Current Setup",
  "analog": {
    "channels": [
      { "fn": 1, "adcMux": 0, "adcRange": 0, "adcRate": 5, "dacCode": 0, "bipolar": false },
      { "fn": 1, "adcMux": 0, "adcRange": 0, "adcRate": 5, "dacCode": 32768, "bipolar": false },
      { "fn": 0, "adcMux": 0, "adcRange": 0, "adcRate": 5, "dacCode": 0, "bipolar": false },
      { "fn": 0, "adcMux": 0, "adcRange": 0, "adcRate": 5, "dacCode": 0, "bipolar": false }
    ]
  },
  "idac": {
    "codes": [0, -12, 0]
  },
  "pca": {
    "vadj1": true,
    "vadj2": false,
    "logic": true,
    "v15": false,
    "usbHub": false,
    "efuse": [true, true, false, false]
  },
  "gpio": [
    { "mode": 0, "value": false },
    { "mode": 2, "value": true },
    { "mode": 1, "value": false }
  ],
  "mux": {
    "devices": [0, 0, 0, 0, 0, 0, 0, 0]
  }
}
```

Key field reference:

| Path | Values |
|---|---|
| `analog.channels[n].fn` | Channel function enum: `0` = high-impedance, `1` = VOUT, `2` = IOUT, `3` = VIN, … |
| `analog.channels[n].dacCode` | 0–65535 (raw 16-bit) |
| `analog.channels[n].bipolar` | `true` = ±12 V VOUT range, `false` = 0–11 V |
| `idac.codes[n]` | −127 … 127 (DS4424 raw code) |
| `pca.efuse` | 4-element bool array — efuse1..4 enable |
| `gpio[n].mode` | `0` = disabled, `1` = input, `2` = output |
| `mux.devices` | 8-element array — ADGS2414D switch-register values |

---

## Desktop UI

The Overview tab shows four Quick Setup tiles (labelled **QS 0** – **QS 3**).
Each tile displays the slot name, save timestamp, and payload size when
occupied.

- **Save** — snapshots current board state into that slot.
- **Apply** — restores that slot to hardware.
- **Delete** — erases the slot.

On firmware older than the Quick Setup feature the tiles fall back gracefully:
the UI catches `404` responses from `/api/quicksetup` and renders the tiles as
disabled without surfacing an error.

---

## Curl Examples

```bash
TOKEN="your-admin-token"
BASE="http://bugbuster.local"

# List all slots
curl "$BASE/api/quicksetup" -H "X-BugBuster-Admin-Token: $TOKEN"

# Save current state to slot 0
curl -X POST "$BASE/api/quicksetup/0" -H "X-BugBuster-Admin-Token: $TOKEN"

# Read slot 0 full JSON
curl "$BASE/api/quicksetup/0" -H "X-BugBuster-Admin-Token: $TOKEN"

# Apply slot 0
curl -X POST "$BASE/api/quicksetup/0/apply" -H "X-BugBuster-Admin-Token: $TOKEN"

# Delete slot 2
curl -X DELETE "$BASE/api/quicksetup/2" -H "X-BugBuster-Admin-Token: $TOKEN"
```
