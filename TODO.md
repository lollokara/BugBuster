# BugBuster TODO

Open work only. Everything delivered has moved to `CHANGELOG.MD` under
`[Unreleased]` - do not re-add completed items here.

Populated by the multi-surface audit of 2026-08-20 and its verification pass.

## How to read this file

**Severity**

| | Meaning |
|---|---|
| P0 | Broken, unsafe, or data-losing. Fix before the next release. |
| P1 | Wrong behaviour or a major capability gap. |
| P2 | Minor inconsistency, missing polish, or a gap with a workaround. |
| P3 | Cosmetic, docs, or nice-to-have. |

**Status** - this matters, do not skip it.

| Tag | Meaning |
|---|---|
| `[VERIFIED]` | Confirmed by reading the cited source directly. Cited lines checked. |
| `[REPORTED]` | Raised by an audit agent, cited but not independently re-checked. **Re-verify before acting.** |
| `[FLASHED]` | Code is on the device but the behaviour has not been provoked and observed. |

**Category** - BUG · PARITY · PROTOCOL · SECURITY · PERF · FEATURE · DOC

## Two rules this backlog exists to enforce

1. **"It builds" is not evidence.** A batch recorded as complete with a clean
   `cargo check` had never been compiled at all. Run the build.
2. **Verifying one instance of a systemic fix proves only that instance.** A
   wrong-HAT fix was confirmed live on one command and turned out to cover 2 of
   15 entry points. Enumerate the class.

## Deriving numbers

Never restate a count, version or opcode from memory, and never derive one by
hand - `grep -c` counts lines, not items.

```bash
python Firmware/tools/check_doc_counts.py --print     # every tracked count
python Firmware/tools/check_proto_version.py          # BBP + DAQ USB + DDP
python Firmware/tools/check_sdkconfig_effective.py    # dead sdkconfig keys
```

---

# Blocked, not deferred

These cannot be progressed on the current bench. Say so rather than letting them
look like neglected work.

| Item | Blocker |
|---|---|
| **RP2040** (RP-1, RP-3, RP-4) | The LA/SWD HAT is **not physically attached**. Cannot flash or test. `fwlab.py` also has no RP2040 target and needs `ota.apply_update(rp2040=True)`. |
| **iOS** (IOS-1 .. IOS-14) | Cannot compile on Windows. Every iOS finding is read-verified only, which is the weakest evidence in this file. |
| **Current-range calibration** | Needs a bench reference meter, and `cal i` writes NVS. Owner decision - never run unasked. |
| **S3-SEC-1 default AP password** | Product decision, see below. |

# Flashed but not proven

Code is on the device. The behaviour has not been provoked.

| Item | What would prove it |
|---|---|
| C6-11 watchdog | Induce a main-loop hang and observe the reboot. |
| C6-9 link-lost banner | Drop the P4 link and confirm the banner replaces demo data. |
| C6-1 DDP `RSP_ERR` | Send a deliberately truncated DDP frame. |
| FEAT-9 MUX rollback | Trigger the interlock (self-test active + U17 S3 write) and confirm the shadow is unchanged. The `[0,0,0,4]` residue is gone, which is not the same thing. |
| DESK-1 STOP on disconnect | Run the desktop app against the device and pull the link. |
| Web UI | Built, but not uploaded to SPIFFS or opened on the device. |

---

# Features

Sequenced by dependency. FEAT-7, FEAT-8, FEAT-9, FEAT-3 and the host half of
FEAT-2 have shipped - see `CHANGELOG.MD`.

### FEAT-1 · web UI DAQ power-analyzer view · **partly present**

A `DAQ` tab exists under `Firmware/ESP32/web/src/tabs/daq/` with DUT supply
control and a calibration banner, and it now compiles. Not yet uploaded to
SPIFFS or exercised on device. Closes WEB-1 and WEB-2 once deployed.

Remaining: live current/voltage read-out, range and sample-rate display, then
control. The desktop DAQ tab is the working reference.

### FEAT-2 · calibration as a first-class concept · **host half done**

`power_analysis.py` now reports `calibrated` / `uncalibrated` / `unknown`, and
the P4 reports the per-range flags. What is left:

- Every surface that displays current must show the uncalibrated state rather
  than silently rendering a plausible number. On this unit that number is out by
  about -653 uA.
- A calibration workflow the owner can actually run.

### FEAT-4 · extend the `Transport` protocol type

The protocol landed in batch 1 and retired the `script_delete()` class of bug.
Extend it so every **USB-only** method is *declared* rather than discovered at
runtime, so a missing HTTP implementation fails at type-check instead of on the
bench.

### FEAT-5 · capture provenance records

Stamp every capture with the exact filter, decimation, range, SR state and
calibration status it was taken under. Until this exists, A/B comparison across a
settings change is unsound and captures are not reproducible. Depends on FEAT-2.
Same underlying gap as P4-6.

### FEAT-6 · generated error tables

One error table generated from `bbp.h` into Python and Rust rather than
hand-copied into both. Hand-copying is what produced the 0x13 collision, where a
rejected MUX route was reported to the user as a calibration failure.
`test_error_code_parity.py` catches divergence today; generating the table
removes the possibility.

---

# Surface: Protocol

Full report: `scratch/audit/PROTO.md`

### PROTO-4 · P1 · PARITY · `[REPORTED]` - Rust client missing 11 commands

Reported missing from `bbp.rs`: scripting `0xF5-0xFD` (8 commands), DSP stream
`0x64-0x65`, `0xEF WIFI_SET_AP_PASSWORD`, `0x47 ADC_LEDS_SET_MODE`. This is the
protocol-level root of DESK-3 and DESK-9. Re-verify each ID before implementing.

### PROTO-6 · P2 · BUG · `[REPORTED]` - `BBP_MAX_PAYLOAD` not enforced when building responses

Validated on receive but reportedly not at every response-construction site. An
over-long response is a firmware-side overflow, so worth confirming.

### PROTO-7 · P2 · PROTOCOL · `[REPORTED]` - WAVE_I decimation has no anti-alias filter

Decimation greater than 1 reportedly drops samples without filtering, so anything
above Nyquist folds back into the displayed waveform. On a measurement instrument
that produces plausible-looking wrong data.

### PROTO-8 · P3 · DOC · `[REPORTED]` - stale `ESP32_FW_VERSION` in `protocol.py`

Reported as 3.4.0 against a real firmware of 5.1.0, and reported as dead code. If
it is dead, delete it rather than updating it.

---

# Surface: ESP32-S3 mainboard firmware

Full report: `scratch/audit/S3.md`

### S3-SEC-1 · P1 · SECURITY · `[VERIFIED]` - needs an owner decision

Every device ships with the **same** softAP password, a compile-time constant:
`#define WIFI_PASSWORD "bugbuster123"` at `Firmware/ESP32/src/config.h:111`. An
NVS override is only written if `wifi_set_ap_password()` is ever called, so a
fresh unit keeps the default. The AP is always WPA2-PSK and the SSID is
`BugBuster`.

Anyone in radio range of an untouched device can drive the instrument.
Deliberately not changed: altering AP auth on the bench unit risks locking the
owner out, and the right answer (random per-device at first boot, printed or
readable over USB) is a product decision. **Settle this before any public
release.**

### S3-6 · P2 · SECURITY · `[REPORTED]` - no rate limiting on HTTP or BBP

No throttling on either control plane. On a device intentionally exposed on a lab
network this is a denial-of-service surface, and it also means a runaway client
script can wedge the instrument.

### MUX-4 · P2 · BUG · `[VERIFIED]` - two callers log a refused MUX write but do not propagate it

`adgs_set_api_all_safe()` returns `false` when the U17-S3 / U23 interlock refuses
a write, and the BBP and HTTP handlers now surface that as `BUSY` / 409. Two
callers still only log:

- `Firmware/ESP32/src/bus/bus_planner.cpp:224` - a refused write means the
  routing plan did not apply, so signals are **not** where the caller believes,
  and `plan_i2c_bus` / `plan_spi_bus` still report success.
- `Firmware/ESP32/src/web/quicksetup.cpp:499` - the quick-setup MUX state is not
  applied, but the slot still reports as applied.

Both were deliberately left as `ESP_LOGE` when the return value was introduced,
on the grounds that making the failure visible beat silently continuing. That was
the right first step and is not the finish: a caller acting on a routing plan
that never reached the hardware is the same class of defect as the original
silent success. Propagate the failure out of both flows.

---

# Surface: RP2040 LA / SWD HAT firmware

**Blocked: the HAT is not attached.** Full report: `scratch/audit/RP2040.md`

The healthiest surface in the audit. All six AGENTS.md non-negotiables that
concern this firmware were verified as correctly implemented.

### RP-1 · P2 · BUG · `[REPORTED]` - level shifter OE and DIR set in the wrong order

Direction is reportedly changed while the output is still enabled, producing a
brief contention glitch on the shifted IO bank. The code fix is written and the
UF2 builds; proving it needs a scope on the shifted bank.

### RP-3 · P2 · BUG · `[REPORTED]` - rail voltage clamping not confirmed

The ADC conversion formula was verified correct, but whether a requested rail
voltage is clamped to a safe range was not. This is a hardware-damage path, so it
deserves an explicit check rather than an assumption.

### RP-4 · P3 · BUG · `[REPORTED]` - spinlock leaked on re-init

Unlikely in practice, but it makes repeated re-init unsafe.

---

# Surface: DAQ HAT - ESP32-P4 firmware

Full report: `scratch/audit/P4.md`

### P4-1 · P0 · BUG · partly addressed - calibration itself is outstanding

The firmware now reports per-range calibration validity honestly
(`cal_have_hi/mid/lo`, all false on this unit) and `power_analysis.py` tags the
results. What remains is calibrating the unit: measured zero-current offset on
the HI/51 ohm range is about **-653 uA** and drifts between runs. Voltage is
excellent by contrast (gain 0.9999, -20 mV offset).

Blocked on a bench reference. `cal i` writes NVS - do not run unasked.

### P4-3 · P1 · BUG · `[REPORTED]` - OTA product-ID check runs after `esp_ota_begin()`

A wrong-target image is written into the staging partition before it is rejected,
leaving the partition polluted. Validate the header first.

### P4-4 · P1 · BUG · `[REPORTED]` - Sinc5 x8 / x16 leaves the ADAQ in 16-bit mode

Subsequent 24-bit reads are misframed. This has bitten the project before during
the Super Resolution work.

### P4-6 · P2 · FEATURE · `[REPORTED]` - no atomic settings snapshot per capture

The host cannot reconstruct the exact filter, decimation and SR state a capture
was taken under. See FEAT-5.

### P4-7 · P2 · BUG · `[REPORTED]` - DUT supply comes back OFF after OTA with no NVS restore

Matches observed behaviour. Either restore it or report clearly that the supply
was dropped - silently powering down a DUT mid-experiment is a data-loss event.

### P4-8 · P3 · BUG · `[REPORTED]` - COARSE/FINE drop counters saturate at uint16

At 100 drops/s the counter wraps in about 11 minutes and then reads as healthy.

### P4-9 · P3 · PERF · `[REPORTED]` - SMU voltage ramp blocks the control plane

A full 127-code span reportedly blocks for about 1.27 s.

---

# Surface: DAQ HAT - ESP32-C6 display firmware

Full report: `scratch/audit/C6.md`

### C6-5 · P1 · FEATURE · `[REPORTED]` - WiFi streaming flag set but ESP-Hosted path untested

This is what blocks DAQ streaming to iOS over WiFi. Either finish it or stop
advertising the flag.

### C6-6 · P1 · PARITY · `[REPORTED]` - no C6-to-registry parity test

The P4, S3 and desktop all have DDP regression coverage; the C6 has none, so
nothing catches drift between the C6 menu and the config registry.

### C6-7 · P2 · UX · `[REPORTED]` - no confirmation on destructive front-panel actions

E-fuse and rail-enable toggles fire on a single press. The front panel is the one
surface where an accidental press is most likely.

### C6-10 · P2 · BUG · `[REPORTED]` - "Factory Reset" resets the DAQ registry only

C6 NVS is untouched, so the label overpromises.

---

# Surface: Desktop app (Tauri + Leptos)

Full report: `scratch/audit/DESKTOP.md`

Rust hygiene was clean: no `unsafe`, `unwrap`/`panic` confined to tests, correct
`tokio::Mutex` versus `std::Mutex` usage, bounds-checked slicing.

### DESK-3 · P2 · PARITY · `[REPORTED]` - no MicroPython scripting UI

Python, MCP and the web UI all have a REPL and script management. The desktop app
has none, because `bbp.rs` lacks commands `0xF5-0xFD` (see PROTO-4). Largest
single desktop gap.

### DESK-4 · P2 · PARITY · `[REPORTED]` - no external I2C / SPI bus UI

The BBP commands exist and Python and MCP both expose scan and transfer.

### DESK-5 · P2 · PARITY · `[REPORTED]` - cannot upload P4 or C6 firmware

Python and MCP can. The desktop app is the surface a user is most likely to have
open when they want to update a HAT.

### DESK-6 · P2 · BUG · `[REPORTED]` - IO ownership keep-alive race on tab switch

A small window between releasing on one tab and claiming on the next.

### DESK-9 · P2 · PARITY · `[REPORTED]` - cannot set the WiFi AP password

`0xEF` is missing from `bbp.rs`. See PROTO-4.

---

# Surface: iOS app

**Blocked: cannot build on Windows.** Every item below is read-verified only.
Full report: `scratch/audit/IOS.md`

The web UI and iOS are both pure HTTP clients, so IOS-5 is the cheapest parity
gap in the project to close - the API surface is already proven and the work is
view-layer only. Treat the web UI as the reference implementation.

| ID | Sev | Title |
|---|---|---|
| IOS-1 | P1 | Admin token stored in `UserDefaults`, an unencrypted plist. Belongs in the Keychain. |
| IOS-2 | P1 | Force unwraps on network-derived data (network port, scope `displayPoints`, `muxStates` when `lastStatus` is nil). |
| IOS-3 | P1 | DAQ stream `.failed` state has no recovery path; requires an app restart. |
| IOS-4 | P1 | BLE polling never cancelled on disconnect. |
| IOS-5 | P1 | Roughly half the web UI's route coverage (55 of ~122). Missing: faults, digital IO, UART bridge, external bus, waveform generator, manual OTA, WiFi config, IO ownership, ADC DSP, advanced HAT controls. |
| IOS-6 | P2 | ~70 `try?` sites swallow errors silently; the app shows stale data with no indication anything failed. |
| IOS-7 | P2 | Optimistic updates never roll back on POST failure. On a hardware control app a user can believe a rail is off when it is on. |
| IOS-8 | P2 | Circuit breaker never resets after an SSE timeout. |
| IOS-9 | P2 | WebSocket REPL never sends a close frame, leaking a device socket slot. |
| IOS-10 | P2 | Unbounded DAQ stream buffers. |
| IOS-11 | P2 | No confirmation on destructive actions (rails, OTA, reset). |
| IOS-12 | P2 | VDUT control shown but not implemented in firmware. Remove it or implement the route. |
| IOS-13 | P3 | Deployment target is iOS 26.0. Valid (Apple's year-based versioning), but a 26.0 *minimum* excludes every device that has not updated. Decide deliberately. |
| IOS-14 | P3 | No ATS local-networking exception declared; direct-IP connections may be refused. |

---

# Surface: On-device web UI

Full report: `scratch/audit/WEB.md`

Security came back clean: no XSS via `innerHTML`, the admin token is sent as a
header rather than a query string, and token storage defaults to `sessionStorage`.

### WEB-3 · P2 · PARITY · `[REPORTED]` - no external I2C / SPI bus UI

Eight routes unused. Same gap as DESK-4.

### WEB-4 · P2 · PARITY · `[REPORTED]` - no P4 or C6 firmware upload

The web UI is the only surface that can reach these routes without installing
anything, which makes it the natural home for HAT firmware updates.

### WEB-10 · P3 · PARITY · `[REPORTED]` - IO ownership and fault masking not exposed

---

# Surface: Python library

### PY-4 · P2 · PARITY · `[REPORTED]` - `0x47 ADC_LEDS_SET_MODE` has no binding

The same agent's claims that `0x0A WIFI_FORGET` and `0x0B SELFTEST_WORKER` are
unbound were **refuted** by the protocol audit. Verify before implementing.

### PY-5 · P2 · PROTOCOL · `[REPORTED]` - no version handshake on the DAQ USB stream

`daq_stream.py` hardcodes a proto version and never checks it against the device.
The four copies are now held in lockstep by CI, which stops them *drifting*, but
a host still cannot detect a device running a different build.

### PY-10 · P3 · DOC · `[REPORTED]` - `idac_cal_clear()` name implies an NVS wipe but is RAM-only

Confirmed RAM-only. The name has already caused one scare.

---

# Surface: MCP server

Full report: `scratch/audit/MCP.md`

### MCP-4 · P1 · PARITY · `[REPORTED]` - no realtime streaming tools

Roughly 6 library streaming methods have no MCP equivalent. Agents can take
snapshots and captures but cannot observe a live signal.

### MCP-5 · P2 · PARITY · partly closed - scripting is the remaining gap

Quick Setup and WiFi join are done (see `CHANGELOG.MD`). Still outstanding:
**scripting file management** - 7 library methods against 1 tool
(`run_device_script`), so an agent cannot manage scripts on the device. Also
alerts and diagnostics.

### MCP-7 · P2 · DOC · `[REPORTED]` - docstrings omit units and preconditions

Missing across many tools: units (V, mA, Hz, samples), returned dict keys, and
preconditions such as "call `configure_io` first", "requires HAT", "USB-only".
For an MCP server the docstring **is** the API, so this is a functional defect,
not a documentation nicety.

### MCP-9 · P3 · DOC · `[VERIFIED]` - wrong VLOGIC comment in `config.py`

`# VLOGIC limits (TPS74601)` - VLOGIC is trimmed by DS4424 OUT0 into the LTM8078
Out2 feedback node. The TPS74601 produces `3V3_ADJ` for the AD74416H.

---

# Documentation

Counts and protocol versions are gated by CI now, so this section covers only
prose that no script can check.

### DOC-1 · P2 · remaining stale docs

- `Docs/power-analyzer.md` reportedly claims a 250 kSPS maximum against a code
  path that reaches 1 MSPS. Verify against source before changing.
- `Firmware/DAQ_HAT/display-protocol.md` is reportedly missing the CAL commands
  and still documents `DDP_CMD_SET_STATUS` as handled when it falls through to
  `RSP_ERR`.
- `.mex/manifests/esp32-mainboard.manifest.md` may still carry the old HTTP route
  count in its body.

---

# Parity matrix

Which control surface can reach which capability. Cells marked `?` were not
conclusively established and need a check before anyone relies on them.

| Capability | Python | MCP | Desktop | Web UI | iOS | C6 panel |
|---|---|---|---|---|---|---|
| ADC / DAC channels | yes | yes | yes | yes | yes | partial |
| Digital IO | yes | yes | yes | yes | no | no |
| Power rails / e-fuse | yes | yes | yes | yes | partial | yes |
| USB-PD | yes | yes | yes | yes | ? | no |
| UART bridge | yes | yes | yes | yes | no | no |
| SWD / CMSIS-DAP | yes | yes | yes | no | no | no |
| Logic analyzer | yes (USB) | yes (USB) | yes (USB) | no | no | no |
| DAQ power analyzer | yes | yes | yes | **built, not deployed** | partial | yes |
| MicroPython scripting | yes | 1 tool only | **no** | yes | partial | ? |
| Quick setup | yes | yes | ? | ? | ? | no |
| OTA - S3 / RP2040 / SPIFFS | yes | yes | yes | yes | yes | no |
| OTA - P4 / C6 | yes (HTTP) | yes (HTTP) | **no** | **no** | no | yes |
| WiFi STA join | yes | yes | ? | yes | ? | no |
| WiFi AP password | yes | yes | **no** | yes | ? | no |
| Calibration | yes | yes | yes | yes | partial | yes |
| External I2C / SPI bus | yes | yes (9 tools) | **no** | **no** | no | no |
| IO ownership | yes | yes (4 tools) | yes | **no** | no | no |
| Memory / diagnostics | yes | yes | yes | yes | partial | yes |
| Self-test | yes | yes | yes | yes | ? | no |

**Structural reading**

- The **desktop app is the weakest surface for scripting and the external bus**,
  both fully wired in Python and MCP, and both blocked on PROTO-4.
- **iOS is roughly half the web UI** by route coverage and is the cheapest parity
  gap to close, since both are pure HTTP clients.
- The **web UI DAQ gap is closing** but is not closed until the bundle is
  deployed.

---

# Fixture: `tests/tools/fwlab.py`

Flashing is the expensive step, so the before/after ritual is automated:

```bash
# one shot: snapshot, build, flash, re-snapshot, diff, smoke
python tests/tools/fwlab.py --host <ip> --token <tok> cycle --target p4 --smoke daq

# or step by step
python tests/tools/fwlab.py --host <ip> snapshot --tag pre
python tests/tools/fwlab.py diff pre post
```

- **Calibration guard.** Every snapshot fingerprints the IDAC/HAT/DAQ calibration
  state including the polynomial coefficients. Any change is a hard failure, and
  `cycle` refuses to flash if it cannot read the fingerprint first. Nothing in
  the fixture ever writes calibration.
- **Numeric tolerance.** Fields differing by less than 2 percent are treated as
  measurement noise, so ADC jitter does not bury a real regression.
- **Stops on regression.** A failed diff aborts before smoke tests run.
- **Missing:** an RP2040 target (`ota.apply_update(rp2040=True)`).

Flash routes, none needing physical access: S3 and SPIFFS by HTTP OTA; C6 by HTTP
OTA via the S3 relay (`/api/ota/upload_c6`, or `tests/tools/daq_push.py c6`); P4
by PIO on COM15 or HTTP OTA; RP2040 by BBP OTA from the S3.

---

Full per-surface reports with evidence live in `scratch/audit/`: `PROTO.md`,
`S3.md`, `RP2040.md`, `P4.md`, `C6.md`, `DESKTOP.md`, `IOS.md`, `WEB.md` and
`MCP.md`, plus the verification pass in `C6-VERIFY.md`,
`RP2040-RLE-VERIFY.md` and `DESKTOP-WEB-VERIFY.md`. Treat them as evidence
appendices, not as a backlog.
