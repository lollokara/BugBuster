# IO Ownership

**BBP protocol version 5** introduces a firmware-resident single-owner table that
covers all 16 controllable IO resources (IO1..IO12 + analog channels CH0..CH3).
At any instant, each slot has either zero or exactly one owner. A write from a
non-owner is rejected with a clear error code rather than silently clobbered.

This document is the canonical reference for the IO ownership system delivered in
the 2026-05-12 landing (PR-5). For historical context on why this was needed see
the design plan at `~/.claude/plans/sky-wizard-fox.md`.

---

## Slot Index Map

| Slot index | Resource |
|---:|---|
| 0 | IO1 |
| 1 | IO2 |
| 2 | IO3 |
| 3 | IO4 |
| 4 | IO5 |
| 5 | IO6 |
| 6 | IO7 |
| 7 | IO8 |
| 8 | IO9 |
| 9 | IO10 |
| 10 | IO11 |
| 11 | IO12 |
| 12 | CH0 (analog, maps to IO3) |
| 13 | CH1 (analog, maps to IO6) |
| 14 | CH2 (analog, maps to IO9) |
| 15 | CH3 (analog, maps to IO12) |

Analog channels CH0..CH3 are independently ownable. Claiming CHn also implicitly
claims the underlying IO (IO3/IO6/IO9/IO12 respectively). Releasing CHn releases
the underlying IO only if no separate IO claim is held for that slot.

---

## Owner Kinds

Five owner kinds are defined in `Firmware/ESP32/src/io_owner.h`:

| Kind | Value | Identity |
|---|---:|---|
| `IO_OWNER_NONE` | 0 | Unowned |
| `IO_OWNER_USB` | 1 | BBP over CDC0 |
| `IO_OWNER_HTTP` | 2 | HTTP REST — keyed by admin-token fingerprint |
| `IO_OWNER_SCRIPT` | 3 | On-device MicroPython VM |
| `IO_OWNER_CLI` | 4 | On-device serial CLI |
| `IO_OWNER_INTERNAL` | 5 | Selftest / boot / autocal — pre-empts any user owner |

---

## Lease Lifecycle

**Claim** — a caller acquires one or more slots by sending `CMD_IO_CLAIM` (BBP),
`POST /api/io/owner` (HTTP), calling `bb.io_claim(...)` (Python), or entering
`bugbuster.claim(...)` (MicroPython). If a slot is already held by a different
owner the request is rejected with `IO_HELD_BY_OTHER`.

**Renew** — a caller re-issues the same claim on a slot it already owns. The
`lease_until_ms` timestamp is extended and the existing claim is preserved. No
error is returned.

**Release** — a caller issues `CMD_IO_RELEASE` / `DELETE /api/io/owner` / exits
the `io_claim` context manager. Passing `n_slots = 0` (BBP) or an empty slot
list releases all slots owned by that caller.

**Expire** — when a non-zero lease duration was specified, `io_owner_tick()` (called
from the firmware main loop) automatically releases slots whose `lease_until_ms`
has passed. The default lease for most callers is 30 seconds; the desktop app
renews on a 2-second timer while the tab is visible, guaranteeing slots free
within a few seconds of a desktop crash.

**Infinite lease** — pass `lease_ms = 0` to hold a slot until explicitly released.
Use with care; a crashed client holding an infinite lease requires an admin
force-release to recover.

---

## BBP Wire Protocol (v5)

`PROTO_VERSION` is bumped from **4 to 5** across all three canonical files:
`Firmware/ESP32/src/bbp/bbp.h`, `python/bugbuster/protocol.py`, and
`DesktopApp/BugBuster/src-tauri/src/bbp.rs`. The lockstep is enforced by the
`proto-version-check.yml` CI gate.

### New Commands

#### `CMD_IO_CLAIM` — `0xA7`

Request ownership of one or more slots.

Payload:

| Field | Type | Description |
|---|---|---|
| `n_slots` | `u8` | Number of slot indices that follow |
| `slots[n_slots]` | `u8[]` | Slot indices (0..15) |
| `lease_ms` | `u32` | Lease duration in ms; 0 = infinite |
| `purpose_tag` | `u32` | FNV-1a hash of caller-supplied label (0 = anonymous) |

Response: `status(u8)` + per-slot status byte array of length `n_slots` (total frame payload = 1 + `n_slots` bytes).

#### `CMD_IO_RELEASE` — `0xA8`

Release one or more slots. Passing `n_slots = 0` releases all slots owned by
the calling session.

Payload:

| Field | Type | Description |
|---|---|---|
| `n_slots` | `u8` | Number of slot indices that follow (0 = release all) |
| `slots[n_slots]` | `u8[]` | Slot indices |

Response: `status(u8)`.

#### `CMD_IO_OWNER_STATUS` — `0xA9`

Query the full ownership table. No payload.

Response: `status(u8)` + 16 records × 10 bytes = 161 bytes total frame payload.

Each record (10 bytes, all multi-byte fields little-endian):

| Field | Type | Bytes |
|---|---|---:|
| `kind` | `u8` | 1 |
| `session_id` | `u8` | 1 |
| `token_fp32` | `u32 LE` | 4 |
| `lease_until_ms` | `u32 LE` (low 32 bits of monotonic time) | 4 |

#### `CMD_IO_FORCE_RELEASE` — `0xAA`

Admin-token-gated unconditional release. Intended as a "get unstuck" escape
hatch, not a normal flow.

Payload:

| Field | Type | Description |
|---|---|---|
| `slot` | `u8` | Slot index (0..15); `0xFF` = release all slots |

Response: `status(u8)`.

### Status Bytes

| Value | Name | Meaning |
|---:|---|---|
| `0x00` | `IO_OK` | Success |
| `0x01` | `IO_HELD_BY_OTHER` | Slot already owned; next byte is current owner kind |
| `0x02` | `IO_NOT_OWNED` | Release path — caller does not own this slot |
| `0x03` | `IO_INVALID_SLOT` | Slot index out of range |
| `0x04` | `IO_LEASE_EXPIRED` | Read-only diagnostic; never returned by claim |
| `0x05` | `IO_ADMIN_REQUIRED` | Force-release attempted without admin token |
| `0x12` | `IO_OWNERSHIP_REQUIRED` | Write rejected; caller must claim the slot first (v5 clients only) |

---

## HTTP Endpoints

All endpoints are admin-token-gated for mutating operations (same
`X-BugBuster-Admin-Token` header used elsewhere in the API).

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/io/owner` | Returns the full 16-slot ownership table as JSON |
| `POST` | `/api/io/owner` | Claim one or more slots |
| `DELETE` | `/api/io/owner` | Release one or more slots (or all owned by this token) |
| `POST` | `/api/io/owner/force` | Admin-only force-release of a specific slot or all slots |

`POST /api/io/owner` request body:

```json
{
  "slots": [3, 6],
  "leaseMs": 30000,
  "purpose": "adc-sweep"
}
```

`GET /api/io/owner` response:

```json
{
  "slots": [
    {"index": 0, "kind": 0, "kindName": "none", "sessionId": 0, "leaseUntilMs": 0},
    {"index": 3, "kind": 2, "kindName": "http", "sessionId": 1, "leaseUntilMs": 1747008000000}
  ]
}
```

---

## Python Library

### Context Manager (recommended)

```python
import bugbuster as bb

with bb.connect_usb("/dev/cu.usbmodem1234561") as dev:
    with dev.io_claim([3, 6], purpose="adc-sweep"):
        # slots 3 and 6 are yours for the duration of this block
        dev.hal.configure_io(3, "adc")
        value = dev.hal.read_adc(3)
    # slots released automatically on exit
```

The `io_claim` context manager accepts an optional `lease_ms` parameter
(default `0` = infinite while the `with` block is open).

### Single-shot calls (legacy style)

If a caller does not enter an `io_claim` context before mutating an IO, the
Python library auto-wraps the call in a single-shot claim/release pair. This
mirrors the firmware's v4 backward-compat path, so single-shot scripts remain
ergonomic without explicit ownership boilerplate.

### Status query

```python
table = dev.io_owner_status()
for slot in table:
    if slot["kind"] != 0:
        print(f"IO{slot['index']+1}: owned by {slot['kindName']}")
```

---

## MCP Tools

The MCP server exposes 4 new IO-ownership tools (total MCP tool count: **59**
after this landing):

| Tool | Description |
|---|---|
| `io_claim` | Claim one or more slots; returns a lease handle |
| `io_release` | Release a lease handle (or specific slots) |
| `io_owner_status` | Return the full 16-slot ownership table |
| `io_force_release` | Admin force-release a slot (requires `admin_token`) |

All existing IO-mutating tools (`configure_io`, `set_voltage`, etc.) accept an
optional `lease_handle: str` parameter. When provided the tool reuses the
existing lease. When absent the tool auto-claims for the duration of the call,
preserving the single-shot ergonomics that existing agent workflows rely on.

Example agent workflow:

```python
handle = mcp.io_claim(slots=[3, 6, 12], lease_seconds=60, purpose="sweep")
try:
    mcp.configure_io(3, mode="adc", lease_handle=handle)
    mcp.set_voltage(6, 3.3, lease_handle=handle)
    mcp.read_adc(12, lease_handle=handle)
finally:
    mcp.io_release(handle)
```

---

## MicroPython (on-device)

```python
import bugbuster

with bugbuster.claim([3, 6], purpose="my-script"):
    ch = bugbuster.Channel(0)
    ch.set_function(bugbuster.FUNC_VOUT)
    ch.set_voltage(3.3)
    print(ch.read_voltage())
# slots released on __exit__
```

`bugbuster.claim()` accepts `slots` (list of 0-based slot indices), an optional
`purpose` string, and an optional `lease_ms` (default `0` = infinite while the
context is open). The MicroPython binding bridge fills in the `IO_OWNER_SCRIPT`
identity automatically from the MP task context.

---

## Backward Compatibility

BBP v4 clients keep working without any changes. When the firmware's version
negotiation detects a v4 handshake it grants every IO-mutating write an implicit
30-second auto-claim-and-release with `purpose_tag = 0`. The client cannot hold
slots explicitly, but will never receive an `IO_OWNERSHIP_REQUIRED` rejection.

HTTP clients that never call `POST /api/io/owner` receive the same implicit
30-second auto-claim per mutating request, keyed on the admin-token fingerprint.

The new `IO_OWNERSHIP_REQUIRED` error (`0x06`) is only returned to callers that
negotiated version 5 or later.

---

## Preemption by Internal Services

Callers with `IO_OWNER_INTERNAL` (selftest, boot autocal, e-fuse fault handler)
can displace any user owner unconditionally:

1. The displaced owner's identity is logged and stored.
2. `IO_OWNER_INTERNAL` holds the affected slots until the internal operation
   completes.
3. On completion, prior ownership is restored **only if the displaced session
   is still alive and the original lease has not expired**. Otherwise the slots
   end up `IO_OWNER_NONE`.

When preemption occurs the firmware emits `EVT_IO_PREEMPTED` as an unsolicited
BBP event. The desktop app, ESP32 web UI, and MCP runtime each surface this as a
banner or exception so the user knows why their slots were taken.

---

## Force-Release

`CMD_IO_FORCE_RELEASE` (`0xAA`) and `POST /api/io/owner/force` are admin-only
operations. They exist as a "get unstuck" escape hatch for situations where a
client crashed while holding an infinite lease or where a MicroPython script
terminated abnormally without releasing its slots.

Force-release is not intended for use in normal automation flows. Prefer explicit
`io_release` calls or finite lease durations that will expire automatically.

---

## Analog MUX Mutual Exclusion

The IO ownership table prevents two software clients from competing for the same
IO slot. A complementary hardware-level invariant prevents two analog routes from
physically shorting signals together regardless of which client holds ownership.

### Rule

The ADGS analog MUX chain enforces **at most one switch closed per ADGS device**
across the four devices in the chain (device indices 0..3). Device index 3 is the
U23 self-test switch device and is exempt from this constraint — the selftest
path may close its own switches independently while user routes are active on
devices 0..2.

### Default behaviour — auto-open-previous

When a caller requests a new analog route on a device that already has an active
switch, the driver automatically opens the previous switch before closing the new
one. This transition is logged at debug level:

```
[adgs] device 1: auto-opening switch 4 before closing switch 7
```

No error is returned. The previous route is silently displaced, which is the
correct behaviour for the common case of sequential single-channel measurements.

### Strict mode

An optional strict mode can be enabled per-call or globally. In strict mode,
requesting a new switch on a device that already has one active returns BBP error
code `ADGS_ROUTE_REJECTED = 0x13` instead of auto-opening the previous switch.
Use strict mode in scripts that must detect accidental route conflicts rather than
silently clobber them.

### Diagnostics endpoint

`GET /api/adgs/routes` returns the live active-switch map for all four ADGS
devices as a JSON object:

```json
{
  "devices": [
    {"index": 0, "activeSwitch": 7,  "exempt": false},
    {"index": 1, "activeSwitch": null, "exempt": false},
    {"index": 2, "activeSwitch": null, "exempt": false},
    {"index": 3, "activeSwitch": 2,  "exempt": true}
  ]
}
```

`activeSwitch` is `null` when no switch is closed on that device. This endpoint
is read-only and does not require an admin token.

### Relationship to the ownership table

These two protection layers are independent and complementary:

| Layer | What it prevents | Enforcement point |
|---|---|---|
| IO ownership table | Two software clients writing the same IO simultaneously | `io_owner_check()` at every write-path entry |
| ADGS MUX rule | Two analog routes shorting signals on the same MUX device | ADGS driver, before every switch-close call |

A caller can hold ownership of IO3 and IO6 simultaneously and still trigger the
MUX rule if it tries to close two switches on the same ADGS device. Conversely,
a misconfigured caller that bypasses the ownership table (e.g. a v4 client with
auto-claim) still cannot create a physical short because the MUX rule operates at
the driver level unconditionally.

---

## Non-Goals

The following are explicitly out of scope:

- **Per-bit ownership** inside a single IO — ownership is whole-IO only.
- **Multi-writer / cooperative editing** — single-owner only.
- **Cross-device coordination** — one ESP32 is the ownership boundary.

---

## New Module

The ownership table is implemented in `Firmware/ESP32/src/io_owner.{h,cpp}`.
It is protected by a `portMUX_TYPE` spinlock and all public functions are
thread-safe. `io_owner_tick(now_ms)` must be called from the firmware main loop
to expire stale leases.
