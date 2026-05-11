# Whole-Repo Audit — 2026-05-11

Read-only audit across 10 surfaces of the BugBuster repository. Findings are
ranked HIGH (security / data loss / crash / wire-protocol break) → MEDIUM
(wrong behavior under realistic conditions) → LOW (polish / dead code / nit).
Each finding includes a `file:line` anchor, a one-line fix sketch, and a
one-line test idea. **No code changes have been made.** Implementation is a
separate task; this document is the worklist.

Surfaces audited:

1. RP2040 firmware (`Firmware/RP2040/src/**`)
2. ESP32 firmware core (`Firmware/ESP32/src/**`, excluding `web/`)
3. ESP32 on-device web UI (`Firmware/ESP32/web/src/**`)
4. Desktop frontend (`DesktopApp/BugBuster/src/**`, Leptos WASM)
5. Desktop backend (`DesktopApp/BugBuster/src-tauri/src/**`, Rust)
6. Python library (`python/bugbuster/**`)
7. MCP server (`python/bugbuster_mcp/**`)
8. Tests + simulator (`tests/**`)
9. Docs, notebooks, READMEs, CHANGELOG, ROUTER
10. CI / scripts / tooling (`.github/workflows`, `Firmware/tools`, `Scripts/`, `DesktopApp/.../scripts`, `.mex/setup.sh`)

Totals: **18 HIGH · 26 MEDIUM · 16 LOW = 60 findings.**

---

## HIGH severity

| ID | Surface | file:line | Finding | Fix sketch | Effort | Test |
|----|---------|-----------|---------|------------|--------|------|
| H01 | RP2040 | `Firmware/RP2040/src/bb_la_usb.c:264-270` | `bb_la_usb_soft_reset()` increments `s_rearm_request_count` twice without consuming the flag, desyncing the rearm handshake if the USB task misses the first increment. | Remove the duplicate increment at line 269; single atomic bump with comment. | S | Assert `s_rearm_request_count − s_rearm_complete_count ≤ 1` across STOP/START cycles. |
| H02 | RP2040 | `Firmware/RP2040/src/bb_la_usb.c:280` vs `:246` | Mixed sync primitives across cores: `bb_la_usb_abort_bulk()` uses `taskENTER_CRITICAL` while `bb_la_usb_soft_reset()` uses `save_and_disable_interrupts()`. Core 0 / Core 1 may race on `s_bulk_data.active`, `s_need_endpoint_rearm`, ring indices. | Use the existing `s_dma_lock` spinlock everywhere that touches cross-core state. | M | 1ms-interval STOP+START stress while capturing — no corruption, no endpoint hang. |
| H03 | RP2040 | `Firmware/RP2040/src/bb_la.c:264-270` ↔ `bb_la_usb.c:351-401` | DMA IRQ can fire during endpoint re-arm in `send_pending()`, mutating `s_stream_ring` while `s_deferred_stop_retries` is being cleared. | Hold the DMA spinlock across the entire re-arm sequence (382→401), or defer DMA restart until USB FIFO state is stable. | M | Trigger endpoint abort with a half-buffer DMA completion pending; verify ring head/tail/count stay coherent. |
| H04 | ESP32 | `Firmware/ESP32/src/bbp/bbp.cpp:166` | `sendMsg()` guards on `s_txMutex && xSemaphoreTake(...)` but the mutex is created lazily in `bbp_init()` (line ~458). Any sender between boot and init silently skips the guard and corrupts `s_msgBuf`/`s_cobsBuf`. | Create `s_txMutex` before any task that can call `sendEvent`/`sendResponse` is spawned (or gate senders on `s_active`). | S | Force an alert before `bbp_init()` and confirm CRC of resulting frame. |
| H05 | ESP32 | `Firmware/ESP32/src/bbp/cmds/cmd_idac.cpp:102` vs `:29` | `IDAC_SET_VOLTAGE` rejects `ch >= 3`, but `IDAC_GET_STATUS` iterates `DS4424_NUM_CHANNELS == 4` and emits 44 bytes for channel 3 from possibly-uninitialized state. | Unify bounds: either accept channels 0..3 everywhere, or exclude channel 3 from `GET_STATUS`. | S | `IDAC_GET_STATUS` after boot returns deterministic channel-3 fields, not garbage. |
| H06 | ESP32 | `Firmware/ESP32/src/net/wifi_manager.cpp:72-126` | `nvs_set_str(sta_pass)` failure is logged but `nvs_commit()` runs anyway; caller never sees the set error because only the commit return is checked. | Bail on `set_err != ESP_OK` before commit; propagate to caller (note: this is on the persist path that H4 already touches via BBP 0xEF). | S | Inject NVS-quota-full and confirm `wifi_set_ap_password` returns persist-failed (0x02). |
| H07 | Web UI | `Firmware/ESP32/web/src/App.tsx:104` | Operator precedence bug in the polling-failure log condition: `!(e instanceof PairingRequiredError) && consecutiveFailures === 1 || consecutiveFailures === stage` binds as `(A && B) || C` — logs on the wrong transitions. | Parenthesize: `(!(e instanceof PairingRequiredError)) && (consecutiveFailures === 1 || consecutiveFailures === stage)`. | S | Force 3 sequential failures and assert console output only at backoff stage transitions. |
| H08 | Web UI | `Firmware/ESP32/web/src/components/SupplySliderCard.tsx:58,63` | Effect on `currentTarget` overwrites the in-edit slider value when the 2.5 s `/api/overview` poll lands mid-drag. | Track `isDirty` and only sync server state on blur/apply/cancel, not during edits. | S | Drag slider, wait through 2 poll cycles, verify position is preserved until Apply. |
| H09 | Desktop FE | `DesktopApp/BugBuster/src/tauri_bridge.rs:101` | `DeviceState` in the frontend declares `channels`, `diag`, `gpio`, `mux_states` as `Vec<…>` while the Tauri backend (`src-tauri/src/state.rs`) emits `[T; 4]` / `[T; 12]` fixed arrays. Causes silent deserialization issues. | Change frontend to fixed arrays matching backend (`[ChannelState; 4]`, `[DiagState; 4]`, `[GpioState; 12]`, `[u8; 4]`). | S | Round-trip a real `DeviceState` over the bridge and assert every slot populates. |
| H10 | Desktop FE | `DesktopApp/BugBuster/src/app.rs:79-82, 407-410, 520-523` | `.unwrap()` on `set_timeout`, `JsFuture`, canvas 2D context, and `dyn_into` inside `ParticleBackground` setup and toast dismissal — any failure during unmount panics the UI thread. | Use `.ok()` / `if let Some(_) = …` patterns; log on failure. | S | Rapidly minimize/restore window during particle animation and during toast fade — no panic. |
| H11 | Desktop BE | `DesktopApp/BugBuster/src-tauri/src/http_transport.rs:1303-1310` | Bare `.unwrap()` on `PayloadReader::get_u8()` for `CMD_WIFI_CONNECT` / `CMD_WIFI_SCAN`. A truncated WiFi payload over HTTP panics the backend. | Replace with `.ok_or_else(|| anyhow!("Malformed WiFi payload"))?`. | S | Send `ssid_len=10` with only 4 bytes following; assert clean error instead of panic. |
| H12 | Desktop BE | `DesktopApp/BugBuster/src-tauri/src/state.rs:84-85` | `DeviceState::from_status_payload()` silently `unwrap_or(0)`s optional trailing fields (`channel_alert_mask`, `rtd_excitation_ua`). A firmware payload shrinkage gives zeros instead of a parse error. | Return `Option<DeviceState>` (or `Result`) and fail when remaining bytes diverge from the spec; warn-log on short payloads. | S | Truncate a real status response by N bytes and assert parse failure with a firmware-mismatch hint. |
| H13 | Python | `python/bugbuster/client.py:305` | `length = resp[0]` immediately after `_usb_cmd(CmdId.GET_ADMIN_TOKEN)` without a length guard. Empty response → `IndexError`. | Add `_require_resp_len(resp, 2, "GET_ADMIN_TOKEN")`. | S | Mock device returning `b""`; expect `ProtocolError`, not `IndexError`. |
| H14 | Python | `python/bugbuster/client.py:2582,2588` | `hat_hvpak_reg_read()` and `hat_hvpak_reg_write_masked()` unpack `resp[0..3]` without verifying `len(resp)`. | Add `_require_resp_len(resp, 2/4, …)` matching the spec for each handler. | S | Feed truncated responses through the simulator; expect clean `ProtocolError`. |
| H15 | Python | `python/bugbuster/client.py:437-443` | Static `_parse_hvpak_bridge()` indexes `resp[0..8]` with no length check. | Guard with `if len(resp) < 9: raise ProtocolError(...)`. | S | 0-, 4-, 8-byte payloads all raise `ProtocolError` with a useful message. |
| H16 | Tests | `tests/mock/handlers/misc.py` (missing) | Simulator has no handler for `WIFI_SET_AP_PASSWORD` (BBP 0xEF) added 2026-05-04; any client call against `--sim` returns `INVALID_CMD`. | Register `_wifi_set_ap_password` that consumes `(len, bytes)` and returns the 3-way status byte (0x00/0x01/0x02). | S | New `tests/device/test_08_wifi.py::test_wifi_set_ap_password_*` over the `--sim` path. |
| H17 | Tests | `tests/simulator/test_sim_completeness.py:31` | The completeness test only checks handler **existence**, not payload validation. Many handlers (e.g., `_register_read`, `_wifi_scan`, `_set_dac_code`) accept arbitrary payloads and return canned successes, hiding real firmware bugs. | Add parametrized payload-fuzz cases: invalid channel, bad length, out-of-range enum → assert each handler errors. | M | New `test_handlers_validate_payload_bounds` with a small fuzz vector per command. |
| H18 | CI | `.github/workflows/desktop-release.yml:64,74,96` | Three third-party actions (`dtolnay/rust-toolchain@stable`, `Swatinem/rust-cache@v2`, `tauri-apps/tauri-action@v0.6.2`) pinned to floating tags rather than commit SHAs. Supply-chain risk on the desktop signing path. | Pin each to a full commit SHA; keep a comment with the release tag for readability. | S | Re-run a release build and verify outputs sign and bundle identically. |

---

## MEDIUM severity

| ID | Surface | file:line | Finding | Fix sketch | Effort | Test |
|----|---------|-----------|---------|------------|--------|------|
| M01 | RP2040 | `Firmware/RP2040/src/bb_la_usb.c:438,443,450` | RLE fallback path doesn't release `s_bulk_data.ring_buf` symmetrically with the compressed path; second buffer can arrive before the first is released. | Save `ring_buf` reference before compression; release on both success and raw-fallback exits. | S | Send 2 compressible + 1 random buffer in quick succession; all three ring slots release within 1 s. |
| M02 | RP2040 | `Firmware/RP2040/src/bb_la_usb.c:551,576` | Deferred-stop retry cap (10 000) triggers an emergency PKT_STOP + `bb_la_stop()` silently; next stream-start can begin before the host has drained the stop, interleaving sessions. | Add a diagnostic counter on emergency-path firings, shorten the deadline (≈100 retries), and gate on endpoint mount status. | S | Force a stuck IN endpoint; host receives PKT_STOP and a clear error within 1 s. |
| M03 | RP2040 | `Firmware/RP2040/src/bb_main.c:502-527` | HVPAK `handle_set_hvpak_pwm()` memsets the struct then never assigns `cfg.current_value` from payload — silently drops initial PWM state. | Assign the field from payload, or document `current_value` as read-only. | S | Set PWM with `initial_value=128` then read back and assert it matches or is explicitly documented. |
| M04 | ESP32 | `Firmware/ESP32/src/bbp/cmds/cmd_idac.cpp:147,220` | `adc_ch` bounds (`>= 4`) and ArgSpec max (`3`) are decoupled. A future caller editing only the spec can drift silently. | Introduce `#define ADC_CHANNELS 4` and use it on both sides. | S | Fuzz `adc_ch` with 4 / 16 / 255 → all rejected. |
| M05 | ESP32 | `Firmware/ESP32/src/mp/scripting.cpp:392` | REPL `for(;;)` polls `xQueueReceive` with `MP_IDLE_CHECK_MS` timeout; if the queue is saturated and timeout is 0 the task never yields, starving idle / tripping WDT. | Compile-time assert `MP_IDLE_CHECK_MS > 0` and add a `vTaskDelay(1)` fallback. | S | Saturate the script queue and observe idle-task progress / WDT-quiet boot log. |
| M06 | ESP32 | `Firmware/ESP32/src/web/webserver.cpp:61,~4298` | `static s_server` written without sync. Recovery + manual `webserver_start()` can race. | Wrap assignment in a static mutex (or use atomic CAS). | S | Concurrent `webserver_start()` calls → second returns `ESP_FAIL`, never corrupts the handle. |
| M07 | Web UI | `Firmware/ESP32/web/src/tabs/scope/ScopeCanvas.tsx:122,82,138-143` | EventSource error path triggers fallback polling, but the existing fallback timer isn't always cleared → dual polling on slow reconnect. | Always null-out `pollFallbackTimer` on error and guard `startFallbackPolling` with the existing `usingFallback` atom. | S | Open Scope on legacy firmware without `/api/scope/stream`; assert exactly one polling channel in DevTools. |
| M08 | Web UI | `Firmware/ESP32/web/src/tabs/system/SignalPath.tsx:199` | `useEffect(() => { ... })` with no deps reads `deviceStatus.value` every render. | Add `[deviceStatus.value]` dependency or drop the effect. | S | Toggle lshift via API; UI updates once, not per render. |
| M09 | Web UI | `Firmware/ESP32/web/src/api/client.ts:266-281` | `request()` only wraps HTTP errors via `res.ok`; network/fetch rejections escape as unhandled promise rejections. | Wrap fetch in try/catch and normalize to `HttpError`. | S | DevTools → offline; every endpoint call surfaces a clean error. |
| M10 | Web UI | `Firmware/ESP32/web/src/tabs/analog/Analog.tsx:77-78` | `pendingVoltage` can be visually clobbered by `/api/overview` poll-driven re-render (no direct state overwrite, but the recomputed `cfg` value flicks the UI). | Track `lastEditMs` and suppress poll-driven UI updates within ~5 s of user interaction. | S | Edit ADC config with a slow apply (throttled network) — stays in edited state through Apply. |
| M11 | Desktop FE | `DesktopApp/BugBuster/src/tauri_bridge.rs:20-51` | `try_invoke` collapses `Err` and `Ok(null)` to `None`; tabs can't distinguish success-null from a failed Tauri call. | Return `Result<Option<T>>` so callers can decide. | S | A cancel-with-null command (e.g., file-picker cancel) stays distinct from an error. |
| M12 | Desktop FE | `DesktopApp/BugBuster/src/tabs/calibration.rs:89`, `src/tabs/signal_path.rs:106` | `set_interval_with_handle` / `spawn_local(slp(500))` loops aren't always cancelled on unmount; the `alive` flag covers `signal_path` but `calibration`'s poll handle relies on a sometimes-unset `RwSignal`. | Add `on_cleanup(...)` that cancels the interval / sets `alive=false`. | S | Toggle calibration on/off 50× and grep DevTools for "update after unmount" warnings. |
| M13 | Desktop FE | `DesktopApp/BugBuster/src/tabs/la.rs:154`, `src/tabs/scope.rs:402-413` | `.parse().unwrap()` on numeric inputs panics the effect on bad UI state. | Replace with `.unwrap_or(sensible_default)`. | S | Paste `"abc"` into LA rate → effect logs once, doesn't crash. |
| M14 | Desktop FE | `DesktopApp/BugBuster/src/tabs/signal_path.rs:80-84,118-134` | `psu_inflight` / `ef_inflight` boolean masks can stay stuck if a second action fires before the 700 ms timer elapses, or get bypassed if ACK arrives past 700 ms. | Replace booleans with `last_action_time` timestamps; suppress poll only when `now - last < 700 ms`. | S | Two rapid PSU toggles — both apply, neither masked. |
| M15 | Desktop BE | `DesktopApp/BugBuster/src-tauri/src/connection_manager.rs:157,168` | Poisoned `connection_status` mutex is silently recovered with `.unwrap_or_else(|e| e.into_inner())`; the UI gets stale state with no warning. | Add `log::warn!("ConnectionStatus mutex poisoned …")` on recovery; consider a generation counter for stale-read detection. | S | Panic a thread holding the lock; warning appears and UI emits a connection-state event. |
| M16 | Desktop BE | `DesktopApp/BugBuster/src-tauri/src/usb_transport.rs:268-276` | A mid-frame `port.write_all` failure leaves the port in undefined state; subsequent `send_command` can silently desync sequence numbers. | Track a `write_failed` flag and refuse further writes until explicit reconnect. | S | Simulate partial-write error; next command fails fast with "write failure detected". |
| M17 | Desktop BE | `DesktopApp/BugBuster/src-tauri/src/keychain.rs:636,646-686` | Keychain migration holds the mutex through the whole block (good), but the in-memory cache can be queried by concurrent `save_token`/`get_token` callers before migration completes, causing repeated keychain lookups. | Pre-load all keys into a local map and swap atomically; or gate on a `once_cell::sync::Lazy`. | S | Call `load_tokens()` then immediately `get_token()` for a known MAC; no second keychain call. |
| M18 | Desktop BE | `DesktopApp/BugBuster/src-tauri/src/usb_transport.rs:234,258,304-307` | `read_capture_blocking()` has no timeout (stream variant has 5 s). Stuck DMA on firmware → indefinite USB-mutex hold blocking reconnect. | Add a 30–60 s timeout and clear `stream_buffer` on timeout. | S | Unplug device mid-capture; reconnect succeeds without app restart. |
| M19 | Desktop BE | `DesktopApp/BugBuster/src-tauri/src/http_transport.rs:90-114` | Legacy-firmware fallback uses `legacy:<url>` as keychain key, so the same device on a new IP can never re-pair. | Emit a UI event prompting firmware update; fall back to device serial if available. | S | Connect to legacy firmware over HTTP → "update firmware" toast; USB pair then HTTP-reconnect with same token. |
| M20 | Python | `python/bugbuster/transport/usb.py:343` | Reader-loop catches `Exception` and breaks → silently loses non-`SerialException` faults. | Narrow to `serial.SerialException` for transients; log others with `exc_info=True`. | S | Inject a non-Serial exception in the reader; it isn't silently dropped. |
| M21 | Python | `python/bugbuster/client.py:359,413` | `_usb_cmd()` drain/retry loops use bare `except Exception`. | Restrict to `(OSError, serial.SerialException)` and log when caught. | S | Inject a port-closed error mid-drain; debug log shows it. |
| M22 | Python | `python/bugbuster/ota.py:223` | `on_progress` callback failures swallowed with `except Exception: pass`. | `log.warning("Progress callback raised: %s", e)`. | S | Progress callback that raises → warning logged, upload continues. |
| M23 | MCP | `python/bugbuster_mcp/tools/io_config.py:88,134` | `configure_io` / `set_supply_voltage` swallow exceptions during fault-check (`except Exception: pass`). Safety warnings silently lost. | Append a warning to the tool result; never silent-drop. | S | Inject a fault-check error; tool result includes a "could not check faults" warning. |
| M24 | MCP | `python/bugbuster_mcp/tools/waveform.py:175-182,277-288` | `capture_adc_snapshot` and `capture_logic_analyzer` busy-wait with `time.sleep`, blocking the MCP event loop up to 10 s / 30 s respectively. | Job-id pattern: tool returns immediately with a handle; separate `*_status` and `*_result` tools poll. | M | Issue `capture_logic_analyzer`; concurrent tool call gets a response within 100 ms. |
| M25 | MCP | `python/bugbuster_mcp/tools/analog.py:138-150` | `write_current` accepts `allow_full_range` but doesn't fully validate that bipolar mode + voltage > 10 V is within device limits. | Reuse the existing `validate_dac_current` and add a bipolar-voltage guard. | S | `write_current(io=3, current_ma=25, allow_full_range=True, voltage=12)` → ValueError. |
| M26 | Tests | `tests/device/test_10_streaming.py:~40` | Streaming tests rely on `event_ready.wait(0.5)` with no explicit timeout assertion — flaky on slow CI. | Raise timeout to 2 s and `assert event_ready.is_set(), "Scope stream did not start within 2s"`. | S | Run the streaming tests under CPU stress; assertion fires with a clear message. |

---

## LOW severity

| ID | Surface | file:line | Finding | Fix sketch | Effort | Test |
|----|---------|-----------|---------|------------|--------|------|
| L01 | RP2040 | `Firmware/RP2040/src/bb_config.h` | Stale `TODO(user): confirm GPIO29` after HAT v2.1 ships. | Resolve & remove the TODO. | S | Compile passes with no TODO marker on a `grep TODO`. |
| L02 | RP2040 | `Firmware/RP2040/src/bb_main.c:131-132,42-46` | `#ifndef BB_HAT_FW_MAJOR` fallback to `0/0` is intentional sentinel, but unprotected build also yields `0.0` silently. | Add `#if BB_HAT_FW_MAJOR == 0 && BB_HAT_FW_MINOR == 0` → `#error "Version not set by CMake"`. | S | `cmake ..` without `-D` flags fails fast. |
| L03 | ESP32 | `Firmware/ESP32/src/diag/board_profile.cpp:76` | `nvs_get_str(active_id)` failure leaves `id[32]` uninitialized; the early-return path doesn't log root cause. | `id[0] = '\0';` before the call; log esp error on non-ESP_OK. | S | Corrupt the `active_id` key; boot log includes the error code. |
| L04 | ESP32 | `Firmware/ESP32/src/hal/ds4424.cpp:547,551` | `nvs_set_blob` succeeds but `nvs_commit` can fail, leaving half-written calibration. | Mark cal as "pending" until commit returns; rollback or retry. | S | Inject commit failure; next read doesn't see the partial blob. |
| L05 | Web UI | `Firmware/ESP32/web/src/tabs/scripts/Scripts.tsx:680-685` | `drainLogs()` swallows fetch errors silently; unpairing mid-session shows nothing in the UI. | Surface "Pairing lost" banner when `/api/scripts/logs` returns 401. | S | Pair, open Scripts, revoke token; banner appears within one poll. |
| L06 | Web UI | `Firmware/ESP32/web/src/components/PairingModal.tsx:69-73` | Uses native `window.confirm` for rotation warning. Not a real CSRF issue but breaks pattern. | Replace with the existing `GlassCard` modal. | S | Visual / a11y check. |
| L07 | Desktop FE | `DesktopApp/BugBuster/src/app.rs:59-93,…,216` | Six `Closure::forget()` listeners with no leak instrumentation. | Add a comment noting intent and consider typed channels later. | S | 10-minute WASM heap profile shows no unbounded growth. |
| L08 | Desktop FE | `DesktopApp/BugBuster/src/tabs/scope.rs:239,299-367` | Intentional app-lifetime listener is not documented as such. | Add a one-line comment: "INTENTIONAL: app-lifetime listener — do not cleanup". | S | Comment present; reviewers don't strip it next time. |
| L09 | Python | `python/setup.py` vs `python/pyproject.toml` | `setup.py` declares `python>=3.10`, `pyproject.toml` declares `>=3.11`. | Delete `setup.py`; PEP 517 is sufficient. | S | `pip install -e python/` succeeds on 3.11+ only. |
| L10 | Python | `python/bugbuster/client.py:2180` | Catch-all `except Exception` in HAT detection is `# noqa: BLE001` but masks transport crashes as `HatNotPresentError`. | Restrict to `(TimeoutError, ProtocolError, DeviceError)`. | S | Yank USB during `hat_get_status`; raises USB error, not "HAT not present". |
| L11 | MCP | `python/bugbuster_mcp/server.py:8` | Docstring says "28 tools in 9 groups"; actual count is **45**. | Update docstring (and rename groups list if needed). | S | `grep -c "@mcp.tool" python/bugbuster_mcp/tools/*.py` matches docstring. |
| L12 | MCP | `python/bugbuster_mcp/tools/ota.py:35-36` | Error message names the `--admin-token` flag, leaking server CLI surface to clients. | Generic "OTA requires admin authentication" string. | S | Trigger the error; output mentions no flags. |
| L13 | MCP | `python/bugbuster_mcp/tools/discovery.py:123-126` | `except Exception: pass` around fault-log fetch. | Append a warning, never silent-drop. | S | Inject a fault-log error; response includes a degraded-state hint. |
| L14 | MCP | `python/bugbuster_mcp/prompts/workflows.py` | Prompts reference `bugbuster://board` / `bugbuster://hat` resources with no runtime validation. | At `register()` time, assert every referenced resource is present. | S | Remove the `board` resource; startup logs/raises a clear error. |
| L15 | Tests | `tests/device/test_01_core.py:20-33` | Ping test hard-codes `result.token == 0xDEADBEEF` which is also the simulator's canned response — simulator and HW look identical even if HW behavior diverges. | Send a random token and assert echo equality. | S | Sim test stays green; if HW ever stops echoing it fails. |
| L16 | Tests | `tests/conftest.py:196,~258` | `--sim` and `--hat` aren't mutually exclusive; running both yields confusing HAT-skip messages instead of an error. | In `pytest_configure`, `pytest.exit("--sim and --hat are mutually exclusive")` if both are set. | S | `pytest tests/device/ --sim --hat` exits cleanly with the message. |

---

## Documentation defects (resolved inline below — no new doc files)

These are user-visible inaccuracies in shipped docs. Listed separately because
they are small, self-contained, and should be batched as a single doc-only PR
rather than mixed with code fixes.

| ID | file:line | Finding | Fix |
|----|-----------|---------|-----|
| D01 | `README.md:15` | Firmware badge shows `ESP 3.0.0 · HAT 2.0`; canon (line 255 + CHANGELOG v1.0.0) is `ESP 3.1.0 · HAT bb-hat-2.1 · Desktop 0.6.0`. | Update the shields.io URL components. |
| D02 | `Docs/ReleaseChecklist.md:78-80` | Pre-push `--expect` values are stale: ESP `3.0.0`, HAT `2.0`, Desktop `0.5.0`. | Bump to `3.1.0`, `bb-hat-2.1`, `0.6.0` respectively. |
| D03 | `Docs/ReleaseChecklist.md:68,80,88-89` | Desktop tag examples use `0.5.0`. | Replace with `0.6.0`. |
| D04 | `Docs/LogicAnalyzer.md:7,69` | Hardcoded `bb-hat-2.0`. | Replace with `bb-hat-2.1`. |
| D05 | `README.md:220,324,358` | "28 tools" repeated; actual is **41** (per CHANGELOG) — and the MCP server itself currently has 45 (see L11). | Update to "41 tools" and align the MCP docstring (L11) in the same PR so both numbers match reality. |
| D06 | `README.md:257` | "Release workflow + version-sync checklist: … — to be added" but the file is substantial and shipped. | Drop "— to be added". |

---

## CI / supply-chain (already-listed in HIGH/MEDIUM tables, summarized here)

- **H18** — Pin floating-tag third-party actions in `desktop-release.yml` to SHAs.
- **M27 candidate** — `.github/workflows/proto-version-check.yml:24` uses `actions/checkout@v4` while others use `@v6.0.2`. Standardize on `@v6.0.2` for parity (not promoted to its own row since it's a one-line edit captured here).
- **L17 candidate** — `Firmware/tools/firmware_version.py:54-68` validates non-guarded RP2040 defines but skips `#ifndef` fallback values, so a developer can land a wrong guarded define and pass CI. Extend regex to also extract guarded values and compare against `CMakeLists.txt`. (Captured here; not in the LOW table to keep it at 16 surface-level entries.)
- **M28 candidate** — `.github/workflows/rp2040-firmware.yml:63` clones `picotool` over HTTPS with no tag or signature pin. Pin a tag and `git verify-commit` HEAD. (Captured here.)
- **L18 candidate** — `Scripts/test_dedup`, `test_la_bug`, `test_la_bug2` are committed ELF binaries (~460 KB each) with no CI references. Either document or delete. (Captured here.)
- **L19 candidate** — `.mex/setup.sh:295` writes `/tmp/mex_scanner_$$.json` without `trap … EXIT` cleanup. (Captured here.)

These six are tracked but not double-counted in the per-severity totals at the
top of this document.

---

## Constraints respected (per `AGENTS.md`)

The following non-negotiables were **not** flagged as bugs and any agent
suggestion that violated them was rejected at aggregation time:

1. USB descriptor subclass patch in `bb_usb_descriptors.c`.
2. TinyUSB single-thread rule; `s_need_endpoint_rearm` + `bb_la_usb_send_pending()` pattern.
3. `tud_vendor_n_write_clear()` reserved for genuine recovery only.
4. No `bb_la_log()` from inside `bb_la_usb_send_pending()`.
5. BBP `PROTO_VERSION` lockstep across the three files (audited: all three still `4`).
6. RP2040 version in both `CMakeLists.txt` and `bb_main.c` (audited: matches).
7. Desktop version across three files (audited: `0.6.0` everywhere).
8. Core affinity (cmd→Core 1, usb→Core 0).
9. `HatPinFunction` reserved slots 1..4.
10. No pre-commit-hook skips / force-pushes / `--amend` on published commits.

---

## Recommended next steps

1. Land **HIGH** in three batched PRs grouped by surface to keep diffs focused:
   - Firmware safety (H01–H06).
   - Frontend + WASM panics (H07–H12).
   - Python length-guards + simulator parity (H13–H17), plus CI pinning (H18).
2. Bundle **MEDIUM** by surface as follow-up PRs once HIGH is bench-verified.
3. Treat the **D**-row documentation defects as a single doc-only PR — they all
   resolve to find/replace edits and are user-visible.
4. **LOW** items can ride along on adjacent feature PRs; no need for a dedicated pass.
