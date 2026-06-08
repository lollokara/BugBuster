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

### 🔴 Bugs

- **Unused Dead Code `#define ADMIN_TOKEN`** — [config.h](file:///Users/lorenzo/Documents/Sviluppo/BugBuster/Firmware/ESP32/src/config.h#L119) contains a static fallback token definition that is never used. Only `ADMIN_TOKEN_HEADER` is used.

### 🟡 Missing / Pending

- **3-Wire RTD Mode Support** — Expose 3-wire RTD configurations in [tasks.cpp](file:///Users/lorenzo/Documents/Sviluppo/BugBuster/Firmware/ESP32/src/tasks.cpp#L1180) (setting `RTD_MODE_SEL=0`, `MUX=3`) and update BBP `SET_RTD_CONFIG` command parameters. Currently, it is hardcoded to 2-wire mode.

- **Expose and Map HAT Power Commands over HTTP** — Implement `GET /api/hat/power` and `POST /api/hat/power` in [webserver.cpp](file:///Users/lorenzo/Documents/Sviluppo/BugBuster/Firmware/ESP32/src/web/webserver.cpp) to query/toggle HAT rail power.

- **SPIFFS OTA over WiFi and USB** — Add a first-class OTA flow for the SPIFFS partition over both transports, including upload, integrity verification, progress reporting, and final apply/reboot handling. Desktop and firmware currently cover firmware OTA, but SPIFFS still needs the same WiFi + USB path.

### ⏳ Deferred

- **HVPAK controls** — command IDs `0xCC–0xEA` are protocol-defined and stubbed in ESP32/Python/Desktop, but the feature is not yet a release target. Allowlisted in `tests/unit/test_hat_parity.py`.  
  Unblock: decide HVPAK release milestone, then implement Python/Desktop/web surfaces end-to-end.

### 🟢 Forward-looking note

- **AD74416H coordinated multi-channel DAC latch** — `WAIT_LDAC_CMD=0` (reset default) means each `setDacCode()` takes effect immediately. A `// TODO` in `ad74416h.cpp:185` notes that if `WAIT_LDAC_CMD` is ever enabled for glitch-free simultaneous updates, the CMD_KEY write must move to a dedicated `latchAllDacs()` rather than firing per single-channel write.  
  File: `Firmware/ESP32/src/hal/ad74416h.cpp:185`  
  Priority: low — only relevant when/if coordinated multi-channel mode is adopted.

---

## Desktop App

### 🔴 Bugs

### 🟡 Missing / Incomplete

- **Scope export** — PNG and JSON export paths show "not implemented yet" toasts.  
  File: `DesktopApp/BugBuster/src/tabs/scope.rs:1405–1406`

- **Scope recording semantics** — web Scope tab is Partial vs desktop: not all streaming/recording controls are mirrored over HTTP.  
  Ref: `Firmware/ESP32/web/docs/desktop-parity-matrix.md`

- **Calibration deep flows** — HAT calibration telemetry is shown in the web HAT card, but desktop-specific import flows and deep cal workflows have no web equivalent.  
  Ref: parity matrix row "Calibration tab deep flows"

- **Extend `parse_hat_get_power` in Tauri Backend** — Parse the trailing 3 HVPAK bytes (`hvpak_part`, `hvpak_ready`, `hvpak_last_error`) written by the firmware in `parse_hat_get_power` in [commands.rs](file:///Users/lorenzo/Documents/Sviluppo/BugBuster/DesktopApp/BugBuster/src-tauri/src/commands.rs#L2557).

### 🟢 Stubs awaiting firmware wiring

- **`set_pin_drive_strength`** — Tauri command only logs the request (`pin`, `drive`); no BBP command or firmware handler exists yet. The comment says hardware wiring is "pending firmware support" (likely via PCA GPIO expander controlling series-resistor bypass switches).  
  File: `DesktopApp/BugBuster/src-tauri/src/commands.rs:2249`

- **`set_efuse_config`** — Tauri command only logs `efuse`, `sw_limit_ma`, `enabled`; no BBP command or firmware handler exists. UI in `board.rs` calls this for per-efuse software current-limit configuration.  
  File: `DesktopApp/BugBuster/src-tauri/src/commands.rs:2264`

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

### 🔴 Bugs

- **`BBP_CMD_PCA_GET_STATUS` (0xB0) Payload Size Mismatch** — `_parse_pca_status` in [client.py](file:///Users/lorenzo/Documents/Sviluppo/BugBuster/python/bugbuster/client.py#L4323) reads/parses only 12 bytes, truncating the 9 trailing enable bytes written by the firmware.

### 🟢 Refactor

- **Batch IO-owner claim+write** — `client.py:4072` has `TODO: batch claim+write into one frame in v6`. Currently does two round-trips when it could be one.  
  File: `python/bugbuster/client.py:4072`

---

## Simulator / Tests — `SimulatedDevice` Refactor

### 🔴 Simulator Bugs

- **`BBP_CMD_PCA_GET_STATUS` (0xB0) Payload Size Mismatch** — Mock `_pca_get_status` handler in [power.py](file:///Users/lorenzo/Documents/Sviluppo/BugBuster/tests/mock/handlers/power.py#L34) only returns 12 bytes instead of 21 bytes.

### 🟢 `SimulatedDevice` Code Quality & Enhancements

- **Static `fw_version` in Simulator** — `simulated_device.py` uses static `fw_version = (1, 0, 0)` which is not derived from any project version constants.

- **Expand Simulator `uart_config`** — Simulator pre-populates only a single UART bridge config; refactor to pre-populate multiple to match physical capabilities.

- **Expand Simulator `hat_power`** — Refactor mock `device.hat_power` to be an array of 2 booleans representing both power connectors independently.

---

## Cross-Stack — Pending Parity Work

### 🟡 Overview / Selftest Worker / Quick Setup

Pattern file: `.mex/patterns/overview-selftest-quicksetup-parity.md`

- **Selftest worker toggle** — `workerEnabled` and `supplyMonitorActive` not yet returned by `/api/selftest`. Worker default must be OFF on fresh flash.
- **CH-D diagnostic reservation** — CH-D overlay should follow `supplyMonitorActive`; desktop fallback for older 25-byte USB selftest payloads needed.
- **Quick-setup: firmware + Python + simulator + web UI now complete** — Desktop Tauri commands (✅), ESP32 BBP handlers `0xF0–0xF4` in `cmd_wifi.cpp` (✅), HTTP routes in `webserver.cpp` (✅), `quicksetup.cpp/h` NVS store (✅), Python `client.py` `quicksetup_list/get/save/apply/delete` (✅ 2026-06-06), simulator `handlers/quicksetup.py` + HTTP routes in `http_routes.py` (✅ 2026-06-06), on-device web UI wiring in `Overview.tsx` and `QuickSetupTile.tsx` (✅ 2026-06-07).

---

## Version / Protocol Hygiene

- **`PROTO_VERSION` CI gate is path-scoped** — `.github/workflows/proto-version-check.yml` runs `check_proto_version.py` only when `bbp.h`, `protocol.py`, or `bbp.rs` are in the diff. A bump that touches none of those files would bypass the check.

- **Broaden CI Protocol Version Check** — Update the CI workflow to trigger on all pushes/PRs or expand checked files to prevent protocol drift.
