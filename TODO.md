# BugBuster TODO

> Legend: 🔴 Bug/broken · 🟡 Missing/incomplete · 🟢 Refactor/cleanup · ⏳ Explicitly deferred

---

## Firmware — RP2040 HAT

### 🔴 Bugs

*(none — `uint8_t` truncation in `bb_la_usb.c` was already fixed; both LIVE STREAM and ONE-SHOT paths use `uint32_t` intermediates with safe post-clamp cast. Verified 2026-06-06.)*

### 🟡 Missing / Pending

- **LA bench validation** — T-LA-01 and T-LA-02 (1 MHz, 4-ch continuous capture with rearm across consecutive runs) not yet run on real hardware.  
  Ref: `.mex/context/la-subsystem.md`

- **GPIO8 IRQ bench validation** — power fault and LA-done events are wired to fire a 2 ms active-low pulse on GPIO8; real-hardware verification pending.  
  Ref: `Firmware/RP2040/README.md:97`

- **LA_DONE/LA_LOG delivery latency bench-test** — unsolicited LA events are now dispatched after releasing `s_hat_mutex` to avoid a deadlock with `g_stateMutex`. The sequencing change (events arrive after the command response) should be verified under load on real hardware.  
  File: `Firmware/ESP32/src/hat/hat.cpp:385`

---

## Firmware — ESP32

### ⏳ Deferred

- **HVPAK controls** — command IDs `0xCC–0xEA` are protocol-defined and stubbed in ESP32/Python/Desktop, but the feature is not yet a release target. Allowlisted in `tests/unit/test_hat_parity.py`.  
  Unblock: decide HVPAK release milestone, then implement Python/Desktop/web surfaces end-to-end.

### 🟢 Forward-looking note

- **AD74416H coordinated multi-channel DAC latch** — `WAIT_LDAC_CMD=0` (reset default) means each `setDacCode()` takes effect immediately. A `// TODO` in `ad74416h.cpp:185` notes that if `WAIT_LDAC_CMD` is ever enabled for glitch-free simultaneous updates, the CMD_KEY write must move to a dedicated `latchAllDacs()` rather than firing per single-channel write.  
  File: `Firmware/ESP32/src/hal/ad74416h.cpp:185`  
  Priority: low — only relevant when/if coordinated multi-channel mode is adopted.

---

## Desktop App

### 🟡 Missing / Incomplete

- **Scope export** — PNG and JSON export paths show "not implemented yet" toasts.  
  File: `DesktopApp/BugBuster/src/tabs/scope.rs:1405–1406`

- **Scope recording semantics** — web Scope tab is Partial vs desktop: not all streaming/recording controls are mirrored over HTTP.  
  Ref: `Firmware/ESP32/web/docs/desktop-parity-matrix.md`

- **Calibration deep flows** — HAT calibration telemetry is shown in the web HAT card, but desktop-specific import flows and deep cal workflows have no web equivalent.  
  Ref: parity matrix row "Calibration tab deep flows"

### 🟢 Stubs awaiting firmware wiring

- **`set_pin_drive_strength`** — Tauri command only logs the request (`pin`, `drive`); no BBP command or firmware handler exists yet. The comment says hardware wiring is "pending firmware support" (likely via PCA GPIO expander controlling series-resistor bypass switches).  
  File: `DesktopApp/BugBuster/src-tauri/src/commands.rs:2249`

- **`set_efuse_config`** — Tauri command only logs `efuse`, `sw_limit_ma`, `enabled`; no BBP command or firmware handler exists. UI in `board.rs` calls this for per-efuse software current-limit configuration.  
  File: `DesktopApp/BugBuster/src-tauri/src/commands.rs:2264`

### 🟢 Dead code / cleanup

- **`calibration.rs` is nearly empty** — after the Calibration tab was merged into Voltages, the file only exports `pub const SLOTS: &[u8] = &[];`. Either delete the file and update `mod.rs`, or document why the constant still needs to live here.  
  File: `DesktopApp/BugBuster/src/tabs/calibration.rs`

---

## ESP32 Web UI

### 🟡 Partial

- **Voltages standalone tab** — supply values shown via Overview/System cards, but there is no dedicated Voltages tab in the on-device web UI equivalent to the desktop tab.  
  Ref: parity matrix row "Voltages tab dedicated panel"

### ⏳ Deferred

- **Logic Analyzer streaming** — USB vendor-bulk path; no HTTP stream parity is architecturally possible. Needs explicit "USB only" messaging in the web UI.  
  Ref: parity matrix row "Logic Analyzer stream"

- **HVPAK web controls** — mirrored from firmware deferral above; keep HTTP/Desktop/MCP parity deferred until HVPAK is a release target.

---

## Python Client

### 🟢 Refactor

- **Batch IO-owner claim+write** — `client.py:3901` has `TODO: batch claim+write into one frame in v6`. Currently does two round-trips when it could be one.  
  File: `python/bugbuster/client.py:3901`

---

## Simulator / Tests — `SimulatedDevice` Refactor

### 🟡 Missing Handlers

✅ **Done 2026-06-06** — All 9 EXT bus commands now have handlers in `tests/mock/handlers/bus.py`. `_NOT_YET_SIMULATED` set removed entirely from `test_sim_completeness.py`. 408 unit + 231 sim tests passing.

### 🟢 `--sim-full` HTTP transport testing

✅ **Done 2026-06-06** — `tests/mock/sim_http_server.py` (WSGI server backed by SimulatedDevice), `--sim-full` pytest flag in `conftest.py`, and `tests/integration/test_sim_http_transport.py` (6 transport-layer tests: GET roundtrip, POST admin-token enforcement, 401/404 HTTP error mapping). `check_admin_auth` in `http_routes.py` updated to case-insensitive header lookup. 6/6 integration tests pass; 408 unit + 231 sim unchanged.

### 🔴 Simulator bugs found by deep dive (2026-06-06)

Trace report: `.omc/specs/deep-dive-trace-sim-bugs-missing-impl.md`

✅ **Fixed 2026-06-06** (413 unit + 231 sim passing):
- `tests/mock/handlers/hat.py:770` — added `return handler` to `_hat_set_led_state`
- `tests/mock/handlers/gpio.py:156` — `DIO_READ` now returns `d.get("input")` (was "output")
- `tests/mock/handlers/misc.py:177` — `WIFI_CONNECT` now sets `device.wifi_connected = True`

**HTTP↔BBP transport divergences:**
✅ **Fixed 2026-06-06** — All 4 divergences resolved:
- `http_routes.py` MUX switch: now enforces one-switch mutual exclusion + updates `adgs_active` (matches BBP handler)
- `http_routes.py` channel func=0: now resets all 5 ADC fields (adc_raw/value/range/rate/mux)
- `handlers/misc.py` USBPD_GET_STATUS: now derives voltage_v/power_w from live `device.usbpd_voltage` via `_USBPD_CODE_TO_V` table; all 6 PDOs reported detected
- `handlers/core.py` DEVICE_RESET: now resets `la_state` to "IDLE" (matches HTTP handler)

**Structural (low priority):**
- `PCA_SET_CONTROL` writes `device.pca_control` dict that no GET ever reads (write-only island) — `handlers/power.py:58-62`
- `IdacChannel` namedtuple missing `step_mv` field — consumed from wire (`idac.py:68`) but not exposed (`client.py:78`)
- `bus.py` state fields (`i2c_devices`, `job_queue`, etc.) not declared in `SimulatedDevice.__init__`

### 🟢 Dead tests cleanup

✅ **Done 2026-06-06** — `test_la_cdc_stream_five_seconds_legacy` and `test_la_cdc_stream_duration_truth_legacy` deleted from `test_11_hat.py`. The orphaned `_find_rp2040_cdc_port()` helper and its section header were removed too.

### 🟢 `SimulatedDevice` Code Quality

File: `tests/mock/simulated_device.py`

✅ **Done 2026-06-06** — PROTO_VERSION now imported from `bugbuster.protocol`, `_register_all_handlers()` refactored to declarative module list + single loop, type annotations added to `dispatch/http_dispatch/emit_event/tick`, `http_routes` import moved to module level, `HatState` dataclass introduced (replaces bare `hat_present` bool across `simulated_device.py`, `handlers/hat.py`, `simulated_transport.py`, `http_routes.py`). 408 unit + 231 sim tests passing.

- **Static `fw_version = (1, 0, 0)`** — not wired to any real version source, so version-sensitive tests hardcode magic tuples. Consider deriving from or at least co-locating with the protocol constants.

- **`uart_config` under-populated** — initialises a single UART bridge entry; firmware supports multiple. Pre-populate a realistic set so device tests don't silently skip multi-bridge scenarios.

---

## Cross-Stack — Pending Parity Work

### 🟡 Overview / Selftest Worker / Quick Setup

Pattern file: `.mex/patterns/overview-selftest-quicksetup-parity.md`

- **Selftest worker toggle** — `workerEnabled` and `supplyMonitorActive` not yet returned by `/api/selftest`. Worker default must be OFF on fresh flash.
- **CH-D diagnostic reservation** — CH-D overlay should follow `supplyMonitorActive`; desktop fallback for older 25-byte USB selftest payloads needed.
- **Quick-setup: firmware + Python + simulator now complete** — Desktop Tauri commands (✅), ESP32 BBP handlers `0xF0–0xF4` in `cmd_wifi.cpp` (✅), HTTP routes in `webserver.cpp` (✅), `quicksetup.cpp/h` NVS store (✅), Python `client.py` `quicksetup_list/get/save/apply/delete` (✅ 2026-06-06), simulator `handlers/quicksetup.py` + HTTP routes in `http_routes.py` (✅ 2026-06-06). Remaining: **on-device web UI** wiring in `Firmware/ESP32/web/src/` (Overview quick-setup section). Verify checklist in pattern file above.

Verify checklist lives in the pattern file above; do not mark done until all firmware + web + desktop checks pass.

---

## Version / Protocol Hygiene

- **`PROTO_VERSION` CI gate is path-scoped** — `.github/workflows/proto-version-check.yml` runs `check_proto_version.py` only when `bbp.h`, `protocol.py`, or `bbp.rs` are in the diff. A bump that touches none of those files (e.g. a refactor that renames a constant in a fourth location) would bypass the check. Consider adding the workflow to the general `push` trigger or as a required pre-merge step regardless of diff path.  
  Ref: `.github/workflows/proto-version-check.yml`
