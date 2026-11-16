# BugBuster TODO

Open work only. Everything delivered has moved to `CHANGELOG.MD` under
`[Unreleased]` - do not re-add completed items here.

## System standby (2026-10-05/06)

Bench-verified on the DAQ HAT (10+ sleep/wake cycles, auto idle sleep) and on the LA HAT
(2026-10-12); details in `CHANGELOG.MD`. Still open:

- Power-cycle confirmation of the AD74416H boot retry.
- Cause of the intermittent ADAQ `reg 0x21` CRC read failures after sleep loops (diagnostics are in).
- Physical check of panel/backlight/LED darkness; held-button wake (VDUT must not toggle).
- Input-power and current measurement asleep vs awake.
- LA HAT inhibitors (script, OTA, LA stream, DAP); degraded / no-Wi-Fi boot.
- A real WebView/CDP run of the desktop standby card.

Populated by the multi-surface audit of 2026-08-20 and its verification pass,
then extended by the **per-feature audit of 2026-10-01** (every device feature
traced end to end and given a Leave / Improve / Refactor verdict). The
2026-10-01 work is the first part of this file: the execution plan, the
verdict matrix, and the new findings. Evidence lives in
`scratch/audit-2026-10/*.md` (one report per area, every finding cites
file:line). The 2026-08-20 sections further down are kept, with stale items
corrected in place.

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

# 2026-10-01 per-feature audit - START HERE

## Method and evidence level

Twelve read-only audits, one per feature area, each written to
`scratch/audit-2026-10/<AREA>.md`:

| Report | Covers | ID prefix |
|---|---|---|
| `ANALOG.md` | AD74416H channels, read/write V/I/R, DAC, wavegen, ADC stream/scope/DSP, IDAC, AVDD, ADC LEDs | `AN-` |
| `IO-MUX.md` | ADGS MUX, digital IO, configure_io routing, IO ownership, faults, self-test, board profiles | `IO-` |
| `POWER.md` | PCA9535, e-fuses, VADJ/VLOGIC, target power-up, USB-PD, HAT rails, telemetry | `PWR-` |
| `COMMS-BUS.md` | UART bridge, external I2C/SPI, bus planner, deferred jobs, register_access | `BUS-` |
| `RP2040-LA-SWD.md` | LA capture/stream/RLE/trigger, SWD, S3 HAT bridge, HAT IO/cal/OTA | `LA-` |
| `DAQ-P4.md` | ADAQ7769 acquisition, ranging, SMU, USB-HS stream, triggers, registry, cal, host decoders | `DAQ-` |
| `DAQ-C6.md` | C6 display/menus, DDP, C6 WiFi, P4/C6 OTA relay, NVS/WDT | `C6-2x/3x/4x` |
| `TRANSPORT.md` | BBP framing, CDC, HTTP/SSE/WS structure, Python + Rust transports, error tables, latency | `TR-` |
| `PLATFORM.md` | OTA (S3/SPIFFS/RP2040), WiFi, auth, Quick Setup, MicroPython, diagnostics, task layout | `PLT-` |
| `CLIENTS-WEB-IOS.md` | Web UI and iOS, full HTTP route-usage matrix, key mismatches | `WEB-2x`, `IOS-2x` |
| `DESKTOP.md` | Every desktop tab, Tauri commands, transports, polling, rendering | `DESK-2x/3x` |
| `MCP-PY-TESTS.md` | Python lib structure, MCP design + tool-to-client signature check, test coverage | `PY-2x`, `MCP-2x`, `TEST-` |

The same defect was often found by two or three audits from different
surfaces. Each is listed **once** below under a canonical ID, with its aliases.

**Tags in this section.** `[VERIFIED]` = the cited lines were re-read by the
lead auditor on 2026-10-01 and the claim holds. `[REPORTED]` = cited by an
audit agent, not independently re-read: **open the report and re-read the
lines before changing code** (agents were wrong twice in the 2026-08-20 audit).
`[BENCH]` = code reading is conclusive about the mechanism but the effect needs
one read-only check on hardware. Line numbers drift - another agent was editing
`client.py` and `webserver.cpp` during the audit; grep the quoted symbol.

Nothing in this audit was built, flashed, or run on hardware.

## Audit remediation status (M1-M10): what is not yet proven

Rows proven on hardware were removed (see `CHANGELOG.MD` `[3.0.0]`). What remains is fixed in
source but not provoked on a board, or still open. The per-item text for fixed items was
removed from the wave sections below; `git log -S<ID> -- TODO.md` recovers it.

**Wave A status (2026-10-01, branch `audit/2026-10-m1-host-fixes`).**

| Status | Items |
|---|---|
| Fixed (host tests) | AN-01, AN-02, MCP-20, MCP-21, MCP-22, MCP-23, MCP-24, PLT-02, LA-04, DAQ-01 (decoder half), DAQ-02, PWR-06, PWR-08, BUS-012 (client half), PY-20, PROTO-8, WEB-KEYS, TEST-2, DESK-21 (BBSC v2 scope buckets) |
| Fixed, unbuilt | IOS-KEYS (static source test only; no Swift toolchain on Windows - needs an Xcode build) |
| Moved to Wave B | IO-11, DESK-20 (the firmware `/api/io/owner` handlers must derive kind/session from the token first) |

**Plan M2 status (2026-10-01, branch `audit/2026-10-m2-s3-firmware`, S3 flashed by OTA).**

| Status | Items |
|---|---|
| Fixed, host tests only | PLT-03 (T1; 30-run soak clean), PWR-01 (T2; HTTP check needs an NVS-free path - fault config is RAM-only, but not exercised on the board), PLT-04 (flags + passkey; iOS now displays the derived passkey and waits for pairing, but needs a phone handshake) |

**Plan M3 status (2026-10-01, branch `audit/2026-10-m3-routing`, S3 flashed by OTA, BBP PROTO 12).**

| Status | Items |
|---|---|
| Fixed, host tests only | IO-2 (break-before-make transient needs a scope), LA-03 (needs the LA HAT, M8), PLT-05 (owner chose no WiFi changes on the board), FEAT-6, PROTO-4, DESK-31 |
| Open (needs a bench decision) | IO-12 / AN-14: after `configure_io` on analog IO9 the device has **S3** closed (firmware auto-route) while the HAL shadow says **S2**; the next HAL MUX write flips it. Which switch is electrically correct needs a wired stimulus. IO-3 label: U17's daisy-chain slot is not recoverable from the binary PCB files - device 2 stays guarded until confirmed |

**Plan M4 status (2026-10-01, branch `audit/2026-10-m4-daq`, P4/C6/S3 flashed over HTTP, USB stream 2 / DDP 9 unchanged).**

| Status | Items |
|---|---|
| Fixed, host tests + build only | DAQ-03 (no IO loopback to measure marker timing), DAQ-16 / P4-7 (iOS path not covered), C6-21 (no C6 reboot / factory reset on the board), C6-22 (visual check not done), C6-23, C6-25 (board kill test inconclusive: staging finished before the S3 reset landed) |
| Open | DAQ capture throughput above 128 kSPS (19 % lost at 256k, 54 % at 512k) - now measured and reported, not fixed |

**Plan M5 status (2026-10-02, branch `audit/2026-10-m5-latency`, S3 + SPIFFS flashed by OTA). M5 complete.**

| Status | Items |
|---|---|
| Fixed, host tests + smoke | TR-7, TR-9, PWR-16, PWR-10 (worker off on this board), AN-07, AN-12, WEB-28 (UI redeployed, vitest), DESK-22, DESK-23 (ADC tab only), DESK-24, DESK-25, DESK-28, DESK-DIAG-RAW (new) |
| Not verified on a client | iOS camelCase decode (IOS-23): T1 only, needs a macOS build by the owner. Desktop UI changes: `cargo check` + tests, no UI run |
| Carried over | DESK-23 for the other tabs (Overview, Diag, Faults rebuild per tick); AN-11 faster path (fewer SPI transactions per sample) |

**Plan M6 status (2026-10-02, branch `audit/2026-10-m6-refactor`; S3, P4, C6 flashed).**

| Status | Items |
|---|---|
| Fixed, host tests | DESK-REFAC (25 dead commands removed), DESK-33, FEAT-4 |
| Gate added, refactor carried over | PY-21 (public API golden snapshot; mixin split not done), TR-11 (USB/HTTP equivalence test, 7 on the board; service layer not done) |
| Not verified on a client | iOS TR-11b changes (T1 only) |
| Open, found here | `tests/http_api` mutating tests send no admin token (401 on the board) |

**Plan M7 status (2026-10-02, branch `audit/2026-10-m7-parity`).**

| Status | Items |
|---|---|
| Done, host tests | BUS-011, BUS-016, MCP-4, DESK-9 |
| Carried over | DESK-3, DESK-4 / WEB-3, IOS-5, DAQ-13 |

**Plan M10 / M9 status (2026-10-02, branch `audit/2026-10-m9-closeout`).**

| Status | Items |
|---|---|
| Done, host tests | IO-23, DAQ-07 (MCP half), MCP-34, MCP-25 (OTA/cal tools), IO-19 |
| Still open | AN-15 firmware voltage clamp for raw codes (needs calibrated prediction + bench), DAQ-07 USB range lock through the registry (P4), MCP-25 rest (`daq_set_source`/`daq_power_setup`/`daq_set_setting` bounds, `require_hat` HAT type), MCP-26, IO-14/15/16, IO-24, PWR-12/13/15, BUS-007/008/013/014/015, C6-5/24/30/31/35/38/40, PLT-11/18, IOS-22, DESK-29/30, LA-13/14, TEST-3/5/6/7 |

**Final hardware run (2026-10-02, `tests/device --daq --skip-destructive`, USB + HTTP + DAQ HAT, no DUT wired):**

| Result | Tests | Cause / action |
|---|---|---|
| Flaky, passed on rerun | `test_large_response_intact`, `test_spectrum_keeps_updating_over_thousands_of_ffts` | open; watch |
| Needs a DUT load | `test_source_enable_produces_current_and_disable_removes_it`, `test_current_is_linear_in_vdut`, `test_current_is_monotonic_in_vdut` | bench had nothing on the DUT terminals; should skip without `--daq-load-ohms` |

## Feature verdict matrix

| # | Feature | Verdict | Driving findings |
|---|---|---|---|
| 1 | Channel config (`configure_io`, channel function) | **REFACTOR** | Host HAL and firmware both drive the MUX; 12-19 frames + several 100 ms dead times per call (TR-6, MCP-31, IO-14) |
| 2 | `read_voltage` | IMPROVE | Cached value, no freshness stamp (AN report) |
| 3 | `read_current` | IMPROVE (bug) | AN-02 |
| 4 | `read_resistance` / RTD | IMPROVE (bug) | AN-03: RTD config and DAC readback skip the C/D logical-physical swap |
| 5 | DAC voltage/current output | IMPROVE | AN-01, AN-04, limits mostly host-side |
| 6 | Waveform generator | **REFACTOR** | AN-05, AN-06, AN-08 |
| 7 | ADC stream / scope / DSP | **REFACTOR** | AN-07, AN-10, AN-11 (polled SPI, `div` ignored) |
| 8 | MCP ADC snapshot | IMPROVE | Polls a cache over USB, capped |
| 9 | IDAC (DS4424) / calibration / AVDD | IMPROVE | AN-15, PWR-16 (NVS commit per set) |
| 10 | ADC LEDs 0x47 | LEAVE (+ bindings) | PY-4 |
| 11 | ADGS MUX matrix | **REFACTOR** | IO-2, IO-3, IO-8, IO-9, IO-13 |
| 12 | Digital IO | **REFACTOR** | IO-1 |
| 13 | IO ownership / leases | **REFACTOR** | IO-5, MCP-20, IO-11, DESK-20, IO-15, IO-16 |
| 14 | Faults / alerts | IMPROVE | MCP-21, IO-7, IO-17 |
| 15 | Self-test | IMPROVE | MCP-21, IO-24 |
| 16 | Board profiles | IMPROVE | IO-10, IO-23 |
| 17 | Mainboard rails / e-fuses / VADJ / VLOGIC | **REFACTOR** | PWR-01..05, PWR-11, PWR-15; three divergent power-up sequences |
| 18 | `target_power_up` / bootloader tools | IMPROVE (bug) | MCP-22 |
| 19 | USB-PD | IMPROVE | PWR-06, PWR-12, PWR-13 |
| 20 | HAT rails | IMPROVE | PWR-07, PWR-08 |
| 21 | Power telemetry / temperatures | **REFACTOR** | PWR-09, PWR-10 |
| 22 | UART bridge | IMPROVE | MCP-23, BUS-007, BUS-008, BUS-014, BUS-015 |
| 23 | External I2C | IMPROVE | BUS-005, BUS-011, BUS-012, BUS-017 |
| 24 | External SPI | IMPROVE | BUS-003, BUS-013 |
| 25 | Bus planner | **REFACTOR** | IO-1 / BUS-001 / BUS-002, MUX-4 |
| 26 | `register_access` (AD74416H regs) | LEAVE | Firmware-gated to scratch registers |
| 27 | LA memory capture / trigger / RLE | **REFACTOR** | LA-01, LA-02, LA-03, LA-07 |
| 28 | LA streaming | IMPROVE | LA-06, LA-08 |
| 29 | LA host (Python/MCP/desktop) | IMPROVE / REFACTOR (desktop decoders) | LA-04, LA-05, LA-13, LA-14 |
| 30 | SWD probe + descriptors | LEAVE | Non-negotiable #1 intact |
| 31 | `setup_swd` / detect | IMPROVE | LA-10, LA-12 |
| 32 | S3 HAT bridge | LEAVE (gating IMPROVE) | LA-09, LA-11 |
| 33 | DAQ acquisition, filters, rates | IMPROVE | DAQ-05, DAQ-06 |
| 34 | DAQ autorange / dwell | LEAVE | Sound |
| 35 | DAQ USB-HS framing | LEAVE | Sound; decoders are the problem |
| 36 | DAQ STATUS record + host decoders | **REFACTOR** | DAQ-01, P4-8 |
| 37 | DAQ triggers / markers / IO roles | IMPROVE | DAQ-03, DAQ-04, DAQ-10 |
| 38 | DAQ settings registry | IMPROVE | DAQ-08, C6-20, C6-21 |
| 39 | DAQ SMU / VDUT | IMPROVE | P4-9 (understated), DAQ-16 |
| 40 | DAQ calibration | IMPROVE | DAQ-13, FEAT-2 |
| 41 | DAQ host analysis (Python/MCP) | IMPROVE | DAQ-02, DAQ-07, MCP-34 |
| 42 | Desktop DAQ tab | IMPROVE | DAQ-01, DAQ-11, DESK-24, DESK-25 |
| 43 | Web DAQ tab | **REFACTOR** | DAQ-11 (wrong JSON keys - always 0) |
| 44 | C6 rendering | LEAVE | |
| 45 | C6 data honesty / menus / sync | IMPROVE | C6-20, C6-21, C6-22, C6-24 |
| 46 | DDP handlers | **REFACTOR** | C6-30, C6-31, C6-38, C6-40 |
| 47 | P4/C6 OTA relay | **REFACTOR** | C6-25, C6-26 |
| 48 | C6 WiFi / ESP-Hosted | IMPROVE | C6-5, C6-35 |
| 49 | BBP framing / CDC | IMPROVE | TR-1, TR-2, TR-3 |
| 50 | BBP dispatch registry | LEAVE | Sorted table + binary search |
| 51 | HTTP server structure | **REFACTOR** | TR-11, WEB-23, PLT-06, WEB-26 |
| 52 | Auth | IMPROVE | PLT-01, WEB-24, PLT-04, WEB-25 |
| 53 | Python USB transport | IMPROVE | TR-7 (blind retries of non-idempotent ops) |
| 54 | Rust transport | IMPROVE | 2 s timeout for slow commands |
| 55 | Error tables | **REFACTOR** | FEAT-6, TR report (five copies, one stale) |
| 56 | OTA S3 / SPIFFS / RP2040 | IMPROVE | PLT-10, PLT-11, PLT-18 |
| 57 | WiFi / onboarding | **REFACTOR** (connect path) | PLT-05, PLT-02 |
| 58 | Quick Setup | IMPROVE | Corrupt slot applies as full reset and returns OK (PLATFORM.md) |
| 59 | MicroPython scripting | IMPROVE | PLT-03, PLT-07, PLT-08 |
| 60 | Diagnostics / memory | IMPROVE | MEM_STATUS covers a subset of tasks |
| 61 | Web UI client layer | **REFACTOR** | WEB-28 (no fetch timeouts), dead IO-lease module |
| 62 | iOS transport | **REFACTOR** | IOS-23, IOS-22, IOS-KEYS |
| 63 | Desktop connection manager / diagnostics / recording | **REFACTOR** | DESK-21, DESK-29, DESK-31 |
| 64 | Python `client.py` | **REFACTOR** | PY-21 (split) |
| 65 | MCP safety gates | IMPROVE | MCP-24, MCP-25, MCP-26 |
| 66 | Tests | IMPROVE | TEST-1..7 (untyped mocks hid MCP-20) |

## New findings, by wave

Format: **ID** (aliases) · severity · category · tag - what is wrong, where,
what to build, how to prove it. Effort S/M/L.

### Wave A - host-only correctness

- **DAQ-01** · P1 · BUG · `[VERIFIED]` - Rust STATUS decoder
  (`DesktopApp/BugBuster/src-tauri/src/daq_proto.rs:~453-466`) reads
  `drop_fine`/`drop_coarse` as u32 at 30/34 and `frames_tx` at 40; firmware
  `usb_proto.h:177-182` has u16 at 30/32, `fine_diag_sticky` at 34, and
  `frames_tx` at 36. Every v3+ field on the desktop is misaligned (drops,
  throughput, ring, temperatures path is separate). The CI gate compares only
  the version byte. **Do:** fix offsets to match the header; add a gate that
  parses the struct comment offsets in all four copies, or a golden STATUS
  frame shared by Python, Rust and iOS tests. The Rust test
  `status_perf_extension` encodes the wrong layout and must change with it. S/M.
- **IO-11** · P1 · BUG · `[VERIFIED]` - web `io_lease.ts:130,148` call
  `/api/io/owner/claim` and `/api/io/owner/release`; firmware registers
  `POST /api/io/owner` and `DELETE /api/io/owner` (`webserver.cpp:5695-5710`).
  The lease banner 404s every 2 s and never claims. **Do:** call the real
  routes, surface errors. S.
- **IOS-KEYS** (AN-17, IOS report) · P1 · BUG · `[REPORTED]` - iOS sends
  `range` (firmware wants `bipolar`) to `vout/range`; omits `mux` on
  `adc/config` (rejected); sends `excitationUa` for RTD; decodes Quick Setup
  list as a bare array (firmware returns `{slots:[...]}`); autorun enable omits
  `?name=` (always 400). `OverviewTab.swift:1018-1046` and others. Read-verify
  only (cannot build on Windows). S each.
- **BUS-012** · P2 · BUG · `[REPORTED]` - Python ext I2C masks addresses with
  `& 0x7F`, so an 8-bit address 0xA0 silently becomes 0x20; firmware does not
  validate either. **Do:** reject > 0x7F with a message naming the 7-bit form;
  validate in firmware too. S.
- **DESK-20** · P1 · PARITY · `[VERIFIED]` mapping, effect `[REPORTED]` -
  desktop `http_transport.rs` has no arm for `CMD_IO_CLAIM` / `RELEASE` /
  `OWNER_STATUS` / `FORCE_RELEASE` (falls into "not implemented for HTTP
  transport", `:2143`). Reported effect: over WiFi, GPIO/DIN/DOUT/WaveGen
  writes are refused. **Do:** map to `/api/io/owner` GET/POST/DELETE and
  `/api/io/owner/force`. S.
### Wave B - S3 firmware safety and correctness

- **WEB-24** · P1 · SECURITY · `[VERIFIED]` - unauthenticated
  `GET /api/daq/wifi_stream/status` returns the DAQ hotspot `ssid` and
  `password` (`net/api_core.cpp:~440-442`, handler `webserver.cpp:2412`).
  **Do:** require the admin token for the credential fields. S.
- **PWR-01** · P1 · SAFETY · `[VERIFIED]` - `handler_pca_set_fault_cfg()`
  (`bbp/cmds/cmd_pca.cpp:~120`) declares `PcaFaultConfig cfg;` and sets only
  two of its fields; `efuse_enable_blackout_ms` (`hal/pca9535.h:107`) is stack
  garbage and is then applied, so e-fuse faults may be ignored for an arbitrary
  window after enable. The HTTP twin (`webserver.cpp:~2341`) reportedly also
  defaults `auto_disable` to false when the key is missing. **Do:** read the
  current config first, overwrite only the sent fields. S.
- **PWR-02** · P1 · SAFETY · `[REPORTED]` - a short that starts inside the
  100 ms blackout is never reported or auto-disabled: the trip check is
  edge-based and the edge is consumed during blackout (`pca9535.cpp:509,531`).
  **Do:** at blackout end, re-evaluate FLT as a level. **Prove:**
  `[BENCH]` power up into a shorted load. S.
- **PWR-03** · P1 · SAFETY · `[REPORTED]` - only the Python client guards raw
  `PCA_SET_PORT`; firmware lets it clear `EN_USB_HUB` (hub reset, drops USB -
  the known `power_set_port(0, 0x00)` hazard) and bypasses the e-fuse
  blackout gate (`pca9535.cpp:386`). **Do:** firmware mask that forces the hub
  and logic enables on unless an explicit override flag is sent. S.
- **IO-3** · P1 · SAFETY · `[REPORTED]`, `[BENCH]` - `config.h:94`
  `U17_DEVICE_IDX = 2`; `bus_planner.cpp:114-126` puts IO 7-9 on MUX device 3
  while `Docs/mainboard-hardware.md:104` says IO9 is U17 / device 2. One of
  them is wrong, and the interlock may guard the wrong device. **Do:** settle
  against the schematic (`PCB Material/Main PCB`) before touching code. S.
### Wave C - DAQ HAT firmware (build P4 and C6)

- **BS-1** · P1 · FEATURE · `[REPORTED]` - battery simulator firmware core
  (P4 `src/battsim/`, C6 menu + battery main screen, DDP v10, `battlog`
  partition). P4 and C6 build; nothing run on hardware yet. **Do:** wired
  flash (partition change), then bench-verify: integrator vs `power_dsp`
  charge, cutoff, reboot resume, dither ripple, 1-min/15-min files. M.
- **BS-2** · P1 · FEATURE · `[REPORTED]` - battery simulator host transport is
  missing: HAT-link status/list/read commands (32 B reply cap -> paged), BBP
  + HTTP routes on the S3, Python binary decode + JSON export, MCP tools,
  manifests. Settings/actions already work generically via `DAQ_CONFIG`. L.
- **BS-3** · P1 · FEATURE · `[REPORTED]` - history v2 (spec
  `Docs/superpowers/specs/2026-10-02-battsim-history-v2.md`) implemented on
  `feat/battsim`; P4 builds. **Do:** bench-verify on a merged image (with the
  `feat/battsim-ui` transport): `bs selftest` (32-day retention, q15
  compaction), constant load vs `power_dsp` (charge 0.1 %, energy 0.5 %),
  `bs crash` mid-run then resume (cumulative fields continuous), host decode
  of real v2 files. M.

### Wave D - latency

- **AN-10 / AN-11** · P2 · PERF · `[REPORTED]` - ADC stream `div` is never
  applied (`bbp.cpp:758`); advertised 9.6 kSPS is ~555 SPS; acquisition polls
  SPI at <= 1 kHz while `ADC_RDY` is configured but never read
  (`main.cpp:296`). **Do:** ADC_RDY-driven acquisition task. L - largest
  single analog improvement.
- **TR-6 / MCP-31** · P2 · PERF · `[REPORTED]` - `configure_io` is 12-19 BBP
  frames plus ~200 ms firmware dead time; first use per block adds 0.5 s.
  **Do:** one lease around the three calls, drop the redundant DISABLED MUX
  step, use firmware auto-claim. Long term: one `IO_CONFIGURE` firmware
  command. M.
- **DESK-22..26** · P1/P2 · PERF · `[REPORTED]` - HAT, Voltages, IO Expander,
  USB-PD tabs refetch on every `device-state` tick (300 ms on HTTP); whole
  card grids rebuilt each tick; DAQ transport mutex held across reads up to
  1 s; DAQ view ships ~14k floats of JSON at up to 30 Hz; the HAT probe polls
  forever on a bare board. **Do:** shared poller with per-tab subscription,
  keyed `<For>`, binary Tauri channel for DAQ frames. M.
- **IOS-23** · P2 · PERF · `[REPORTED]` - ~6 req/s steady state, six
  sequential requests every 5th cycle; status replies carry both camelCase and
  snake_case copies of every field. **Do:** drop the duplicate keys server-side
  once every client reads one form. M.
- **PWR-09 / PWR-10** · P2 · PERF · `[REPORTED]` - DAQ-HAT telemetry measures
  3 rails at 1 Hz (~0.6 s each) in `mainLoopTask` (`hat.cpp:1462`); "cached"
  supply reads run a monitor step in the BBP and HTTP tasks
  (`cmd_selftest.cpp:84`, `api_core.cpp:1000`). **Do:** one background
  sampler, readers return the cache. M.
### Wave E - refactors

- **TR-11** · P2 · REFACTOR · `[REPORTED]` - `api_core` is not a shared layer
  and the cmd registry backs only a few HTTP routes; HTTP handlers re-implement
  BBP handlers and ignore `sendCommand()` return values, replying `ok:true` when
  the command was dropped. Root of AN-04, the HTTP 200 `{"ok":false}` problem
  (WEB-26 / IOS-21) and most cross-transport drift. **Do:** one C service
  function per capability, BBP and HTTP as thin adapters (the cmd registry
  pattern in `http_adapter.cpp` is the template). L, incremental.
- **PY-21** · P2 · REFACTOR · `[REPORTED]` - `client.py` is ~4.8k lines with a
  USB/HTTP branch in most methods. Split into per-capability mixins with a
  transport strategy; extend `Transport` (FEAT-4) so HTTP gaps fail at type
  check. L.
### Wave F - parity features (reuse existing primitives)

- **DESK-3** scripting UI - add `0xF5-0xFD` to `bbp.rs`, Tauri commands, and
  a tab; the HTTP routes already exist. L.
- **DESK-4 / WEB-3** external I2C/SPI UI - IDs already in `bbp.rs:132-138`; no
  commands or tab. M each.
- **DAQ-13** - per-range meter calibration reachable only from the P4 serial
  console; expose over BBP DAQ sub-ops + MCP, gated (`cal` writes NVS). M.
- **IOS-5** - unchanged; use `CLIENTS-WEB-IOS.md` route matrix as the
  checklist.
### Wave G - RP2040 (blocked: LA HAT not attached)

- **LA-01** · P1 · `[REPORTED]` - triggered capture programs begin with
  `irq wait 0` (`bb_la_trigger.pio:51,58,65`), which may raise rather than
  wait; triggers might fire immediately. Read the PIO ISA before changing.
- **LA-02** · P1 · `[REPORTED]` - `sm_config_set_in_shift(..., false, ...)`
  shifts left, so samples within each word could be in reverse order relative
  to every LSB-first decoder. One-line fix if confirmed on hardware.
- **LA-05..LA-12** - see `RP2040-LA-SWD.md` (28-byte readout per round trip;
  `HAT_CMD_LA_USB_SEND` has no BBP mapping and five desktop commands are dead;
  ring overrun check off by one; DAQ branch of `hat_update_leds` unreachable;
  `setup_swd` returns success on failed voltage set; S3 never reads the HAT
  IRQ line).

## Items the 2026-10-01 audit found already fixed or wrong

2026-10-02 (M9): P4-3, P4-4, MCP-9, PY-10, DESK-5, WEB-4, WEB-10, IOS-3,
IOS-4, IOS-9, IOS-12 and IOS-14 were re-read against source and are recorded in
`CHANGELOG.MD`; the stale `.mex` facts were corrected. Still open:

| Item | Finding |
|---|---|
| IOS-11 | Mostly fixed (SignalPath has a confirm); rails/OTA/reset still to check. |
| RP-1, RP-3, RP-4 | Addressed in source (RP-3 bounded by cal-window clamp, `bb_hat_v2.c:511-562`); still needs hardware to close. |
| PROTO-4 | Partly fixed: `0xEF` and external bus IDs now in `bbp.rs`; still missing `0xF5-0xFD`, `0x64/0x65`, `0x47`, `EVT 0x88`, `0x0C MEM_STATUS`. |
| PROTO-8 | Not dead: `simulated_device.py:55` uses `ESP32_FW_VERSION`. Update, do not delete. |

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

# Built, not flashed (2026-10-01)

Compiles; never run on hardware. Do not treat as done.

| Item | What would prove it |
|---|---|
| C6 NVS kept across C6 OTA (P4 `c6_flasher.c` hole) | Flash the P4 first, change a C6 setting (brightness), OTA the C6, confirm it survives. |
| C6 boot `WAIT` / link-lost banner rework | Cold-boot the board: no banner; then drop the P4 link: one banner, cleared on recovery. Read the C6 log line "first P4 measurement N ms after boot". |
| C6 Settings > DUT Supply live state | Toggle ON then OFF from the menu; value follows. |
| C6 PD warning expiry | Press supply ON before the S3 boots; warning clears within 3 s or as soon as PD is valid. |
| C6 home `SET` setpoint line | Supply OFF shows grey SET + white live V; check text fits at 20.00 V. |
| E-fuse current monitor (S3 + all hosts) | **Partly live-verified 2026-10-01 over HTTP** on OFF e-fuses: attach, move, stop, ~0 mA reading, supply measure refused while active and correct after. Still open: ON e-fuse (needs-confirm + power cycle), a known load, MCP path, auto-cal and raw U23 writes refused while active. **Verify the logical->EFUSE_MON_n map** in `efuse_imon.cpp` (assumes logical 3/4 = physical pairs 4/3 like the EN bits). iOS card unbuilt (needs Xcode). |

# Flashed but not proven

Code is on the device. The behaviour has not been provoked.

| Item | What would prove it |
|---|---|
| C6-11 watchdog | **Cannot work as configured** - task WDT panic is off (see C6-23). Fix, then induce a hang. |
| C6-9 link-lost banner | Banner is transient and fabricated numbers keep rendering (see C6-22). Fix first. |
| C6-1 DDP `RSP_ERR` | Send a deliberately truncated DDP frame. |
| DESK-1 STOP on disconnect | Run the desktop app against the device and pull the link. |
| Web UI | Uploaded to SPIFFS several times in M5/M6 (HTTP and USB); scope SSE and pollers checked live (WEB-28). Every tab not yet opened on the device. |

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

2026-10-01: `0xEF` and the external-bus IDs are now in `bbp.rs`. Still missing:
scripting `0xF5-0xFD`, DSP stream `0x64-0x65`, `0x47 ADC_LEDS_SET_MODE`,
`EVT 0x88`, `0x0C MEM_STATUS`. Root of DESK-3. Re-verify each ID before
implementing.

### PROTO-6 · P2 · BUG · `[REPORTED]` - `BBP_MAX_PAYLOAD` not enforced when building responses

Validated on receive but reportedly not at every response-construction site. An
over-long response is a firmware-side overflow, so worth confirming.

### PROTO-7 · P2 · PROTOCOL · `[REPORTED]` - WAVE_I decimation has no anti-alias filter

Decimation greater than 1 reportedly drops samples without filtering, so anything
above Nyquist folds back into the displayed waveform. On a measurement instrument
that produces plausible-looking wrong data.

### PROTO-8 · P3 · DOC · `[REPORTED]` - stale `ESP32_FW_VERSION` in `protocol.py`

Reported as 3.4.0 against a real firmware of 5.1.0. 2026-10-01: **not dead** -
`simulated_device.py:55` uses it. Update it (or derive it from
`firmware_version.py`), do not delete it.

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

P4-3 (OTA product-ID order) and P4-4 (Sinc5 x8/x16 16-bit mode) were found
fixed / refuted on 2026-10-01 - see the table in the 2026-10-01 section.

### P4-6 · P2 · FEATURE · `[REPORTED]` - no atomic settings snapshot per capture

The host cannot reconstruct the exact filter, decimation and SR state a capture
was taken under. See FEAT-5.

### P4-7 · P2 · BUG · `[REPORTED]` - DUT supply comes back OFF after OTA with no NVS restore

Matches observed behaviour. 2026-10-01: off-at-boot is deliberate
(`daq_settings_glue.c:410-411` plus a hardware pull-down), so do not restore
it. The real gap (DAQ-16): no OTA path warns that the supply is on before
rebooting the P4. Add that warning to every P4 OTA entry point.

### P4-8 · P3 · BUG · `[REPORTED]` - COARSE/FINE drop counters saturate at uint16

2026-10-01: it **saturates** at 65535 (`daq_board.c:932`), it does not wrap.
The firmware comment claiming u32 is false, and the Rust decoder assumes u32 -
the root of DAQ-01. Widen to u32 with a `usb_proto.h` version bump in all four
copies, in the same change as the DAQ-01 decoder fix.

### P4-9 · P3 · PERF · `[REPORTED]` - SMU voltage ramp blocks the control plane

2026-10-01: understated. A full span is 254 codes x 10 ms = about 2.54 s,
against a 300 ms S3 timeout, and registry writes apply synchronously on the
link tasks (DAQ-08). Move the ramp off the control path.

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

C6 NVS is untouched, so the label overpromises. 2026-10-01: worse than stated -
the reset is never pushed to the C6, which later resends its stale values and
undoes part of the reset (C6-21). Fix C6-21 first.

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

DESK-5 (P4/C6 upload) is fixed - see the 2026-10-01 table.

### DESK-6 · P2 · BUG · re-scoped 2026-10-01 - IO ownership races

The tab-switch path is a no-op (all 9 `SLOTS` are `&[]`). The real races are
per-action claim/release and the keep-alive snapshot (DESK-35), and IO-5
(`IO_RELEASE` wire mismatch) means desktop releases never land anyway. Fix IO-5
first.

### DESK-9 · P2 · PARITY · `[REPORTED]` - cannot set the WiFi AP password

`0xEF` is now in `bbp.rs:189`; still no Tauri command, HTTP mapping or UI.

---

# Surface: iOS app

**Blocked: cannot build on Windows.** Every item below is read-verified only.
Full report: `scratch/audit/IOS.md`

The web UI and iOS are both pure HTTP clients, so IOS-5 is the cheapest parity
gap in the project to close - the API surface is already proven and the work is
view-layer only. Treat the web UI as the reference implementation.

| ID | Sev | Title |
|---|---|---|
| IOS-1 | P1 | Admin token stored in `UserDefaults`, an unencrypted plist. Belongs in the Keychain. 2026-10-01: partly moved; `bugbuster_token` is still written to `UserDefaults` (`ConnectionManager.swift:692`, IOS-22). |
| IOS-2 | P1 | Force unwraps on network-derived data (network port, scope `displayPoints`, `muxStates` when `lastStatus` is nil). 2026-10-01: partial. |
| IOS-3 | - | Fixed per 2026-10-01 audit. |
| IOS-4 | - | Fixed per 2026-10-01 audit. |
| IOS-5 | P1 | Roughly half the web UI's route coverage (55 of ~122). Missing: faults, digital IO, UART bridge, external bus, waveform generator, manual OTA, WiFi config, IO ownership, ADC DSP, advanced HAT controls. |
| IOS-6 | P2 | ~70 `try?` sites swallow errors silently; the app shows stale data with no indication anything failed. |
| IOS-7 | P2 | Optimistic updates never roll back on POST failure. On a hardware control app a user can believe a rail is off when it is on. 2026-10-01: `OverviewTab` rolls back, but only over BLE - HTTP handlers return 200 `{"ok":false}` (IOS-21 / TR-11). |
| IOS-8 | P2 | Circuit breaker never resets after an SSE timeout. |
| IOS-9 | - | Fixed per 2026-10-01 audit. |
| IOS-10 | P2 | Unbounded DAQ stream buffers. |
| IOS-11 | P2 | Mostly fixed (SignalPath has a confirm); rails/OTA/reset still to check. |
| IOS-12 | - | Wrong: VDUT routes exist (`api_core.cpp:465-516`). |
| IOS-13 | P3 | Deployment target is iOS 26.0. Valid (Apple's year-based versioning), but a 26.0 *minimum* excludes every device that has not updated. Decide deliberately. |
| IOS-14 | - | Likely non-issue: ATS exempts IP literals and `.local`. Confirm on a device. |

---

# Surface: On-device web UI

Full report: `scratch/audit/WEB.md`

Security came back clean: no XSS via `innerHTML`, the admin token is sent as a
header rather than a query string, and token storage defaults to `sessionStorage`.

### WEB-3 · P2 · PARITY · `[REPORTED]` - no external I2C / SPI bus UI

Eight routes unused. Same gap as DESK-4.

WEB-4 and WEB-10 are fixed - see the 2026-10-01 table. The web IO-lease module
is broken, though (IO-11).

---

# Surface: Python library

### PY-4 · P2 · PARITY · `[REPORTED]` - `0x47 ADC_LEDS_SET_MODE` has no binding

The same agent's claims that `0x0A WIFI_FORGET` and `0x0B SELFTEST_WORKER` are
unbound were **refuted** by the protocol audit. Verify before implementing.

### PY-5 · P2 · PROTOCOL · `[REPORTED]` - no version handshake on the DAQ USB stream

`daq_stream.py` hardcodes a proto version and never checks it against the device.
The four copies are now held in lockstep by CI, which stops them *drifting*, but
a host still cannot detect a device running a different build.

### PY-10 · P3 · DOC - `idac_cal_clear()` name implies an NVS wipe but is RAM-only

Docstring fixed (2026-10-01). Only the rename (with a deprecated alias) remains.

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

MCP-9 is fixed (2026-10-01).

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
| USB-PD | yes (USB select does not commit, PWR-06) | yes (same) | yes | yes | yes | no |
| UART bridge | yes | yes | yes | yes | no | no |
| SWD / CMSIS-DAP | yes | yes | yes | no | no | no |
| Logic analyzer | yes (USB) | yes (USB) | yes (USB) | no | no | no |
| DAQ power analyzer | yes | yes | yes (STATUS misdecoded, DAQ-01) | **built, wrong JSON keys (DAQ-11)** | partial | yes |
| MicroPython scripting | yes | 1 tool, returns before completion (PLT-08) | **no** | yes | partial | ? |
| Quick setup | yes | yes | ? | ? | ? | no |
| OTA - S3 / RP2040 / SPIFFS | yes | yes | yes | yes | yes | no |
| OTA - P4 / C6 | yes (HTTP) | yes (HTTP) | yes | yes | no | yes |
| WiFi STA join | yes | yes | ? | yes | ? | no |
| WiFi AP password | yes (result inverted, PLT-02) | yes (same) | **no** | yes | ? | no |
| Calibration | yes | yes | yes | yes | partial | yes |
| External I2C / SPI bus | yes | yes (no sync I2C write, BUS-011) | **no** | **no** | no | no |
| IO ownership | yes (release wire broken, IO-5) | **`io_claim` broken (MCP-20)** | USB only (DESK-20) | card yes, lease module 404s (IO-11) | no | no |
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
