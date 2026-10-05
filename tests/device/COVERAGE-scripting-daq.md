# Test coverage: scripting runtime, `daq` module, sub-project 1 iOS fixes

Audit of 2026-10-03 against spec `2026-10-03-scripting-daq-espfleet-design.md` §2–§3 and the
plan "Shared JSON contract". Branch state: Tasks 1–4 and 6–11 merged; **Task 5** (the
`/api/scripts/*` routes in `api_core_handle`, `net/api_scripts.cpp`) is being written in a
parallel worktree, so its guards (`tests/unit/test_route_parity.py` scripts entries,
`tests/unit/test_scripts_api_parity.py`) are listed as *pending Task 5*.

Legend – **Unit/host/sim** = runs without a board (`tests/unit`, `tests/firmware_host`, simulator).
**HW** = new files in this directory, real board only (all skip under `--sim`/`--sim-full`:
the simulator has no MicroPython VM and no `/api/scripts` v2 surface). HW files:
`test_hw_scripts_runtime.py` (SR), `test_hw_daq_module.py` (DM), `test_hw_ble_tunnel_routes.py` (BT),
helpers in `_hw_scripts.py`.

| # | Behaviour | Unit / host / simulator tests | Hardware tests | Gap |
|---|---|---|---|---|
| 1 | One script slot: 409 busy, `replace=1` (3 s stop, then VM reset) | `firmware_host/test_script_runtime::test_admission_rule`; `test_scripting_slot::{admission_and_claim_share_one_critical_section, replace_never_deletes_the_vm_task, replace_waits_then_queues_a_reset, replace_timeout_is_three_seconds, slot_released_only_by_its_own_completion}`; `unit/test_script_runtime_client::{run_file_busy_raises_script_busy_error, run_file_sends_background_and_replace_flags}`; `unit/test_mcp_scripting::{run_file_reports_the_busy_holder, run_file_replace_starts_in_background}`; *pending Task 5:* `test_scripts_api_parity` | SR `second_run_is_409_busy_naming_the_holder`, `replace_stops_the_holder_and_runs_the_new_script` (answers < 6 s, holder never finishes, new script logs), `replace_of_a_holder_blocked_in_one_long_call…` (review-focus 1, `time.sleep(12)` holder) | Source/behaviour only; **the two-client race** (BLE + HTTP at once, review-focus 2) has no HW test (needs a BLE central). "VM reset" is only observable as "the new script ran afterwards". Simulator: no 409 at all (`mock/handlers/scripts.py` is the legacy BBP slot). |
| 2 | Named + sourced runs (`manual/autorun/repl`), `state`, `lastExit`, `startedAt` | `test_script_runtime::test_enum_names_match_spec`; `test_scripting_slot::{status_carries_spec_fields, stop_is_classified_not_counted_as_error}`; `test_autorun_repl_runtime::{status_reports_name_and_boot_flag, repl_lines_are_repl_sourced…}`; `unit/test_script_runtime_client::{status_parses_runtime_v2_fields, status_defaults_keep_old_positional_equality}` | SR `status_has_legacy_and_v2_fields`, `background_run_reports_name_source_state_and_exit` (running → done, `name`, `source=manual`, `fileSlotId/Name`, `startedAt`, `totalRuns`), `error_script_logs_a_traceback…` (`lastExit=error`, `totalErrors+1`), `stop_is_classified_stopped_not_error` | `source=repl` and `source=autorun` never observed on a board (REPL is a WebSocket – no WS client in the repo; autorun needs a reboot, see #7). `startedAt` is only asserted positive: epoch-vs-uptime-ms fallback is not distinguished. Simulator has none of the v2 fields. |
| 3 | Structured log lines `<ts_ms> <L> <src> <text>\n`; `logs?since=` non-draining | `test_script_runtime::{line_assembler_prefixes_and_levels, line_assembler_splits_overlong_lines, format_line_truncates_but_keeps_newline, query_helpers}`; `test_scripting_slot::{ring_receives_structured_lines, partial_line_flushed_before_done}` | SR `every_ring_line_is_structured`, `background_run…` (order, `I mpy`, monotonic ts), `error_script…` (`E` level traceback), `partial_last_line_is_flushed…`, `logs_since_is_non_draining` | Ring wrap / `dropped` counter and multi-page (> 4096 B) paging not driven on hardware. `sys`-source and `W`/`D` lines not provoked. BLE JSON reply (`{next,n,dropped,data}`) not seen (HTTP re-frames to `text/plain` + `X-BugBuster-Log-Next`). |
| 4 | `background=1` | `unit/test_script_runtime_client::run_file_sends_background_and_replace_flags`; `unit/test_mcp_scripting` wait/background tests | SR `background_run…` (returns < 3 s, `background:true` echoed), `background_flag_is_echoed_either_way` | None beyond the interpretation that behaviour is identical either way (plan interpretation 4). |
| 5 | `eval`/REPL refused (409) while a file script runs | `test_autorun_repl_runtime::{repl_lines_are_repl_sourced_and_refused_while_busy, repl_ctrl_c_cannot_stop_a_file_script}`; `unit/test_script_runtime_client::eval_busy_raises_script_busy_error` | SR `eval_is_refused_with_409_while_a_file_script_runs` (409 body names holder, eval works again after stop) | **REPL WebSocket** refusal and Ctrl-C not tested on hardware (needs a WS client; add `websockets` to `requirements-test.txt` first). |
| 6 | Chunked upload: restart from `off=0`, wrong offset, oversize | `firmware_host/test_script_storage_chunks::{sequential_chunks_commit_on_final, wrong_offset_or_name_is_rejected, offset_zero_restarts, oversize_and_empty_and_bad_names}` | SR `chunked_upload_roundtrip_and_temp_file_hidden`, `chunk_offset_zero_restarts…`, `chunk_wrong_offset…naming_the_expected_offset`, `chunk_oversize…leaves_nothing` (14 × 3072 B), `chunk_bad_names_are_rejected[×6]`; BT `a_ble_sized_chunk_fits_the_512_byte_request_and_uploads` | Temp-file survival across a reboot / stale temp cleanup at boot not tested. No negative test for `b64` > 4096 chars or invalid base64. |
| 7 | Autorun `source=autorun`, `ranThisBoot` | `test_autorun_repl_runtime::{autorun_submits_as_a_named_autorun_file_script, autorun_result_uses_this_runs_exit…, boot_check_records_that_autorun_ran, status_reports_name_and_boot_flag}` | SR `autorun_status_has_snake_and_camel_fields` (read-only), BT `autorun_enable_and_disable_routes_exist_without_changing_autorun` | **End-to-end autorun is untested on hardware**: needs enable → reboot → `source=autorun`, `ranThisBoot=true`, log lines. Left out deliberately: reboots the board and changes persistent config (candidate for a `--allow-reboot` destructive test once the coordinator wants it). |
| 8 | Every `/api/scripts/*` route over HTTP **and** the BLE tunnel (`api_core`) | *pending Task 5:* `test_route_parity` scripts entries (one per route, dispatch in `api_core_handle` + HTTP delegates), `test_scripts_api_parity`; existing `test_api_route_parity`/`test_route_parity` pattern from `6fd1c108` | BT `scripts_route_is_registered_over_http[×9]` + autorun enable/disable probe + `files_get_and_list_agree…` (HTTP delegates to the same `api_core_handle` body the tunnel runs); BT `a_ble_sized_chunk_fits_the_512_byte_request` (frames the request exactly as `ble_service.cpp` does and asserts ≤ 512 B) | **No host-side BLE tunnel exists** (no `bleak`, no BLE central in `python/` or `tests/`; tunnel = GATT write `{"id","path","body"}` + notify frames, bonded central required). Tunnel parity is therefore guarded by source tests (Task 5) + HTTP-over-the-same-bodies only. `test_scripts_over_the_real_ble_tunnel` is a permanent, visible skip. BLE-only JSON framings (`logs` base64 JSON, `files/get` paging) are not exercised anywhere until a BLE client exists. |
| 9 | `daq` module: present, vdut, read, run.*, samples, ENODEV, name length | `firmware_host/test_moddaq_src::{module_is_registered…, api_surface_matches_spec, every_hardware_call_checks_for_a_daq_hat, presence_means_a_connected_daq_hat, samples_maps_missing_run_to_enoent…, name_length_checked}`; `test_daq_codec` (TLV/status/meta/list/S1 codecs vs Python); `unit/test_firmware_stubs::test_daq_stub_matches_spec_surface`; `unit/test_stubs_to_json::test_daq_run_is_a_namespace…` | DM (no HAT needed) `daq_module_imports_and_exposes_the_spec_surface`, `without_a_daq_hat_every_call_raises_enodev` (12 calls); DM (DAQ tier, `--daq`) `vdut_set_read_back_and_enable`, `vdut_out_of_range_raises_valueerror…`, `battery_run_lifecycle_samples_and_delete` (new/list/status/start/pause/stop/delete/samples/since/max), `run_edit_changes_params_live…` (edit, TypeError, bad chem), `run_name_length_is_checked` (25 B → ValueError, 23 B ok), `samples_and_run_id_argument_validation` (ENOENT, ValueError), `script_stop_interrupts…` | The `ENODEV` test runs only on a board **without** a HAT, the DAQ tier only **with** one – one rig cannot cover both in one run. `daq.run.new` name check is behind `require_daq()`, so on a HAT-less board a long name raises ENODEV, not ValueError (by design, asserted nowhere). Not covered on HW: `self_discharge`, `ext_load`, `cutoff_mv_cell`, `peukert`, `dither` edit keys, `remaining_s`/`energy_j` values, run `depleted` state, `EIO` on a dead HAT link, runs with > 100 entries (list paging). Simulator has no `daq` module. |
| 10 | P4 `BS_HOP_S1_SINCE` (live 1 s window) | `firmware_host/test_battsim_s1::{ring_window, reply_fits_one_hat_frame}`; `unit/test_battsim_s1_since::{opcode_and_limits_match_firmware, parse_s1_since, samples_since_pages_until_no_more}` | DM `p4_s1_since_over_the_bbp_path_agrees_with_daq_samples` (Python `BattSim.samples_since` over HTTP `/daq/bs` vs MicroPython `daq.samples` for the same run, `since_s` slicing), DM `battery_run_lifecycle…` | Empty ring after a **P4 reboot** (review-focus 5), the 1 h ring wrap, and a gap/`RF_GAP` flag are not provoked on hardware. The USB (`CMD_DAQ_CONFIG`) transport of `samples_since` is not run on HW (only HTTP). |
| 11 | VDUT current limit (`currentLimitMa` setpoint; `daq.vdut(amps_limit=)`) | `device/test_18_daq_api::{vdut_setpoint_roundtrip, vdut_setpoint_rejects_out_of_range_with_http_400}` (HW, existing, needs `--daq`); `fixtures/http/daq__vdut__status.json`; `test_16_daq_stream::test_out_of_range_setpoints_are_rejected…` (HW, existing) | BT `vdut_setpoint_with_current_limit_roundtrips_and_is_restored` (also: missing `currentLimitMa` → 400, never defaulted), DM `vdut_set_read_back_and_enable` (REST `currentLimitMa` agrees with `daq.vdut(amps_limit=0.2)`), DM `vdut_out_of_range…` | **No simulator/host test**: `/api/daq/vdut/*` is not in `tests/mock/http_routes.py`. Current *limiting* behaviour under load (output actually clamps at the limit) is not tested – needs a known load. |
| 12 | `/api/selftest/supplies` shape (camelCase `valid, suppliesOk, avddHiV, dvccV, avccV, avssV, tempC`) over the BLE tunnel; wifi/update GETs | `unit/test_route_parity` (selftest/supplies, wifi/scan, update/status|check in the shared-route set); iOS-side decode in `iOSApp/Tests/DiagnosticsTransportTests.swift`; `firmware_host/test_supply_cache_readers` (cached readers) | BT `selftest_supplies_has_the_camelcase_firmware_shape`, `selftest_supplies_cached_has_three_named_rails`, `wifi_and_update_status_gets` (`/api/ota/status` == `/api/update/status` shape), `wifi_scan_returns_a_networks_list` | **Simulator drift**: `tests/mock/http_routes.py` answers `/selftest/supplies` with **snake_case** (`supplies_ok`, `avdd_hi_v`…) while the firmware (and iOS) use camelCase; no HTTP fixture exists (`fixtures/http/_index.json` marks it skipped). Fixing it means editing `tests/mock` (out of scope for this worker). `/api/update/check` is not run on HW (network + release-server dependent, resets risk noted in `api_core.cpp`). |

## New hardware tests (what each checks)

`test_hw_scripts_runtime.py` – 24 tests: v2 status shape · chunk round-trip with hidden temp file ·
`off=0` restart · wrong offset (HTTP 400, expected offset named, still resumable) · oversize upload
(rejected, nothing left behind, name reusable) · 6 bad names · background run fields + ordered
structured logs · every line matches `<ts> <L> <src> <text>` · partial line flushed · traceback at
level `E` + `lastExit=error` · non-draining `since=` · missing script 400 · 409 busy names the
holder · `replace=1` · replace of a blocked holder · eval 409 while a file script runs · stop →
`lastExit=stopped` and no error count · `background` echo · autorun status shape.

`test_hw_daq_module.py` – 10 tests (2 need no HAT): module surface · ENODEV without HAT · VDUT
set/read/enable/disable with V·I=P and REST agreement · VDUT range errors keep the setpoint ·
battery run lifecycle with samples · run edit/TypeError/ValueError · name length · samples/run_id
argument validation (ENOENT) · stop interrupts a HAT polling loop · P4 `S1_SINCE` agreement between
the Python BBP client and MicroPython.

`test_hw_ble_tunnel_routes.py` – 18 tests: 9 scripts routes registered over HTTP · autorun
enable/disable probe (non-mutating) · upload/download parity · BLE-frame ≤ 512 B chunk upload ·
visible skip for the real BLE tunnel · selftest supplies (+cached) · wifi / update status ·
wifi scan · VDUT setpoint with `currentLimitMa`.

**Safety/cleanup.** Every script/file is `bbt_hw_*.py` and removed on teardown, the slot is stopped
on entry and exit; DAQ-tier tests are `requires_daq` + `requires_daq_http` + `destructive`, snapshot
the VDUT status over REST and restore setpoint + enable, stop the loaded run and delete every run
named `bbt_*` (a `daqclean` script) even when the test failed. The repo's session-level autouse
reset (`reset_to_defaults` at start/end) still applies.

## Running

```bash
# Hardware-free: all new files skip with a reason, nothing else changes
PYTHONPATH=python pytest tests/device --sim -q

# Real board over HTTP, admin token read from the board over USB (the USB port is used only for that)
PYTHONPATH=python pytest tests/device/test_hw_scripts_runtime.py tests/device/test_hw_daq_module.py \
    tests/device/test_hw_ble_tunnel_routes.py --device-http=<ip> --device-usb=<port> -v

# Real board over HTTP, no USB: pass the token
PYTHONPATH=python pytest tests/device/test_hw_scripts_runtime.py tests/device/test_hw_daq_module.py \
    tests/device/test_hw_ble_tunnel_routes.py --device-http=<ip> --admin-token=<token> -v

# Add the DAQ-HAT tier (ENERGISES THE DUT TERMINALS) on a board with a DAQ HAT
PYTHONPATH=python pytest tests/device -k "hw_daq or hw_ble or hw_scripts" \
    --device-http=<ip> --device-usb=<port> --daq -v
```

The scripts API exists only over HTTP and the BLE tunnel (the v2 features have no BBP/USB command),
so `--device-http` is mandatory; USB is just a convenient token source.
