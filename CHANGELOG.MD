# CHANGELOG

## [Unreleased — post-v1.0.0 docs sweep + bug-fix pass] — 2026-05-04

Cross-repo documentation refresh and tracked-backlog clearance. No public
API or wire-protocol breakage; one new BBP opcode (`0xEF`) and one new
HTTP endpoint (`POST /api/wifi/ap_password`) added in a backward-compatible
way. All 326 unit tests pass; ESP32 PIO build and Desktop wasm/host
`cargo check` are green. Hardware bench validation pending for RP2040 LA
ring-overflow path, ESP32 AP-password live change, and SWD detect.

### Added

#### Documentation
- New `Docs/QuickSetup.md` — user guide for the 4-slot NVS-backed preset
  system (HTTP CRUD, BBP `0xF0..0xF4`, desktop tile UI, 1000-byte limit).
- New `Docs/MicroPython-Troubleshooting.md` — 10-entry FAQ covering eval
  queue, heap watermark, REPL close code 4002, persistence vs ephemeral,
  `vfs_posix` stdout routing, log cursor (`?since=`), upload + autorun.
- New `Docs/bugs-improvements-2026-05-04.md` — backlog snapshot from the
  sweep (now resolved or rescoped).
- New host-stack Mermaid diagram in root `README.md` (Desktop + MCP +
  Python lib ↔ ESP32 BBP v4 ↔ RP2040 HAT).
- `Firmware/ESP32/README.md` gained MicroPython Scripting, Board Profiles,
  and External Bus subsections; `README_CALIBRATION.md` notes board-profile
  scoping and on-device script access.
- `Firmware/HAT_Architecture.md` §6 BBP table extended with HVPAK advanced
  range `0xDB–0xDF` / `0xE5–0xEA`. `Firmware/HAT_Protocol.md` gained
  §5.4 covering HVPAK backend commands `0x14–0x1F`.
- `Firmware/BugBusterProtocol.md` Appendix A: HVPAK advanced `0xDB–0xEA`,
  ext-bus `0x75/0x76` and `0xB8–0xBE`, QuickSetup `0xF0–0xF4`,
  `WIFI_SET_AP_PASSWORD 0xEF`.
- `Firmware/FirmwareStructure.md` updated for Vite/Preact `web/` build,
  MicroPython component, `bus/ext_bus`, `quicksetup` modules.

#### CI / tooling
- New `Firmware/tools/check_proto_version.py` plus
  `.github/workflows/proto-version-check.yml` — gates `BBP_PROTO_VERSION`
  sync across `Firmware/ESP32/src/bbp/bbp.h`, `python/bugbuster/protocol.py`
  and `DesktopApp/BugBuster/src-tauri/src/bbp.rs` on every push/PR.
- `Firmware/tools/firmware_version.py` extended to handle the new RP2040
  single-source CMake form and cross-check the legacy `bb_main.c` `#ifndef`
  fallback.
- `python/pyproject.toml` (new): PEP 517 metadata, classifiers,
  `requires-python = ">=3.11"`, `bugbuster-mcp` console-script entry.

#### ESP32 firmware
- New BBP command `BBP_CMD_WIFI_SET_AP_PASSWORD = 0xEF` and HTTP endpoint
  `POST /api/wifi/ap_password` (admin-token gated). Password persists in
  NVS, falls back to the firmware default if absent or invalid.
- Python lib (`BugBuster.wifi_set_ap_password`) and MCP tool
  (`wifi_set_ap_password`) wired to the new opcode (USB + HTTP).
- `wifi_manager.cpp` now calls `esp_task_wdt_reset()` around the blocking
  `esp_wifi_scan_start()` and inside the per-AP copy loop.

#### RP2040 firmware
- Single source of truth for the HAT firmware version: `PROBE_VERSION` in
  `Firmware/RP2040/CMakeLists.txt` is parsed at configure-time and
  injected as `BB_HAT_FW_MAJOR/MINOR` via `target_compile_definitions`.
  `bb_main.c` retains `#ifndef` guards as a standalone-IDE-build fallback.
- LA status response now exposes `ring_overflow` as a backward-compatible
  trailing byte (response grew from 28 to 29 bytes); flag clears on next
  `HAT_LA_ARM`.

#### Desktop
- `din` and `dout` tabs registered in `src/app.rs` TABS array and content
  router; underlying Tauri commands were already wired.
- HTTP admin tokens migrated to the OS keychain via the `keyring`
  crate (macOS Keychain / Windows DPAPI / Linux Secret Service). One-time
  migration imports any existing `tokens.json` and deletes the plaintext
  file on success; in-memory fallback only on keyring failure (no
  silent plaintext rewrite).

#### Python library
- `client.py` raises `ProtocolError` (with command name) on short
  responses for `PING`, `GET_DEVICE_INFO`, `GET_ADC_VALUE`, `SCRIPT_LOGS`,
  and on missing `states` key in the `MUX_GET_ALL` HTTP path. Helper
  `_require_resp_len()` introduced.

#### Tests / notebooks
- 5 Jupyter notebooks rewritten to target the current architecture
  (`bugbuster.BugBuster`/`BugBusterHAL`, BBP v4, current PIO/CMake/cargo
  flows): `setup_environment`, `esp32s3_flash_manager`,
  `rp2040_firmware_build`, `desktop_app_launcher`, `test_dashboard`.
- `tests/README.md` documents the 5-layer architecture
  (unit/simulator/mock/synthetic/device), markers, env vars, and
  cross-platform setup. `tests/run_tests.py` and `tests/setup.sh` refreshed.

### Changed
- Versions in root `README.md`, `DesktopApp/BugBuster/README.md`,
  `tests/README.md` brought in line with the v1.0.0 release (ESP `3.1.0`,
  HAT `bb-hat-2.1`, Desktop `0.6.0`; test count `249`).
- `CLAUDE.md` replaced empty template with a pointer to `AGENTS.md` and
  `.mex/ROUTER.md`.
- `Docs/ReleaseChecklist.md`: combined-release subsection added; manual
  PROTO_VERSION grep replaced with a pointer to the new CI gate; RP2040
  two-file warning replaced with the single-source rule.
- `Docs/ExternalBus.md` pending-items list reformatted as `## Roadmap`
  with explicit status labels.
- MCP tool inventory in `python/bugbuster_mcp/README.md` updated to 41
  tools (was advertised as 28; actual count had drifted to 40 before
  the WiFi AP-password tool added the 41st).
- `Docs/fix_plan_progress.md` gained `## 2026-04-27` date headers.

### Moved
- `Docs/knowledge.md` → `.mex/context/knowledge-deep-dive.md` (149 KB
  engineering deep-dive moved out of user-facing `Docs/`).

### Fixed
- `Firmware/RP2040/README.md` no longer contradicts itself on the SWD
  detect / LA-done IRQ status; both states described accurately. The
  SWD target-detect implementation in `bb_swd.c` (line-reset + DPIDR
  read) was already present — the previous "STUB" note was stale.
- `wifi_connect()` no longer re-applies the firmware default when an
  NVS-backed AP password is in use (was a `WIFI_PASSWORD` literal trap).

### Notes (bench-pending)
- RP2040 LA ring-overflow flag end-to-end propagation under sustained
  DMA pressure.
- ESP32 AP-password change applied live (vs requiring an AP restart).
- RP2040 SWD detect: turnaround bit-position and parity drain need
  hardware confirmation.

## [BugBuster v1.0.0 — ESP 3.1.0 / HAT bb-hat-2.1 / Desktop 0.6.0] — 2026-04-27

First **combined release** — a single GitHub release (`release-v1.0.0`)
bundles the Desktop installers, ESP32-S3 firmware images and RP2040 HAT
firmware artifacts, alongside a top-level `SHA256SUMS.txt`.

### Added

#### Release engineering
- New **Combined Release** CI workflow (`.github/workflows/release.yml`):
  pushing a `release-v*` tag (or running it via `workflow_dispatch`) builds
  RP2040 + ESP32 + Desktop in parallel via `workflow_call` and assembles a
  single draft GitHub Release with every artifact attached.
- The three component workflows gain a `workflow_call` trigger so they can
  be invoked as reusable workflows. Per-component tag releases
  (`hat-fw-v*`, `esp-fw-v*`, `desktop-v*`) still work independently.
- Desktop crates now declare `description`, `authors`, `license`,
  `repository` and `homepage`, plus a `[package.metadata] release-date`.

#### On-device MicroPython scripting
- Vendored `micropython` submodule (forked at `lollokara/micropython`,
  branch `bugbuster`, single-patch on `extmod/vfs_posix_file.c` so ESP
  `sys.stdout` / `sys.stderr` route through the BugBuster HAL ring even
  when SPIFFS imports promote stdio to POSIX-VFS file objects).
- MicroPython VFS enabled with BugBuster shims; example scripts and
  scripting documentation included.
- Scripting REST surface: `/api/scripts/*` with mode reported as a
  string enum, `last_run_id` correctly carried across `status_set_done`
  resets, autorun success predicate fixed (`&&` not `||`).
- WebSocket REPL session protected by a 10 s auth-frame deadline.
- GIL released around `vTaskDelay` in `mp_hal_delay_ms` (V2-G prep).
- Soft-GC watermark now triggers `gc_collect()` after eval.
- Atomic increment for `s_next_id` in `scripting_run_string`/`run_file`
  and for `s_evtSeq` in BBP — no more duplicate IDs / sequence numbers.

#### BBP / firmware refactor
- BBP split into **adapters and command modules** (`bbp_adapter_dispatch`
  + `bbp_codec.h` shared codec helpers — local helpers in `bbp.cpp`
  removed).
- Admin authentication enforcement: `CMD_FLAG_ADMIN_REQUIRED` now
  honoured in `bbp_adapter_dispatch`, `handle_bus_post_dispatch` gates
  bus POSTs, `handler_get_admin_token` bounds its memcpy.
- ScriptEval `ArgSpec` table now declares the missing `flags` field.

#### Hardware / HAL
- **HAT PCB final revision shipped.**
- Final IO mapping for the 12-IO ESP32 surface; PCB-only hardware path
  (`BREADBOARD_MODE` removed — see Removed).
- I²C and SPI wrappers exposed in both Python client and device.
- Dual-HVPAK / MUX / EFUSE refinements; PCA9535 fault-response path
  applies the EFUSE3/4 silkscreen swap.
- HUSB238: corrected `PD_STATUS0` / `STATUS1` section comment labels.

#### Networking / web
- ESP32 web assets refreshed; new MCP4725 example asset shipped.
- Localhost-only CORS policy in `http_adapter` (replaces wildcard).
- CORS `Access-Control-Allow-Origin` lifetime now spans the response.
- HTTP `OOM` path on body deserialization nulls `body_out`/`body_len_out`
  to prevent double-free.

### Changed

- All BBP-gated `serial_print*` call sites in `cli.cpp`, `tasks.cpp`,
  `web` and `main.cpp` now route through `term_*` so post-BBP-init
  output respects the BBP framing gate (no more raw bytes on the wire
  after the protocol comes up). Pre-BBP sites are explicitly annotated.
- ESP32 firmware bumped 3.0.0 → **3.1.0**; RP2040 HAT bumped
  bb-hat-2.0 → **bb-hat-2.1**; Desktop bumped 0.5.0 → **0.6.0**.

### Removed

- **`BREADBOARD_MODE` config flag** — hardware is PCB-only now; the
  breadboard build path was unused.
- Dead `s_adcStreamDiv` / `s_adcDivCount` write-only ADC divider state
  in `bbp.cpp`.
- Unused includes in `bbp.cpp` (`esp_timer.h`, `esp_wifi.h`, `math.h`).
- Duplicate `voltage_from_selected_pdo` in `husb238.cpp`.
- Dead first `memcpy` in `adgs_get_api_states`.
- Duplicate `coredump_diag_print_boot_report` at early boot.
- Duplicate `mp_hal_set_interrupt_char` strong definition in micropython
  port.

### Fixed (concurrency / ordering — Wave 4A / 4B / 4C)

- **Mutex / lock-rank discipline**: `g_stateMutex` now held before
  hardware mutation in `gpio_config` / `gpio_output`; `tasks_apply_*`
  return `false` and log on `g_stateMutex` timeout instead of silently
  proceeding; `g_spi_bus_mutex` held across the full RMW in
  `startAdcConversion` / `enableAdcChannel` / `clearAdcDataReady`;
  `bus_planner` MUX-program / VADJ ordering swapped to unwind a
  lock-rank inversion.
- **Selftest serialization**: `s_selftest_mutex` now serializes
  `read_channel_d` callers; the two 5 s blocking delays in
  `selftest_measure_internal_supplies` removed.
- **Task creation**: `xTaskCreatePinnedToCore` return is now checked
  for `adcPoll` / `faultMon` / `cmdProc` / `wavegen`.
- **WaveGen**: `taskWavegen.stillActive` initialised `true` so the task
  survives `g_stateMutex` timeout.
- **Network state**: `s_sta_connected` / `s_sta_ip` guarded by spinlock
  in `wifi_manager`; `uart_bridge` config struct guarded by per-bridge
  spinlock; racy `tinyusb_cdcacm_read` fallback removed from
  `usb_cdc_cli_read`.
- **HAT LA event delivery**: unsolicited LA events now buffered in
  `hat_command_internal` and dispatched after the mutex is released.
- **DIO bus-planner ordering**: `bus_planner_route_digital_input` now
  called before `dio_configure` at all bypass sites.
- **PCA9535 fault ISR** install deferred until `g_stateMutex` exists
  during boot.

### Fixed (correctness / safety)

- **Stack smash** in `cli/edit`: `snprintf` truncation and tail length
  now clamped.
- **`vlogic_v` bounds check** [1.2, 3.6] V added in
  `bus_planner_apply_i2c` / `apply_spi`.
- **Web selftest**: cJSON channel deref guarded in calibrate handler.
- **UART bridge**: `bus_planner` now called before pin assignment in
  `install_uart`.
- **Scripting persistence mode** propagated correctly in
  `scripting_run_file`.

### Build / repo hygiene

- Submodule config: `Firmware/ESP32/components/micropython/micropython`
  ignores untracked files (so a build that drops `__pycache__` etc.
  doesn't dirty the parent working tree).
- 3.4 GB of stale `.omx/team` session state removed from the working
  copy (not history-rewritten — strictly local cleanup).

### Known issues

- LA bench proof for 1 MHz / 4-ch capture (T-LA-01, T-LA-02) and the
  physical LA-done IRQ wire from RP2040 GPIO8 to ESP32 still pending
  hardware validation sessions.
- `gh release` for the combined workflow currently produces a **draft**
  release — promote it to public manually after validating the
  installers.

---

## [ESP 3.0.0 / HAT bb-hat-2.0 / Desktop 0.5.0] — 2026-04-16

Three-component coordinated release. Protocol spec bumped to v3.0.0 / BBP wire
v4. Tags: `esp-fw-v3.0.0`, `hat-fw-v2.0`, `desktop-v0.5.0`.

### ⚠ Breaking changes

- **BBP handshake is now 14 bytes** (magic[4] + proto[1] + fw[3] + mac[6]).
  Hosts that hard-coded an 8-byte read must be updated. The Python
  `USBTransport` accepts both, emitting a `log.warning` on the legacy path.
- **`GET_STATUS` response grew 155 → 167 bytes** (per-channel stride 28 → 30
  with `channel_alert_mask`; new 4-byte trailing `mux_states` block).
- **Mutating HTTP routes require `X-BugBuster-Admin-Token`.** The token is
  fetched once over USB via the new `GET_ADMIN_TOKEN` (0x74) opcode and keyed
  on the device MAC.
- **`/api/device/info` now emits `macAddress`** (and `mac_address` alias);
  older firmware omits it — the desktop app will reject HTTP connections to
  firmware that cannot expose a MAC with a clear "firmware too old" message
  instead of looping on the pairing prompt.

### Added

#### Protocol (BBP v4)
- `GET_ADMIN_TOKEN` (0x74) — transient auth token, USB-only.
- `MUX_SET_ALL` (0x90), `MUX_GET_ALL` (0x91), `MUX_SET_SWITCH` (0x92) —
  32-switch matrix control with firmware-enforced dead-time.
- `HAT_LA_LOG_ENABLE` (0xEB), `HAT_LA_LOG_EVT` (0xEC),
  `HAT_LA_USB_RESET` (0xED), `HAT_LA_STREAM_START` (0xEE) — LA streaming
  control and unsolicited log-relay events.
- Error codes `0x0B`..`0x11` — HVPAK diagnostics, `ERR_TIMEOUT`,
  `ERR_UNSAFE_REGISTER`.
- `channel_alert_mask` (u16) per channel; `mux_states[4]` trailer.

#### Logic Analyzer (RP2040 HAT)
- Vendor-bulk transport on endpoint `0x87` (replaces CDC relay); 8 × 2432
  byte ring buffer sized for 1 MHz / 4-channel continuous capture.
- SMP core affinity + SIE endpoint reset + `tud_vendor_n_fifo_clear`
  two-step rearm for reliable consecutive runs.
- STOP-first preflight on START reuse to recover from libusb stuck-busy
  endpoints on macOS.
- Observability: `usb_rearm_pending`, `request_count`, `complete_count`
  counters surfaced via `HAT_LA_STATUS`.
- LA-done IRQ (RP2040 GPIO8 → ESP32) wired in firmware (physical HAT PCB
  wire pending).
- Log relay: `bb_la_log()` messages forwarded as `0xEC LA_LOG` events.
- Rendering optimizations (dense mode), E2E tests.

#### HVPAK backend
- Dual-HVPAK mailbox contract: I2C `0x48`, identity register `0x48`
  (`0x04` = SLG47104, `0x15` = SLG47115-E), command register `0x4C`, preset
  voltages `1200/1800/2500/3300/5000 mV`.
- Capability-gated LUT / bridge / analog / PWM / raw-register access;
  descriptor validation per `Docs/hvpak-descriptor-validation.md`.
- HVPAK-specific error codes with host-side decoding.

#### Board profiles
- JSON schema (`Docs/board_profiles.md`) with nested `{value, locked}` for
  VLOGIC / VADJ1 / VADJ2, per-pin `name` / `type` / `direction`, and optional
  `swd` / `uart` blocks.
- Desktop **Board** tab — pin-mode editor, rail lock, drive-strength
  selector, profile import/export (resolves target via user pick or
  `BUGBUSTER_REPO_ROOT` / `CARGO_MANIFEST_DIR`).
- MCP surface: `list_boards()` tool, `set_board(name)` tool,
  `bugbuster://board` resource, rail-lock enforcement in
  `bugbuster_mcp/safety.py`.
- Example profiles: `NewBoard.json`, `stm32f4_discovery.json`.

#### REST API
- `GET /api/debug` — combined DS4424 / HUSB238 / PCA9535 diagnostics.
- `GET /api/mux`, `POST /api/mux/switch`, `POST /api/mux/all` — 32-switch
  matrix access.
- `/api/device/info` now emits `macAddress` / `mac_address`.

#### Python client / MCP
- `BugBuster.get_mac_address()` — reads from the 14-byte handshake on USB,
  from `/api/device/info` on HTTP.
- `BugBuster.mux_get` / `mux_set_all` / `mux_set_switch`.
- `BugBuster.hat_la_log_enable`, `on_la_log` event callback.
- `BugBuster.get_debug_info()` — HTTP wrapper for `/api/debug`.
- `get_status()` now includes `mux_states` on both transports.
- New MCP tools `list_boards`, `set_board`; new `bugbuster://board`
  resource.

#### Simulator / tests
- `tests/mock/SimulatedDevice` + 102 BBP handlers + `SimulatedUSBTransport`
  + `SimulatedHTTPTransport` + `/api` router.
- `tests/simulator/` meta-tests for BBP handler completeness and
  proto-version sync.
- `tests/device --sim` flag to run the full device suite hardware-free.
- `tests/unit/test_rail_lock.py` — 7 tests covering board-profile rail
  lock enforcement.

#### Desktop app
- 21-tab layout including new **Board** tab (profile editor) and
  consolidated **HV Digital** tab.
- **Improved Board UI**: Functional IO badges (GPIO, ADC, GPI, GPO) replace
  ambiguous center dots.
- **Weak Drive Warning**: Badges turn high-contrast **Orange** when Weak (2k)
  resistor drive is active.
- **Reactive Pins**: Port names and modes now update instantly on the board
  tiles during inspector edits.
- Scope state hoisted via context with ADC pre-config before
  `start_scope_stream`.
- 2 × 2 tile dashboard with per-tile rolling sparkline.
- VDAC bipolar range validation + logging.
- LA dense rendering, stream mode, decoder panel (UART / I²C / SPI).
- Logging default bumped to `bugbuster=debug`.

### Changed

- `BugBusterProtocol.md` bumped to v3.0.0 (BBP wire v4).
- **Renamed "Analog" to "ADC"** across the stack (Badges, Inspector, Protocol
  constants) for hardware consistency.
- `pico-sdk` submodule: forked TinyUSB with `abort_done` timeout fix and
  `tud_vendor_n_fifo_clear` so LA vendor-bulk rearm is safe.
- `_normalize_http_usbpd` maps firmware camelCase (`voltageV`, `currentA`,
  `sourcePdos`) onto the snake_case shape the USB parser produces
  (`voltage_v`, `current_a`, `pdos`).
- Many CSS refinements across UI tabs (consistent spacing, glass theme).

### Fixed

- **HTTP pairing loop on every connect.** Firmware now emits `macAddress`
  on `/api/device/info`; desktop keys pairing on the real MAC instead of
  falling back to `00:00:00:00:00:00`.
- `session.reset_session` in `bugbuster_mcp` missed `global _active_board`;
  the active profile now actually clears on reset.
- LA vendor-bulk endpoint lifecycle: deferred-stop retry, endpoint reset on
  rearm, stale-DATA drain on host side.
- DMA overrun diagnostics surfaced to `PKT_ERROR` with `info=0x03`.
- `bb_la_usb.c` endpoint reuse no longer hangs across libusb timeouts on
  macOS (STOP-first preflight).
- `SimulatedHTTPTransport` now injects the admin token for POSTs, closing
  13 pre-existing `--sim` failures.
- `test_sim_completeness` adds the 3 new LA commands and `LA_LOG_EVT` to
  its event-only exclusions.
- Various channel/GPIO/MUX HTTP route shapes aligned to firmware.

### Testing

- `pytest tests/unit tests/simulator tests/device --sim -q`:
  **420 passed, 64 skipped** (HAT / SWD / LA hardware-only), 0 failures.
- All three CI version-gate scripts green:
  - `Firmware/tools/firmware_version.py rp2040 --expect 2.0`
  - `Firmware/tools/firmware_version.py esp32 --expect 3.0.0`
  - `DesktopApp/BugBuster/scripts/desktop_version.py --check --expect 0.5.0`

### Documentation

- Top-level `README.md` — screenshot set updated to the new
  `Docs/Images/Screen{Overview,Board,WaveGen,SignalPath,Scope,LogicAnalyzer,UART,Diagnostics}.png`;
  test module list expanded; simulator testing quickstart added.
- `python/bugbuster_mcp/README.md` — new Board Profiles section with schema
  example; hardware-free testing section.
- `Firmware/ESP32/README.md` — 14-byte handshake, admin-token
  enforcement, simulator reference.
- `Firmware/RP2040/README.md` — LA vendor-bulk streaming, SMP/SIE rearm,
  LA-done IRQ, log relay.
- `DesktopApp/BugBuster/README.md` — 21 tabs, HTTP pairing flow, Board
  configuration workflow.
- `tests/README.md` — three-layer taxonomy (unit / simulator / device).
- `.mex/ROUTER.md` — refreshed current state and known-issues sections.

### Known issues

- Single CDC0 client limit — no multiplexing between MCP server, desktop,
  and `pio device monitor`.
- HTTP transport does not support ADC/LA streaming by design.
- Python `HTTPTransport` does not implement the admin-token pairing
  handshake; real HTTP use from the Python client requires a token provided
  out of band (the desktop app handles this).
- LA bench: bench proof for 1 MHz / 4-ch (T-LA-01, T-LA-02) and LA-done
  physical wire are pending hardware sessions.
