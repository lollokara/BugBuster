# BugBuster TODO

> Legend: 🔴 Bug/broken · 🟡 Missing/incomplete · 🟢 Refactor/cleanup · ⏳ Explicitly deferred

---

## Firmware — RP2040 HAT

### 🔴 Bugs

- **`uint8_t` truncation in `bb_la_usb.c`** — remaining byte count is cast to `uint8_t` before the 60-byte clamp, producing zero-payload DATA frames and `duration=0` captures on long runs.  
  File: `Firmware/RP2040/src/bb_la_usb.c`  
  Ref: `.mex/context/la-subsystem.md` → "Known Bugs"

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

The following BBP commands are in `_NOT_YET_SIMULATED` in `tests/simulator/test_sim_completeness.py` — CI passes them as known gaps, but they need handlers before simulator coverage is complete:

| Command | Description |
|---------|-------------|
| `EXT_JOB_SUBMIT` | Queue deferred external I2C/SPI job |
| `EXT_JOB_GET` | Poll deferred job result |
| `EXT_I2C_SETUP` | Configure external I2C bus |
| `EXT_I2C_SCAN` | Scan I2C bus for devices |
| `EXT_I2C_WRITE` | I2C write transaction |
| `EXT_I2C_READ` | I2C read transaction |
| `EXT_I2C_WRITE_READ` | Combined I2C write+read |
| `EXT_SPI_SETUP` | Configure external SPI bus |
| `EXT_SPI_TRANSFER` | SPI transfer |

File: `tests/simulator/test_sim_completeness.py:_NOT_YET_SIMULATED`  
Handlers go in: `tests/mock/handlers/` (new `bus.py` handler module + entry in `simulated_device._register_all_handlers`)

### 🟢 Dead tests cleanup

Two tests in `test_11_hat.py` carry permanent `@pytest.mark.skip` because the CDC streaming data path was removed — stream data now goes to vendor-bulk only. These are dead bodies that will never run; they should be deleted and their coverage replaced by entries in `test_la_usb_bulk.py` if not already covered.  
  File: `tests/device/test_11_hat.py:559` and `:680`

### 🟢 `SimulatedDevice` Code Quality

File: `tests/mock/simulated_device.py`

- **`PROTO_VERSION` is hardcoded** (`= 7`) instead of imported from `bugbuster.protocol.BBP_PROTO_VERSION`. Any protocol version bump requires two edits and risks silent drift.  
  Fix: `from bugbuster.protocol import BBP_PROTO_VERSION; PROTO_VERSION = BBP_PROTO_VERSION`

- **`_register_all_handlers()` boilerplate** — 12 separate `try/except ImportError` blocks. Refactor to a declarative list of module paths with a single registration loop; makes missing/broken handlers immediately visible instead of silently skipped.

- **No type annotations** on `dispatch()`, `http_dispatch()`, `emit_event()`, `tick()`. Add `bytes`, `dict`, `int`, `None` annotations for IDE/mypy support.

- **Static `fw_version = (1, 0, 0)`** — not wired to any real version source, so version-sensitive tests hardcode magic tuples. Consider deriving from or at least co-locating with the protocol constants.

- **`uart_config` under-populated** — initialises a single UART bridge entry; firmware supports multiple. Pre-populate a realistic set so device tests don't silently skip multi-bridge scenarios.

- **`http_dispatch()` re-imports on every call** — `from tests.mock import http_routes` runs on every HTTP dispatch. Cache the import or load it during `_register_all_handlers()`.

- **HAT state is a bare `bool`** — `hat_present` gives no depth. Rail voltages, calibration flags, and LA state are tracked ad-hoc across handler modules. Introduce a `HatState` dataclass (or a `hat` sub-object on the device) so handler modules share a structured HAT model.

---

## Cross-Stack — Pending Parity Work

### 🟡 Overview / Selftest Worker / Quick Setup

Pattern file: `.mex/patterns/overview-selftest-quicksetup-parity.md`

- **Selftest worker toggle** — `workerEnabled` and `supplyMonitorActive` not yet returned by `/api/selftest`. Worker default must be OFF on fresh flash.
- **CH-D diagnostic reservation** — CH-D overlay should follow `supplyMonitorActive`; desktop fallback for older 25-byte USB selftest payloads needed.
- **Quick-setup: firmware + web still missing** — desktop Tauri commands (`quicksetup_list/get/save/apply/delete` in `commands.rs:1032`) and the Overview tab UI are already wired. What's missing is the **ESP32 firmware** side (BBP handlers + `/api/quicksetup/*` HTTP routes) and the **on-device web UI** wiring. Slot JSON must stay under `BBP_MAX_PAYLOAD` (cap at 1 KB).

Verify checklist lives in the pattern file above; do not mark done until all firmware + web + desktop checks pass.

---

## Version / Protocol Hygiene

- **`PROTO_VERSION` CI gate is path-scoped** — `.github/workflows/proto-version-check.yml` runs `check_proto_version.py` only when `bbp.h`, `protocol.py`, or `bbp.rs` are in the diff. A bump that touches none of those files (e.g. a refactor that renames a constant in a fourth location) would bypass the check. Consider adding the workflow to the general `push` trigger or as a required pre-merge step regardless of diff path.  
  Ref: `.github/workflows/proto-version-check.yml`
