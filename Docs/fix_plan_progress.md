# Firmware-Wide Audit & Fix Plan — Progress Log

**Started:** 2026-04-27
**Plan reference:** `/Users/lorenzo/.claude/plans/pure-meandering-spark.md`
**Audit scope:** Whole ESP32 firmware (`Firmware/ESP32/`), not only `mp/` MicroPython surface.
**Output documents (when complete):**
- `Docs/firmware-audit-2026-04-27.md` — consolidated audit + prioritised fix list
- `.mex/context/firmware-fields.md` — per-field navigation index
- `.mex/context/scripting-runtime.md` — scripting-specific deep-dive
- `.mex/patterns/audit-firmware-field.md` — reusable audit pattern

This file is the **live work log**. It updates each time a wave returns or a finding is processed. Read this first when re-entering the session.

---

## Status Overview

| Phase | State | Wave | Notes |
|---|---|---|---|
| 1A | 🟢 Done | A1 ✅ mp/ · A2 ✅ mp-port · A3 ✅ web/ | A1: 13/11/5/3/2 · A2: 5/4/3/1/3 · A3: 5/3/2/2/3. Total: 23 bugs / 18 slop / 10 perf / 6 lock / 8 drift. **3 sev-5**, ~10 sev-4. Schema clean. |
| 1B | 🟢 Done | B1 ✅ bbp/ · B2 ✅ bus/ · B3 ✅ hal/ | B1: 8/1/0/0/2 · B2: 9/4/4/2/4 · B3: 2/4/0/1/1. **B3 sev-5 EFUSE fault-cross — needs spot-check vs B2 finding** (different code path). |
| 1C | 🟢 Done | C1 ✅ net/ · C2 ✅ cli/ · C3 ✅ diag/ | C1: 5/4/2/2/3 · C2: 6/5/3/1/4 (sev-5 stack smash + sev-4 BBP gate bypass) · C3: 4/3/2/1/2. |
| 1D | 🟢 Done | D1 ✅ top-level · D2 ✅ dio+hat | D1: 8/5/2/0/1 · D2: 5/3/2/2/5. **PHASE 1 COMPLETE.** |
| 2 | 🟢 Done | `.mex/` index | `context/firmware-fields.md`, `context/scripting-runtime.md`, `patterns/audit-firmware-field.md`, `patterns/INDEX.md` row, `ROUTER.md` rows + edges + last_updated. |
| 3 | 🟢 Done | Synthesis & audit doc | `Docs/firmware-audit-2026-04-27.md` written: exec summary, Wave-1 sev-5 shortlist (8), Wave-2 auth/integrity (8 sev-4), Wave-3 concurrency (10 sev-4), Wave-4 lower-sev by field, cross-cutting themes, V2 follow-ups, recommended fix sequencing. |

**Legend:** 🟢 done · 🟡 running · 🔴 blocked · ⏳ queued · ⚠️ partial

---

## Findings Tally (live)

| Field | Bugs | Slop | Perf | Lock-order | Contract drift | Top severity |
|---|---|---|---|---|---|---|
| mp | 13 | 11 | 5 | 3 | 2 | **sev 4** (scripting_run_file persist uninit) |
| micropython_port | 5 | 4 | 3 | 1 | 3 | **sev 5** (duplicate `mp_hal_set_interrupt_char`) |
| web | 5 | 3 | 2 | 2 | 3 | **sev 5** (CORS UAF + selftest NULL-deref) |
| bbp | 8 | 1 | 0 | 0 | 2 | **sev 4** (admin-flag dead, OOB on token, ArgSpec missing flags) |
| bus | 9 | 4 | 4 | 2 | 4 | **sev 5** (rank-4→rank-2 deadlock + VADJ-before-MUX HW damage) |
| hal | 2 | 4 | 0 | 1 | 1 | **sev 5** (pca9535 EFUSE fault-cross — kills wrong channel on overcurrent) |
| net | 5 | 4 | 2 | 2 | 3 | **sev 4** (CDC ring race; UART bridge bypasses bus_planner; wifi globals unguarded) |
| cli | 6 | 5 | 3 | 1 | 4 | **sev 5** (cli_edit stack smash on long tab-complete) |
| diag | 4 | 3 | 2 | 1 | 2 | **sev 3** (board_profile rail-locks ignored by AutoCal; ChD restore race; supply_volt torn read) |
| top-level | 8 | 5 | 2 | 0 | 1 | **sev 4** (g_stateMutex NULL during 750ms ISR-vs-init window) |
| dio + hat | 5 | 3 | 2 | 2 | 5 | **sev 4** (s_hat_mutex unranked + bbpSendEvent under lock; DIO bypasses bus_planner) |

---

## Critical findings shortlist (filled as waves return)

> Top sev-5 (crash/auth/RCE), then sev-4 (leak/lock-order/contract). Updated each wave.

### From `mp/` (Wave A1)

**sev-4**
- `scripting.cpp:652` — `scripting_run_file()` builds a `ScriptCmd` with `cmd.persist` **uninitialized**. File-runs from a PERSISTENT VM silently revert to EPHEMERAL teardown (V2-A contract violation).

**sev-3**
- `net_bridge.cpp:119` — HTTP body OOM path frees `s.body` then returns `-2`; caller in `modbugbuster_net.c:113` re-frees → double-free / heap corruption. Same pattern in `http_post` at line 173.
- `scripting.cpp:178` — `repl_ws_forward(str, len)` is **commented out** with `// V2-B disabled` despite V2-B.1 shipping. Web REPL never sees script stdout.
- `autorun.cpp:185` — Success predicate uses `||` instead of `&&`: `(total_errors==0 || last_error_msg[0]=='\0')`. Misreports failures as success; `total_errors` is cumulative across runs in PERSISTENT mode.
- `scripting.cpp:500` — Soft 80% watermark hit is recorded but `gc_collect()` is never called; V2-A spec line 156 requires it.
- `scripting.cpp:382` — `mp_stack_set_top()` uses `volatile int stack_dummy` which compiler may not allocate to stack — non-portable on LX7 -O2.
- `repl_ws.cpp:154` — `s_tx_used` read outside `s_tx_mutex` (data race).
- `repl_ws.cpp:206` — `session_close()` races with `repl_tx_task` shutdown on `s_repl_fd/s_session_count`.
- `scripting.cpp:609` — `s_next_id++` is not atomic; HTTP and BBP tasks both increment.
- `net_bridge.cpp:220` — `mqtt_publish` busy-polls 500×10 ms blocking GIL and ignoring `scripting_stop_requested()` for up to 5 s.
- `autorun.cpp:183` — `last_run_id` always reads 0 (status_set_done clears `current_script_id` before the post-run poll).
- `autorun.cpp:100` — `read_io12_high()` returns false on routing error; comment+message text are inverted relative to gate-3 inversion in code.

### From `web/` (Wave A3)

**sev-5**
- `webserver.cpp:137` — **Stack UAF** in `set_cors_headers()`: passes `char origin[96]` (stack-local) to `httpd_resp_set_hdr` which stores the raw pointer. Every CORS-matched response dereferences freed stack memory. Affects every route hit by a browser with a matching Origin.
- `webserver.cpp:1855` — **NULL deref** in `handle_post_selftest_calibrate()`: `cJSON_GetObjectItem(body,"channel")->valueint` with no null check. Missing field crashes the httpd worker task.

**sev-4**
- `webserver.cpp:3066` — **Missing admin auth** on `handle_bus_post_dispatch()`. `/api/bus/i2c/scan|read|write_read` and `/api/bus/spi/transfer` are reachable **unauthenticated** — anyone on the network can drive raw I2C/SPI.
- `webserver.cpp:3643` — **Contract drift**: `/api/scripts/status` emits `mode` as a JSON **number** but the V2 spec defines a **string enum**. Every client parsing this gets undefined/null. Cross-checks against `client.py` and `Scripts.tsx`.
- `repl_ws.cpp:245` — WS REPL session slot held forever by a connected-but-unauthed client (no read deadline / heartbeat). DoS for the single-session lock.
- `http_adapter.cpp:37` — Wildcard `Access-Control-Allow-Origin: *` on `/api/registry/*` contradicts the localhost-only policy elsewhere; CSRF surface.

**sev-3**
- `webserver.cpp:3730` — `static char s_names[...]` inside route handler corrupted by concurrent GETs (httpd has multiple workers).

### From `components/micropython/` (Wave A2)

**sev-5**
- `mphalport.c:123` — Duplicate strong symbol `mp_hal_set_interrupt_char`. Both the port stub and `shared/runtime/interrupt_char.c` (guarded by `MICROPY_KBD_EXCEPTION=1`) define it. Linker pick is non-deterministic — Ctrl-C in REPL may silently fail.

**sev-4**
- `scripting.cpp:382` — `mp_stack_set_top(&stack_dummy)` passes a mid-stack address; Xtensa stack grows down so `stack_top` must be the **highest** addr. GC under-scans upper frames → premature collection of live roots.
- `mphalport.c:56` — `mp_hal_delay_ms` does **not** release MP_GIL. Lock-order doc designates this as a mandatory release point. V2-G blocker.

**sev-3**
- `mphalport.c:112` — `mp_hal_stdin_rx_chr` competes with CLI for CDC0 FIFO; non-deterministic byte loss in REPL/`input()`.
- `bb_logging.py:20` — Calls non-existent `bugbuster.ticks_ms()`; `except AttributeError` permanently swallows → every timestamp prints `[----------]`. `time.ticks_ms` also disabled via `MICROPY_PY_TIME_*=0`.
- `mpconfigport.h:77/81` — `MICROPY_VFS=1` but `MICROPY_PY_VFS=0` — Python `os.mount` unavailable despite VFS being live in C; intentional but undocumented.
- `mphalport.h:70` — `poll()` stub as `static inline` in mphalport.h not visible to `vfs_posix_file.c` which `#include <poll.h>` (resolves to shim/poll.h, no `poll()` body). Fragile under LTO/`-Os`.
- `CMakeLists.txt:370` — `mpy-cross` rebuilt only if binary missing; submodule bump silently uses stale binary → bytecode-format mismatch with no build-time warning.

### From `bus/` (Wave B2)

**sev-5**
- `bus_planner.cpp:214` — **Confirmed lock-order inversion**: `bus_planner_apply_*` holds `s_planner_mutex` (rank 4) → calls `adgs_set_api_all_safe` → eventually `xSemaphoreTakeRecursive(g_spi_bus_mutex)` (rank 2). Classic ABBA. Under V2-G this is a deterministic deadlock; under v1 it's reachable any time another rank-2 holder later wants rank-4.

**sev-4**
- `bus_planner.cpp:170` — **Unsafe power sequence**. VADJ + e-fuse enabled in step 4 BEFORE step 5 programs the MUX switch. Stale MUX state can briefly route the freshly-energised rail onto the wrong IO terminal → potential damage to user hardware connected to neighbouring IOs.
- `bus_planner.cpp:230` — `vlogic_v` has no bounds check; only `supply_v > 5.0` is validated. NaN, negative, or 5.0 V on a 3.3 V level-shifter rail reaches `ds4424_set_voltage` unguarded.
- `bus_planner.cpp:181` — Silent VLOGIC mismatch: scripts can request `vlogic=2.9` and the DS4424 programs that exact voltage with no quantisation/feedback to the caller.

**sev-3**
- `bus_planner.h` — **No `bus_planner_release_*` API**. After `apply_*` succeeds, VADJ/VLOGIC/MUX/e-fuse stay on indefinitely. Sequential calls overlap reservations.
- `bus_planner.cpp:247` — No pin-conflict detection. `apply_i2c(2,3)` then `apply_spi(2,4,5,6)` reuses IO 2 across two configs; no rejection.
- `bus_planner.cpp:32` — Lazy `xSemaphoreCreateMutex()` race on first call → leaked mutex handle if two tasks hit `apply_*` simultaneously at boot.
- `bus_planner.cpp:214` — `adgs_set_api_all_safe` returns `void`; MUX programming failure is silent → rails stay on with unconfirmed signal path.

### From `bbp/` (Wave B1)

**sev-4**
- `bbp_adapter.cpp:12` — **`CMD_FLAG_ADMIN_REQUIRED` defined but never read** by `bbp_adapter_dispatch()`. Any future opcode marked admin-required is silently un-gated.
- `cmd_status.cpp:708` — `handler_get_admin_token()` truncates `tlen` to `uint8_t` for length prefix but does **not bound** before `memcpy(resp+pos, token, tlen)`. Token > 1023 bytes overruns `s_rspBuf` (1024-byte static).
- `cmd_script.cpp:45` & `:370` — **Wire-format drift**: handler reads `flags` byte at payload[0] (bit 0 = persist) but `s_script_eval_args` ArgSpec table omits the flags field. Adapters generating frames from ArgSpec produce malformed payloads; **persist=true is reachable only via raw binary**, every ArgSpec-built call mis-parses src_len.

**sev-3**
- `bbp.cpp:373` — CRC mismatch silently dropped, **no error frame returned to host** → host stalls until timeout (this is the documented "BBP CRC fragility" V1 follow-up).
- `bbp.cpp:284` — `s_evtSeq++` outside `s_txMutex`; concurrent senders can stamp duplicate sequence numbers, breaking host-side missed-event detection.
- `bbp.cpp:568` — `esp_log_level_set("*", ESP_LOG_NONE)` on handshake silences **all** ESP-IDF component logs globally for the connection lifetime.

### From `hal/` (Wave B3)

**sev-5**
- `pca9535.cpp:415-441` — **EFUSE3/4 fault response crossed.** `check_changes()` iterates raw physical bit positions but `decode_inputs`/`decode_outputs` apply the silkscreen-cross. So a physical FLT_3 (= logical EFUSE4) overcurrent reads as channel 3, gates on `efuse_en[2]` (EFUSE3 enable), logs the wrong channel, and auto-disables `PCA_CTRL_EFUSE3_EN` — **kills the wrong rail** while the faulting channel stays powered. Not contradicted by the bus/ agent's finding (that was on the enable-write path; this is the fault-response path).

**sev-4**
- `ad74416h.cpp:245-320` — RMW non-atomicity: `startAdcConversion`, `enableAdcChannel`, `clearAdcDataReady` release `g_spi_bus_mutex` between the read and write phases. Other callers (e.g. ADGS spi_transfer) can interleave; shadow CONFIG can diverge from hardware.
- `husb238.h:19-39` — Header doc has PD_STATUS0/STATUS1 register labels swapped relative to what the implementation actually reads. Future maintainers reading the header will wire up the wrong byte.

**sev-3 / 2**
- `ds4424.cpp:532-600` — NVS calibration blob has no magic/version field. Layout change with same size silently loads garbage.
- `husb238.cpp:60-140` — 4 overlapping voltage-decode helpers; partial duplication of nibble→voltage tables.
- `adgs2414d.cpp:447-452` — Dead first `memcpy` in `adgs_get_api_states()` (overwritten 2 lines later).
- `husb238.cpp:221-243` — `s_requested_pdo` sticky fallback never cleared on DETACH; stale after re-plug or autonomous renegotiation.

### From `net/` (Wave C1)

**sev-4**
- `usb_cdc.cpp:185` — `usb_cdc_cli_read()` fallback path drains TinyUSB FIFO directly, racing the RX callback's drain. Both run unlocked → **interleaved byte loss/reordering on CLI/BBP/MP stdin**. Compounds Wave A2 finding on stdin contention.
- `uart_bridge.cpp:340` — `uart_bridge_set_config()` deletes the UART driver, mutates `s_bridges[id].config`, then reinstalls, **all without locking**. `bridge_task` reads the same struct + calls `uart_read_bytes` on the now-deleted driver → undefined behaviour, likely crash via NULL ringbuf.
- `wifi_manager.cpp:116` — WiFi state globals (`s_sta_connected`, `s_sta_ip`) written by tcpip_adapter thread, read by HTTP/MP threads holding `g_stateMutex`, **with no mutex/atomic**. Data race on Xtensa LX7.
- `uart_bridge.cpp:240` — **UART bridge bypasses bus_planner**. Enabling it on IO1/IO2 while a MP script has routed I2C to the same terminals silently overwrites the GPIO matrix — both buses corrupted.
- `usb_cdc.cpp:161` — Contract violation: `serial_available() > 0` does NOT guarantee `serial_read()` returns a byte (fallback path can return 0). CLI/MP treat this as a precondition; stale `0xFF` cast from `-1` can enter input stream.

**sev-3**
- `wifi_manager.cpp:206` — `wifi_connect()` tears down AP for up to 50 s during STA attempt. HTTP/BBP callers issuing the connect over AP **lose their transport mid-call**, never receive a response.
- `wifi_manager.cpp:130-191` — `esp_wifi_init/set_mode/set_config/start` and `esp_wifi_get_mac` return values unchecked. Silent bring-up failure → unknown WiFi state.
- `uart_bridge.cpp:120` — NVS save discards `nvs_set_*` return values; failed set + successful commit silently writes partial config.
- `serial_io.cpp:42` — Lazy `xSemaphoreCreateMutex` init in `serial_print/println/printf` racy at boot; two tasks both create + leak handles.
- `wifi_scan` synchronous `block=true` for 2-4 s pinning httpd worker / BBP cmd processor.

### From `diag/` (Wave C3)

**sev-3**
- `selftest.cpp:628` — **AutoCal hardcodes stop thresholds (VADJ 15V/3V, VLOGIC 5V/1.8V); never consults `board_profile.{vadj1Locked, vadj2Locked, v_max}`**. A profile with `vadj2Locked=true` and `v_max=5V` will still allow the sweep up to 15V → potential damage to a 5V-only DUT on that rail.
- `selftest.cpp:94` — Channel-D snapshot/restore race: lock dropped between snapshot copy (line 100) and `tasks_apply_channel_function(3, CH_FUNC_VIN)` (line 121). Concurrent BBP/HTTP can overwrite ch3; restore writes stale `prev_func`.
- `selftest.cpp:429` — `s_supply_volt` written without sync, returned as live pointer to readers; torn float read on Xtensa, partial-update visibility.
- `selftest.cpp:196` — `s_cal_trace_rtc` written + returned as live pointer; HTTP/BBP readers can see partial frame.
- `selftest.cpp:787` — Two **unconditional `delay_ms(5000)`** in `selftest_measure_internal_supplies()` block the calling HTTP task for 10 s, stalling all concurrent requests.

**sev-2**
- `selftest.cpp:329` — Cal trace never cleared on success; stage=9/active=0 sentinel persists across reboots, returning stale cal data to post-mortem readers.
- `adc_leds.cpp:133` — `readAlertStatus()` called twice per tick for the same register read.
- `board_profile.cpp:67` — `nvs_open(..., NVS_READWRITE)` for a read-only namespace; blocks concurrent writers unnecessarily.

**Contract drift**
- `selftest.h` — Worker/supply-monitor/voltages exposed over HTTP but **completely absent from BBP**. BBP-only desktop clients silently miss these fields; not documented.

### From `cli/` (Wave C2)

**sev-5**
- `cli_edit.cpp:306` — **Stack smash in `replace_token_with`**: when `snprintf` truncates (returns ≥256), `tail_len = 255 - written` evaluates ≤0, cast to `size_t` becomes multi-GB, passed to `memcpy` → overwrites entire stack past 256-byte `newline[]` buffer. Trigger: tab-complete on a near-full (>~200 char) input line.

**sev-4**
- `cli_cmds_dev.cpp` (157 sites) — **CLI bypasses the BBP gate**. Direct `serial_print*` / `serial_printf` calls don't go through `term_emit` (which checks `bbpCdcClaimed()`). During an active BBP session this **injects ASCII into the binary protocol stream**, corrupting host-side framing.
- `cli_cmds_dev.cpp:333` — `cli_cmd_adc_cont` busy-waits up to 60 s with no `vTaskDelay`/yield → starves CLI task, blocks BBP preemption + TUI exit.
- `cli_cmds_dev.cpp:540` — `cli_cmd_sweep` is `while(true)` with internal `serial_read()` poll → **consumes BBP handshake bytes**, silently defeating the handshake interlock.

**sev-3**
- `cli_term.cpp:73` — Off-by-one truncation guard in `term_printf`: condition `> sizeof(buf)` should be `>= sizeof(buf)`; exact-fill snprintf leaves no NUL.
- `cli_edit.cpp:558` — CSI parser drops leading-empty parameter (`ESC[;5H` style); push on `';'` regardless of `s_csi_has_digit`.
- `cli_menu.cpp:779` — Lock-order risk: `take_snapshot` holds `g_stateMutex` (rank 3) and calls `adgs_get_all_states` which may acquire SPI mutex (rank 2) → ABBA with poll task. Confirm in synthesis.

**Contract drift**
- `cli_cmdtab.cpp:99` + `cli_cmds_dev.cpp` + `cli_menu.cpp:905` — `diagcfg` source range advertised three different ways: short_help "0-9", handler rejects >9, menu `kDiagSourceItems` has 14 entries (0-13). Selecting source 12 in TUI then `diagcfg 12` yields "invalid source".

### From top-level (Wave D1)

**sev-4**
- `main.cpp:251` vs `:319` — **`g_stateMutex` NULL-deref window**. `pca9535_install_isr()` (line 251) registers the GPIO ISR + spawns `pcaISR` task before `initTasks()` (line 319) creates `g_stateMutex`. ~750 ms gap (500 ms VANALOG settle + 100+100 ms RESET pulse + `device.begin()`). Any PCA9535 INT during this window calls `pca_fault_handler()` → `xSemaphoreTake(NULL, ...)` → FreeRTOS configASSERT panic / boot loop on a fault event at cold-power-on.

**sev-3 (systemic patterns)**
- `tasks.cpp:1321/1331/1341/1354` — **4 unchecked `xTaskCreatePinnedToCore`**: adcPoll, faultMon, cmdProc, wavegen. On heap exhaustion at boot, tasks silently don't exist → device looks alive on HTTP but never polls ADC, never monitors faults, never processes commands. No diagnostic.
- `state_lock.h` — **`ScopedStateLock` defined but never used** in tasks.cpp/main.cpp (30+ raw `xSemaphoreTake(g_stateMutex, …)` sites with inconsistent 5/10/50 ms timeouts). Canonical pattern is dead code.
- `tasks.cpp:538-667` — `tasks_apply_*` (channel_function/gpio_config/gpio_output/dac_code/dac_voltage/dac_current) **return `true` even when `g_stateMutex` acquisition fails**. SPI write succeeded but cache is stale; callers read back the old value and report wrong state to clients.
- `tasks.cpp:1209` — `taskWavegen` initialises `stillActive = false`. On 5 ms mutex timeout the loop **silently exits mid-cycle**, freezing DAC at last value. Triggered by fault-monitor SPI health-check holding the mutex.
- `main.cpp:329` — `serial_println("Scripting engine ready")` printed **unconditionally** even when `scripting_init()` silently disabled the engine on queue/task creation failure.
- `main.cpp:370` — `mainLoopTask` creation failure logs but `app_main` continues to `initWebServer()` → device exposes HTTP without serial/CLI/BBP heartbeat.

**slop (sev 2)**
- `main.cpp:165` and `:380` — `coredump_diag_print_boot_report` called **twice**.
- `tasks.cpp:1106` comment says "Core 0", actual pinned to Core 1 at `:1361`.
- `config.h:158` — `ADMIN_TOKEN "0000…00"` Phase 1 placeholder still live; auth system shipped but token never replaced.
- `tasks.cpp:989` — `static float s_cal_divider = 1.0f` inside switch case; persists across calibration runs.

**Positive (no findings)**
- CMD_ADC_CONFIG correctly takes rank 2 → 3 (no inversion).
- `pca_fault_handler` rank-3-only, releases before subsequent calls.
- MP task contract matches spec exactly (Core 0, prio 1, 16 KB stack, queue depth 4, 1 MB PSRAM heap).
- `ScopedStateLock` correctly non-copyable.

### From `dio/`+`hat/` (Wave D2)

**sev-4**
- `hat.cpp:229-240` — `bbpSendEvent(BBP_EVT_LA_DONE/LA_LOG)` called **while `s_hat_mutex` is held**. `s_hat_mutex` is **unranked** in lock-order doc; if `bbpSendEvent` internally acquires `g_stateMutex` (rank 3) a circular wait forms. Note `hat_poll` does this correctly (releases mutex first).

**sev-3**
- **All firmware-side DIO callers bypass `bus_planner_route_digital_input`**: `cmd_dio.cpp:56`, `webserver.cpp:1603`, `quicksetup.cpp:464`. ADGS2414D MUX never closed, IO terminal never powered → **GPIO toggles internally but signal never reaches the physical connector**.
- `hat.cpp:188` — `hat_recv_frame` reports the wire LEN byte to caller even when payload was clamped to `max_payload_len`; future smaller-buffer caller reads OOB.
- `hat.cpp:1099` — `hat_la_done_consume` claims atomicity in header but is non-atomic check-then-clear; on dual-core ESP32-S3 can lose LA-done edges silently.
- `hat.cpp:1029` — `hat_la_read_data` payload bytes 4-5 form uint16 length, but API param is `uint8_t` → byte 5 always 0; max chunk effectively capped at 255.
- `tasks.cpp:584` / `:601` — `tasks_apply_gpio_config` / `_output` mutate hardware **outside** `g_stateMutex` then take it for cache update. Window where hardware ≠ shadow.

**sev-2/3 Contract**
- `tasks.h` GpioState vs `dio.h` DioState — parallel structs, manually synced in 3 places (tasks.cpp:409, quicksetup.cpp:491, internal copy). Silent drift if either struct field changes.
- `hat.h:54` — PCB mode LA-done arrives via shared `PIN_HAT_IRQ` (GPIO15) but no ISR is registered on PCB → `hat_poll` 10 ms cadence can miss the ~2 µs RP2040 BB_LA_DONE pulse.

---

## Phase 1 totals

**11 fields audited.** Cumulative tallies:

- bugs: ~62
- slop: ~38
- perf: ~19
- lock-order: ~12
- contract drift: ~28

**sev-5 (8):** mp/scripting_run_file persist uninit (sev 4 borderline); mp_hal_set_interrupt_char dup; web CORS UAF; web selftest NULL deref; bus_planner rank-4→2 lock; pca9535 EFUSE fault-cross; cli_edit stack smash.

**sev-4 (~15):** g_stateMutex NULL boot window; CDC ring race; UART bypasses bus_planner; CLI BBP gate bypass; bbpSendEvent under unranked s_hat_mutex; bus_planner VADJ-before-MUX; vlogic_v unguarded; CMD_FLAG_ADMIN_REQUIRED never enforced; admin token OOB; cmd_script flags ArgSpec drift; web admin missing on /api/bus/*; WS REPL no auth deadline; web mode field type drift; ad74416h RMW non-atomic; husb238 header label swapped.



---

## Slop deletion candidates (filled as waves return)

> Deletable with no behaviour change. Per-entry one-line + cite.

_(none yet — Wave A in flight)_

---

## Cross-cutting themes (synthesis)

> Patterns surfaced across multiple fields (e.g. "every binding that allocates >4 KB does so in DRAM, not PSRAM").

_(empty — fills after Phase 3)_

---

## V2 follow-ups reconciliation

From `Docs/scripting-plan-v2.md` § "Open follow-ups":

| Follow-up | Status when audit started | Re-checked? | Audit verdict |
|---|---|---|---|
| MCP `run_device_script(src, persist=…)` | Not yet shipped | ⏳ | — |
| BBP CRC client-side recovery | Not yet shipped | ⏳ | — |
| `pullups='internal'` host warning | Not yet shipped | ⏳ | — |
| `allow_split_supplies` parity for `bugbuster.I2C/SPI` | Not yet shipped | ⏳ | — |
| Network-binding bench against real hosts | Pending | ⏳ | — |
| MICROPY_VFS re-enable | Gated on shim | ⏳ | — |

---

## Decision log

- **2026-04-27 14:?? — Audit launched.** Plan `pure-meandering-spark.md` approved. Auto mode active. Default to all 4 waves; user did not request staged execution.
- **Schema fixed.** Every agent reports `{field, paths_audited, summary_line, findings:{bugs, slop, perf, lock_order_findings, contract_drift}, open_questions}`. Severity 1–5.
- **Agent type:** `oh-my-claudecode:code-reviewer` Sonnet (read-only, severity-rated). `Explore` rejected (returns code maps, not graded findings).
- (2026-04-27) Wave 1 #1 landed: f5bc610 — CORS UAF fixed by moving Origin buffer to caller frame (16 call sites updated, build green).
- (2026-04-27) Wave 1 #2 landed: de38f73 — pca9535 fault-response path now applies EFUSE3/4 silkscreen cross via physical_to_logical[4] table (efuse_en lookup, event channel, auto-disable target).
- (2026-04-27) Wave 1 #4 landed: 964dc4a — cli_edit replace_token_with stack smash closed: clamp snprintf truncated `written` to sizeof-1, derive tail_len from remaining capacity (non-negative).
- (2026-04-27) Wave 1 #6 landed: 90bb03d — selftest calibrate handler now NULL/IsNumber-guards body["channel"] before deref.
- (2026-04-27) Wave 1 #3 + #7 landed: 1a0c5be — apply_power_and_mux now drops planner mutex around adgs SPI call (no rank-4→rank-2 inversion) and programs MUX state before energising per-route VADJ + e-fuse rails.
- (2026-04-27) Wave 1 #8 landed: 61a0ebb — PCA9535 fault ISR install moved past initTasks(); pca_fault_handler defensively returns when g_stateMutex is still NULL.
- (2026-04-27) Wave 1 #5 landed: a31d33d — mphalport.c port stub for mp_hal_set_interrupt_char gated behind `#if !MICROPY_KBD_EXCEPTION` so only the shared runtime definition links; verified via xtensa-elf-nm.
- **(2026-04-27) Wave 1 complete.** All 8 sev-5 audit findings landed as 7 atomic commits (paired #3+#7), each with build-green verification. Wave 2 / 3 not started — awaiting explicit approval.
- (2026-04-27) Wave 2 #1 landed: bfb7e2f — add check_admin_auth() at top of handle_bus_post_dispatch(); /api/bus/* now require admin token.
- (2026-04-27) Wave 2 #2 landed: 9e08069 — bbp_adapter_dispatch() now checks CMD_FLAG_ADMIN_REQUIRED; bbp_session_is_admin() stub returns true (cable-gated policy).
- (2026-04-27) Wave 2 #3 landed: 565c491 — handler_get_admin_token: clamp tlen to min(0xFF, BBP_MAX_PAYLOAD-1) before both bbp_put_u8 prefix and memcpy.
- (2026-04-27) Wave 2 #4 landed: ed83ed3 — WS REPL auth deadline: one-shot FreeRTOS timer (10 s) closes unauthenticated session; disarmed on successful auth or session_close().
- (2026-04-27) Wave 2 #5 landed: 53296b0 — s_script_eval_args: add flags (ARG_U8) as first field to match handler wire layout (flags=payload[0], src_len=payload[1..2]).
- (2026-04-27) Wave 2 #6 landed: 82c0d7a — /api/scripts/status: mode now emitted as "EPHEMERAL"/"PERSISTENT" string; client.ts type updated; Scripts.tsx normalizeMode updated.
- (2026-04-27) Wave 2 #7 landed: 4dd5c15 — http_adapter: replace Access-Control-Allow-Origin: * with localhost-only conditional-origin logic in both send_json_resp and send_err_resp.
- (2026-04-27) Wave 2 #8 landed: 0835989 — scripting_run_file: cmd.persist now propagated from s_mode so PERSISTENT VM file-runs stay persistent (V2-A contract fix).
- **(2026-04-27) Wave 2 complete.** All 8 sev-4 auth/integrity findings landed as 8 atomic commits, each with build-green verification. Wave 3 not started — awaiting explicit approval.
- (2026-04-27) Wave 3 #W3-9 landed: ca94c8a — husb238.h PD_STATUS0/STATUS1 section comment labels swapped to match actual register usage in husb238.cpp (doc-only, no macro value change).
- (2026-04-27) Wave 3 #W3-1 landed: ac471b0 — net_bridge.cpp: null *body_out/*body_len_out before returning -2 on OOM path in both http_get and http_post to prevent double-free in caller.
- (2026-04-27) Wave 3 #W3-3 landed: 888077b — mphalport.c: wrap vTaskDelay(1) with MP_THREAD_GIL_EXIT/ENTER in mp_hal_delay_ms; no-op with MICROPY_PY_THREAD=0, enables V2-G GIL release on thread flip.
- (2026-04-27) Wave 3 #W3-4 landed: b8ec71b — usb_cdc.cpp: remove racy tinyusb_cdcacm_read fallback from usb_cdc_cli_read; return 0 when ring empty, callers retry.
- (2026-04-27) Wave 3 #W3-5 landed: a97ea0c — uart_bridge.cpp: add s_bridge_mux[] spinlocks; guard config struct write in set_config and snapshot fields in bridge_task to eliminate set_config/bridge_task data race.
- (2026-04-27) Wave 3 #W3-7 landed: d8ebb7f — wifi_manager.cpp: add s_wifi_state_mux spinlock; protect all writes/reads of s_sta_connected and s_sta_ip; wifi_get_sta_ip copies into static buffer under lock to prevent torn string reads.
- (2026-04-27) Wave 3 #W3-8 landed: c440430 — ad74416h.cpp: hold g_spi_bus_mutex (recursive) across full RMW in startAdcConversion, enableAdcChannel, and clearAdcDataReady to prevent SPI interleaving between read and write phases.
- **(2026-04-27) Wave 3 complete.** All 7 sev-4 concurrency findings landed as 7 atomic commits, each with build-green verification.
- (2026-04-27) Wave 3 #W3-2 closed (no fix): scripting.cpp:386 mp_stack_set_top — `volatile int stack_dummy` is already the first local in taskMicroPython; missed prologue bytes hold no MP object pointers so GC cannot miss live roots. Theoretical correctness issue only; no code change warranted.
- (2026-04-27) Wave 3 #W3-6 DEFERRED — uart_bridge.cpp:240 bypasses bus_planner (uart_set_pin without MUX coordination). Architectural: requires user decision on whether uart_bridge should call bus_planner_apply_digital or receive a pre-validated pin list. Not safe to implement without design input.
- (2026-04-27) Wave 3 #W3-10 DEFERRED — hat.cpp:229-240 bbpSendEvent called under s_hat_mutex (via hat_command→hat_command_internal chain). Fix requires either (a) ranking s_hat_mutex as rank 5 leaf (implies bbpSendEvent must never acquire ranks 1-4 — needs verification) OR (b) buffering unsolicited events inside hat_command_internal and sending them after mutex release. User must decide approach before this is safe to implement.
- (2026-04-27) Wave 3 #W3-6 landed: 160bf38 — uart_bridge.cpp: call bus_planner_route_digital_input for TX and RX IO terminals before uart_set_pin in install_uart; failure is non-fatal (log warning, proceed).
- (2026-04-27) Wave 3 #W3-10 landed: 8295d9b — hat.cpp: buffer unsolicited BBP_EVT_LA_DONE/LA_LOG events in HatPendingEvent[] inside hat_command_internal; dispatch after xSemaphoreGive(s_hat_mutex) in hat_command to eliminate deadlock substrate (bbpSendEvent under s_hat_mutex).
- (2026-04-27) Wave 4A #1 landed: fe5b08b — autorun.cpp: success predicate changed from || to && (total_errors==0 && last_error_msg[0]=='\0').
- (2026-04-27) Wave 4A #2 SKIPPED — repl_ws_forward already uncommented and active at scripting.cpp:151; V2-B fix already shipped in a prior session.
- (2026-04-27) Wave 4A #3 landed: 535da76 — scripting.cpp: call gc_collect() after eval when soft GC watermark (80%) hit in persistent mode (V2-A spec §4).
- (2026-04-27) Wave 4A #4 landed: fcac792 — scripting.cpp: replace s_next_id++ with __atomic_fetch_add in both scripting_run_string and scripting_run_file.
- (2026-04-27) Wave 4A #5 landed: bdcd8bc — repl_ws.cpp: remove racy bare read of s_tx_used outside s_tx_mutex in repl_tx_task drain loop; use drain return value to control iteration.
- (2026-04-27) Wave 4A #6 landed: d9b61b6 — bbp.cpp: replace s_evtSeq++ with __atomic_fetch_add in sendEvent() to prevent duplicate sequence numbers from concurrent senders.
- (2026-04-27) Wave 4A #7 landed: b44468f — main.cpp: remove early duplicate coredump_diag_print_boot_report() call (line 172); keep the later call at step 17 after all peripherals are initialised.
- (2026-04-27) Wave 4A #8 landed: 285e653 — tasks.cpp: check xTaskCreatePinnedToCore return for adcPoll, faultMon, cmdProc, wavegen; log ESP_LOGE on failure and continue.
- (2026-04-27) Wave 4A #9 landed: 3d0a0e7 — tasks.cpp: taskWavegen stillActive initialised true so a transient 5ms mutex timeout does not exit the waveform loop mid-cycle.
- (2026-04-27) Wave 4A #10 landed: 1453ee4 — bus_planner.cpp: add isnan/range check for vlogic_v [1.2, 3.6] V in both bus_planner_apply_i2c and bus_planner_apply_spi before apply_power_and_mux.
- (2026-04-27) Wave 4A #11 landed: f3e11c5 — tasks.cpp: tasks_apply_gpio_config/output/dac_code/dac_voltage/dac_current return false and log on g_stateMutex timeout. tasks_apply_channel_function (void) deferred; changing its return type touches >3 files.
- (2026-04-27) Wave 4A #12 landed: 47532c7 — scripting.cpp/h + autorun.cpp: add last_script_id to ScriptStatus; status_set_done captures current_script_id before zeroing it; autorun reads last_script_id.
- **(2026-04-27) Wave 4A complete.** 11 fixes landed (10 commits + 1 no-op), 1 pre-shipped, build green after each commit.
- (2026-04-27) Wave 4B #1 landed: 04280d9 — replace 2×5s blocking delay_ms in selftest_measure_internal_supplies with poll-for-source + 3.5s ADC settle (wait_diag_slot_ready).
- (2026-04-27) Wave 4B #2 landed: 109a1c5 — add s_selftest_mutex (leaf lock) to serialize concurrent read_channel_d callers; prevents ch3 snapshot/restore race between boot_check and HTTP supply measurement.
- (2026-04-27) Wave 4B #3 landed: 530a0e3 — add bus_planner_route_digital_input before dio_configure at all 3 bypass sites (webserver.cpp, cmd_dio.cpp, quicksetup.cpp); failure non-fatal.
- (2026-04-27) Wave 4B #4 landed: b8e5d26 — tasks.cpp: restructure tasks_apply_gpio_config/output to take g_stateMutex before hardware mutation; hardware and state cache now updated atomically under the lock.
- **(2026-04-27) Wave 4B complete.** 4 fixes landed as 4 atomic commits, build green after each.
- (2026-04-27) Wave 4C #1 landed: fb29ca9 — adgs2414d.cpp: remove dead first memcpy in adgs_get_api_states (sync_api_main_from_physical overwrote it immediately).
- (2026-04-27) Wave 4C #2 landed: 35b4f0e — husb238.cpp: remove duplicate voltage_from_selected_pdo; replace sole call site with decode_status_voltage(sel, true) (same mapping). Other 3 helpers have distinct HW-register encodings — untouched.
- (2026-04-27) Wave 4C #3 landed: 4c560ca — bbp.cpp: delete 71-line local put_*/get_* codec block; add #include "bbp_codec.h"; rename 17 call sites to bbp_put_*/bbp_get_* prefix.
- (2026-04-27) Wave 4C #4 SKIPPED — bbpDetectHandshake esp_log_level_set("*", ESP_LOG_NONE): intentional design. bbpExitBinaryMode CRITICAL comment (bbp.cpp:601-609) documents why logs stay suppressed (text on CDC#0 corrupts host still reading binary frames). s_cdcClaimed is sticky-until-reboot by design. Fix (a) contradicts the CRITICAL comment; fix (b) is already effectively implemented (only fires on magic sequence over CDC#0). Collateral damage (UART0 console loses IDF logs after BBP session) is the accepted trade-off.
- (2026-04-27) Wave 4C sweep #1 landed: 30a6743 — bbp.cpp: remove write-only s_adcStreamDiv and s_adcDivCount (processAdcStream never reads them; rate estimate uses local div directly).
- (2026-04-27) Wave 4C sweep #2 landed: d78f9c7 — bbp.cpp: remove unused #include esp_timer.h, esp_wifi.h, math.h (no symbols from these used after codec extraction).
- **(2026-04-27) Wave 4C complete.** 5 changes landed (6 commits), 1 skipped with rationale. Build green after each commit.
- (2026-04-27) Cleanup #1 landed: 98f42c6 — chore(config): remove BREADBOARD_MODE; hardware is PCB-only. 8 files, 113 LOC deleted. config.h, status_led.cpp, selftest.cpp, pca9535.cpp, dio.cpp, dio.h, hat.h, main.cpp. PCB branches kept throughout. Build green: Flash 27.2%, RAM 36.9%.
- (2026-04-27) Cleanup #2 landed: fd75896, 5cedf30, 507f340, 0cc6233 — serial_print* → term_* (BBP gate) migration across webserver.cpp (2 sites), cli_cmds_sys.cpp (~45 sites), cli_cmds_dev.cpp (~40 sites), main.cpp (4 post-mainLoopTask sites). tasks.cpp FATAL abort paths (2 sites) annotated as pre-BBP boot output and left as-is. Build green: Flash 27.2%, RAM 36.9%.

---

## Next actions

**All phases complete.** Audit landed 2026-04-27.

Spot-check verification done after advisor pushback (initial Phase-3 verify section was a meta-summary of agent reasoning, not source reads). Eight sev-5 / sev-4 boot-window findings opened directly:

- ✅ web/webserver.cpp:146-150 (CORS UAF) — confirmed; line corrected from agent's :137 (comment line)
- ✅ web/webserver.cpp:1863 (NULL deref) — confirmed; line corrected from agent's :1855 (comment line)
- ✅ bus/bus_planner.cpp:170-217 (VADJ-before-MUX) — confirmed
- ✅ bus/bus_planner.cpp:214 (rank-4→2 inversion at adgs entry point) — confirmed
- ✅ cli/cli_edit.cpp:301-310 (stack smash) — confirmed; refinement: agent missed an inner clamp at lines 306-308, but the clamp produces a negative tail_len cast to size_t for memcpy, AND `&newline[written]` is itself OOB when written ≥ sizeof(newline)
- ✅ main.cpp:251/319 (g_stateMutex NULL window) — confirmed
- ✅ hal/pca9535.cpp:415-441 + :279-281 (EFUSE fault-cross) — confirmed; reconciled agent disagreement: write path correctly applies cross, fault-response path does not
- ⚠️ mphalport.c:123 (mp_hal_set_interrupt_char dup) — port-stub side confirmed; ROM_LEVEL_EXTRA_FEATURES confirmed in mpconfigport.h:24; the dup-definition in `shared/runtime/interrupt_char.c` was not opened directly. Plausible but flagged as **needs maintainer confirmation** in the audit doc.

**Deliverables:**
- `Docs/firmware-audit-2026-04-27.md` — the canonical audit doc with prioritised fix-sequence (Wave 1 sev-5 / Wave 2 auth+integrity / Wave 3 concurrency / Wave 4 cleanup / Wave 5 slop deletion)
- `.mex/context/firmware-fields.md` — per-field navigation map for all future sessions
- `.mex/context/scripting-runtime.md` — V2 scripting deep-dive
- `.mex/patterns/audit-firmware-field.md` — re-runnable Sonnet code-reviewer pattern, registered in `patterns/INDEX.md`
- `.mex/ROUTER.md` — new edges + routing-table rows + last_updated stamp
