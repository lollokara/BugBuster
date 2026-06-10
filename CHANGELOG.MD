# CHANGELOG

## [Unreleased]

*(no changes yet)*

## [1.3.3] — 2026-06-10

### Fixed
- **Lint cleanup**: removed unused `deviceMac` import in `Voltages.tsx`; renamed `dir` parameter to `direction` in `client.py:hat_set_level_shift` and `http_routes.py` to avoid shadowing Python builtin; removed redundant `RwSignal` rebindings in `diag.rs`; replaced `useless_format!` calls with `.to_string()` in `commands.rs` and `push_str` literal in `la_store.rs`.

### Added

#### Desktop app
- **Selftest worker toggle + CH-D reservation overlay**: `DiagTab` now fetches worker state on mount via `fetch_selftest_status()`, shows an enable/disable toggle card, and reactively dims the CH-D alert cell with an amber `⚡MON` badge when `supply_monitor_active` is true. (`src/tabs/diag.rs`)

#### ESP32 firmware
- **HAT IO block LED analog awareness**: `hat_update_leds()` now includes the AD74416H channel function in the IO block `io` flag — setting `FUNC_VOUT` (or any non-HIGH_IMP function) on a channel now lights the corresponding IO block LED yellow when no e-fuse supply is active, matching the expected "Analog enabled, no supply" indicator. `tasks_apply_channel_function` now calls `hat_update_leds()` on completion so LEDs update immediately. (`Firmware/ESP32/src/hat/hat.cpp`, `Firmware/ESP32/src/tasks.cpp`)

#### ESP32 firmware (web API)
- **`GET /POST /api/hat/power`**: Two new HTTP handlers in `webserver.cpp` expose HAT connector power state and toggling over HTTP. `GET` returns `{connA, connB}` objects with `enabled`/`currentMa`/`fault` plus `ioVoltageMv`; `POST` accepts `{connector, enable}`. Routes registered alongside existing HAT v2 handlers.

#### ESP32 web UI (on-device)
- **Signal path MUX → IO block LED**: toggling a MUX switch via the Signal Path tab now calls `hat_update_leds()` after state update (`handle_post_mux_switch`, `handle_post_mux_all` in `webserver.cpp`). `hat_update_leds` also checks `g_deviceState.muxState` per block (mapping `{0,1,3,2}`) so any active switch lights the IO block LED yellow/green. (`hat.cpp`, `webserver.cpp`)
- **Web GitHub firmware update**: new `GitOtaCard` in the System → Device section lets users check for nightly releases and selectively install ESP32 and/or RP2040 updates. Calls `GET /api/update/check`, `POST /api/update/apply`, `GET /api/update/status` with progress polling. Added `UpdateCheckResult`/`UpdateStatus` types to `types.ts` and `api.update` namespace to `client.ts`. (`OtaCard.tsx`, `System.tsx`, `client.ts`, `types.ts`)
- **GPIO picker redesign**: replaced the truncated `<select>` in the ESP32 GPIO card with three compact pill toggle buttons (Hi-Z / Out / In); cells are now two-row (IO label + HIGH/LOW on top, mode pills on bottom); HIGH/LOW button is disabled when mode ≠ Out. (`Digital.tsx`, `styles/tabs.css`)
- **REPL browser freeze fix**: fixed two bugs in `Repl.tsx` that caused the browser to become unresponsive after connecting. (1) `ResizeObserver` loop: `term.resize()` changed xterm DOM height, which re-fired the observer — added an early-exit guard that skips resize when cols/rows haven't changed. (2) Stale closure: `ws.onmessage` captured `status` at connect time (`"disconnected"`) so `setStatus("connected")` fired on every incoming message causing a render storm — replaced with a `statusRef` that stays current across closures.
- **`bb_helpers` API docs**: Added `bb_helpers.settle`, `bb_helpers.dac_ramp`, and `bb_helpers.channel_sweep` entries (new "Helpers" group) to the on-device Scripts tab API reference. (`Firmware/ESP32/web/src/tabs/scripts/apiDocs.ts`)
- **Voltages tab**: New `Voltages.tsx` lazy tab aggregates supply rails (from `/api/overview`), IDAC channels (`/api/idac`), and HAT power rails (`/api/hat/v2/rails`, shown only when HAT detected). Polled every 2 s. Registered in `App.tsx` between Signal Path and System.

#### TypeScript API client
- **`hatGetPower` / `hatSetPower`**: New API client functions in `client.ts` targeting `GET/POST /api/hat/power`.

#### Python client
- **`hat_get_power` / `hat_set_power` HTTP transport**: Both methods now support HTTP transport (previously raised `NotImplementedError`); parse response into the same dict format as the USB path. (`python/bugbuster/client.py`)
- **`selftest_status()` extended fields**: USB path now parses `worker_enabled` and `supply_monitor_active` from the 23-byte firmware payload; falls back gracefully for older 21-byte firmware. (`python/bugbuster/client.py`)

#### Simulator
- **`GET/POST /hat/power` HTTP routes**: Added to `tests/mock/http_routes.py` to back the new Python HTTP transport.

#### Desktop app (prior entries)
- **Scope PNG export**: Export dropdown "PNG" now captures the live scope canvas via `canvas.to_data_url()`, prompts for a save path (`.png`), and writes the file via the new `save_scope_png` Tauri command (base64 decode + `fs::write`). (`src/tabs/scope.rs`, `src-tauri/src/commands.rs`)
- **Scope JSON export**: Export dropdown "JSON" serialises all four channels (label + `{time_ms, value}` point arrays) and the export timestamp to pretty-printed JSON, prompts for a save path (`.json`), and writes it via the new `write_text_file` Tauri command. (`src/tabs/scope.rs`, `src-tauri/src/commands.rs`)
- **`pick_png_save_file` / `pick_json_save_file` Tauri commands**: Two new file-dialog helpers for PNG and JSON save paths, analogous to the existing `pick_save_file`. (`src-tauri/src/commands.rs`, `src-tauri/src/lib.rs`)

#### Simulator / Tests
- **`ESP32_FW_VERSION` constant**: Added `ESP32_FW_VERSION = (3, 4, 0)` to `python/bugbuster/protocol.py`; `SimulatedDevice.fw_version` now imports and uses it instead of the stale hardcoded `(1, 0, 0)`.
- **Simulator `uart_config` expanded**: Pre-populated with two UART bridge entries matching firmware defaults (UART1: tx=4/rx=2/921600, UART2: tx=7/rx=6/921600). (`tests/mock/simulated_device.py`)
- **Simulator `hat_power` array**: `SimulatedDevice.__init__` now initialises `hat_power = [False, False]` (two connectors). `HAT_SET_POWER` handler indexes by connector, `HAT_GET_STATUS` and `HAT_GET_POWER` pack per-connector state. (`tests/mock/simulated_device.py`, `tests/mock/handlers/hat.py`)

## [1.3.2] — 2026-06-09

### Added

#### Desktop app
- **Startup update popup**: On mount, if a desktop app update is detected, a fixed overlay popup appears with the new version, a contextual subtitle (stable vs nightly build), and Install & Restart / Later buttons. The popup is dismissible; the update section in Diagnostics remains for manual re-check. (`src/tabs/diag.rs`)
- **Version picker / Browse Releases**: Added a "Browse Releases" button in the App Updates card that calls the new `list_desktop_releases` Tauri command. Fetches up to 20 GitHub releases, filters to platform-appropriate installers (.dmg / .exe / .AppImage), and populates a dropdown. Selecting a version and clicking "Install Selected" calls `install_desktop_version` which downloads to a temp file, launches the installer natively, and exits the running app. (`src/tabs/diag.rs`, `src-tauri/src/commands.rs`)
- **Nightly build detection in `check_app_update`**: If the current version contains a hyphen (e.g. `1.1.1-123`), the command now also checks the nightly manifest directly and compares build numbers, returning `is_nightly: true` in `AppUpdateInfo`. (`src-tauri/src/commands.rs`)
- **App Updates card in Diagnostics tab**: Added a new "App Updates" card to the bottom of the Diagnostics tab (`src/tabs/diag.rs`). On mount it auto-checks for a desktop app update via the `check_app_update` Tauri command. Shows current version, a spinner while checking, a "Up to date" green indicator when current, or the new version string + release notes preview (first 200 chars) + "Install & Restart" button when an update is available. The button calls `apply_app_update` which downloads and restarts the app. A "Check for Updates" button is always visible to re-run the check.

#### Docs / Planning
- **TODO: Desktop auto-update + macOS Gatekeeper bypass** — Added two tracked items under Desktop App: (1) in-app GitHub release checking and download via `tauri-plugin-updater`; (2) options for distributing unsigned macOS builds without Gatekeeper blocking (ad-hoc codesign, quarantine strip, Homebrew cask).

### Fixed

#### Desktop app
- **Token persistence after app restart**: Replaced OS keychain (`keyring` crate) with a plain `tokens.json` file in the app data directory. Unsigned macOS builds cannot access the keychain, causing admin tokens to be stored in-memory only and lost on restart — requiring USB re-pairing every launch. Tokens are now read from `tokens.json` on startup (`load_tokens`) and written back atomically on every USB pairing (`save_token`). Removed `keyring = "3"` dependency. (`src-tauri/src/connection_manager.rs`, `src-tauri/Cargo.toml`)

## [1.3.1] — 2026-06-09

### Added

#### ESP32 firmware
- **TUI ESP_LOG suppression**: `cli_menu_enter()` now calls `esp_log_level_set("*", ESP_LOG_NONE)` before switching to the alt-screen, and `cli_menu_leave()` restores `ESP_LOG_INFO` after leaving, preventing log output from corrupting the TUI. (`cli/cli_menu.cpp`)
- **Stack overflow canary**: Added `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y` to `sdkconfig.defaults`. FreeRTOS now writes canary words at the bottom of every task stack and validates them on each context switch; overflow triggers `abort()` which saves a core dump to the `coredump` flash partition. (~0.04% CPU overhead.)
- **Stack high-water mark logging**: `tasks_log_stack_hwm()` (new function in `tasks.cpp` / `tasks.h`) reports free words remaining on each task stack (`adcPoll`, `faultMon`, `cmdProc`, `wavegen`) using `uxTaskGetStackHighWaterMark()`. Logs a `*** LOW ***` warning when any task drops below 128 words free. Call from CLI or let it fire automatically after 60 s at steady state.
- **Post-creation heap log**: `initTasks()` now logs internal free heap and largest contiguous block after all tasks are created, making it easy to see headroom at the console without connecting a debugger.
- **`Firmware/tools/check_memory.py`** — post-build memory size gate. Runs automatically as a PlatformIO post-build action (wired in `platformio.ini`). Checks static DRAM (`.data + .bss < 160 KB` baseline; current: 144 KB) and estimated flash binary size vs OTA partition from ELF sections (fail at 90%; current: ~44%). Also runnable standalone: `python Firmware/tools/check_memory.py <firmware.elf> <partitions.csv>`.
- **3 new CLI commands** (`version`/`ver`, `heap`, `stack_hwm`/`hwm`) added to `cli_cmds_sys.cpp` and the command table in `cli_cmdtab.cpp`. `version` shows ESP32 app version, build date, IDF version, and RP2040 HAT firmware. `heap` shows internal free/min-ever/largest-block + PSRAM with fragmentation warning. `stack_hwm` prints FreeRTOS high-water marks for all 4 measurement tasks with `*** LOW ***` warning below 128 words. `g_wavegenTask` promoted from `static` to a proper global exported in `tasks.h` (was `s_wavegenTask`; updated in `bbp.cpp`, `webserver.cpp`, `cmd_streaming.cpp`).

#### Docs
- **`Docs/SimulatedDevice.md`** — full reference for the BugBuster Python simulator: handler coverage table (144 commands across 15 files), test file → feature matrix, state field inventory, known gaps, and a reliability improvement roadmap.
- **`Docs/MemoryTesting.md`** — ESP32-S3 memory safety guide: SRAM layout diagram, pre-ship checklist (binary gate, stack HWM, heap fragmentation, canary), common failure modes and fixes, and CI integration notes.

### Fixed

#### ESP32 firmware
- **Removed unused `ADMIN_TOKEN` fallback**: The static `#define ADMIN_TOKEN "000..."` in `config.h` was a Phase-1 placeholder never referenced by any code path — the live token always comes from `auth_get_admin_token()`. Removed to eliminate dead-code confusion. (`config.h`)

#### Python client / simulator
- **`BBP_CMD_PCA_GET_STATUS` (0xB0) payload truncation**: `_parse_pca_status()` previously read only 12 bytes, silently dropping the 9 trailing enable fields that firmware has always written (`vadj1_en`, `vadj2_en`, `en_15v`, `en_mux`, `en_usb_hub`, `efuse_en[0..3]`). Parser now reads all 21 bytes and exposes the enable flags in the returned dict; older firmware returning 12 bytes degrades gracefully. (`python/bugbuster/client.py`)
- **Simulator `PCA_GET_STATUS` returns full 21-byte payload**: `_pca_get_status` handler in `tests/mock/handlers/power.py` now appends the 9 enable bytes, keyed by the `PcaControl` integer enum (0–8), so round-trip tests against the simulator match the real firmware. (`tests/mock/handlers/power.py`)
- **0x07 self-test alignment**: `SELFTEST_SUPPLY_VOLTAGES_CACHED` is now the canonical BBP name for command `0x07`; the Python client exposes `selftest_supplies_cached()` and keeps `selftest_efuse_currents()` as a compatibility alias. The simulator and HTTP mock now return cached rail voltages from `/api/selftest/supplies/cached`, and the MCP discovery tool reports the cached supply snapshot under `supply_voltages_cached`. (`python/bugbuster/constants.py`, `python/bugbuster/client.py`, `tests/mock/*`, `python/bugbuster_mcp/tools/discovery.py`)

#### Desktop app
- **HAT status SWD parsing**: `parse_hat_status()` now skips the 3 inserted HVPAK metadata bytes (`hvpak_part`, `hvpak_ready`, `hvpak_last_error`) before reading the SWD fields, so `dap_connected`, `target_detected`, and `target_dpidr` stay aligned. (`commands.rs`)
- **GPIO HTTP transport framing**: `CMD_GET_GPIO_STATUS` now serializes all 12 logical GPIOs as full 5-byte records (`id`, `mode`, `output`, `input`, `pulldown`) instead of the legacy 6-pin layout. (`http_transport.rs`)

### Changed

#### CI
- **Proto-version check runs on every push/PR**: `.github/workflows/proto-version-check.yml` path filter removed; `BBP_PROTO_VERSION` sync is now verified on all pushes and pull requests, not only when the three protocol files are in the diff. (`.github/workflows/proto-version-check.yml`)

#### Tests
- **Regression coverage**: Added parser and transport regression tests for HAT status framing, cached supply payloads, and the simulator route coverage for `/api/selftest/supplies/cached`. (`tests/unit/test_hat_status_parsing.py`, `tests/device/test_09_selftest.py`, `tests/simulator/test_sim_completeness.py`)

## [1.3.0] — 2026-06-08

### Added

#### ESP32 firmware
- **USB OTA control command (0x77)**: New BBP command for firmware updates over USB. Implements `INFO` (read partition/version info), `BEGIN` (start upload with SHA-256 validation), `CHUNK` (960-byte data chunks), and `ROLLBACK` ops. Integrated with `update_manager` to support reliable USB-driven flashing of both ESP32 and RP2040. (`bbp.h`, `cmd_ota.cpp`)
- **RP2040 Stage Helper**: New `update_manager_flash_rp2040_stage` function allows flashing a pre-staged RP2040 binary from SPIFFS without re-fetching a manifest, improving the "local upload" flow. (`update_manager.cpp`)
- **WIFI_FORGET command (0x0A)**: New BBP command erases saved STA credentials (`sta_ssid` / `sta_pass`) from NVS and disconnects. Implemented in `wifi_manager.cpp` (`wifi_forget_credentials()`), registered in `cmd_wifi.cpp`, documented in `BugBusterProtocol.md`. Returns a bool indicating NVS erase success.
- **RP2040 HTTP upload endpoint**: New `POST /api/ota/upload_rp2040` endpoint in `webserver.cpp` accepts a raw RP2040 `.bin` body (admin-gated, max 1 MB) and stages it to `/scripts/update-rp2040.bin` on SPIFFS for the update manager to pick up.

#### Desktop app
- **USB OTA Transport**: Added a dedicated `usb_upload_blob` flow that pushes firmware binaries directly over USB using the new BBP 0x77 command. Provides a robust fallback when the ESP32 is not on WiFi or has no internet access. (`commands.rs`)
- **Robust Tauri Bridge**: Refactored `try_invoke` to use a standard `wasm-bindgen` bridge with the `catch` attribute. This eliminates the need for `js_sys::Function::new_with_args` and custom JS wrappers, providing a more efficient and stable path for Tauri commands that safely handles rejections and avoids re-entrancy panics in the WASM task executor. (`tauri_bridge.rs`)
- **Desktop OTA from GitHub releases**: Full desktop-driven firmware update flow added. `fetch_github_releases` Tauri command queries the GitHub Releases API (up to 10 releases), downloads each release's `bugbuster-update-manifest.json` if present (with fallback to asset-name parsing), and returns `DesktopGitRelease` structs with per-component version, URL, size, and SHA-256. `start_desktop_ota` downloads RP2040 and ESP32 binaries from GitHub, verifies SHA-256, and pushes them to the device via `/api/ota/upload_rp2040` and `/api/ota/upload`. Progress is streamed to the frontend via `desktop-ota-progress` Tauri events. (`commands.rs`, `tauri_bridge.rs`)
- **OTA per-device version picker**: The Firmware Update card in the Diagnostics tab shows independent version dropdowns for ESP32 Mainboard and RP2040 HAT. Each dropdown lists only releases that have a valid binary for that device, labelled as `<tag> (vX.Y.Z)`. Updates can target different releases per device in a single operation. Checkboxes auto-enable when the selected version is newer than installed firmware. (`diag.rs`)
- **OTA context & "Update Available" badge**: App-level `OtaContext` provided to all components carries release list, current versions, progress, and error/success state. A pulsing "Update Available" button appears in the header status bar when any connected device has an older firmware than the latest GitHub release, linking directly to the Diagnostics tab. (`app.rs`, `styles.css`)
- **WiFi Forget Tauri command**: `wifi_forget` command and `tauri_bridge` wrapper wired up, enabling "Clear Saved Credentials" from the desktop Diagnostics WiFi section. (`commands.rs`, `lib.rs`, `tauri_bridge.rs`)
- **Connection error toast**: `ConnectionPanel` now surfaces a "Connection failed" error toast when `connect_device` returns an error instead of silently ignoring it. (`connection.rs`)

#### CI
- **Archived nightly rollback builds**: Release workflow now publishes a versioned `nightly-build<run_number>` archive alongside the rolling `nightly` release, with manifest URLs rewritten to the archive tag. Archives are pruned to keep the latest three, providing three rollback builds at all times. (`.github/workflows/release.yml`)

### Fixed

#### Desktop app
- **Reactive Disposal Panics**: Added `alive` guards (using `Arc<AtomicBool>` and `on_cleanup`) to `OverviewTab`, `VoltagesTab`, `HatTab`, `LaTab`, and `DiagTab`. These guards prevent async tasks from updating signals after a component is unmounted, which previously triggered "access to disposed reactive value" panics during tab navigation. (`overview.rs`, `voltages.rs`, etc.)
- **WASM Task Executor Panic**: Replaced high-frequency `new_with_args` JS bridge with a native `extern "C" catch` bridge, resolving "RefCell already borrowed" errors in the WASM single-threaded task executor. (`tauri_bridge.rs`)
- **WASM panic on network error**: All `tauri_bridge` fetch/status/LA/OTA functions were using the raw `invoke` extern which panics the WASM runtime when the underlying JS promise rejects (e.g. device unreachable, HTTP timeout). Converted every call site to `try_invoke`, which catches rejections and returns `None`/default instead. Affected: `fetch_wifi_status`, `fetch_wifi_scan`, `wifi_forget`, `fetch_firmware_info`, `fetch_idac_status`, `fetch_usbpd_status`, `fetch_pca_status`, `fetch_hat_status`, `upload_firmware`, `la_get_view`, `la_get_capture_info`, `la_export_vcd`, `la_export_json`, `la_import_json_file`, `la_decode_uart/i2c/spi`, `la_delete_range`. (`tauri_bridge.rs`)
- **RP2040 OTA apply fails after upload**: After uploading the RP2040 binary to SPIFFS via `/api/ota/upload_rp2040`, the ESP32 httpd briefly stops accepting new connections while the filesystem write finalises, causing the immediate `/api/update/apply` POST to fail with a transport error. Fixed by adding an 800 ms pause after upload and retrying the apply request up to 3 times (2 s apart) on transport-level errors. HTTP error responses (4xx/5xx) are not retried. (`commands.rs`)
- **OTA runtime crash**: `start_desktop_ota` was declared as a synchronous `pub fn` Tauri command. Calling `tokio::spawn` from a sync command handler panics with "no reactor running" because Tauri doesn't guarantee a Tokio runtime in that context. Changed to `pub async fn`. (`commands.rs`)

### Changed

#### Desktop app
- **reqwest TLS**: `reqwest` dependency switched to `rustls-tls` with `default-features = false` for deterministic TLS without system OpenSSL dependency. (`Cargo.toml`)
- **Log filter**: Default log filter tightened to `info,bugbuster_lib::bbp=warn` to reduce BBP protocol noise. (`lib.rs`)
- **`ConnectionManager` now `Clone`**: Derived `Clone` on `ConnectionManager` to allow it to be passed into spawned OTA tasks. (`connection_manager.rs`)
- **Keychain miss log**: Missing keychain entry now logs a `warn` with a human-readable message instead of silently returning `None`. (`connection_manager.rs`)
- **Linux dropdown fix**: Added explicit `option { background; color }` CSS rule so dropdowns are readable on Linux GTK themes that default to white-on-white. (`styles.css`)

#### ESP32 firmware
- **Update manager skip-manifest optimisation**: `update_manager_apply` now skips fetching the GitHub manifest when an RP2040 binary is already staged locally and only an RP2040 update is requested, saving an HTTPS round-trip. (`update_manager.cpp`)

---

## [1.2.0] — 2026-06-08

### Added

#### ESP32 firmware
- **GitHub Update Subsystem**: Introduced a robust update manager (`update_manager`) to check for available releases on GitHub, parse update manifests, stage updates, and verify SHA-256 and CRC-32 checksums.
- **TUI & CLI Update Interfaces**: Added a serial CLI command (`fwupdate` / `update`) and interactive menu picker to check, list, and apply updates. Features a two-column picker comparing current vs available versions, component selection, and colored upgrade/downgrade indicators.
- **Unsolicited Commit Log Forwarding**: Added real-time log forwarding during RP2040 updates. Intercepts commit logs from RP2040 UART and relays them directly to ESP32 logs. Introduces a `s_commit_in_progress` state to bypass standard UART flushes, timeouts, and reconnect retries.
- **MicroPython HAT v2 bindings**: Full HAT interface exposed via MicroPython (`modbugbuster_hat.c`) including HAT status queries, rail configuration, calibration, IO voltage control, SWD setup, LED control, and LA routing. New example script `14_hat_v2.py` demonstrates usage.
- **PD/VADJ safety guard** (`pd_vadj_guard`): Prevents voltage configuration above negotiated USB-C PD input or measured rail voltage. Checks integrated into IDAC command handler, webserver JSON responses, and MicroPython bridge (I2C/SPI constructors). Python client `check_vadj_pd_limit()` provides advisory checks at the host level.
- **QuickSetup client APIs**: Python client now exposes `get_quicksetup()`, `save_quicksetup()`, `apply_quicksetup()`, `list_quicksetups()`, and `delete_quicksetup()` for preset management.
- **ADC DSP streaming enhancements**: Fixed task notification race (ulTaskNotifyTake → xTaskNotifyWait), added NaN/Inf filtering, proper memory barriers, and improved buffer handling to prevent reading recycled data. ADC poll task now seeds values on read failure and includes micro-delay at high rates.
- **HAT HTTP parity**: REST endpoints for all HAT v2 commands previously USB-only (status, detect, caps, rail enable/voltage, calibration, IO configuration, SWD setup, LED, LA routing, pin config). Enables HAT control via HTTP when USB BBP is unavailable.
- **HAT parity guardrails & MCP health**: New MCP tools for HAT state verification and consistency checking. Desktop HTTP transport now maps BBP HAT commands to `/api/hat` and `/api/hat/v2` REST endpoints.

#### RP2040 firmware
- **RAM-Resident Commit Flow**: Rewrote the flash commit logic to run entirely from RP2040 RAM on Core 0. Halts Core 1, disables global interrupts, and copies the staged image sector-by-sector into RAM using a volatile buffer before invoking raw flash erase and program APIs. This prevents execution of flash-resident code (like inlined `memset` or division helper routines) when erasing/programming sector 0, avoiding system hangs.
- **Watchdog Reboot**: Implemented a direct register-based watchdog reboot helper to trigger a clean restart upon completion of the flash commit.

#### Desktop app
- **3D Interactive Model View**: Integrated a beautiful, interactive 3D splash/connection UI powered by Three.js. Implements gyroscopic parallax/tilt for the 3D PCB models based on mouse movements, custom material heuristics, and dynamic lighting. Includes 3D PCB models (`bugbuster.glb` and `BugBusterMain.glb`).
- **Dynamic SMD LED Animations**: Extracts native SMD LED meshes from the GLTF models of the boards to drive realistic breathing/boot animations using emissive materials and point lights.
- **Screenshot Automation**: Added a `Ctrl+Alt+S` keyboard shortcut to cycle through all categories/tabs, perform programmatic mock configurations (WaveGen, Scope, Logic Analyzer), and automatically capture screenshots.
- **Category-based tab organization**: UI refactored with a category bar grouping tabs into logical sections (Overview, Measurement, Hardware, Debug). Improved visual hierarchy and navigation.
- **Local font bundling**: Added Inter (normal + italic) and JetBrains Mono (normal + italic) WOFF2 fonts; removed reliance on external CDN. Updated styles.css with extensive polish (cards, buttons, shadows, animations).
- **Consolidated HAT UI**: Calibration tab removed (functionality moved to Voltages tab). HAT UI simplified with streaming status focus; per-rail voltage/enable controls, routing, SWD, and LA USB reset consolidated.
- **HAT signal conditioning UI**: Added controls for HAT level-shifter OE/DIR, VLOGIC presets, and HS/LS route selection to the Logic Analyzer tab.

#### Testing & CI
- **Update Manifest Integration**: Configured CI release workflow to generate a JSON update manifest (`bugbuster-update-manifest.json`) alongside release assets.
- **SimHTTP server**: New mock HTTP server (`sim_http_server.py`) for hardware-free HTTP transport testing. Test suite extended with integration tests exercising the HTTP path without hardware.
- **Expanded mock handlers**: New `handlers/quicksetup.py` and `handlers/bus.py` for broader BBP command coverage. Integration tests added (`tests/integration/test_sim_http_transport.py`).
- **CI improvements**: Desktop release workflow updated with run-number support for Windows MSI versioning. Improved artifact staging to handle run-numbered filenames.

### Changed

#### ESP32 Firmware / Optimizations
- **Memory Optimization**: Replaced large on-stack arrays and static task buffers with dynamic, heap-allocated structures. Update worker tasks now allocate their 12KB stack in internal memory (`MALLOC_CAP_INTERNAL`) during flashing, while check-only tasks run in PSRAM, reclaiming memory completely upon task termination to prevent heap fragmentation.
- **Wi-Fi Buffer & Stack Tuning**: Reduced static Wi-Fi TX/RX DMA buffer counts (to 6 TX and 8 RX) and disabled hardware-accelerated AES in mbedTLS configurations to preserve internal RAM for TLS handshakes.
- **CDC CLI restoration**: Added `bbpRestoreCdcCli()` to clear CDC claim, restore serial logging, and release USB sessions when BBP goes idle. CLI loop now restores CDC state when needed so serial prompt/editor state recovers after interactive commands.
- **Logical ↔ physical IO/MUX mapping fixes**: PCA9535 EFUSE3/EFUSE4 bit/EN/FLT swaps corrected. Bus planner device indices and comments updated to match PCB wiring. Signal path and layout components now use `MUX_DEVICE_BY_LOGICAL` translation to maintain natural A/B/C/D channel order in UI.
- **WebSocket streaming refactor**: Ring buffer API pushed header+payload atomically. Persistent TX task created at register time; TX semaphore blocks indefinitely. Frames dropped when no client connected; simplified task lifecycle.
- **BBP protocol v8**: Formal version bump reflecting HAT v2, ADC DSP, and ownership lease features added post-v7.
- **HAT calibration success code**: Changed from 0 to 1 for consistency.

#### RP2040 Firmware
- **Heap Implementation Upgrade**: Switched RP2040 FreeRTOS heap from Heap 1 to Heap 4 to support dynamic memory reclamation during update worker tasks.

#### Python client
- **HTTP HAT command mappings**: `hat_get_caps()`, `hat_get_rail_status()`, `hat_set_rail_enable()`, `hat_set_rail_voltage()`, `hat_set_led_state()`, `hat_set_io_voltage()`, and `hat_setup_swd()` now use HTTP `/api/hat/v2` endpoints. Field names and return shapes aligned with REST API.
- **PD/VADJ advisory**: `check_vadj_pd_limit()` added as a utility; callsites in `io_config` now emit warnings when rail voltage exceeds PD limits.
- **Improved error handling**: Short response checks added for `PING`, `GET_DEVICE_INFO`, `GET_ADC_VALUE`, `SCRIPT_LOGS`. Missing `states` key in `MUX_GET_ALL` HTTP path now caught. Helper `_require_resp_len()` introduced.

#### Desktop Rust backend
- **Explicit disconnect on BBP probe failure**: Improved error handling with clearer disconnection sequencing.
- **Tauri command helpers**: Transport modules now expose/send command helpers to tidy up transport logic.
- **Dead code annotations**: Added `#[allow(dead_code)]` where appropriate to reduce compiler noise.

#### Documentation & scripts
- **MeasureCurrent.py**: UI and DSP mode enhancements — backend selection, persistent FFT spectrum (decay-based), improved envelope/mean/spike rendering. Added USB keepalive thread (prevents macOS CDC idling), safer ADC/HAT cleanup on exit, and layout/style tweaks.
- **Scratch test scripts**: New utilities for ADC testing, mDNS/HTTP probing, matplotlib plotting, and CSV polling.
- **Updated examples & notebooks**: Desktop app launcher, ESP32 flash manager, and RP2040 firmware build notebooks refreshed.

### Fixed

- **Update File Verification**: Added size checks in ESP32 `update_manager.cpp` to reject truncated or corrupted staged update files (such as incomplete 8KB files), falling back to downloading release binaries.
- **ADC DSP robustness**: Task notification race fixed with xTaskNotifyWait; buffer snapshots prevent reading recycled data. Stats now properly initialized with NaN/Inf filtering applied post-FFT.
- **PCA9535 fault ISR installation**: Deferred until `g_stateMutex` exists during boot (avoids race during initialization).
- **IO ownership lease table**: Test coverage expanded; simulator now includes `WIFI_SET_AP_PASSWORD (0xEF)` handler.
- **Selftest ADC behavior**: Now respects IO ownership of slots 14/15, preventing unauthorized access during scripting.

### Removed

- **Obsolete calibration.rs**: Desktop calibration tab removed; functionality consolidated.
- **Legacy CDC tests**: Unused serial/USB test imports cleaned up (`test_11_hat.py`).
- **Log data**: Stale CSV from `Scripts/logs/Log013.csv` removed.

### Internal

- **Simulator completeness**: Meta-tests updated for new LAcommands and `LA_LOG_EVT` handling. `test_sim_completeness` refactored to assert handler presence more thoroughly.
- **Submodule updates**: `esp-dsp` added as a git submodule for DSP processing on ESP32. Debugprobe submodule version bumped.
- **Build hygiene**: Minor CMakeLists.txt and sdkconfig refinements. Firmware tools now handle both single-source and legacy fallback version schemes.

---

## [1.1.0] — 2026-06-04

### HAT & Protocol (v2.0 / `bb-hat-3.0`)
- **Protocol v2.0**: Major update to the HAT control plane.
- **Extended Calibration**: RP2040 firmware now supports deep telemetry (min/max/gap metrics) and DS4424 validation flags for 1.8V–36V adjustable rails.
- **Hardware Profile**: Updated to `bb-hat-3.0` with improved signal routing (CH0-CH3 direct PIO) and rail labels.
- **Error Handling**: Added `HAT_ERR_CAL_INVALID` to block supply enablement without valid calibration.

### Desktop App (v0.7.0)
- **Multi-platform Bundles**: CI/CD now produces DMG (macOS), AppImage/DEB/RPM (Linux), and MSI (Windows) installers.
- **Tauri v2**: Migrated to Tauri 2.0 with improved workspace build integration.
- **Live Power UI**: Corrected VADJ3/4 slider ranges (1.5V–35.0V) and added live measured voltage telemetry in the Power tab.
- **UI Redraws**: Improved reactivity; components now redraw immediately after hardware control changes.

### Firmware (ESP32-S3)
- **IO Ownership v5**: Comprehensive 16-slot ownership table (IO1..IO12 + CH0..CH3) enforcing access control across all transports (USB, HTTP, SCRIPT, UI).
- **Reduced Log Noise**: Downgraded per-frame HAT TX/RX info logs to DEBUG level to stop flooding the serial console.
- **Bug Fixes**: Fixed PCA9535 state refresh in power management and corrected initial VADJ slider defaults.

### CI/CD & Infrastructure
- **Combined Release**: New workflow automates the creation of a unified draft release containing Desktop, ESP32, and RP2040 artifacts.
- **Checksums**: Automatically generates `SHA256SUMS.txt` for all release assets.
- **Workflow Reliability**: Fixed artifact pathing in Rust workspaces and updated `tauri-apps/tauri-action` to current stable versions.

---

## [1.0.0] — 2026-04-20

- `PROTO_VERSION` bumped **4 → 5** across all three canonical files
  (`Firmware/ESP32/src/bbp/bbp.h`, `python/bugbuster/protocol.py`,
  `DesktopApp/BugBuster/src-tauri/src/bbp.rs`) in lockstep (CI gate extended
  to assert v5).
- 4 new BBP commands: `CMD_IO_CLAIM 0xA7`, `CMD_IO_RELEASE 0xA8`,
  `CMD_IO_OWNER_STATUS 0xA9`, `CMD_IO_FORCE_RELEASE 0xAA`.
- New error code `IO_OWNERSHIP_REQUIRED 0x12` returned to v5 callers that
  attempt a write without claiming the slot first.

#### Firmware
- New module `Firmware/ESP32/src/io_owner.{h,cpp}`: 16-slot ownership table,
  portMUX spinlock, `io_owner_acquire / release / check / get / tick /
  force_release` API. `io_owner_tick()` called from main loop to expire
  stale leases.
- `Firmware/ESP32/src/bbp/cmds/cmd_io_owner.cpp`: handler for the 4 new BBP
  commands.
- Write-path guard (`io_owner_check`) installed at every IO-mutating BBP
  command handler (`cmd_channel`, `cmd_dac`, `cmd_adc`, `cmd_dio`, `cmd_pca`,
  `cmd_ext_bus`) and at the 9 mutating HTTP routes in `webserver.cpp`.
- New HTTP endpoints: `GET/POST/DELETE /api/io/owner` and admin
  `POST /api/io/owner/force`.
- `EVT_IO_PREEMPTED` unsolicited BBP event emitted when `IO_OWNER_INTERNAL`
  (selftest / autocal) displaces a user owner; prior ownership restored on
  completion if the original session is still alive.

#### Python library
- `BugBuster.io_claim(slots, *, lease_ms, purpose)` context manager;
  auto-wrap path for single-shot calls (claim + release per call).
- `BugBuster.io_owner_status()` returns the full 16-slot table.

#### MCP server
- 4 new IO-ownership tools: `io_claim`, `io_release`, `io_owner_status`,
  `io_force_release`. Total MCP tool count: **59**.
- All existing IO-mutating tools accept optional `lease_handle` parameter;
  auto-claim when absent preserves legacy single-shot ergonomics.

#### Desktop + ESP32 web UI
- Each tab acquires its slot footprint on mount and releases on unmount via
  a 5 s lease renewed on a 2 s timer (guarantees free within seconds of a
  crash).
- `IO_HELD_BY_OTHER` response surfaces a banner with current owner kind and a
  "Force take (admin)" button.
- New `Firmware/ESP32/web/src/api/io_lease.ts` helper wraps the
  claim/renew/release loop for the ESP32 web UI.

#### MicroPython
- `bugbuster.claim(slots, purpose="", lease_ms=0)` context manager
  (`__enter__` / `__exit__`); `IO_OWNER_SCRIPT` identity filled in
  automatically from MP task context.

### Backward compatibility
- v4 BBP clients: every IO-mutating write receives an implicit 30 s
  auto-claim + release; `IO_OWNERSHIP_REQUIRED` is never returned to v4
  callers.
- HTTP clients that never call `POST /api/io/owner`: same implicit 30 s
  auto-claim per request, keyed on admin-token fingerprint.

---

## [Unreleased — whole-repo audit] — 2026-05-11

### Added

#### Desktop app
- **SPIFFS OTA section**: Added a dedicated Firmware Update subsection for the ESP32 SPIFFS partition. It now pulls `spiffs.bin` from the Git release asset list, supports WiFi and USB transports, and emits the same staged progress/status updates as the main firmware updater. (`diag.rs`, `commands.rs`, `tauri_bridge.rs`)

#### CI
- **SPIFFS release asset packaging**: The ESP32 release workflow now includes `spiffs.bin` in the flash zip and exposes it in `bugbuster-update-manifest.json`, so desktop OTA can fetch the filesystem image directly from GitHub releases. (`.github/workflows/release.yml`)

Read-only audit across 10 surfaces of the repo (RP2040 fw, ESP32 fw core,
ESP32 web UI, desktop Leptos, desktop Tauri/Rust, Python lib, MCP server,
tests + simulator, docs, CI/scripts). No code changes; the consolidated
worklist lives in `Docs/bugs-improvements-2026-05-11.md`.

Totals: **18 HIGH · 26 MEDIUM · 16 LOW + 6 doc defects = 60 findings**.

Highlights to schedule next:
- RP2040: rearm-counter double-increment + cross-core sync mismatch in
  `bb_la_usb.c`; DMA-IRQ-during-rearm race in `bb_la.c` (H01–H03).
- ESP32: `s_txMutex` race before `bbp_init()` runs (H04); `IDAC` channel
  bounds inconsistent between SET_VOLTAGE and GET_STATUS (H05); WiFi
  password persist NVS error not propagated (H06).
- Web UI: operator-precedence bug in App.tsx polling-failure log (H07);
  slider state clobbered by `/api/overview` poll (H08).
- Desktop FE: `Vec` vs fixed-array drift between Leptos and Tauri serde
  structs (H09); 3 unwrap-on-cleanup panics (H10).
- Desktop BE: WiFi-payload `.unwrap()` panic over HTTP (H11);
  silent-zero `unwrap_or(0)` on optional status fields (H12).
- Python: 3 `resp[0]`-without-length-guard sites (H13–H15).
- Tests: simulator missing `WIFI_SET_AP_PASSWORD` (0xEF) handler (H16);
  completeness test asserts only handler existence (H17).
- CI: floating-tag third-party actions on the desktop release path (H18).
- Docs: stale version badge in `README.md`; `ReleaseChecklist.md`
  `--expect` values lag canon; "28 tools" repeated three times in README
  (real count 41 / actual MCP server 45).

Non-negotiables in `AGENTS.md` re-verified during the audit (USB descriptor
patch, TinyUSB single-thread rule, BBP `PROTO_VERSION` lockstep across three
files, RP2040 two-file version, desktop three-file version) — all still hold.

## [Unreleased — target flashing, OTA, WiFi reconnect] — 2026-05-11

Post-v1.0.0 follow-up focused on getting target-device flashing working
end-to-end from the MCP/notebook side, plus ESP32 OTA hardening and a
WiFi reconnect path that survives WPA3-SAE. No BBP wire-protocol changes;
one new HTTP endpoint family (`/api/uart_bridge/*` config) and several
new MCP tools added in a backward-compatible way.

### Added

#### ESP32 firmware
- WiFi reconnect now runs on a dedicated worker task created with
  `xTaskCreateWithCaps` and a PSRAM stack, replacing the FreeRTOS
  Timer Service callback path that was overflowing the timer stack
  during WPA3-SAE association. Event-group bits and includes adjusted
  accordingly.
- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0` in `sdkconfig.defaults` /
  `sdkconfig.esp32s3` so mDNS and other components can spill into PSRAM
  instead of starving internal heap.
- OTA: bootloader-style autodiscovery of the active partition slot and
  SHA-256 verification of the staged image before commit.
- SPIFFS partition is now selected by label rather than fixed offset,
  matching the documented upload workflow.

#### MCP / Python lib (target-device control)
- New MCP tools: `target_power_up`, `enter_bootloader`,
  `release_bootloader` for ESP32-target-on-DUT flashing flows.
- `BugBuster.uart_bridge_*` HTTP config endpoints and matching MCP
  schema, allowing pin map (`tx_io`, `rx_io`, `boot_io`) and baud to be
  set without a USB session.
- HTTP admin-token auth wired through the MCP server (same token
  scheme as the rest of the HTTP API).
- `python` package gained a `network` extra (`zeroconf`, `websockets`)
  for the new notebook autodiscovery flow.

#### Desktop
- App-launcher tab with per-target tiles; tiles surface live logs and
  expose start/stop button actions (Tauri commands wired through to the
  desktop runtime).
- Notebook integration: desktop-app commands can now be invoked live
  from a notebook session against a running desktop instance.

#### Notebooks
- ESP32 notebook gained device autodiscovery (zeroconf-based) and an
  end-to-end OTA workflow that drives the new
  autodiscovery + SHA-256 verification path.

### Fixed

#### ESP32 / target flashing
- `enter_bootloader` sequencing: BOOT held low **before** VADJ is
  applied, MUX writes go through the direct register path
  (`dio_configure`/`dio_write` for the BOOT pin), and the MUX is
  re-asserted after every firmware call because `bus_planner` clobbers
  it on each entry. BOOT is now held low until `esptool` actually
  connects, then released.
- Default pin mapping corrected (`tx_io=2`, `rx_io=3`, `boot_io=1`) and
  VADJ is power-cycled at the start of the sequence.
- Workaround for the firmware TX MUX bug: IO directions are re-asserted
  after `set_uart_config` because the UART driver was clearing them.
- `uart_bridge` no longer stalls USB: GPIO matrix is pre-routed before
  `uart_driver_install` (was causing SPI corruption when the driver
  re-claimed pins after install).
- Target tools use `session.get_hal()` instead of the stale `bb.hal`
  global, so power/MUX state stays coherent across calls.
- Small-task allocation regression fixed (heap-caps path).

#### MCP schemas
- `mux_control` schema rewritten to be OpenAI-compatible (no array
  schemas, no nested unions); tool re-enabled after temporary disable.

### Changed
- ESP32 UART bridge power sequencing tightened (VADJ off → pin
  pre-route → VADJ on → driver install) to match the bootloader entry
  path.
- Desktop launcher tiles now stream backend logs into the tile panel
  instead of only the global log view.

### Notes (bench-pending)
- WiFi reconnect task: validated against the WPA3-SAE overflow; long-soak
  reconnect-storm behaviour still to be measured.
- OTA SHA-256 verification path: golden-image flow confirmed; failure
  injection (truncated/tampered image) pending.

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
