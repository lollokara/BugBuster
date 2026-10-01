# BugBuster TODO

Open work only. Everything delivered has moved to `CHANGELOG.MD` under
`[Unreleased]` - do not re-add completed items here.

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

## Execution plan for the next agent

Ordered by risk-to-user over effort. Inside a wave, items are independent unless
noted. **Every wave ends with:** ruff + mypy (scope in AGENTS.md), unit +
`--sim` suites, the firmware build for every target touched, the manifest
update (non-negotiable #12), and a `CHANGELOG.MD` `[Unreleased]` entry.

1. **Wave A - host-only correctness (no flashing).** Python, MCP, web UI, iOS,
   desktop. Cheapest, highest value: several MCP tools return wrong numbers or
   report success on failure today. Items: AN-01, AN-02, MCP-20, MCP-21,
   MCP-22, MCP-23, PLT-02, LA-04, DAQ-01, DAQ-02, IO-11, WEB-KEYS, IOS-KEYS,
   PWR-06, BUS-012, DESK-20, PY-20.

   **Wave A status (2026-10-01, branch `audit/2026-10-m1-host-fixes`).** Each
   fix is a `test(ID)` commit that fails on the old code followed by a
   `fix(ID)` commit. Verified in unit / `--sim` / vitest / `cargo test` only -
   **not yet on hardware**, so the per-item tags below stay as they were.

   | Status | Items |
   |---|---|
   | Fixed (host tests) | AN-01, AN-02, MCP-20, MCP-21, MCP-22, MCP-23, MCP-24, PLT-02, LA-04, DAQ-01 (decoder half), DAQ-02, PWR-06, PWR-08, BUS-012 (client half), PY-20, PROTO-8, WEB-KEYS, TEST-2 |
   | Fixed, unbuilt | IOS-KEYS (static source test only; no Swift toolchain on Windows - needs an Xcode build) |
   | Confirmed, design pending | DESK-21 - Scope tab Record writes a header-only BBSC file: the writer is fed only by `EVT_ADC_DATA` and the Scope tab starts only the scope stream |
   | Moved to Wave B | IO-11, DESK-20 (the firmware `/api/io/owner` handlers must derive kind/session from the token first) |
2. **Wave B - S3 firmware safety and correctness.** One build, one OTA flash,
   then the live checks listed per item. Items: TR-2, PLT-03, PLT-01, WEB-24,
   PWR-01, PWR-02, IO-1, IO-5, AN-04, IO-8 (+ MUX-4), PLT-06, WEB-23, BUS-003,
   BUS-005, PLT-10, WEB-25, WEB-NULL, PWR-03.
3. **Wave C - DAQ HAT firmware (P4 + C6, build both).** C6-20, C6-23, C6-22,
   DAQ-04, DAQ-05, DAQ-06, DAQ-03, C6-25, P4-8. DAQ-01's protocol half (widen
   the drop counters) goes here and bumps `usb_proto.h` in all four copies
   (`.mex/patterns/daq-usb-proto-change.md`).
4. **Wave D - latency.** TR-1 (confirm first), TR-3, AN-07, AN-06, AN-10/AN-11,
   IO-13, TR-6 / MCP-31, TR-9, DESK-22..26, WEB-28, IOS-23, PWR-09, PWR-10.
5. **Wave E - refactors.** TR-11 (one service layer for BBP + HTTP), FEAT-6
   (generated error tables), PY-21 (split `client.py`), IO-2 (MUX
   break-before-make), the three power-up sequences (PWR-REFAC), DESK-REFAC.
6. **Wave F - parity features.** DESK-3, DESK-4 / WEB-3, MCP-5, MCP-4, BUS-011,
   DAQ-13, DAQ-10, IOS-5, BUS-016.
7. **Wave G - RP2040 (blocked until the LA HAT is attached).** LA-01, LA-02,
   LA-03 (the S3 half can go in Wave B), LA-05..LA-12.

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

- **AN-01** · P1 · BUG · `[VERIFIED]` - Python over HTTP posts `{"limit_8mA": ...}`
  (`python/bugbuster/client.py:1223`) but the handler reads `"limit8mA"`
  (`Firmware/ESP32/src/web/webserver.cpp:1169`). The 8 mA current limit is
  silently never applied over WiFi. **Do:** send `limit8mA`. **Prove:** sim
  test asserting the posted key; on device, read back `limit8mA` in the
  response. S.
- **AN-02** · P1 · BUG · `[VERIFIED]` - `PortHAL.read_current()`
  (`python/bugbuster/hal.py:569-573`) returns `(adc.value / 12.0) * 1000.0`,
  but firmware already converts to mA (`tasks.cpp` `convertAdcCode()`,
  `adcCodeToCurrent(...) * 1000.0f` for every `CH_FUNC_IIN_*`). Result is about
  83x too large, and MCP `read_current` inherits it. **Do:** return
  `adc.value`. **Prove:** unit test with a simulated 12.0 mA reading. S.
- **MCP-20** (IO-4) · P1 · BUG · `[VERIFIED]` - MCP `io_claim`
  (`python/bugbuster_mcp/tools/io_owner.py:123`) calls
  `bb.io_claim(slots, lease_seconds, purpose)`; the real method is
  `io_claim(self, slots, *, lease_ms=0, purpose="")` and is a
  `@contextmanager` (`client.py:4381`). Always `TypeError`; even if fixed
  positionally it would claim and release immediately. **Do:** call
  `bb._io_claim_raw(slots, lease_seconds*1000, purpose)` or add a public
  non-context `io_claim_lease()`; fix `tests/unit/test_mcp_io_owner.py:56`,
  which locks in the wrong call, and use `create_autospec(BugBuster)` there.
  **Prove:** the tool test passes against an autospec'd client. S.
- **MCP-21** (IO-6, IO-7, IO-18) · P1 · BUG · `[REPORTED]` - MCP `selftest`
  returned `all_pass: true` with boot test failed and supplies at 0 V (the
  agent executed this against the simulator); it reads keys the library does
  not return (`boot_ok`, supply names). `check_faults` reads `channel_alerts`
  (`discovery.py:188`) but the key is `channels`, and reports "No active
  faults" after an e-fuse auto-disable cleared `efuse_flt`
  (`pca9535.cpp:100,554`). **Do:** align keys with `_parse_status` /
  `_parse_faults`; include the auto-disable latch in the fault report.
  **Prove:** sim tests that inject a failed boot test and a tripped e-fuse. S.
- **MCP-22** (IO-10, PWR-04, PWR-05) · P1 · SAFETY · `[VERIFIED]` -
  `target_power_up`, `enter_bootloader`, `release_bootloader`
  (`python/bugbuster_mcp/tools/target.py:47,130,243`) map rail 2 to `EFUSE2`.
  Per `bus_planner.cpp:92-139`, VADJ1 feeds EFUSE1+EFUSE2 (IO 1-6) and VADJ2
  feeds EFUSE3+EFUSE4 (IO 7-12). Rail 1 also leaves EFUSE2 off. The
  post-check reads `get_status()["power"]` (`target.py:67,202`), a key
  `get_status()` never returns (`client.py:1009-1015`), so `success` is
  always true. No `validate_vadj_voltage` either. **Do:** map rail -> both
  e-fuses of its rail, read faults from `get_faults()` / PCA status, validate
  voltage with the board profile. Ideally replace all three sequences with one
  firmware-side sequence (see PWR-REFAC). **Prove:** sim test on rail 2
  asserting EFUSE3/4 toggled and a tripped fuse yields `success: false`. S.
- **MCP-23** (BUS-006) · P1 · BUG · `[VERIFIED]` - MCP `uart_config`
  `_PARITY_MAP = {"none": 0, "even": 1, "odd": 2}`
  (`tools/debug.py:189`); firmware maps 1 = ODD, 2 = EVEN
  (`net/uart_bridge.cpp:189-190`). `parity="even"` configures odd. **Do:** use
  the existing `UART_PARITY_MAP` from `bugbuster_mcp/config.py`. S.
- **PLT-02** · P1 · BUG · `[VERIFIED]` - `WIFI_SET_AP_PASSWORD` replies
  `0x00` = applied+persisted, `0x01` = applied not persisted, `0x02` = failed
  (`bbp/cmds/cmd_wifi.cpp:195-198`). Python returns `bool(resp[0])`
  (`client.py:4216`), i.e. success reads as False and failure as True. The
  mock (`tests/mock/handlers/misc.py`) encodes the inverted meaning too.
  **Do:** return a small result (`applied`, `persisted`) or `resp[0] != 0x02`;
  fix the mock. S.
- **LA-04** · P1 · BUG · `[VERIFIED]` - `capture_logic_analyzer`
  (`tools/waveform.py:~375-393`) treats `hat_la_decode()` output as edges, but
  it returns one 0/1 per **sample** (`client.py:3546-3561`). `transitions` is
  the sample count, so `frequency_hz` is always rate/2 and the protocol hints
  are wrong; `edges[:100]` is raw samples. **Do:** count level changes and emit
  real edge timestamps. **Prove:** unit test with a synthetic square wave. S.
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
- **DAQ-02** · P1 · BUG · `[REPORTED]`, mechanism `[VERIFIED]` -
  `_drain_voltage()` (`python/bugbuster/daq_stream.py:~445-460`) compares
  voltage-stream indices (`rec.start_index + k`) with current-timebase indices
  (`cap.start_index + len(cap.voltage)`) with no rate ratio. With current at
  8 kSPS and voltage at 64 kSPS the agent reproduced 1.25 V reported for a true
  3.0 V. **Do:** scale by the V/I rate ratio from the stream header, or
  timestamp both. **Prove:** unit test with mismatched rates. S.
- **IO-11** · P1 · BUG · `[VERIFIED]` - web `io_lease.ts:130,148` call
  `/api/io/owner/claim` and `/api/io/owner/release`; firmware registers
  `POST /api/io/owner` and `DELETE /api/io/owner` (`webserver.cpp:5695-5710`).
  The lease banner 404s every 2 s and never claims. **Do:** call the real
  routes, surface errors. S.
- **WEB-KEYS** (DAQ-11, WEB report) · P1 · BUG · `[REPORTED]` - web UI reads
  JSON keys the firmware does not send: DAQ tab `voltage_v` / `current_a` /
  `cal_have_*` / `type` (firmware: `measuredVoltageV`, `measuredCurrentMa`,
  `typeName`, `fwMajor/fwMinor`) so V/I/P show 0 and "UNCALIBRATED" is
  permanent; GitHub update card `.newer` (firmware `updateAvailable`); Voltages
  tab `c.voltage` (firmware `targetV`); RTD posts `current_uA` (firmware reads
  `current` or `excitation_ua`). Full list with handler lines in
  `CLIENTS-WEB-IOS.md`. **Do:** fix each, then add a TS type per route
  generated from or checked against one JSON fixture per handler. S each.
- **IOS-KEYS** (AN-17, IOS report) · P1 · BUG · `[REPORTED]` - iOS sends
  `range` (firmware wants `bipolar`) to `vout/range`; omits `mux` on
  `adc/config` (rejected); sends `excitationUa` for RTD; decodes Quick Setup
  list as a bare array (firmware returns `{slots:[...]}`); autorun enable omits
  `?name=` (always 400). `OverviewTab.swift:1018-1046` and others. Read-verify
  only (cannot build on Windows). S each.
- **PWR-06** · P1 · BUG · `[REPORTED]`, Python half `[VERIFIED]` - over USB
  `usbpd_select_voltage()` sends only `USBPD_SELECT_PDO`
  (`client.py:4133-4147`), and MCP `usb_pd_select` (`tools/power.py:70`) never
  calls `usbpd_go()`. Desktop and HTTP send select + go. **Do:** confirm the
  HTTP handler does both, then commit over USB too. S.
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
- **PY-20** (PY-4) · P2 · PROTOCOL · `[REPORTED]` - the constants parity test
  only checks Python `CmdId` -> firmware, never firmware -> Python, so
  `ADC_LEDS_SET_MODE` 0x47 and `WIFI_FORGET` 0x0A have no `CmdId` and nothing
  fails. **Do:** make the test bidirectional with an explicit allow-list for
  intentionally unbound IDs. S.

### Wave B - S3 firmware safety and correctness

- **TR-2** · P1 · BUG · `[VERIFIED]` - `BBP_COBS_MAX` is
  `BBP_MAX_PAYLOAD + 2 + 1` = 1027 (`bbp/bbp.h:41-42`), but COBS adds one byte
  per 254 plus one, so a 1024-byte message encodes to up to 1029 bytes and
  `sendFrame()` writes the delimiter at `s_cobsBuf[cobsLen]`
  (`bbp/bbp.cpp:~162-166`) - a heap/static overrun of up to 3 bytes on
  near-maximum responses. Separately `sendMsg()` silently **drops** any
  response over the limit with only a log line, so the host times out instead
  of getting an error (`SCRIPT_LOGS` can reach this). **Do:** size
  `BBP_COBS_MAX` as `n + n/254 + 2`, bound COBS decode output, and send
  `BBP_ERR_*` when a response does not fit. **Prove:** unit test of the COBS
  encoder at 1024 bytes of non-zero data; live `SCRIPT_LOGS` with a full log. S.
- **PLT-03** · P1 · BUG · `[VERIFIED]` - `scripting_run_string()` declares
  `ScriptCmd cmd;` uninitialised (`mp/scripting.cpp:690`) and sets only
  `id/payload/len/persist`; the worker reads `cmd.is_lint` and gives
  `cmd.lint_sem` (`:510,531`). Stack garbage can route a run as a lint and
  `xSemaphoreGive()` a wild pointer. Every other site uses `= {}`. **Do:**
  `ScriptCmd cmd = {};`. S.
- **PLT-01** · P0 · SECURITY · `[VERIFIED]` - `POST /api/registry/set_dac_code`,
  `set_dac_voltage`, `set_dac_current` (`web/http_adapter.cpp:93-249`,
  registered by `http_adapter_register()` at `webserver.cpp:5761`) drive the
  DAC with **no admin check** (`http_adapter.cpp` contains no auth call);
  every other mutating route calls `check_admin_auth()`. No host calls them.
  **Do:** delete the three routes (or gate them), update the route count gate
  and the esp32-mainboard manifest. **Prove:** unauthenticated POST returns
  404/401. S.
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
- **IO-1** (BUS-001, BUS-002) · **P0** · SAFETY · `[VERIFIED]` -
  `bus_planner_route_digital_input()` calls
  `apply_power_and_mux(..., 3.3f, 3.3f, ...)` (`bus/bus_planner.cpp:~511`),
  i.e. sets **VADJ and VLOGIC to 3.3 V**, re-arms the e-fuse, and writes a MUX
  state holding only that route. Callers: `cmd_dio.cpp:73` (every DIO config),
  `webserver.cpp:1551`, `uart_bridge.cpp:277,284` (RX setup clears the TX
  switch), `quicksetup.cpp:475`, `mp/autorun.cpp:102`. A user running a 5 V or
  12 V DUT on that rail has it silently dropped to 3.3 V by configuring one
  digital input. `target.py:164,170` already works around the route wipe.
  **Do:** if the rail is enabled, keep its voltage; merge the new route into
  the current MUX state instead of replacing it. **Prove:** `[BENCH]` set VADJ1
  to 5 V, configure IO2 as digital input, read VADJ1 back (read-only check
  after a consented rail set). M.
- **IO-5** · P1 · BUG · `[VERIFIED]` - firmware `IO_RELEASE` parses
  `u8 slot_idx, u8 session_id` (`bbp/cmds/cmd_io_owner.cpp:~76-90`); Python
  sends `u8 n, slots[]` (`client.py:~4372-4378`), and desktop
  (`commands.rs:~3497`), the simulator and the docs agree with Python. Real
  releases land on the wrong slot or none. **Do:** change firmware to accept
  `n, slots[]` with the caller's session (four consumers agree); update the
  BBP manifest. New evidence for DESK-6. S.
- **AN-04** (TR-4) · P1 · PERF/BUG · `[VERIFIED]` -
  `tasks_apply_dac_voltage()` (`tasks.cpp:~991-1003`), used by HTTP and BLE,
  always calls `setVoutRangePreservingOutput()`, which parks the output,
  waits and restarts the ADC. The BBP path guards it with
  `range_change_needed` (`tasks.cpp:~1110-1122`). Every HTTP DAC write costs at
  least 25 ms and glitches the output. **Do:** share the guard. S.
- **IO-8** (+ MUX-4) · P1 · BUG · `[REPORTED]` - `mux_set_switch` over BBP and
  HTTP and `tasks.cpp:890` report success when the U17/U23 interlock refused
  (`adgs2414d.cpp:443-447,561-563`). Same class as MUX-4, which is still open
  (`bus_planner.cpp:224-229`). Fix both in one pass. S.
- **IO-3** · P1 · SAFETY · `[REPORTED]`, `[BENCH]` - `config.h:94`
  `U17_DEVICE_IDX = 2`; `bus_planner.cpp:114-126` puts IO 7-9 on MUX device 3
  while `Docs/mainboard-hardware.md:104` says IO9 is U17 / device 2. One of
  them is wrong, and the interlock may guard the wrong device. **Do:** settle
  against the schematic (`PCB Material/Main PCB`) before touching code. S.
- **PLT-06** · P1 · BUG · `[VERIFIED]` - seven upload loops `continue` forever
  on `HTTPD_SOCK_ERR_TIMEOUT` (`webserver.cpp:3823,3935,3994,4326,4590,4633,4781`).
  One stalled client wedges the only httpd task. **Do:** bounded retry count
  (e.g. 5) then abort the upload. S.
- **WEB-23** (TR-5) · P1 · PERF · `[VERIFIED]` loop, impact `[BENCH]` -
  `handle_get_scope_stream()` (`webserver.cpp:748`) runs `for (;;)` inside the
  httpd task with no `httpd_req_async_handler_begin()` and no auth check.
  While a scope tab is open every other request likely stalls. **Do:** move
  to an async handler / dedicated task, or retire SSE in favour of the existing
  WS stream. **Prove:** `curl -N /api/scope/stream` then `curl -m 3
  /api/status` from a second shell. M.
- **BUS-003** · P1 · BUG · `[VERIFIED]` init, limit `[REPORTED]` - external SPI
  bus initialised with `SPI_DMA_DISABLED` (`bus/ext_bus.cpp:310`); ESP-IDF caps
  non-DMA transfers at 64 bytes, but firmware, Python, MCP and docs advertise
  512. **Do:** `SPI_DMA_CH_AUTO` with a DMA-capable buffer (or cap and document
  64). S.
- **BUS-005** · P2 · BUG · `[REPORTED]` - deferred bus `submit_job` reuses
  slots that are DONE but not yet fetched, and the worker runs the lowest slot
  rather than the oldest job (`ext_bus.cpp:~462,540-548`). Results lost and
  reordered. **Do:** FIFO sequence numbers; never reuse an unfetched DONE slot.
  The simulator completes jobs instantly, so add a sim mode that does not
  (BUS-021). M.
- **PLT-10** · P1 · BUG · `[REPORTED]` - BBP OTA session has no timeout or
  abort; a host drop mid-SPIFFS upload leaves the web partition unmounted
  (`cmd_ota.cpp:259`), and desktop never sends abort. **Do:** inactivity
  timeout that aborts and remounts. S.
- **WEB-25** · P2 · SECURITY · `[REPORTED]` - CORS origin check is a prefix
  match (`webserver.cpp:157-158`), so `http://localhost.evil.com` passes. S.
- **WEB-NULL** · P2 · BUG · `[REPORTED]` - several handlers dereference
  `cJSON_GetObjectItem(...)->valueint` without a NULL check
  (`webserver.cpp:1542,2711,2746,2972` and others). A malformed body reboots
  the device. **Do:** grep `GetObjectItem(` without `IsNumber` guard; fix all. S.
- **PLT-05** · P1 · BUG · `[REPORTED]` - `wifi_connect` runs on httpd /
  `bbpCli`, blocks for up to a minute, tears down the AP, and on failure leaves
  the bad SSID configured (`wifi_manager.cpp:318-410`). Over the AP the client
  never receives its own response. **Do:** async connect with status polling,
  keep AP up until STA succeeds, roll back on failure. M.

### Wave C - DAQ HAT firmware (build P4 and C6)

- **C6-20** · P1 · BUG · `[VERIFIED]` - `daq_settings_set_i32()`
  (`Firmware/DAQ_HAT/ESP32P4/src/config/daq_settings.c:~170-174`) calls
  `s_apply` even when the value did not change, and the C6 resends all
  mirrored keys on any menu edit. A brightness change can re-apply sample
  rate, stop acquisition, zero the energy accumulator and rewrite V_DUT.
  **Do:** gate `s_apply` on `changed`, with an explicit `DAQ_F_ACTION` flag for
  keys that are commands; make the C6 send only the edited key. S.
- **C6-21** · P1 · BUG · `[REPORTED]` - P4 -> C6 sync only pushes changes and
  only 2 keys on C6 reappearance; factory reset is never pushed, so the C6 later
  resends stale values. **Do:** full snapshot push on link-up and after reset. M.
- **C6-23** (C6-11) · P1 · BUG · `[VERIFIED]` - C6 `sdkconfig.esp32c6:1178-1179`
  has `CONFIG_ESP_TASK_WDT_PANIC` unset and a 5 s timeout; `main.c:92-94`
  claims "10 s ... triggers a reboot". A hung loop only logs. **Do:** enable
  panic (or `esp_task_wdt_reconfigure` with `trigger_panic = true`). C6-11 is
  not "flashed but unproven" - it cannot work as configured. S.
- **C6-22** (C6-9) · P1 · BUG · `[REPORTED]` - fabricated V/I still render in
  FAULT state (`ESP32C6 main.c:208-216`); Diagnostics fabricates rails with
  `valid = 0xFFFF` (`menu.c:250`) and the PD guard reads that. **Do:** show
  dashes when data is stale. S.
- **C6-25** · P1 · BUG · `[REPORTED]` - an interrupted C6 staging/push wedges
  every later S3-driven C6 update (`relay_stage_begin` rejects stale state; S3
  abort after reboot cannot clear it). **Do:** stale-state timeout or allow
  begin to reset a non-active session. S.
- **DAQ-04** · P1 · BUG · `[REPORTED]` - marker handler
  (`daq_board.c:1768`) and STOP flush (`:654-658`) write the shared
  `frame_buf` from higher-priority tasks with no lock. **Do:** route both
  through the producer via a queue. M.
- **DAQ-05** · P1 · BUG · `[REPORTED]` - missed DRDY edges are never counted;
  at 256 kSPS the profile shows ~23% conversion loss while drop counters read
  ~100 (`data/p4_profile_O2.json`). **Do:** count missed conversions from the
  ADC sequence / timer and report them in STATUS. M.
- **DAQ-06** · P2 · BUG · `[REPORTED]` - registry rates 10k/50k/100k/250k/1M
  produce 8/64/128/256/512 kSPS; x8 filter unreachable (`adaq7769.c:389`).
  **Do:** advertise actual rates, or fix the table. Resolves DOC-1's "1 MSPS"
  question. S.
- **DAQ-03** · P1 · BUG · `[REPORTED]` - markers carry no S3 timestamp; the P4
  stamps arrival (`daq_board.c:1763-1770`) and `sync_epoch` is never read. The
  "exact sample index" claim in docs is false. M.
- **P4-8** - see the corrected entry below; widen to u32 together with DAQ-01.

### Wave D - latency

- **TR-1** · P1 · BUG · `[BENCH]` - `usb_cdc_cli_write()` calls
  `tinyusb_cdcacm_write_queue()` once and returns the count
  (`net/usb_cdc.cpp:192-196`); `sendFrame()` ignores it. TX FIFO is 512 B
  (`sdkconfig.esp32s3:2337`). Frames over the free FIFO space may lose their
  tail. **Confirm first** with one live `QS_GET` or `IO_OWNER_STATUS`; then
  loop until all bytes are queued (with a deadline). S.
- **TR-3** · P1 · BUG · `[BENCH]` - device leaves BBP after 60 s idle
  (`bbp.cpp:90,638-640`) and no host sends a keepalive. Plausible root of
  "every command times out" after idle. **Do:** PING every 20-30 s from the
  Python and Rust transports when idle. **Prove:** idle 70 s, then a command. S.
- **AN-07** · P1 · PERF · `[REPORTED]` - `processScopeStream` holds
  `g_stateMutex` across USB and WS sends (`bbp.cpp:440-477`); ADC poll drops
  data. **Do:** snapshot under lock, send outside it (the SSE handler already
  does this). S.
- **AN-06** · P1 · PERF · `[REPORTED]` - waveform loop spins `taskYIELD()`
  until the next sample (`tasks.cpp:~1680-1685`), which only yields to equal
  or higher priority; lower-priority tasks on that core starve at >= 10 Hz.
  **Do:** esp_timer / `vTaskDelayUntil` pacing. M.
- **AN-10 / AN-11** · P2 · PERF · `[REPORTED]` - ADC stream `div` is never
  applied (`bbp.cpp:758`); advertised 9.6 kSPS is ~555 SPS; acquisition polls
  SPI at <= 1 kHz while `ADC_RDY` is configured but never read
  (`main.cpp:296`). **Do:** ADC_RDY-driven acquisition task. L - largest
  single analog improvement.
- **AN-12** · P2 · PERF · `[REPORTED]` - `CMD_ADC_CONFIG` holds the shared SPI
  mutex across `delay_ms(5)` and `delay_ms(20)` (`tasks.cpp:1153-1179`). S.
- **IO-13** · P2 · PERF · `[REPORTED]` - unconditional 100 ms all-open dead
  time on every MUX write, also when no switch opens. **Do:** dead time only
  for switches going open -> different closed. M.
- **TR-6 / MCP-31** · P2 · PERF · `[REPORTED]` - `configure_io` is 12-19 BBP
  frames plus ~200 ms firmware dead time; first use per block adds 0.5 s.
  **Do:** one lease around the three calls, drop the redundant DISABLED MUX
  step, use firmware auto-claim. Long term: one `IO_CONFIGURE` firmware
  command. M.
- **TR-9** · P2 · PERF · `[REPORTED]` - every HAT/DAQ MCP tool issues an
  uncached `HAT_GET_STATUS` first; firmware adds a UART `GET_DAP_STATUS` the P4
  does not implement. **Do:** cache HAT type per connection, invalidate on
  HAT event. S.
- **TR-7** · P2 · BUG · `[REPORTED]` - `_usb_cmd` retries every opcode after a
  timeout, including I2C/SPI writes and `SCRIPT_EVAL`. **Do:** retry only an
  allow-list of idempotent reads. S.
- **DESK-22..26** · P1/P2 · PERF · `[REPORTED]` - HAT, Voltages, IO Expander,
  USB-PD tabs refetch on every `device-state` tick (300 ms on HTTP); whole
  card grids rebuilt each tick; DAQ transport mutex held across reads up to
  1 s; DAQ view ships ~14k floats of JSON at up to 30 Hz; the HAT probe polls
  forever on a bare board. **Do:** shared poller with per-tab subscription,
  keyed `<For>`, binary Tauri channel for DAQ frames. M.
- **DESK-28** · P2 · PERF · `[REPORTED]` - 30 Hz `adc-stream` event has no
  frontend listener and the ADC tab never stops the stream. S.
- **WEB-28** · P2 · PERF · `[REPORTED]` - no fetch timeouts, so a busy device
  pins all 6 browser connections; only SignalPath uses the backoff helper.
  Scope redraws at 60 fps unconditionally and its SSE error counter never
  resets (latches into 4 req/s polling). M.
- **IOS-23** · P2 · PERF · `[REPORTED]` - ~6 req/s steady state, six
  sequential requests every 5th cycle; status replies carry both camelCase and
  snake_case copies of every field. **Do:** drop the duplicate keys server-side
  once every client reads one form. M.
- **PWR-09 / PWR-10** · P2 · PERF · `[REPORTED]` - DAQ-HAT telemetry measures
  3 rails at 1 Hz (~0.6 s each) in `mainLoopTask` (`hat.cpp:1462`); "cached"
  supply reads run a monitor step in the BBP and HTTP tasks
  (`cmd_selftest.cpp:84`, `api_core.cpp:1000`). **Do:** one background
  sampler, readers return the cache. M.
- **PWR-16** · P3 · PERF · `[REPORTED]` - every `ds4424_set_voltage()` commits
  NVS (`ds4424.cpp:472`). Debounce. S.

### Wave E - refactors

- **TR-11** · P2 · REFACTOR · `[REPORTED]` - `api_core` is not a shared layer
  and the cmd registry backs only a few HTTP routes; HTTP handlers re-implement
  BBP handlers and ignore `sendCommand()` return values, replying `ok:true` when
  the command was dropped. Root of AN-04, the HTTP 200 `{"ok":false}` problem
  (WEB-26 / IOS-21) and most cross-transport drift. **Do:** one C service
  function per capability, BBP and HTTP as thin adapters (the cmd registry
  pattern in `http_adapter.cpp` is the template). L, incremental.
- **FEAT-6** - five hand copies of the error table now (Python constants,
  `error_mapping.py`, Rust, docs, firmware); `error_mapping.py` still has the
  0x13 collision and no 0x14, and its regex never matches `DeviceError`
  text. Generate from `bbp.h`.
- **PY-21** · P2 · REFACTOR · `[REPORTED]` - `client.py` is ~4.8k lines with a
  USB/HTTP branch in most methods. Split into per-capability mixins with a
  transport strategy; extend `Transport` (FEAT-4) so HTTP gaps fail at type
  check. L.
- **IO-2** · P1 · BUG · `[REPORTED]`, `[BENCH]` - break-before-make is undone:
  the readback frame re-sends the old shadow, so the dead time elapses with old
  routes closed (`adgs2414d.cpp:161,425,428`). M.
- **PWR-REFAC** · P2 · REFACTOR - three divergent power-up sequences (Python
  HAL, MCP `target.py`, firmware bus planner; the planner enables the e-fuse
  right after the DCDC with no inrush settle, `bus_planner.cpp:254-266`). One
  firmware `RAIL_POWER_UP(rail, V, settle_ms)` command used by all. M.
- **DESK-REFAC** · P2 - 26 Tauri commands never invoked by the frontend
  (counting method in `DESKTOP.md`); delete or wire. `error_to_string` is dead
  and the UI only shows "Failed" (DESK-33). S.
- **PWR-11** · P2 · SAFETY - the >12 V confirm gate exists only in MCP
  `set_supply_voltage`; MCP `idac_control`, BBP, HTTP, BLE and every UI bypass
  it, and firmware clamps silently. Move the gate to firmware (a confirm flag
  in the command) and surface the clamp. M.

### Wave F - parity features (reuse existing primitives)

- **DESK-3** scripting UI - add `0xF5-0xFD` to `bbp.rs`, Tauri commands, and
  a tab; the HTTP routes already exist. L.
- **DESK-4 / WEB-3** external I2C/SPI UI - IDs already in `bbp.rs:132-138`; no
  commands or tab. M each.
- **MCP-5 / PLT-08** - `run_device_script` returns right after enqueue, so
  logs are always empty (`tools/scripting.py:46-52`); add wait-for-completion
  and script file management tools (12 `script_*` client methods have no
  tool). M.
- **MCP-4** - streaming: add bounded "observe for N seconds" tools rather than
  open streams. M.
- **BUS-011** - MCP has no synchronous I2C read/write, setup, close or status
  tools; deferred jobs cannot do a plain write. An agent cannot write one I2C
  register today. S.
- **BUS-016** - I2C register dump and SPI-flash read helpers on top of existing
  primitives. S.
- **DAQ-13** - per-range meter calibration reachable only from the P4 serial
  console; expose over BBP DAQ sub-ops + MCP, gated (`cal` writes NVS). M.
- **DAQ-10** - `pre_samples` stored but unused, Python capture discards
  pre-trigger data, no trigger-on-current-threshold. M.
- **IOS-5** - unchanged; use `CLIENTS-WEB-IOS.md` route matrix as the
  checklist.
- **DESK-9** - AP password: ID is in `bbp.rs:189`; needs a Tauri command,
  HTTP mapping and UI. S.

### Wave G - RP2040 (blocked: LA HAT not attached)

- **LA-01** · P1 · `[REPORTED]` - triggered capture programs begin with
  `irq wait 0` (`bb_la_trigger.pio:51,58,65`), which may raise rather than
  wait; triggers might fire immediately. Read the PIO ISA before changing.
- **LA-02** · P1 · `[REPORTED]` - `sm_config_set_in_shift(..., false, ...)`
  shifts left, so samples within each word could be in reverse order relative
  to every LSB-first decoder. One-line fix if confirmed on hardware.
- **LA-03** · P1 · `[VERIFIED]` - `handler_hat_la_config()`
  (`bbp/cmds/cmd_hat.cpp:~583-591`) parses 9 bytes and never forwards the RLE
  byte; desktop RLE toggle and Python `rle_enabled` do nothing. The S3 half can
  ship in Wave B.
- **LA-05..LA-12** - see `RP2040-LA-SWD.md` (28-byte readout per round trip;
  `HAT_CMD_LA_USB_SEND` has no BBP mapping and five desktop commands are dead;
  ring overrun check off by one; DAQ branch of `hat_update_leds` unreachable;
  `setup_swd` returns success on failed voltage set; S3 never reads the HAT
  IRQ line).

## Items the 2026-10-01 audit found already fixed or wrong

Each was checked by an audit agent against source; confirm then move to
`CHANGELOG.MD` or delete.

| Item | Finding |
|---|---|
| P4-3 | Fixed: product-ID check precedes `esp_ota_begin()` (`ota.c:63-80`). |
| P4-4 | x8 fixed (`adaq7769.c:316-325`); x16 was never a 16-bit mode - refuted. |
| MCP-9 | Fixed (`bugbuster_mcp/config.py` now names LTM8078). |
| PY-10 | Docstring fixed (`client.py:~2371` says RAM-only); name unchanged. |
| DESK-5 | Fixed: `ota_upload_daq` (`commands.rs:~807`), UI `diag.rs:1105-1172`. |
| WEB-4 | Fixed: P4/C6 upload exists in the web UI. |
| WEB-10 | Fixed: `IoOwnershipCard.tsx`, `FaultsCard.tsx` exist. |
| IOS-3, IOS-4, IOS-9 | Fixed per `CLIENTS-WEB-IOS.md`. IOS-11 mostly fixed. |
| IOS-12 | Wrong: VDUT routes exist (`api_core.cpp:465-516`). |
| IOS-14 | Likely non-issue: ATS exempts IP literals and `.local`. |
| RP-1, RP-3, RP-4 | Addressed in source (RP-3 bounded by cal-window clamp, `bb_hat_v2.c:511-562`); still needs hardware to close. |
| PROTO-4 | Partly fixed: `0xEF` and external bus IDs now in `bbp.rs`; still missing `0xF5-0xFD`, `0x64/0x65`, `0x47`, `EVT 0x88`, `0x0C MEM_STATUS`. |
| PROTO-8 | Not dead: `simulated_device.py:55` uses `ESP32_FW_VERSION`. Update, do not delete. |
| `.mex` docs | `scripting-runtime.md` lists two fixed bugs as open; `esp32-firmware.md` says BBP v9/v10 (source: 11); `desktop-app.md` says 172 commands and describes a tab-switch claim that no longer exists; web manifest says 8 tabs (`App.tsx` has 9). |

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
