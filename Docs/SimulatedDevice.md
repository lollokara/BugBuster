# BugBuster Simulated Device

> Reference for the Python `SimulatedDevice` used in `tests/device --sim` and `tests/simulator`.  
> Covers: what is simulated, which tests exercise each area, known gaps, and how to make the platform more reliable.

---

## Purpose

The `SimulatedDevice` (in `tests/mock/simulated_device.py`) provides a full in-process software replica of the BugBuster hardware stack. It lets the Python client, MCP server, and HTTP routes be exercised without physical hardware, and acts as the pre-release validation platform for all non-firmware-binary changes.

Two transport modes are supported:
- **BBP USB (simulated CDC)** — `--sim` flag, uses `SimulatedTransport` in `tests/mock/transport.py`
- **HTTP** — `--sim-full` flag, spins up a real `ThreadingWSGIServer` backed by `SimulatedDevice` (`tests/mock/sim_http_server.py`)

---

## Handler Coverage Map

144 BBP commands are registered across 15 handler files.

| File | Commands | Count |
|------|----------|-------|
| `handlers/core.py` | PING, GET_STATUS, GET_DEVICE_INFO, GET_FAULTS, GET_DIAGNOSTICS, GET_ADMIN_TOKEN, DEVICE_RESET, DISCONNECT, SELFTEST_STATUS, SELFTEST_MEASURE_SUPPLY, SELFTEST_SUPPLY_VOLTAGES_CACHED, SELFTEST_AUTO_CAL, SELFTEST_INT_SUPPLIES, SELFTEST_WORKER, CLEAR_ALL_ALERTS, CLEAR_CHAN_ALERT, SET_ALERT_MASK, SET_CH_ALERT_MASK, QS_LIST, QS_GET, QS_SAVE, QS_APPLY, QS_DELETE | 23 |
| `handlers/channels.py` | SET_CHANNEL_FUNC (full), SET_DAC_CODE, SET_DAC_VOLTAGE, SET_DAC_CURRENT, SET_ADC_CONFIG, SET_DIN_CONFIG, SET_DO_CONFIG, SET_DO_STATE, SET_VOUT_RANGE, SET_CURRENT_LIMIT, SET_AVDD_SELECT, GET_ADC_VALUE, GET_DAC_READBACK, SET_RTD_CONFIG | 14 |
| `handlers/gpio.py` | GET_GPIO_STATUS, SET_GPIO_CONFIG, SET_GPIO_VALUE, DIO_GET_ALL, DIO_CONFIG, DIO_WRITE, DIO_READ | 7 |
| `handlers/mux.py` | MUX_SET_ALL, MUX_GET_ALL, MUX_SET_SWITCH (with ADGS mutual-exclusion) | 3 |
| `handlers/power.py` | PCA_GET_STATUS (21 bytes), PCA_SET_CONTROL, PCA_SET_PORT, PCA_SET_FAULT_CFG, PCA_GET_FAULT_LOG | 5 |
| `handlers/uart.py` | GET_UART_CONFIG, SET_UART_CONFIG, GET_UART_PINS | 3 |
| `handlers/idac.py` | IDAC_GET_STATUS, IDAC_SET_CODE, IDAC_SET_VOLTAGE, IDAC_CALIBRATE, IDAC_CAL_ADD_POINT, IDAC_CAL_CLEAR, IDAC_CAL_SAVE | 7 |
| `handlers/misc.py` | REGISTER_READ, REGISTER_WRITE, SET_WATCHDOG, SET_LSHIFT_OE, SET_SPI_CLOCK, USBPD_GET_STATUS, USBPD_SELECT_PDO, USBPD_GO, WIFI_GET_STATUS, WIFI_CONNECT, WIFI_SCAN, WIFI_SET_AP_PASSWORD, START_WAVEGEN, STOP_WAVEGEN | 14 |
| `handlers/hat.py` | HAT_DETECT, HAT_GET_STATUS, HAT_SET_PIN, HAT_SET_ALL_PINS, HAT_RESET, HAT_SET_POWER, HAT_GET_POWER, HAT_SET_IO_VOLT, HAT_SETUP_SWD, HAT_LA_CONFIG, HAT_LA_ARM, HAT_LA_FORCE, HAT_LA_STATUS, HAT_LA_READ, HAT_LA_STOP, HAT_LA_TRIGGER, HAT_LA_LOG_ENABLE, HAT_LA_USB_RESET, HAT_LA_STREAM_START, HAT_GET_CAPS, HAT_GET_RAIL_STATUS, HAT_SET_RAIL_ENABLE, HAT_SET_RAIL_VOLTAGE, HAT_SET_LED_STATE, HAT_LA_SET_ROUTE, HAT_CALIBRATE_START, HAT_CALIBRATE_STATUS, HAT_CALIBRATE_IMPORT, HAT_SET_IO_BANK, HAT_SET_LEVEL_SHIFT | 30 |
| `handlers/streaming.py` | START_ADC_STREAM, STOP_ADC_STREAM, START_SCOPE_STREAM, STOP_SCOPE_STREAM | 4 |
| `handlers/scripts.py` | SCRIPT_EVAL, SCRIPT_STATUS, SCRIPT_LOGS, SCRIPT_STOP, SCRIPT_UPLOAD, SCRIPT_LIST, SCRIPT_RUN_FILE, SCRIPT_DELETE, SCRIPT_AUTORUN | 9 |
| `handlers/io_owner.py` | IO_CLAIM, IO_RELEASE, IO_OWNER_STATUS, IO_FORCE_RELEASE | 4 |
| `handlers/bus.py` | EXT_I2C_SETUP, EXT_I2C_SCAN, EXT_I2C_WRITE, EXT_I2C_READ, EXT_I2C_WRITE_READ, EXT_SPI_SETUP, EXT_SPI_TRANSFER, EXT_JOB_SUBMIT, EXT_JOB_GET | 9 |
| `handlers/quicksetup.py` | (re-exported via core.py) | — |

**Not yet simulated (3 commands):**

| Command | ID | Notes |
|---------|----|-------|
| `OTA` | 0x77 | USB firmware update — safe to stub with error response |
| `START_ADC_DSP_STREAM` | 0x64 | DSP-filtered stream — copy ADC stream logic |
| `STOP_ADC_DSP_STREAM` | 0x65 | Stop DSP stream |

---

## Test File → Feature Coverage

| Test File | What It Validates |
|-----------|-------------------|
| `test_01_core.py` | ping, firmware version, device info, reset, fault clearing |
| `test_02_channels.py` | all channel functions (VOUT/IOUT/VIN/IIN/DIN/DO/RTD), ADC config, DAC readback |
| `test_03_gpio.py` | GPIO output, input, high-Z mode |
| `test_04_mux.py` | MUX switch control, ADGS mutual-exclusion enforcement |
| `test_05_power.py` | DS4424 IDAC (code/voltage/calibrate), PCA9535 (21-byte status, set_control) |
| `test_06_usbpd.py` | USB-PD contract, PDO list, voltage sanity |
| `test_07_wavegen.py` | waveform generator start/stop |
| `test_08_wifi.py` | STA/AP status, scan results, field validation |
| `test_09_selftest.py` | selftest boot status, supply measurement, cached supply snapshot |
| `test_10_streaming.py` | ADC and scope streaming event delivery |
| `test_11_hat.py` | HAT detect, power, pin control, LA config/arm/status, calibration |
| `test_12_faults.py` | alert clearing, alert mask |
| `test_12_io_ownership.py` | IO claim/release/lease, session tracking |
| `test_13_dio.py` | DIO output/input/disabled, write, read |
| `test_14_uart.py` | UART config, pin query |
| `test_15_swd.py` | SWD/DAP setup (HAT-only) |
| `test_sim_core.py` | round-trip ping/status on USB and HTTP transports |
| `test_sim_completeness.py` | all 144 handlers registered, PROTO_VERSION match |

---

## SimulatedDevice State Fields

The device tracks 59+ attributes grouped by subsystem:

**Core**: `fw_version`, `uptime_ms`, `spi_ok`, `die_temp_c`, `admin_token`  
**Channels (×4)**: `function`, `adc_raw/value/range/rate/mux`, `dac_code/value`, `din_state/counter/threshold/mode/debounce/sink/oc_detect`, `do_state/mode`, `vout_bipolar`, `current_limit`, `avdd_select`, `channel_alert/mask`, `rtd_excitation_ua`  
**GPIO/DIO**: `gpio[12]` (mode/output/input/pulldown), `dio[12]`  
**MUX/Power**: `mux_states[4]`, `adgs_active[4]`, `pca_control` (int keys 0–8 per `PcaControl` enum)  
**Alerts**: `alert_status`, `alert_mask`, `supply_alert_status`, `supply_alert_mask`, `live_status`  
**UART**: `uart_config[1]` (uart_num, tx/rx pins, baud, bits, parity, stop, enabled)  
**IDAC**: `idac[4]` (code, target_v, actual_v, v_min/max, step_mv, calibrated)  
**USB-PD**: `usbpd_voltage`  
**Waveform**: `wavegen_running`, `wavegen_config`  
**WiFi**: `wifi_connected`  
**Scripts**: `script_running/id/total_runs/errors`, `script_last_error`, `script_log_ring`, `script_files`, `autorun_*`  
**Quick-setup**: `qs_slots[4]`  
**HAT**: `hat` (HatState), `hat_pins[8]`, `hat_power`, `hat_io_volt`, `hat_cal_*` (14 fields), `hat_io_bank`, `hat_level_shift`, `hat_led_states[8]`, `hat_rails[3]`  
**LA**: `la_state` ("IDLE"/"ARMED"/"CAPTURING"/"DONE"), `la_config`, `hat_la_route`  
**I2C/SPI/Jobs**: `i2c_devices`, `_i2c_config`, `_spi_config`, `job_queue`, `_next_job_id`  
**Session**: `_usb_session_id`, `_pending_events`, `_transport`, `_stream_stop`, `_stream_thread`

---

## Known Gaps and Reliability Issues

### Priority 1 — Fixed

| Issue | Fix |
|-------|-----|
| `PCA_GET_STATUS` returned 12 bytes, dropped 9 enable fields | `power.py` now returns 21 bytes keyed by `PcaControl` int enum |
| `_parse_pca_status` silently truncated enable flags | `client.py` parser reads all 21 bytes with graceful fallback |

### Priority 1 — Active Bugs

| Issue | File | Impact |
|-------|------|--------|
| **Streaming thread crash when no transport** — `device._transport` is `None` in unit tests; accessing `_event_handlers` raises `AttributeError` | `handlers/streaming.py:58,94` | Streaming tests silently fail |
| **LA ARM/FORCE skip to DONE** — both handlers set `la_state = "DONE"` unconditionally, bypassing ARMED → CAPTURING → DONE | `handlers/hat.py:395,403` | Can't validate LA state machine in tests |

### Priority 2 — Coverage Gaps

| Gap | File | Notes |
|-----|------|-------|
| `IDAC_CAL_ADD_POINT` is a no-op — doesn't store calibration points | `handlers/idac.py` | Calibration curve tests always pass vacuously |
| `USBPD_SELECT_PDO` accepts any value with no range check (valid: 1–6) | `handlers/misc.py` | Invalid PDO codes silently accepted |
| `OTA` (0x77) has no handler — raises "unknown command" | — | Blocks OTA flow tests |
| `START/STOP_ADC_DSP_STREAM` (0x64/0x65) missing | — | DSP stream tests unrunnable |

### Priority 3 — Robustness

| Issue | Notes |
|-------|-------|
| `QS_GET/QS_SAVE` don't validate slot index | Out-of-range slot silently ignored |
| `WIFI_SET_AP_PASSWORD` persist-failure path (result=0x02) never tested | Only happy-path tested |
| `IO_CLAIM` emits hardcoded event ID `0x86` instead of `CmdId.EVT_IO_PREEMPTED` | Lease expiry events misidentified |
| Time never advances automatically in tests — `device.tick(now_ms)` must be called manually for lease expiry | |
| HAT calibration handlers return hardcoded state=2 (success) — can't test failure/timeout paths | |

---

## How to Run Simulator Tests

```bash
# Unit tests only (no hardware, no transport)
PYTHONPATH=python python -m pytest tests/unit -q

# Simulator (BBP transport in-process)
PYTHONPATH=python python -m pytest tests/device --sim -q

# Simulator over real HTTP (WSGI server)
PYTHONPATH=python python -m pytest tests/integration -q --sim-full

# Specific subsystem
PYTHONPATH=python python -m pytest tests/device/test_05_power.py --sim -v
```

---

## Reliability Improvement Roadmap

### Short-term (unblock existing test gaps)

1. **Fix streaming transport guard** (`streaming.py:58,94`) — check `device._transport is not None` before dispatching events. For transport-less tests, queue events in `_pending_events` instead.

2. **Fix LA state machine** (`hat.py`) — `HAT_LA_ARM` → set `la_state = "ARMED"`, `HAT_LA_FORCE` → set `la_state = "CAPTURING"` then `"DONE"` asynchronously (or `"DONE"` immediately for synchronous tests). This unblocks `test_11_hat.py` LA sequences.

3. **Add OTA stub handler** (`core.py`) — return a "not supported in simulator" error code (`BBP_ERR_UNSUPPORTED`) rather than crashing with "unknown command".

4. **Add DSP stream handlers** (`streaming.py`) — copy the ADC stream loop with a DSP-filtered sine (can just pass through the same synthetic data).

### Medium-term (correctness)

5. **IDAC calibration round-trip** — store calibration points in `idac[ch]['cal_points']` in `IDAC_CAL_ADD_POINT`, use them in `IDAC_CAL_SAVE` to compute a linear fit, validate with `test_05_power.py`.

6. **USBPD range validation** — return `BBP_ERR_INVALID_PARAM` for PDO codes outside 1–6.

7. **Slot bounds check in QS_GET/QS_SAVE** — return error payload for `idx >= 4`.

8. **IO event ID fix** — replace hardcoded `0x86` with `int(CmdId.EVT_IO_PREEMPTED)`.

### Long-term (platform quality)

9. **Deterministic time model** — add `device.advance_time(ms)` helper that advances `_now_ms` and fires any expired leases automatically. Removes the need for manual `tick()` calls in tests.

10. **HAT calibration failure paths** — add `device.hat_cal_force_error = True` hook that makes `HAT_CALIBRATE_STATUS` return a failure, so the client error-handling path can be tested.

11. **Persistent CI gate** — add `.github/workflows/sim-tests.yml` that runs the full `--sim` suite on every push touching `python/`, `tests/`, or the firmware BBP headers, so simulator regressions are caught before release.

---

## Adding a New Handler

1. Add the command to `CmdId` in `python/bugbuster/constants.py` (if new).
2. Create or extend a handler file under `tests/mock/handlers/`.
3. Register via `device.register_handler(CmdId.MY_CMD, _my_handler(device))` in the `register()` function.
4. Add state fields to `SimulatedDevice.__init__` if the command mutates state.
5. Add a corresponding HTTP route in `tests/mock/http_routes.py` if the command has an HTTP equivalent.
6. Verify coverage: `test_sim_completeness.py` will fail if the command appears in `CmdId` but has no handler.

See `.mex/patterns/add-bbp-command.md` for the full cross-stack checklist.
