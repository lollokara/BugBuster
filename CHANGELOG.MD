# CHANGELOG

## [Unreleased]

### Added

- **`USB_META_SETTLING` now reports the FUSION's real state, not the range
  manager's control lock (ESP32-P4 2.4.1).** The flag is what every consumer
  uses to decide whether a sample is trustworthy, but it was driven by
  `range_manager_settling()` - a control-side hold that is a strict superset of
  the actual disturbance, because the fusion exits its blackout early whenever
  the ADC's `FILT_NOT_SETTLED` bit clears. Measured at 64 kSPS before the
  change: **51% of samples flagged settling were already fully FINE-sourced**
  and perfectly good. It now comes from `current_fusion_settling()` - true only
  while the fusion is leaning on COARSE or cross-fading back to FINE.

  Measured at 64 kSPS, same DUT:

  | build | settling share | composition of flagged samples |
  |---|---|---|
  | 2.3.0 (lock-based) | 1.19% | 51% FINE, 31% blend, 18% coarse |
  | 2.4.0 (fusion-based) | 3.23% | 87% coarse, 13% blend - but every LO sample flagged |
  | **2.4.1** | **0.42%** | **97.4% blend, 2.6% coarse, 0% FINE** |

  2.4.0 had a bug of its own, caught in verification because `coarse-in-settling`
  came back exactly equal to the LO sample count: the fusion's LO branch returns
  early, so a blackout armed on entry to LO never aged and pinned the flag on
  for the entire LO period. Any counter in that function must be aged in the
  early-return path too. On LO, COARSE is the intended source, not a fallback
  covering a transition.

- **Autorange stability controls (ESP32-P4 2.3.0): ODR-aware settle lock and  adaptive anti-flap.** Measured on a bursty 3 V DUT, 45 s captures, normalised
  by time spent outside the HI range:

  | config | range changes | settling share |
  |---|---|---|
  | dwell off (old behaviour) | 8914 | 88.95% |
  | dwell 10 ms only | 4375 | 48.02% |
  | dwell + settle lock + anti-flap | **3148** | **4.57%** |

  Usable (non-settling) samples went from 11% to 95%. Two independent causes,
  both now fixed:
  - `AR_LOCK_SAMPLES` was a fixed 64 **samples**, commented as "256 us at
    250 kSPS" - but the same constant is 8 ms at 8 kSPS, 31x longer in wall
    clock, and it drives the `USB_META_SETTLING` flag. At ~108 changes/s that
    alone flagged ~86% of samples. Replaced by `DAQ_K_RANGE_LOCK_US`
    (default 256 us), converted to samples against the live ODR and floored at
    `AR_LOCK_MIN_SAMPLES` (8) so the converter's own filter settle is always
    covered. The transient being waited out is analog, so it belongs in
    microseconds; only the ADC filter settle belongs in samples.
  - A fixed dwell cannot settle a load whose burst period exceeds it.
    `DAQ_K_RANGE_FLAP` adds adaptive backoff: the effective dwell doubles (to
    32x) each time an up-range arrives *while the dwell is still running* -
    the exact signature of oscillating across a boundary - and relaxes one step
    once a change happens from a settled state.

  **The anti-flap detector costs nothing per sample.** It is evaluated inside
  `apply_range()`, which is rare, rather than on the per-sample path that runs
  at up to 256 kSPS; the only hot-path addition in this whole change is one
  conditional decrement for the dwell countdown.

  Exposed as persisted registry keys, the P4 CLI `dwell [dwell|lock|flap
  <value>]`, and the MCP tool `daq_range_stability`.

- **Autorange down-range dwell (ESP32-P4), to stop range thrashing on a bursty
  load.** `AR_CONFIRM_SAMPLES` is a glitch filter, not a thrash guard - it only
  needs a few *consecutive* quiet samples, so a periodically bursting DUT
  down-ranges in every gap and up-ranges on the next burst. Bench-measured on a
  3 V DUT: **6231 range changes in 45 s, leaving ~50% of samples flagged
  SETTLING**. `AR_DOWN_DWELL_US` (`config.h`, default 10 ms) now imposes a
  minimum hold in the coarser range before a down-range may commit, with
  `range_manager_set_down_dwell_us()` / `range_manager_get_down_dwell_us()` and
  a new P4 CLI command `dwell [microseconds]` (0 disables) for bench tuning.
  The dwell is stored as a precomputed **sample countdown**, not a timestamp:
  `range_manager_step()` runs per sample at up to 256 kSPS and must never read a
  wall-clock timer. `range_manager_set_odr()` keeps that conversion honest and
  is called wherever the FINE ODR is cached in `daq_board.c`. Trade-off: while
  parked in a coarser range the low-current tail after a burst is measured on a
  bigger shunt, so this trades sub-uA tail resolution for far fewer transition
  artefacts.
- **The dwell is a persisted, wire-settable setting.**
  `DAQ_K_RANGE_DWELL_US` (`common/daq_config_registry.h`, group `0x01` index
  `0x0A`, `DAQ_T_U32`, `DAQ_F_P4_LOCAL|DAQ_F_PERSIST`) stores it in NVS and
  survives a power cycle. Reachable three ways: the P4 CLI `dwell
  [microseconds]`, BBP `DAQ_CONFIG` from the host, and the new MCP tool
  `daq_set_range_dwell`. The default lives once, in
  `DAQ_RANGE_DWELL_US_DEFAULT` in the shared registry header, so the
  compile-time boot value and the schema default cannot drift. No C6 changes
  are needed - it is P4-local and absent from `SEND_KEYS[]`, exactly like
  `DAQ_K_SOURCE_ENABLE`, and the C6's `field_apply_i32` ignores unknown keys.
- `tests/unit/test_daq_registry_parity.py` - parses
  `daq_config_registry.{h,c}` and asserts the Python `DaqKey` mirror matches it
  on names, values and types, and that every key has a schema row. This is the
  guard that was missing when four keys drifted unnoticed.
- **ESP32-P4 DAQ HAT firmware 2.1.0 -> 2.2.0** (minor: backward-compatible
  feature - the range dwell setting).
- **The MCP OTA tools now bootstrap themselves from a USB session.** They wrap
  HTTP-only endpoints, so they previously failed with "OTA requires admin
  authentication" even though the USB link they were already holding could
  answer both missing questions. `_make_ota()` now fetches the hardware-derived
  token via `GET_ADMIN_TOKEN` (USB-only by design, precisely to bootstrap this)
  and reads the device's own STA IP from `wifi_get_status()` when no host was
  given. Explicit `host` / `admin_token` arguments still win, and an HTTP-only
  session is unaffected.

- **Power-consumption analysis from Python and the MCP server.** The DAQ HAT's
  measurement data plane (the ESP32-P4's own USB-HS vendor-bulk endpoint) had
  only ever been decoded by the desktop app, the iOS client and the hardware
  test suite; the Python library could reach the control plane but not the
  samples. Two new modules close that gap:
  - `python/bugbuster/daq_stream.py` - `usb_proto.h` frame decoder plus a
    `DaqStream` pyusb transport and a `CaptureAccumulator` that fills index
    gaps with NaN and zero-order-holds voltage onto the current timebase, so a
    capture is always a pair of equal-length, honestly-indexed arrays.
  - `python/bugbuster/power_analysis.py` - host-side post-processing:
    trapezoidal energy/charge integrals, power-state segmentation by
    clustering log10(current), transition counts, duty cycle, burst
    periodicity and jitter, marker-bracketed energy windows, range/saturation
    quality accounting and a min/max waveform preview. It runs on the PC by
    design - the P4's CPU budget goes to keeping the converters fed.
- **15 MCP tools for power profiling** (`python/bugbuster_mcp/tools/daq_power.py`):
  `daq_power_stream_status`, `daq_power_setup`, `daq_set_current_range`,
  `daq_set_sample_rate`, `daq_power_capture` (+ `_start`/`_status`/`_result`
  for long or trigger-armed runs), `daq_power_report`, `daq_power_window`,
  `daq_power_compare`, `daq_power_markers`, `daq_power_export`,
  `daq_power_list_captures`, `daq_power_drop_capture`. Total MCP tool count is
  now 108 across 17 modules.
- `pyusb` added to `python/requirements.txt`. It is imported lazily and is only
  needed for the DAQ data plane.
- **The MCP server now starts with no arguments.** `--transport` defaults to
  `auto` (USB when a board is attached, HTTP otherwise) and `--port` to
  auto-detection: `bugbuster.discovery.find_usb_port()` ranks serial ports by
  USB descriptor (Espressif VID `0x303A`, mainboard PID `0x4002`) and CDC
  interface index, then confirms the choice with the BBP handshake. The
  mainboard exposes two CDC interfaces on one composite device, so VID/PID
  alone cannot tell BBP (CDC0) from the text console (CDC1), and some hosts
  report no location string at all - hence the probe. `BUGBUSTER_TRANSPORT` /
  `BUGBUSTER_PORT` / `BUGBUSTER_HOST` override. `.vscode/mcp.json` no longer
  carries a hardcoded COM port.
- `discover_devices` now scans USB as well as mDNS, and reports which port is
  the likely BBP port plus the session's active transport/port.
- `libusb-package` added to `python/requirements.txt`. Windows has no
  `libusb-1.0` on PATH, so `usb.core.find` returned None there with no error -
  indistinguishable from "device not plugged in". `daq_stream.py` now falls
  back to the bundled backend, which removes the Zadig step entirely.

### Fixed

- **The ADC sample rate silently reverted on every P4 reboot (2.4.3).** Sample
  Rate and Filter/Decimation are two front-ends onto one piece of hardware
  state, with no defined precedence. `daq_settings_apply_all()` walks the schema
  in key order, and `apply_sr_mode()`'s SR-OFF path (key 0x09, applied last)
  ended with `apply_sample_rate()` - so on every boot the Sample Rate enum
  overwrote whatever Filter/Decimation the operator had set. Bench-observed: set
  64 kSPS, power-cycle, come back at 8 kSPS, measuring at a different bandwidth
  than the UI claimed. Now:
  - Filter/Decimation is the authoritative record and the only thing that
    programs the ODR at boot (`s_boot_apply` guards the Sample Rate branch);
  - an explicit runtime Sample Rate write still programs the part, then mirrors
    the filter + decimation the driver actually chose back into the store
    (`sync_filter_keys_from_hw`), so the two front-ends and NVS never disagree;
  - a re-entrancy guard (`s_rate_sync`) stops that mirror re-programming the ADC.

  Verified on hardware: set decimation x128, reboot, comes back at 64000 sps.
  The first attempt at this fix (2.4.2) made it worse - the mirror also ran
  during boot, where Sample Rate is applied *before* Filter/Decimation, so it
  clobbered the stored value before it was ever used. Order of application in
  `daq_settings_apply_all()` matters whenever two keys touch one register.

- **The BBP USB reader thread could die silently, poisoning the link until the
  owning process was restarted.** `_reader_loop()` caught `OSError`/`TypeError`
  and simply `break`-ed, leaving `_running` True and the port open. Because the
  port was still open, `send_command()` kept writing successfully while nothing
  read the replies - so every command timed out forever and the sequence number
  kept climbing, while HTTP and the DAQ USB data plane both stayed healthy. Hit
  twice on the bench, both times triggered by USB re-enumeration when the
  ESP32-P4 reset during an OTA. Now:
  - the reader marks the link down and hands a `LinkDownError` to every waiting
    command instead of letting each one burn its full timeout;
  - `USBTransport.is_healthy()` reports port-open **and** reader-alive, because
    `is_open` alone stays true through this failure;
  - `USBTransport.reconnect()` rebuilds the port + handshake, and
    `send_command()` calls it automatically (`auto_reconnect`, default on);
  - new MCP tools `link_status` and `reset_link` recover a wedged link without
    restarting the server.
  Regression tests: `tests/unit/test_usb_link_recovery.py`.

- **Host capture assembly was O(n^2), causing 87.7% sample loss on long runs.**
  `CaptureAccumulator` drained pending voltage samples from a list with
  `pop(0)`. On a 180 s run the host fell far enough behind the stream that the
  DEVICE started dropping frames to back-pressure (`fifo_drop_frames` 6217),
  and a 180 s capture took 373 s of wall clock. Switched to a `deque`; the same
  capture now loses 0 samples with +0.2 s overshoot. Bench-verified.
- **The capture loop only checked its deadline between USB reads**, so one slow
  batch overshot the requested duration. It now also breaks mid-batch.
- **`daq_stream` trusted the WAVE_I header's sample rate, which goes stale
  after an ODR change.** Observed advertising 128000 sps while the part was
  converting at 8000 - a silent 16x error on every integral and timestamp.
  Captures now reconcile against the STATUS frame's `odr_sps` (documented as
  what the device actually applied), and report `rate_source` plus a warning
  when the two disagree.
- **`daq_set_sample_rate` exposed stream decimation without warning that it
  aliases.** The firmware documents it as a naive keep-1-of-N drop with no
  anti-alias filter and deliberately does not expose it as a user knob. Values
  above 1 now require `i_understand_aliasing=True`, and the docstring points at
  the ADC ODR as the correct way to trade bandwidth for capture length.
- **Two power states could be labelled "peak"** in the same report when the
  clusterer produced six or more levels.
- **`python/bugbuster/daq_config.py` was missing four acquisition registry
  keys** the firmware has had: `FILTER`, `DECIMATION`, `REJECT_5060` and
  `SR_MODE` (`daq_config_registry.h` group `0x01`, indices `0x06`-`0x09`).
  `DaqConfig.get_all()` returned them as unnamed raw keys `0x106`-`0x109`.
  They matter for power profiling - the Sinc3 filter and super-resolution
  settings set the noise floor, which decides whether a sub-µA sleep current
  is resolvable at all.

- **ESP32 update manager: C6/BLE-triggered ESP32 self-update never rebooted
  the device.** `fw_apply_task()` (the worker behind the C6 menu's
  `DDP_MB_FW_APPLY` -> `api_core_fw_apply_async()` path) called
  `update_manager_apply_release_index()` via `api_core_handle()`, discarded
  the response, and exited — it never checked
  `update_manager_reboot_pending()` or called `esp_restart()`. The new ESP32
  image was correctly downloaded, verified, and armed in the other OTA slot,
  but the device just kept running the old one indefinitely, with no error
  reported anywhere. `webserver.cpp`'s separate `/api/update/apply` HTTP
  handler (`http_update_apply_task()`) already did this correctly; the two
  handlers had drifted (see the "TWO `/api/update/apply` handlers" warning in
  `.mex/patterns/multi-mcu-ota.md`). Mirrored the same check + 1 s settle
  delay into `fw_apply_task()`.

- **ESP32 update manager: `apply_daq_ota()` logged every successful
  non-resumed DAQ (P4/C6) transfer as "failed".** The `http_ok` check compared
  `status` against `s.offset == 0`, but `s.offset` is mutated in place by
  `hat_ota_event_handler()` as bytes arrive — by the time the check runs after
  a successful transfer, it always equals the full image size, so the
  "fresh request" branch (`s.offset == 0 && status == 200`) could never be
  true after a real successful download. Bench-observed: a full, correct
  1,114,480/1,114,480-byte transfer (`err=0 status=200`) still logged
  `DAQ OTA attempt 1/5 failed`. It silently self-healed on the next attempt
  (`daq_resume_offset()` sees the P4 already has everything and short-circuits
  to `ok=true`), costing a spurious ~1 s delay and an extra HAT round-trip on
  every single DAQ apply, but never surfaced as a real failure — until now
  it's fixed properly by comparing against a `start_offset` captured before
  the transfer instead of the post-transfer `s.offset`.

- **ESP32 update manager: no progress on the C6 screen during a GitHub-release
  P4/C6 apply (only S3-target applies showed it).** `hat_ota_event_handler()`
  (used by `apply_daq_ota()` for the GitHub-download path) fires ~17
  back-to-back `hat_ota_data()` calls per 4096-byte HTTP buffer with no yield
  between them — a healthy connection to GitHub's CDN delivers data fast
  enough for this to starve `s_hat_mutex` for the whole transfer, exactly the
  same class of bug already fixed in `update_manager_push_local()`'s chunk
  loop (see the "HAT command 0x5A/0x5D: failed to take mutex" fix above).
  Starving `s_hat_mutex` blocks `MB_POLL` (`0x5D`), which is how the C6
  screen's own progress poll gets through — bench-observed: progress showed
  for the first ~1 s of a P4 GitHub-release apply, then froze for the rest of
  the transfer even though it continued correctly in the background. (ESP32
  self-updates were unaffected: updating the S3 itself never touches the HAT
  link at all.) Added the same 1 ms yield used in the local-push fix.

- **ESP32 update manager: GitHub-releases-API fallback path never recognized
  P4/C6 assets, always failing DAQ HAT applies with "release has no P4/C6
  image" even when the release had them.** `fill_component_from_asset()` only
  matched two hardcoded name patterns (RP2040 `bugbuster-hat-rp2040-v*.bin`,
  ESP32 `bugbuster-esp32s3-v*-ota.bin`); `collect_release_options()` never
  called it for P4 or C6 at all, so `UpdateManifest.p4`/`.c6` were always
  left zeroed on this path — regardless of what assets the release actually
  published. (The nightly-manifest.json path, `fetch_manifest()`, already
  handled all four via `read_component()`; this fallback is what actually
  runs whenever the manifest URL 404s, e.g. every non-`nightly`-tagged
  release, which is the common case.) Generalized the matcher to a
  prefix/suffix table for all four targets (P4: `bugbuster-daq-p4-v*-ota.bin`,
  C6: `bugbuster-daq-c6-v*-merged.bin`, confirmed against a live release's
  actual asset names) and scan all four per asset instead of two.

- **ESP32 WiFi: `wifi:m f null` allocation-failure spam and WiFi link stalls
  under normal operation.** The closed-source WiFi blob (`libnet80211.a`)
  logs `"m f <frame-type>"` ("mem fail") whenever it cannot allocate a buffer
  to build a management frame; `"null"` is the 802.11 NULL data frame used to
  signal power-save awake/asleep transitions to the AP. No code in this
  firmware ever called `esp_wifi_set_ps()`, so the default modem-sleep power
  save mode was active and firing NULL-frame allocations on every PS
  transition — a small but frequent allocation racing everything else that
  needs the same tight internal-DMA-RAM pool (WiFi's own static buffers, BLE,
  httpd, TLS). Sustained failures here starved the link badly enough to
  produce `esp-tls: select() timeout` during GitHub update checks. Added
  `esp_wifi_set_ps(WIFI_PS_NONE)` in `wifi_manager.cpp` right after the main
  `esp_wifi_start()` — this is a mains-powered bench instrument, so power
  save has no benefit and only adds allocation churn. The setting persists
  across the STA-credential-probe path's own stop/start cycle (only
  `esp_wifi_deinit()` clears it, and nothing here calls that), so one call
  covers every WiFi (re)start.

- **ESP32 update manager: local DAQ HAT (P4/C6) push starved the HAT link's
  other users.** `update_manager_push_local()`'s chunk-send loop called
  `hat_ota_data()` back-to-back with no yield between calls, so this task
  re-took `s_hat_mutex` the instant it released it for the whole transfer,
  starving lower/equal-priority HAT link traffic — telemetry (`0x5A`) and
  `MB_POLL` (`0x5D`, also how the C6 screen's own progress-bar poll gets
  through) — logged as repeated `HAT command 0x5A/0x5D: failed to take mutex`
  on the bench during a local C6 push. Added a 1 ms yield once per outer
  ~4 KB read block, matching the pattern already used by the RP2040 and
  DAQ-OTA download loops elsewhere in this file. The GitHub-release DAQ OTA
  path (`hat_ota_event_handler`) is naturally throttled by TLS/network
  receive rate and was left as-is.

- **ESP32 update manager: memory exhaustion during large GitHub API responses.**
  The device was running out of internal RAM when downloading the 588 KB
  GitHub releases list, causing WiFi driver allocation failures (`wifi:mem fail`).
  The GitHub releases HTTP response buffer now allocates from PSRAM (1 MB cap)
  instead of internal RAM, eliminating contention with the WiFi driver's own
  buffers entirely. Added explicit NULL checks for JSON parsing failures with
  separate error messages and heap diagnostics to track OOM conditions.

- **ESP32 update manager: OTA asset download failing with "Buffer length is
  small to fit all the headers".** GitHub's `browser_download_url` redirects
  once to a signed CDN URL (`release-assets.githubusercontent.com`) whose query
  string alone runs 1.5-2 KB; the outgoing request line didn't fit in the
  `buffer_size_tx=1024` byte TX buffer used by every `esp_http_client_config_t`
  in update_manager.cpp, so `http_header_generate_string()` failed and the OTA
  never started. Raised `buffer_size_tx` to 4096 across all four HTTP client
  configs (release-list GET, RP2040/ESP32/DAQ-HAT asset downloads); allocations
  that size land in PSRAM automatically per this project's
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0` tuning, so it costs no internal RAM.

- **ESP32 update manager: `update_manager_available_targets()` always reported
  RP2040 as updatable.** The mask unconditionally OR'd in `UPDATE_TARGET_RP2040`
  regardless of what MCU was actually on the HAT link, so a request naming
  RP2040 was accepted (and its GitHub asset downloaded, and a flash attempted
  that could only fail at `hat_fw_begin()`) even with a DAQ HAT (ESP32-P4-based,
  no RP2040 present) connected. Now RP2040 and the DAQ HAT targets (P4/C6) are
  correctly mutually exclusive based on the connected HAT's reported type. Also
  made the "Applying release - RP2040/ESP32" diagnostic log lines conditional
  on what was actually requested — they used to print for both unconditionally,
  which read as a phantom RP2040 update on a single-target ESP32 apply.

- **ESP32-S3 webserver: `ESP_ERR_HTTPD_HANDLERS_FULL` on ~6 routes at every
  boot.** `httpd_config_t.max_uri_handlers` was hardcoded to 128 in
  `webserver.cpp`, but the real route count across webserver.cpp (128) +
  http_adapter.cpp (4) + ws_stream.cpp (1) + repl_ws.cpp (1) is 134 — so the
  last few registrations (both registry DAC routes and webserver's own
  wildcard `/*` fallback) silently failed every boot. Raised to 150. (Note:
  there is no `CONFIG_HTTPD_MAX_URI_HANDLERS` Kconfig option in ESP-IDF's
  esp_http_server component — this is a runtime `httpd_config_t` field, not a
  compile-time setting.)

### Changed

- **C6 screen: firmware-update progress bar.** The "Firmware" screen used to
  show only a static "UPDATING" state pill for the whole duration of an
  apply, with no indication of which MCU was being flashed or how far along
  it was. It now takes over the screen with the same progress-bar layout used
  by the DUT calibration wizard (`render_cal()`): active device name, a "do
  not power off" warning, and a live percentage bar. Wired end-to-end: new
  `active_target`/`progress_done`/`progress_total` fields on
  `ddp_mb_fwinfo_t` / `hat_mb_fwinfo_t` (byte-identical, tunneled as an opaque
  blob through the P4 — no P4-side changes needed) sourced from
  `update_manager`'s existing `s_update.progress_done/progress_total`, which
  already covers all four targets (RP2040, ESP32, P4, C6) via the shared HTTP/
  DAQ-OTA download paths. C6 poll cadence during APPLYING raised from 4 s to
  1 s so the bar moves smoothly.

### Added

- **Super Resolution acquisition mode (DAQ HAT).** Trades bandwidth for
  resolution: the ADAQ7769-1s run Sinc3 at maximum decimation — the narrowest
  noise bandwidth the part offers — and the P4 then applies a windowed-sinc FIR
  low-pass before decimating to **1 ksps current / 500 sps voltage**. Averaging
  N samples buys ~sqrt(N) noise reduction; the FIR supplies the anti-alias
  filtering that keeps stage-1 broadband noise from folding back into the
  passband, which a plain keep-1-of-N decimator cannot do.
  - New `Firmware/DAQ_HAT/ESP32P4/src/dsp/sr_filter.{c,h}` — allocation-free
    decimating FIR built on esp-dsp's `dsps_dotprod_f32`, mirrored history
    buffer (no modulo in the inner loop), coefficients normalised to unity DC
    gain so the filter cannot bias the calibrated amps/volts scale.
  - New shared registry key `DAQ_K_SR_MODE` plus the agreed rate constants
    (`DAQ_SR_ADC_DECIM`, `DAQ_SR_CURRENT_SPS`, `DAQ_SR_VOLTAGE_SPS`) in
    `Firmware/DAQ_HAT/common/daq_config_registry.{h,c}`, so P4, C6, S3, desktop
    and iOS cannot disagree about the output rates.
  - The FIR decimation is derived from the ODR the ADC actually reports rather
    than assumed, so a different MCLK or MCLK divider still lands on the
    advertised rates. While SR is on it owns Filter / Decimation / Sample Rate;
    50/60 Hz rejection still applies (SR is always Sinc3). The DSP energy /
    charge tail keeps integrating at the raw ODR, so totals stay correct.
  - Wire: `s3link_acq_config_t` / `hat_acq_config_t` gain an appended `sr_mode`
    byte (a 2-byte payload from an older S3 still parses and means "off"), and
    `POST /api/daq/acq_config` accepts `{"sr_mode": true}` without requiring a
    meaningless filter/decimation alongside it.
  - Toggles on the C6 menu, the desktop DAQ settings panel and the iOS
    acquisition sheet; the rate/filter controls disable themselves while SR is
    active on every surface.

- **C6 "SR" home-screen badge.** Cyan badge in the header, width-reserved like
  the SRC badge so the Pac-Man bolt stream never overdraws it — the low
  waveform rate is never mistaken for a stalled stream.

- **DAQ HAT pipeline cycle profiler + `perf` CLI command.** `faststat` reports
  how many samples survived, which says nothing about *where* the time went —
  so a saturating pipeline could not be attributed to the SPI capture core, the
  DSP consumer, or the wire push without guessing. New
  `Firmware/DAQ_HAT/ESP32P4/src/perf/daq_perf.{c,h}` timestamps every stage of
  both hot loops with the RISC-V cycle counter, and `perf <on|off|reset|show|
  tasks>` dumps per-stage count/avg/min/max/%core alongside FreeRTOS per-task
  CPU shares, stack high-water marks, heap and the active tunables.
  - Compiled out entirely with `-DDAQ_PERF_ENABLED=0` (in `platformio.ini`):
    verified to remove 1152 B of RAM and 7754 B of flash and leave no residual
    branch in either hot loop. Sampling is also off at runtime until `perf on`.
  - Records are padded to a full 64-byte L2 line so the two cores never share
    one, and `daq_perf_reset()` self-calibrates the probe's own overhead so
    measurements can be read net of the instrument.
  - Enables `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` /
    `USE_TRACE_FACILITY` (esp_timer source) for the task table.
  - New host tools `tests/tools/daq_p4_profile.py` (ODR sweep that attaches a
    cycle budget to every throughput point, plus bottleneck attribution and a
    projected ceiling) and `tests/tools/daq_p4_sanity.py` (proves samples are
    real: variation, raw-code variation, response to a DUT-supply step, and a
    voltage linearity sweep).

- **`ADAQ_RING_PREFER_INTERNAL` capture-ring placement knob** (`config.h`,
  default 0 = PSRAM). Measured: internal SRAM at 4096 samples/bus lifts the
  saturation point 443 → 469 kSPS (+5.9%) and captures 320001/320000 with zero
  missed edges, but costs ~200 KB of internal SRAM that WiFi and OTA also need
  and cuts stall tolerance from 256 ms to 16 ms — hence off by default.


- **C6 Firmware screen (installed vs available, with update).** Uses the full
  284 px width for a four-MCU table (RP2040 / S3 / P4 / C6) showing installed
  and available versions side by side; OK opens a release picker for the
  highlighted MCU, OK again confirms before anything is flashed. Absent MCUs
  are unselectable rather than flashable.
  - Reaches GitHub through the existing C6 → P4 → S3 mainboard tunnel via new
    request types `DDP_MB_FWINFO` / `DDP_MB_FW_APPLY` and `ddp_mb_fwinfo_t`;
    no new HAT commands, so it inherits the tunnel's chunking and reassembly.
  - `DDP_PROTO_VERSION` 8 → 9.

- **Board temperature on the USB stream (STATUS extension v7).** The two AD7415
  sensors (U2 analog area, U28 power area) are now stamped on every `STATUS`
  frame as `t_board0_c10` / `t_board1_c10` (0.1 °C, `USB_TEMP_NA` = 0x7FFF).
  At SR rates the residual is dominated by low-frequency drift, and without a
  temperature on the same frames thermal drift is indistinguishable from
  converter 1/f. Values are **cached** from the ~1 Hz `diagnostics_push()` poll,
  never read inline: the STATUS frame is built on `daq_fast_task`, where a
  blocking I2C transaction would stall the acquisition producer. The ADAQ die
  sensors are deliberately excluded — they share the converter and are only
  readable while acquisition is stopped. Append-only, so older parsers are
  unaffected.

- **`tests/tools/daq_noise_sweep.py` + `daq_noise_plot.py`** — noise
  characterisation across every filter / hardware-decimation setting and SR,
  with the DUT supply live (`--vdut`, USB-PD precondition checked, supply
  restored on every exit path). Reports detrended sigma, robust p-p, noise
  density and bits-vs-reference, and cross-checks SR against *naive*
  keep-1-of-N decimation of the reference to the same rate — the comparison
  that separates real filtering from bandwidth reduction. Captures whose yield
  falls outside 70–150% of `rate × window` are flagged `SUSPECT` and excluded
  from both the verdict and the plots, since a truncated or desynced window
  still produces a plausible sigma. Found three of the firmware bugs above.

### Fixed

- **Super Resolution used only 1/8 of the ADC's available decimation.**
  `DAQ_SR_ADC_DECIM` was 1024 (8 ksps) with a FIR ÷8 behind it, which measured
  **worse than simply setting Sinc3 to its maximum**: 60.5 nA vs 46.1 nA at the
  same 1 ksps output. The FIR only moved 63.3 → 60.5 nA, because its input was
  already 1/f-correlated and averaging cannot remove 1/f — whereas the ADC's own
  Sinc3 acts on the modulator bitstream before that correlation exists. Now
  8192, so fMOD/8192 lands exactly on `DAQ_SR_CURRENT_SPS`; SR then matches the
  best hardware setting (44.7 nA vs 44.7 nA). The FIR remains in the voltage
  path, which cannot change its own ODR.

- **Super Resolution produced zero voltage samples.** `apply_sr_mode()`
  reprogrammed all three ADAQs, putting VOLTAGE (U23) at the same ODR as COARSE
  (U22). They share SPI bus B and one SYNC line and cannot be phase-staggered,
  so both asserted DRDY on the same edge every sample and the voltage channel
  starved — `WAVE_V` count was 0 for the whole of SR mode. VOLTAGE now keeps
  `VOLTAGE_ODR_TARGET_SPS` and the FIR decimates from there, matching the ruling
  already documented for `CTRL_MSG_SET_ACQ_CONFIG`.

- **Leaving Sinc5x8 left the ADAQ in 16-bit conversion mode.**
  `adaq7769_set_filter()` wrote the interface-format register only when
  *entering* the 16-bit path, and `adaq7769_set_sinc3()` cleared the shadow flag
  without writing the register at all. The driver then believed 24-bit while the
  part stayed 16-bit, misframing every subsequent read until a full re-init —
  visible as a collapsed sample yield on any filter selected after Sinc5x8. The
  register is now written on every filter change.

- **The C6 reported firmware 0.2 regardless of what was flashed.**
  `ESP32C6/src/ddp.h` hardcoded `DDP_FW_MAJOR 0` / `DDP_FW_MINOR 2`, never wired
  to `version.h`. Now derived from it. The DDP wire carries only major/minor, so
  the patch level is still not visible upstream.

- **The P4's reported version was the HAT board revision, not its firmware.**
  `current_p4_build_id()` read `hat_get_state()->fw_version_*`, which comes from
  `HAT_CMD_GET_INFO` and reads 1.0 on a DAQ HAT whatever P4 image is running
  (confirmed on the bench immediately after a successful P4 OTA). It now uses
  `HAT_CMD_GET_VERSION` via `hat_get_version()`, cached because that is a
  blocking round trip reached from the ~1 Hz mainboard-tunnel poll.

- **Waveform frames had no latency bound, only a size bound.** `WAVE_I` /
  `WAVE_V` flushed solely on reaching 3200 samples, a throughput figure chosen
  for high ODRs ("12.5 ms batches at typical ODRs"). Nothing capped how long a
  *partial* batch could sit, so the lower the rate the longer the host waited —
  at Super Resolution's 1 ksps / 500 sps a frame would have been held for
  **3.2 s / 6.4 s**, making the scope look frozen rather than slow.
  `usb_stream_push_sample()` / `_push_voltage()` now derive a per-batch sample
  limit from the rate at batch-open time (`USB_WAVE_FLUSH_HZ`, 100 ms worst
  case). The limit is computed once per batch rather than consulting the clock
  per sample, because the hot path runs at up to 256 ksps; rates at or above
  32 ksps clamp to the full batch, so high-rate behaviour is unchanged.

- **The USB waveform stream delivered nothing — every WAVE frame was dropped.**
  `sdkconfig.defaults` asked for `CONFIG_TINYUSB_VENDOR_TX_BUFSIZE=32768`, but
  PlatformIO never merges new defaults into an already-generated `sdkconfig`,
  so the build shipped the 8192-byte default. `WAVE_I` frames are 16038 B and
  `WAVE_V` 12838 B, so `emit_frame_inplace()`'s `writable() < total` check
  could **never** pass: this was not back-pressure but a permanent 100% drop of
  both waveform record types, leaving a connected host with only 10 Hz summary
  frames. Measured on hardware, before → after: `WAVE_I` **149 → 128,100 Sa/s**
  (exactly the configured ODR), `bytes_per_sec` 2,620 → 900,807,
  `fifo_drop_frames` 147-of-151 → 0-of-549, wire frame loss 49.5% → 0.000%.
  New `_Static_assert`s tie `USB_WAVE_*_BATCH` to the FIFO size; the existing
  asserts only checked `USB_MAX_PAYLOAD` (16384), which is exactly why a
  16038-byte frame slipped through.

- **Summary emission stalled the acquisition consumer.** At 10 Hz,
  `daq_board_stream_summary()` runs inline on `daq_fast` and was measured at
  ~1.5 ms per call. Profiling ruled out the obvious suspects — the 4096-point
  FFT is 9 µs, `write_flush()` 0.7 µs and the `write_available()` probe 2.2 µs.
  Two real costs were found and fixed: `power_dsp.c:stat_finish()` used
  double-precision `sqrt()` twice (software-emulated on the P4's
  single-precision FPU) and discarded the extra precision in a `float` cast —
  now `sqrtf`; and the four small summary frames each issued their own
  `tud_vendor_write()` — they are now coalesced into one via
  `usb_stream_batch_begin/end()`. Uncontended cost (the `min` column, free of
  preemption) **748 → 347 µs, −54%**. The remaining gap between min and average
  is higher-priority USB work preempting the window, not compute.

- **Connecting to the P4 console reset the board.** Opening or closing the
  USB-Serial-JTAG port asserted DTR/RTS, rebooting the P4 — so any measurement
  that touched the console silently restarted the device first (energy and
  charge accumulators reset to zero mid-run). `tests/lib/p4_console.py` now
  holds both lines deasserted on open *and* close.

- **`find_p4_port()` only worked on macOS** — it globbed `/dev/cu.usbmodem*`,
  so every P4 host tool auto-detected nothing on Windows and Linux. It now
  probes pyserial's port enumeration as well. Likewise `tests/lib/daq_link.py`
  resolves a libusb backend via `libusb_package`, since pyusb's default search
  finds no libusb on Windows (the firmware's MS OS 2.0 descriptors already make
  Windows auto-bind WinUSB, so no Zadig step is needed).

- **`/api/ota/releases` never reported DAQ HAT update state.** `option_json()`
  serialised only the `rp2040` and `esp32` components even though
  `UpdateManifest` already parsed `p4` and `c6`, so every client silently got
  nothing for the DAQ HAT. Both components are now emitted, with new
  `current_p4_build_id()` / `current_c6_build_id()` helpers.

### Changed

- **DAQ HAT P4 now builds at `-O2`** (`CONFIG_COMPILER_OPTIMIZATION_PERF`)
  instead of the IDF default `-Og`. Both hot loops are hand-written C on a
  360 MHz RISC-V core with no DMA assist, so they are exactly the shape `-O2`
  helps most. Measured at the saturation point (ODR ratio 32): aggregate
  capture **405 → 443 kSPS (+9.2%)**, with per-stage costs down 13–18%
  (`cap.scan` −18.2%, `fast.loop` −17.2%, `fast.fusion` −13.2%). It is also
  *smaller*: 1,101,942 B flash vs 1,115,446 B and 16,560 B RAM vs 17,144 B.
  Revert to `COMPILER_OPTIMIZATION_DEBUG` only for step-debugging.

- Firmware versions: ESP32-S3 `5.0.0` → `5.1.0`, DAQ HAT P4 `2.0.5` → `2.1.0`,
  DAQ HAT C6 `2.0.0` → `2.1.0`, Desktop `2.0.0` → `2.1.0`. BBP wire stays at
  v10 (no BBP command or payload changed).
- C6 `SETTINGS_VERSION` 5 → 6 (the settings struct grew an `sr_mode` field, so
  the previous NVS blob must be rejected rather than misread).

- **Live ESP32-S3 memory telemetry across every surface.** The S3 runs tight on
  internal SRAM, but heap and per-task stack headroom were only visible through
  the `heap` / `stack_hwm` serial CLI commands, which need exclusive use of
  CDC0 — so the desktop app, the MCP server and CI could not observe memory
  pressure at all. `/api/status`'s `freeHeap` sums internal and PSRAM, so on a
  PSRAM board it stays healthy-looking while the pool that actually runs out is
  nearly exhausted.
  - New BBP command `MEM_STATUS` (`0x0C`, `Firmware/ESP32/src/bbp/cmds/cmd_memory.cpp`)
    with a schema-versioned payload: internal and PSRAM free / min-ever /
    largest contiguous block / total, uptime, and per-task declared-vs-high-water
    stack. Takes no state lock — a diagnostic that blocks behind `g_stateMutex`
    is useless exactly when the device is struggling.
  - New HTTP route `GET /api/system/memory` (`net/api_core.cpp` +
    `web/webserver.cpp`), camelCase and snake_case aliases as elsewhere.
  - New `bugbuster.memory` module and `BugBuster.get_memory_status()` returning
    `MemoryStatus` with derived `fragmentation_pct`, `worst_task` and
    `warnings()`; works over USB and HTTP.
  - New MCP tool `device_memory`.
  - New `tests/tools/mem_watch.py` — refreshing terminal dashboard, session
    low-water tracking, `--stress` to sample under load, `--json`/`--csv`
    export, and `--fail-under-kb` / `--fail-task-pct` thresholds so the same
    tool serves interactive debugging and CI.
  - New shared task registry `tasks_get_registry()` (`tasks.h`/`tasks.cpp`) is
    now the single source of truth for the CLI, the BBP command and the HTTP
    route; `cli_cmd_stack_hwm` no longer keeps its own copy of the table.
- **Eight previously unreachable BBP commands bound on the Python client** —
  `get_diagnostics`, `idac_calibrate`, `idac_cal_add_point`, `idac_cal_clear`,
  `power_set_port`, `set_avdd_select`, `set_do_config`, `usbpd_go`. Each was
  defined in `bbp.h`, enumerated in `constants.py` and implemented in the
  firmware, but no client method ever sent it.
- **CI quality gates** (`.github/workflows/python-tests.yml` rewritten):
  a `lint` job (ruff hard-gated on the `F`/`B`/`E9` real-defect rules, mypy
  advisory) and a `test` matrix across Ubuntu 3.11/3.12/3.13 plus Windows and
  macOS. All six suites now run — `tests/simulator`, `tests/integration` and
  the full `tests/device --sim` set were previously never executed by CI.
  Coverage is collected and enforced by a new ratchet
  (`tests/tools/coverage_ratchet.py` + `tests/coverage-baseline.json`) that
  fails on a drop and makes lowering the floor an explicit, reviewable diff.
- **Cross-surface parity gates** — `tests/unit/test_bbp_command_parity.py`
  (firmware `bbp.h` ↔ `constants.py` ↔ client reachability, duplicate-opcode
  detection) and `tests/unit/test_idac_wire_format.py` (per-channel record
  layout derived from the firmware source).

### Fixed

### Changed

- **Internal SRAM: 15 640 bytes of static DRAM reclaimed (100.0 KB → 84.8 KB,
  build RAM 31.3% → 26.5%).** Two independent findings.

  *Registry (7424 B).* `cmd_registry.cpp` held `CmdDescriptor s_registry[256]`
  and `memcpy`'d every subsystem's descriptor block into it — 8192 B of `.bss`
  for 143 registered commands, on a device with ~40 KB of internal heap free.
  The per-subsystem blocks are already `static const`, so the linker keeps them
  in flash `.rodata` at zero internal-RAM cost; the registry now stores
  *pointers* into them (192 slots × 4 B = 768 B) and sorts the pointer array.
  Descriptor fields (`name`, `args`, `rsp`) were flash-resident already, so
  this introduces no new access class.

  *`EXT_RAM_BSS_ATTR` on function-scope statics (8216 B).* The attribute is
  **silently ignored** on a `static` declared inside a function — the buffer
  lands in internal DRAM regardless. `cmd_script.cpp` carries a comment saying
  exactly this, but four buffers had fallen into the trap anyway:
  `hat.cpp: hat_build_scripts_blob::names` (2112 B),
  `ws_stream.cpp: tx_task::tx_buf` (1500 B),
  `repl_ws.cpp: repl_tx_task::tx_buf` (512 B), and
  `repl_ws.cpp: s_tx_ring` (4096 B — file-scope, but simply missing the
  attribute while its exact sibling in `ws_stream.cpp` had it). All four are
  websocket/DDP payload staging — not task stacks, never touched from an ISR —
  so PSRAM is safe, matching five existing `EXT_RAM_BSS_ATTR` buffers including
  two identical copies of the same script-name array. Verified end to end on
  hardware: 57 USB scope events → 57 websocket frames forwarded, exact parity.

  Measured on hardware after OTA, under `mem_watch --stress`: internal free
  **40.2 → 54.4 KB**, min-ever **30.2 → 44.5 KB**, largest contiguous block
  **22.0 → 36.0 KB**, fragmentation **45.2 % → 33.8 %**. Cross-checked against
  the device's own `heap` / `stack_hwm` CLI, which agrees digit for digit.

### Known issues

- **ADC DSP throughput is bounded by the `adcPoll` loop, not the converter.**
  Even with the rate now applied (below), a 9.6 kSPS request yields ~512
  samples/s — adcPoll reads over SPI once per FreeRTOS tick, so ~1000 samples/s
  is the architectural ceiling. `effectiveRateHz` now reports this bound rather
  than the converter rate. Getting closer to the converter's rate needs a
  DMA/DRDY-driven read path, which is a separate piece of work.

### Fixed

- **`START_ADC_DSP_STREAM`'s `rate_code` was accepted and never applied.**
  `bbpStartAdcDspStream()` used `rate_code` only to compute the `effectiveRate`
  it echoed back; it never touched the AD74416H's CONV_RATE. A `SPS_9600`
  request advertised ~37 windows/s (as the client docstring also promised) and
  delivered **one 256-sample window per ~20 s** — measured on hardware over both
  USB and the WiFi websocket. Three separate causes, all fixed:
  1. The rate is now written to the channel (via the existing `CMD_ADC_CONFIG`
     path) and **restored when the stream stops**, so a DSP session no longer
     leaves the channel and the poll cadence permanently at 9.6 kSPS.
  2. Setting CONV_RATE alone was not enough: `ADC_CONV_CTRL` also runs the four
     diagnostic slots and the sequence advances at its slowest member, which
     still capped the channel at ~46 samples/s. The DSP stream now claims
     refcounted scope mode (`tasks_scope_mode_enter`) for its channel — the same
     mechanism `START_SCOPE_STREAM` already used for this exact reason — and
     releases it on stop, restoring the diagnostic slots.
  3. `effectiveRateHz` is now derived from `min(converter rate, poll-loop
     ceiling)` instead of the converter rate alone, so it reports 3 windows/s
     where the device measures 2.0 — instead of 37.

  Measured on hardware, 256-sample windows: **12.5 → 512 samples/s at 9.6 kSPS
  (41x)**, first window at 0.63 s instead of 20.5 s, and the window rate now
  tracks the requested rate (200 SPS → 128 samples/s). Channel `adc_rate` and
  the ADC diagnostic slots both verified restored after the stream stops.
- **A DSP stream on a `HIGH_IMP` channel streamed zeros forever.** `adcPoll`
  skips `CH_FUNC_HIGH_IMP` channels entirely — it neither reads them nor lets
  them raise the poll rate — so the stream emitted stale cached values with no
  indication anything was wrong. `bbpStartAdcDspStream()` now returns false and
  the transports surface it (BBP `INVALID_STATE`, HTTP `409`). Invalid
  `rate_code` values (`AdcRate` is not contiguous — 2, 5, 7, 10, 11 are not
  rates) are rejected too, instead of being silently written to CONV_RATE.
  The simulator mirrors both rejections, and a new parity gate
  (`test_simulator_adc_rate_codes_match_the_firmware_enum`) derives the valid
  set from `ad74416h_regs.h` so the simulator cannot drift from the firmware.


- **A USB disconnect silently reset logical channel 0.**
  `bbpExitBinaryMode()` → `bbpStopWavegen()` → `wavegen_stop_and_reset()`
  (`bbp/bbp.cpp`) enqueued `CMD_SET_CHANNEL_FUNC(HIGH_IMP)` unconditionally,
  including on devices that had never started a waveform — and
  `WavegenState.channel` defaults to 0, so every host disconnect wiped channel
  0's function and DAC output behind the host's back. It is now guarded on the
  `wavegen.active` snapshot the function already takes. Verified on hardware:
  channel 0 set to `VIN` now survives a disconnect/reconnect cycle (it
  previously came back as `HIGH_IMP`).
- **`cmdProc`'s stack was under-sized, and `tasks.h` recorded its peak wrongly
  by 2x.** The comment claimed a measured peak of 832 B with "margin 1216";
  the real peak was **1672 B of 2048, leaving 376 B** — below the ~1 KB minimum
  headroom that same comment mandates and inside the firmware's own
  `*** LOW — overflow risk ***` band. Both earlier right-sizing passes measured
  too early in boot. Isolated on hardware (fw 5.0.0): fresh boot 824 B, one USB
  connect/disconnect cycle 1672 B, saturating there. The cost is
  `tasks_apply_channel_function()` — SPI stop/restart, `ADC_CONFIG` read-back,
  `RTD_CONFIG` write, MUX routing, `tasks_rebuild_adc_conv_ctrl()`,
  `clearAllAlerts()`, `hat_update_leds()` — running on this task, reached via
  the unconditional wavegen reset above. The BBP `SET_CH_FUNC` handler does not
  contribute (`cmd_channel.cpp` calls it synchronously on `bbpCli`; measured
  +0 B on `cmdProc`). With the wavegen guard in place the disconnect path no
  longer touches `cmdProc` at all, but `BBP_CMD_DEVICE_RESET` still enqueues
  the same handler for all four channels, so `TASK_STACK_CMDPROC` is raised
  **2048 → 3072** to keep ≥ 1 KB over the true worst case (1400 B margin).
  Measured on hardware after the change: peak 824 B, 2248 B free, and no growth
  across connect/disconnect cycles. The +1024 B this costs is funded several
  times over by the registry change above.

- **`idac_get_status()` returned garbage over USB.** The client parsed 26 bytes
  per channel for 4 channels while the firmware writes 44 bytes per channel for
  3, and skipped the leading channel-index byte entirely — so every field was
  misaligned from the first record onward. Observed on hardware:
  `target_v = 726302457856.0 V` where the device's own CLI reported `3.299 V`.
  The client now derives the channel count from the payload length. Nothing
  caught this because `tests/mock/handlers/idac.py` had been written to match
  the broken parser rather than the firmware; the simulator now emits the real
  wire format.
- **`stop_scope_stream()` did not exist.** `on_scope_data()`'s own docstring
  referenced it and `STOP_SCOPE_STREAM` (`0x63`) was implemented in the
  firmware and the simulator, but no client method sent it — a started scope
  stream could only be stopped by resetting the device, leaving the ADC
  diagnostic slots disabled.
- **Stack high-water warnings never fired.** `tasks_log_stack_hwm()` compared
  ESP-IDF's byte-valued `uxTaskGetStackHighWaterMark()` against word-valued
  thresholds (`< 128` / `< 256`), so a task with 380 bytes of headroom — 81% of
  its stack used — was reported as healthy. Thresholds are now 512 / 1024 bytes
  and the log says "bytes free" instead of "words free".
- **The entire `tests/unit` suite failed to collect on Windows.** 90
  `Path(...).read_text()` calls used the platform default encoding, which is
  cp1252 on Windows and cannot decode the box-drawing characters in the
  firmware comment banners. All source reads now go through
  `tests/lib/srcread.py`, which pins UTF-8 and resolves paths from the repo
  root.
- Closure-capture bug in `tests/device/test_10_streaming.py`: the ADC callback
  captured `events` and `done` by reference inside a 3-cycle loop, so a late
  callback appended into the next cycle's state.
- Unreachable duplicate `serial.SerialException` clause in
  `python/bugbuster/transport/usb.py`, a broken `"ScriptSession"` forward
  reference in `client.py`, and 9 missing `raise ... from` chains.

- **DAQ HAT hardware-in-the-loop test suite** — ~92 hardware tests plus 44 unit
  tests, built from nothing. Entry point `python3 tests/tools/daq_gate.py`
  (fast tier by default, `--full` for slow tests incl. the FFT freeze guard,
  `--wifi` to add the WiFi tier). Four tiers: `tests/device/test_16_daq_stream.py`
  (tier 1, P4 USB-HS: link/protocol/lifecycle, rate/decimation, record
  integrity, throughput floor, DSP/FFT correctness, range/SMU/linearity,
  STATUS counters, arm/markers), `test_17_daq_control.py` (tier 2, BBP control
  plane: DAQ_CONFIG/DAQ_CAL/DAQ_MEASURE/DAQ_TRIG, incl. cross-layer
  DAQ_MEASURE-vs-stream agreement and the trigger→marker path),
  `test_18_daq_api.py` (tier 3, `api_core` HTTP routes) with a new static
  guard `tests/unit/test_api_route_parity.py` (every route in `api_core.cpp`
  must be registered in `webserver.cpp`), and `test_19_daq_wifi.py` (tier 4a,
  WiFi provisioning without joining the hotspot). New shared libraries in
  `tests/lib/` — `daq_proto.py`, `daq_records.py` (typed record decoders, now
  the single source of truth for wire record layouts on the host side),
  `daq_capture.py`, `daq_link.py`, `p4_console.py` — extracted from the
  existing bench tools so the repo has exactly one frame decoder. New conftest
  options `--daq`, `--daq-wifi`, `--daq-p4-serial`, `--daq-load-ohms`; new
  markers `requires_daq`, `requires_daq_bbp`, `requires_daq_http`, `daq_wifi`.
  Assertions are relational only (linearity, monotonicity, self-consistency)
  — the DUT load resistance is never asserted against.

### Fixed

- **DAQ HAT: FFT disable stopped computing spectra but never stopped
  transmitting them.** `spectrum_push()` correctly gated new FFTs on the
  `enabled` flag, but the emit path in `daq_board.c` kept shipping the last
  computed magnitude buffer forever, so a client that asked the device to
  stop sending spectra kept receiving stale ones indefinitely. Found by the
  new DAQ test suite; fixed alongside the item below.
- **DAQ HAT: the FFT header always reported the Hann window**, regardless of
  which window was actually configured. The window was correctly *applied* to
  the computation — only the reported window ID in the FFT record header was
  wrong, so a host had no reliable way to know which window produced a given
  spectrum.
- **`GET /api/daq` 404'd over plain HTTP** even though `api_core` already
  dispatched it correctly over BLE — the route was simply never registered in
  `webserver.cpp`. Found by the new tier-3 HTTP API tests.
- **`DAQ_CONFIG` GET_ALL never worked over BBP.** The P4 packed its reply
  pages up to 240 bytes (`HATP_MAX_PAYLOAD`), but the S3's HAT-frame receiver
  silently rejects anything over 32 bytes (`HAT_FRAME_MAX_LEN`) *before*
  reading the payload — every GET_ALL reply was dropped, retried once, and
  surfaced to callers as a bare `DeviceError 0x11` timeout with no indication
  of the real cause. The same undersized-reply bug was also found and fixed
  in the DAQ config schema handler. Fixed by paging P4→S3 replies at the
  32-byte wire limit; `daq.get_all()` now returns its full key set in well
  under a second, and BBP control-plane test latency dropped by roughly half
  across the board as a side effect (the undersized-frame retry had been
  silently slowing other calls too, not just GET_ALL).

### Changed

- **A digital-IO output pin's reported input level now reflects reality
  instead of always reading `false`.** Output pins were previously
  write-only: `DioState.input_level` stayed permanently `false` for any pin
  configured as OUTPUT, because the pin was never reconfigured to also read
  back its own level. This is user-visible on every surface that shows DIO
  state — the on-device web UI, the desktop app, and MCP tools all now show a
  live level for output pins where they previously always showed off/false.
  It also fixes a real functional gap: the DAQ trigger/flag engine watches
  `input_level` to detect edges, so an S3-driven output pin's own transitions
  were invisible to the S3's own trigger engine and could never fire a
  self-stimulus flag or trigger.

### Known issues

- **Frame drops at the shipping default DAQ rate.** 42–53 `fifo_drop_frames`
  observed on an idle, single-consumer bench at the default 32000 SPS/ch —
  suspected cause is the 800-sample `USB_WAVE_V_BATCH` flushing at ~80 Hz
  against WAVE_I's ~10 Hz cadence, transiently tripping USB back-pressure.
  Recorded as a strict xfail in the new test suite so it surfaces the moment
  it's fixed.
- **`ring_high_water` in the DAQ USB STATUS record is permanently dead**,
  hardcoded to 0 in `daq_board.c` rather than reading a live figure from the
  acquisition ring buffer.
- **Desktop and iOS diverge on out-of-range DAQ source setpoints.** The
  desktop's USB `CMD_SET_SOURCE` path clamps an out-of-range value to the
  nearest legal one; the HAT/HTTP/BLE VDUT path used by iOS rejects it
  outright. Same underlying hardware action, two different behaviors — needs
  a product decision on which is correct.
- Open questions from this pass: the OTA upload response's
  `sha256Verified:false` field is unexplained; exposing `wifi_ap_sta_count()`
  would make the DAQ WiFi idle-teardown test fully conclusive rather than
  inferred.

## [2.0.0] — 2026-07-29

Combined release. Component versions: **ESP32-S3 firmware v5.0.0**, **RP2040 HAT
firmware bb-hat-5.0**, **DAQ HAT ESP32-P4 v2.0.0**, **DAQ HAT ESP32-C6 v2.0.0**,
**Desktop app v2.0.0**.

Wire protocols are **unchanged** — BBP v10, DDP v8, DAQ USB v2. Everything below
was added additively (appended opcodes, replies widened with their original
prefix preserved), so an older peer still parses what it knew. The major version
reflects the size of the release, not a break in compatibility.

### Added

- **Multi-MCU OTA — the DAQ HAT's P4 and C6 are now updatable from a release.**
  Previously only the ESP32-S3 and RP2040 HAT could be. The S3 became the
  orchestrator: it downloads from GitHub and streams images over the HAT UART
  link straight onto the wire, with **no S3-side staging file** — the P4's 5 MB
  `staging` partition is already the durable, SHA-256-verified, NVS-resumable
  buffer, and a second copy would have consumed two thirds of the 3 MB `scripts`
  SPIFFS holding user MicroPython files. New S3 wire constants and wide-frame
  senders (`HAT_CMD_OTA_BEGIN/DATA/END`, 236 B per frame over the 240-byte DAQ
  link; the generic 32-byte HAT helpers would have been ~20x slower),
  `HAT_CMD_DAQ_C6_VERSION` (0x6A) and `HAT_CMD_DAQ_RELAY_APPLY` (0x7A), and
  `HATP_CMD_OTA_STATUS` widened from 10 to 19 bytes with `relay_stage` progress.
  `update_manager_apply()` now takes a target bitmask over
  {RP2040, ESP32, P4, C6} instead of two booleans, and **enforces
  RP2040 → C6 → P4 → S3 ordering in firmware** so no client can brick the C6 by
  updating the P4 first — the C6's ROM-loader push is driven *by* the P4, so the
  P4 must still be running its old image. Resume is driven from the P4's own
  byte count (the S3 holds no resume state) via HTTP `Range`, and a resumed
  request must answer 206. See `.mex/patterns/multi-mcu-ota.md`.
- **CI builds and publishes P4 and C6 firmware images.** New
  `.github/workflows/daq-hat-firmware.yml`; `bugbuster-update-manifest.json`
  gained optional `p4` and `c6` entries. The two chips take **different image
  formats** and this is the dangerous part: the P4 asset is an app-only OTA
  image bound for `esp_ota_write()` and an A/B slot, while the C6 asset is a
  **full merged image from 0x0** (bootloader + partition table + app), because
  the C6 is written by the ESP-ROM loader starting at flash offset 0. An
  app-only binary published as the C6 asset would overwrite its bootloader and
  brick the chip — no A/B slot, no second MCU to recover it. New
  `Firmware/tools/esp_image_tool.py` builds the merged image and verifies both
  formats; since both open with the same `0xE9` magic and a real app-only image
  is ~1.27 MB (already past the 0x10000 app offset), it discriminates on the
  partition-table magic at 0x8000 plus a second `0xE9` at 0x10000.
  `firmware_version.py` now accepts `p4` and `c6`.
- **DAQ WiFi stream — force-recycle instead of power-cycling.** New
  `HAT_CMD_DAQ_WIFI_STREAM_RECYCLE` (0x79) /
  `POST /api/daq/wifi_stream/recycle` unconditionally tears down and rebuilds a
  wedged or interrupted P4 WiFi stream, removing the power-cycle dependency
  entirely. A bring-up **stage byte** (requested/ap/dns/tcp) rides the STARTING
  reply and surfaces through `/api/daq/wifi_stream/status` so clients can show
  real progress instead of a blank wait, and the P4 auto-tears-down after 60 s
  with no TCP client connected.
- **DAQ acquisition configuration.** ODR / filter / decimation config command
  with device-confirmed readback of the values actually applied, a
  filter-aware picker for the FINE+COARSE current path, and `SET_RATE` SPS
  requests mapped onto stream decimation.
- **DUT-supply (VDUT) control over both HTTP and BLE.**
  `/api/daq/vdut/status|enable|setpoint`, reachable over BLE for free through
  the shared `api_core` dispatcher. Setpoints are re-validated on the P4 against
  the SMU limits and **rejected rather than clamped** when out of range.
- **Signal-integrity work on the DAQ acquisition path.** Per-sample ADAQ CRC
  verification for glitch rejection at the source, isolated-outlier despiking on
  device, raw-resolution deep zoom with progressive LOD tiers, and per-record-type
  TX/RX counters plus voltage-ADC health surfaced in the STATUS record.
- **iOS: off-main scope render pipeline and an explicit DAQ link state machine.**
  Socket, decode, and sample storage moved off the main thread; a pure,
  Foundation-only `DaqLinkState` reducer replaced seven ad-hoc connection flags
  and drives a three-rung recovery ladder (redial socket → rejoin hotspot →
  recycle device) with a data-stall watchdog and a Retry affordance. Also: iPad
  adaptive layout across every tab, and a mock/demo device mode for UI work
  without hardware.

### Fixed

- **A successful C6 update could end with a black display.** `c6_flasher_begin()`
  drives `C6_BOOT_PIN` low and never releases it, and both `_finish()` and
  `_abort()` reset the C6 from inside the call — so releasing the strap
  afterwards is too late and the chip returns to ROM download mode silently,
  which a P4 reset does not clear. All three call sites now release first.
- **The new DAQ OTA helpers did not check a DAQ HAT was attached**, so with an
  RP2040 LA HAT they would have pushed OTA frames at firmware that reads those
  opcodes differently. Now gated on `HAT_TYPE_DAQ_POWER`.
- **The S3 could pull an unverified image out of the P4's staging partition.**
  Staging is shared between the C6 relay and the S3's own pull, so "something is
  staged" never meant "staged for us" — a C6 image written into the S3's OTA
  slot would brick the mainboard. Now requires both `RELAY_STAGED` and
  `RELAY_TARGET_S3`.
- **`HATP_CMD_DAQ_WIFI_STREAM_RECYCLE` was never reachable** — the handler
  existed but was missing from `s3_link.c`'s explicit dispatch allow-list, so it
  answered `RSP_ERROR` as dead code.
- **Wedged DAQ WiFi link required a power cycle.** Root cause was `wifi_ap.c`'s
  `s_up` flag staying stale-true after a stop-side error, which made
  `wifi_ap_start()`'s cached-flag short-circuit silently no-op forever. Also
  fixed: per-step init guards so a failed retry resumes rather than replaying
  non-idempotent steps that hard-abort the board, `FAILED`-state decay, and a
  generation counter replacing a boolean cancel flag that a later START could
  un-cancel, letting a superseded task stamp `READY`.
- **Memory.** The update worker's stack leaked 12 KB of internal RAM per update
  (`vTaskDeleteWithCaps` was missing), and the Core-1 task stacks were
  oversized — together freeing ~19 KB of internal RAM. On the P4, any task
  touching flash or NVS must use an internal-RAM stack:
  `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` means a plain `xTaskCreate()`
  may land a stack in PSRAM, which is corrupted when the D-cache is disabled
  cross-core during an OTA write.
- **iOS `fMOD` was wrong (8.192 MHz), making every derived ODR 8x too low.**
- Numerous DAQ streaming fixes: atomic per-frame TCP writes (a partial lwIP
  `send()` desynced the magic-resync stream so the phone decoded float bytes as
  fake samples), non-blocking stream socket so backpressure cannot stall
  acquisition, softAP PMF set to capable-not-required (PMF-required is a known
  associate-then-deauth cause), and the `LAST`-flag bug where a STARTING poll
  could be misread as finished-but-not-ready and abort the whole bring-up.

### Changed

- Version scheme: all five components moved to a major version together. The
  private `.mex` knowledge base is no longer tracked as a gitlink in this repo.
- The ESP32-C6's vendored `wifi_hosted` component is now tracked in git. It was
  covered by a blanket ignore rule and is a local-path dependency that nothing
  can re-fetch, so the C6 could previously only be built on a machine that
  already had a copy — no CI job could produce a C6 image.

### Earlier work also shipping in this release (accumulated since v1.5.0)

### Added
- **DAQ HAT (ESP32-P4) staged/resumable relay-update core.** New 5 MB `staging` partition
  (P4 NVS grown 24 KB → ~988 KB to make room) plus a `relay_stage` module that writes/verifies
  (SHA-256) a full C6 or S3 firmware image before anything downstream is touched, with NVS-backed
  resume state so an interrupted stage or push picks up where it left off instead of restarting.
  A `relay_c6` engine pushes a verified image to the on-module ESP32-C6 over the existing UART
  bootloader link with retry/backoff and resumes from the last acked offset. The ESP32-S3
  mainboard can also pull a staged image from the P4 (`HATP_CMD_STAGE_READ`/`hat_stage_read()`,
  S3 stays master) instead of only fetching over WiFi/HTTPS, feeding the same `esp_ota_write()`
  path it already uses. Ingest works both over the S3 HAT link and directly from the desktop app
  over USB (`USB_CMD_OTA_BEGIN/DATA/END/ABORT`); the existing USB status heartbeat now carries
  staging progress so the desktop can poll for begin/write/verify success. A mutex now guards the
  shared relay state so the USB, S3-link, and C6-push paths can't clobber each other mid-transfer.
  Not yet wired: CI/release manifest support for this path, any TUI/Desktop/Web UI trigger
  surface, and an actual caller for the S3 pull function (deferred pending a safeguard that
  confirms the P4's image is fully staged and verified before pulling). See
  `.mex/patterns/daq-hat-relay-update.md`.
- **iOS DAQ WiFi streaming.** The P4 can now bring up a WiFi softAP that iOS clients connect to for
  direct DAQ data streaming, bypassing the S3/mainboard network stack. New HAT protocol commands
  `HAT_CMD_DAQ_WIFI_STREAM_START/STOP/INFO` (0x5F/0x67/0x68) let the S3 trigger P4 WiFi softAP
  bring-up, poll for credentials (SSID/password/IPv4 endpoint), and tear down the AP. P4-side WiFi
  infrastructure: `wifi_ap.c/h` handles softAP bring-up with configurable SSID/password, `daq_wifi_ident.c/h`
  broadcasts the P4 via mDNS (optional fallback captive DNS in `captive_dns.c/h`), and `tcp_backend.c/h`
  relays DAQ waveform/status frames over TCP to connected clients in real-time. C6 display (`ESP32C6/`)
  shows a static "WiFi streaming enabled" screen while active. iOS app (`DaqWifiStreamManager.swift`)
  opens raw TCP sockets to the P4, decodes full `usb_proto.h` v2 frames (WAVE_I/WAVE_V/MARKER/STATUS),
  and renders stream health + credentials in a new `DaqWifiStreamView.swift` sheet launched from Diagnostics.
  Typechecks clean against iphoneos26.5 SDK; hardware bring-up (C6/P4/S3 integration, end-to-end
  throughput, teardown on connection loss) pending bench verification.

### Fixed
- **DAQ HAT (ESP32-P4) reference-meter cal now subtracts the open-circuit baseline.** The
  meter/readings cal previously fit the raw ADC voltage (leakage-per-V_DUT + signal) against the
  reference-DMM current, so V_DUT-correlated leakage on the high-impedance 51 Ohm sense node
  produced a bogus multi-hundred-mV offset. The live-median capture now subtracts the stored
  open-circuit baseline (`smu_base_offset`, keyed on the live V_DUT DS4424 code) in ADC-code
  space, exactly like the runtime `fast_emit` path, so the fit is on the true current. The
  confirm dialog now shows whether a baseline exists and warns to run `cal base` (open circuit)
  first. Also added `cal clear <lo|mid|hi>` and fixed `smu_cal_range_reset` to revert to the
  nominal transfer instead of keeping stale coefficients.
- **DAQ HAT (ESP32-P4) current-limit calibration no longer fails on the hard current ceiling
  (was `flags 0x0046`).** The onboard supply can only source so much current, so the low end of
  the IDAC sweep pins the output at a flat plateau where the reading does not respond to code
  changes ("the needle doesn't move"). The sweep previously flagged `TARGET_UNREACHED` (2 A never
  reached) plus low/high coverage and aborted. `run_current_cal()` now prunes those
  non-responsive codes (`prune_stuck_points()`), keeps only the responsive ramp, validates
  coverage against the span actually achieved, and treats an unreached 2 A target as expected
  rather than a failure.
- **DAQ HAT (ESP32-P4) USB-HS bring-up — the P4 now enumerates, streams, and auto-binds
  WinUSB on Windows.** A chain of four defects kept the high-speed measurement port
  (`303A:4001`) from working:
  1. **Boot order** — `app_main()` started the USB-HS stack *before* the S3/C6 inter-processor
     links, so a stalled USB bring-up left the C6 stuck showing "simulation". The S3 and C6
     links now come up first; USB-HS bring-up can never gate live telemetry.
  2. **TinyUSB task starvation** — the device task defaulted to core 1, the dedicated
     acquisition core where the DRDY capture task runs at priority 20 / ~100% CPU. The
     priority-5 usbd task never drained its event queue, so `osal_queue_send()` failed
     (`queue_event 382: ASSERT FAILED`) and every USB event was dropped — enumeration never
     completed. The USB task is now pinned to core 0 at priority 13 (above the fast-path
     consumer) so it always drains the queue.
  3. **Re-enumeration watchdog fighting a valid link** — with no VBUS-sense line, the
     software re-plug watchdog keyed off `tud_mounted()`; a driverless (Windows "Code 28")
     device enumerates but never gets `SET_CONFIGURATION`, so it looped connect/disconnect
     every few seconds. The watchdog now also honours `tud_connected()` and only forces a
     re-plug when the bus is genuinely idle.
  4. **WinUSB never auto-bound (Code 28)** — added a WCID / Microsoft OS 2.0 descriptor
     (`bcdUSB` → `0x0210`, BOS + MS OS 2.0 set with the `WINUSB` compatible ID and a
     `DeviceInterfaceGUID`). Crucially, because this is a **single-function / non-composite**
     device, the compatible ID must sit at the **device level** (directly under the set
     header) — Windows does not load the composite parent driver that would process a
     configuration/function subset, so a nested subset is read but never applied. Windows
     now auto-installs WinUSB with no Zadig. (`bcdDevice`/serial were also bumped to escape a
     cached `CONFIGFLAG_FAILEDINSTALL` from the pre-WCID firmware.)

### Added
- **DAQ HAT (ESP32-P4) — interactive per-range reference-meter current calibration (TUI).**
  New `tui` action `m` opens a LOW / MID / HIGH range picker. For the chosen range the P4 forces
  that shunt, sweeps 10 output voltages, averages its own ADAQ reading per step, and prompts the
  operator to type the series-connected reference DMM's current — in **µA on the 51 Ω (HIGH)
  range** and mA on MID/LOW. Successive runs **accumulate** their raw points (Add vs
  Clear+restart), so several series resistors can each cover a sub-decade (e.g. 0.1–20 µA, then
  20–100 µA, …) and the fit is refit over the whole accumulated set. A least-squares fit of
  (ADC volts → true amps) yields the range's zero-current offset and gain correction, which are
  applied to `range_manager` live and persisted to NVS (`rngcal` blob, incl. the raw points,
  loaded on boot). Setup: a series resistor + reference DMM in the DUT path, sized per sub-range.
- **DAQ HAT — `c6logs` CLI command.** Bridges the C6 UART2 to the P4 debug console and resets
  the C6 into normal boot so its boot + runtime log streams to the operator (companion to
  `c6boot`, which forces ROM download for esptool). Exit with Ctrl-] or 30 s idle; the DDP
  display link is restored on exit.
- **DAQ HAT — `flash_via_p4.py` builds before flashing.** The C6 flasher now runs
  `pio run -e esp32c6` first so it always programs a fresh image (`--no-build` to skip).
- **`Notebooks/flash_DAQ.ipynb`.** One-stop build+flash notebook: a single setup cell
  (auto-detects the P4 USB-Serial-JTAG port by VID/PID `303A:1001`), a Build+Flash P4 cell,
  and a Build+Flash C6-via-P4 cell.
- **DAQ HAT C6 display + mainboard tunnel — feature set.** Home screen: **hold OK toggles the
  DUT supply** (off when on, on when off); the Pac-Man header is trimmed to fit a **temperature
  indicator** (highest of the two AD7415 board sensors); the current tile shows **"OFF"** while
  the supply is off; and both tiles gain an **accent glow** while the supply is on. Diagnostics:
  the root **Settings screen now shows S3/P4/ADAQ1-3 health bubbles** (green/red from the DDP
  diagnostics snapshot + over-temp guard), and the S3 telemetry push now **measures the VADJ1/
  VADJ2/VLOGIC rails on demand** when the background self-test monitor is disabled (rails were
  reading `--`). New **Main Board Settings** menu: a **Power** submenu with VLOGIC/VADJ1/VADJ2
  voltage **setpoints** (DS4424 IDAC) + four **e-fuse** enable toggles, and a **Scripts** submenu
  that lists the S3 MicroPython scripts and can **run/stop** them with live **RUNNING/CRASHED/
  EXITED/IDLE** status. These tunnel C6→P4→S3 over a new DDP `MB_REQUEST/MB_RESPONSE` (0x19/0x1A,
  `DDP_PROTO_VERSION` 5) + HAT `MB_POLL/MB_RESULT` (0x5D/0x5E, chunked over the 32-byte link); the
  P4 **guards the tunnel off during PC acquisition** (`fast_running`), so the full device
  bandwidth is preserved while streaming.
- **Desktop — DAQ HAT detection fix.** The app now resolves `HatKind` from the reported
  `hat_type` byte (`0x10` → DAQ, else LA) instead of assuming any detected HAT is the RP2040 LA
  HAT, so the DAQ HAT surfaces its own interface instead of the LA tabs.
- **DAQ HAT TUI — voltage calibration hotkey.** The ESP32-P4 bring-up TUI (`tui` command)
  gains a **`k` "cal V"** hotkey that opens a modal yes/no confirmation popup. Choosing **Yes**
  runs the onboard V_DUT voltage calibration: it enables V_DUT at its minimum, steps the DS4424
  IDAC through **all 255 codes** (`-127..+127`, step 1), averages **100 ADC readouts** per code
  off the VOLTAGE ADAQ, and saves the resulting 255-point table to NVM. A live progress panel
  (bar + code/measured-voltage readout) is shown while it runs, and **ESC** aborts. The stored
  table is then consumed on every `smu_set_voltage()` call. Cal blob format bumped to v2
  (`SMU_CAL_MAX_POINTS` 128→255, `SMU_CAL_SAMPLES_PER_PT` 64→100).
- **DAQ HAT TUI — current calibration hotkey.** New **`j` "cal I"** hotkey with its own confirm
  popup runs the current-limit calibration: it sets V_DUT to **3 V** (headroom to reach the
  target), prompts the operator to **short the output**, forces the **50 mΩ (LO)** shunt, and
  steps the DS4424 current-limit code up until the output reaches **2 A**, saving the code→amps
  table to NVM. Live progress + ESC-to-abort.
- **DAQ HAT — open-circuit baseline (current offset) calibration.** New **`b` "cal base"** TUI hotkey
  + confirm popup runs a baseline sweep with the output **open**: for each current range
  (**HI / MID / COARSE**) it forces that range, then steps V_DUT across all **255 DAC codes**,
  settling **200 ms** and averaging **1000 current readings** per code, recording the raw
  open-circuit ADC code (and the **board temperature** at cal time) per range. The tables are
  persisted to a dedicated NVM blob (`SMU_BASE_*`). At runtime the stored baseline is subtracted
  **in software**, per sample, in the fusion path (`daq_board` `fast_emit`), keyed on the **live
  autoranged range** and current V_DUT code — so it tracks HI↔MID autoranging (the two share the
  FINE ADC but have different baselines) and needs no mid-stream register writes. (An earlier
  attempt used the ADAQ hardware `OFFSET` register, but a single per-ADC register cannot hold two
  range baselines nor be rewritten during continuous-read streaming, so open-circuit current did
  not null under autoranging; the software subtraction fixes this.) Bench-verified nulling to
  <16 µA open-circuit across 3–12 V. Baseline blob format is v3.
- **DAQ HAT — scriptable `cal` REPL command.** `cal <status|v|i|base|clroff>` drives the same
  calibration engine from the serial console with plain-text progress (prompts auto-acked), plus a
  `status` view of stored tables, per-range temps, and the live applied ADC offset — usable for
  bench automation without the full-screen TUI.


## [1.5.0] — 2026-06-29

### Added
- **Desktop HAT tab — professional refactor of target power & SWD bring-up.** The HAT tab now
  exposes direct per-rail control for **VLOGIC** (level-shifter reference, 1.7–5.0 V), **VADJ3**
  and **VADJ4** (0–36 V target supplies) — each with a voltage entry box, an **Apply** button with
  a live preview of the pending setpoint, and a discrete **Enable** toggle. Enabling VADJ3 or VADJ4
  above **3.4 V** now pops a confirmation dialog warning that the level can damage a 3.3 V target.
  A new **SWD Target** panel actively probes the target and shows the **DPIDR**, plus one-click
  "Prep 3.3 V / 1.8 V" helpers that set VADJ4 + VLOGIC, enable the level-shifter OE, and run
  detection in sequence (this replaces the old "Setup 3.3 V" buttons that only set I/O voltage and
  connector power and never actually connected SWD). Rail voltage control stays available in the
  Voltages tab as before.
- **Active SWD target detection across the stack.** New BBP command **`HAT_DETECT_TARGET` (0xDB)**
  forwards to the RP2040's `HAT_CMD_GET_TARGET_INFO (0x21)` to perform a live SWD line-reset + DPIDR
  read and returns `{detected, dpidr}`. Wired end-to-end: ESP32 `hat_detect_target()` +
  `POST /api/hat/v2/swd/detect` (HTTP/BLE via `api_core`), Python `BugBuster.hat_detect_target()`,
  and desktop `hat_detect_target` Tauri command. Previously the desktop only read *cached* DAP
  status, which was never refreshed, so a connected target always showed as "No target".
- **HAT rail setpoint (`target_mv`) exposed to hosts.** `hat_get_rail_status` /
  `hat_set_rail_enable` / `hat_set_rail_voltage` responses now carry the DS4424 configured setpoint
  per rail (9 bytes/rail on the wire; `targetVoltageMv` over HTTP/BLE), so the UI can show the
  requested VADJ voltage even while a rail is disabled. The RP2040 already sent this field; the
  ESP32 BBP handler was dropping it before forwarding to the host.

### Fixed
- **SWD "Detect Target" reported no target on working parts (e.g. STM32F4).** The RP2040's
  `bb_swd_detect_target()` hand-rolled the SWD line-reset + DPIDR read with raw
  `probe_write_bits`/`probe_read_bits` and mishandled the SWD turnaround, so it returned "No target"
  even for parts that program fine over OpenOCD. It now drives the **CMSIS-DAP SWD engine**
  (`SWJ_Sequence` + `SWD_Transfer` from `sw_dp_pio.c`) — the same proven path OpenOCD/pyOCD use —
  performing a proper line-reset/JTAG-to-SWD switch and reading DPIDR with retries. (Requires
  reflashing the RP2040 HAT firmware.)
- **Desktop LA / HAT tabs hidden even with a HAT connected, and "Prep SWD" freezing the UI.**
  HAT detection in `app.rs` probed the device exactly once when the connection came up; because the
  ESP32↔RP2040 HAT handshake lags the host connection by a second or two, that single probe often
  missed the HAT and latched `HatKind::None` for the whole session, hiding the **Logic Analyzer**
  and **HAT** tabs. Detection now retries every 3 s using the lightweight `detected` status and
  **latches** once a HAT is found (it then stops issuing detection commands). The earlier interim
  fix polled `hat_get_caps` continuously, which flooded the single-client BBP/CDC0 link with
  `0xC3` timeouts (`0x11`) and, combined with the SWD bring-up sequence, stalled the UI — that flood
  is removed. The **Prep 3.3/1.8 V** helper now awaits each step in order (so commands don't pile
  onto the HAT-UART bridge), only enables the level-shifter **OE** if VLOGIC actually came up
  (avoids the `0x03 INVALID_PARAM` OE rejection), and drops redundant post-detect round-trips.
- **DAQ HAT display firmware — full diagnostics deep-dive (ESP32-C6 + C3).** `Firmware/DAQ_HAT/ESP32C6/` now has a full 5-submenu diagnostics system: Temperatures (die temps from all 4 ADAQ7769-1 pairs, ESP32-P4 tsens, C6 tsens), Power (VADJ, USB-PD bus), Power Rails (VREF, 5 V, 3.3 V sense), ESP32-P4 (P4 die temps, SPI activity), ESP32-C6 (C6 tsens, UART DDP stats). A sparkline detail view (`menu_detail_view()`) shows a 130-sample history for any selected metric. P4→S3 telemetry relay added: new HAT command `HAT_CMD_DAQ_TELEMETRY 0x5A` (`hat.h/hat.cpp` in S3 mainboard firmware) lets P4 push its diagnostics blob to S3, which then forwards it to C6 via DDP `DDP_CMD_SET_DIAGNOSTICS 0x14` (expanded `ddp_diag_t` struct). USB-JTAG set as primary console on both C6 and C3 (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in `sdkconfig.defaults`; frozen per-chip sdkconfig files deleted and regenerated). **ESP32-C3 test board compatibility:** three hardware incompatibilities fixed — (1) `DAQ_UART_TX/RX_PIN = -1` in `config.h` (GPIO16 = `SPIHD`, GPIO17 = `VDD_SPI` on C3 are QSPI flash hold lines; any `uart_set_pin` remap corrupts flash and causes a boot loop), (2) `#ifndef TARGET_C3` guard around `npx_init()` in `main.c` (no WS2812B chain on C3; RMT init panics), (3) `#ifdef TARGET_C6` guard around `temperature_sensor_install(-10, 110)` in `menu.c` (C3 tsens HAL asserts on max > 80 °C). C3 test board (`esp32-c3-devkitm-1`, COM9) now boots stably into the display firmware with no watchdog resets.
- **HAT rail auto-calibration from the iOS app (over BLE + WiFi).** The iOS
  Diagnostics calibration pickers now list the HAT power rails (VADJ3, VADJ4)
  whenever a HAT is detected, in addition to the mainboard IDAC channels.
  Selecting a HAT rail and starting auto-calibration kicks off the RP2040
  cal-engine sweep; viewing its data shows the stored per-rail points. To make
  this reachable over BLE, `api_core` now dispatches
  `POST /api/hat/v2/calibrate/start` (`{railId}`) — previously only the HTTP
  webserver handled it, so BLE clients could not trigger HAT-rail calibration.
  HAT rail entries are hidden when no HAT is present.
- **RP2040 cal-point export → `GET /api/hat/calibration`.** New HAT UART command
  `HAT_CMD_CALIBRATE_EXPORT (0x4D)` / `HAT_RSP_CALIBRATE_EXPORT (0x8B)` reads the
  stored per-rail calibration points back from the RP2040 (paginated, since a
  full table exceeds the 255-byte HAT frame). The ESP32 (`hat_calibrate_export`)
  gathers all pages and `api_core` exposes them on `GET /api/hat/calibration`
  (optional `?rail=N`) alongside the live cal-engine state/validation metrics —
  `points:[{dacCode,measuredV}]`, `pointsCount`, `pointsValid`, `pointsRail`.
  Works over HTTP and BLE; the iOS app shows it in the Calibration data sheet
  (`fetchHatCalibration()`). HAT firmware bumped to **bb-hat-3.4**.
- **iOS:** removed the manual IP/token connection form — Bonjour + BLE
  autodiscovery replaces it.

### Fixed
- **RP2040 CMSIS-DAP SWD probe — PIO never drove GPIO16/18.** Three stacked firmware bugs prevented any output on the SWD connector pins: (1) `probe_config.h` included `board_pico_config.h` (SWCLK=2, SWDIO=3) instead of the BugBuster config (SWCLK=18, SWDIO=16) because `DEBUG_ON_PICO=1` is defined in CMakeLists — fixed by checking `BB_HAT_FIRMWARE` first; (2) `board_bugbuster_hat_config.h` was missing `#define PROBE_IO_RAW` and `#define PROBE_SM 0`, so `probe.h` never included `probe.pio.h` and `probe_program`/`probe_gpio_init` were undefined; (3) `probe_sm_init()` in `probe.pio` used `PROBE_PIN_OFFSET` (= 12 from upstream defaults) for `pio_sm_set_consecutive_pindirs` instead of the actual SWDIO/SWCLK pins — fixed by using explicit per-pin calls. After all three fixes, OpenOCD reads `SWD DPIDR 0x2BA01477` (Cortex-M4) and halts the STM32F429 target correctly.
- **GPIO 10/11 unreachable over HTTP (single-digit pin assumption).** `gpio_suffix()`
  in `webserver.cpp` used a hardcoded `p += 11` (skip `/api/gpio/` + one digit), so
  two-digit pins 10 and 11 fell through to `return NULL` → HTTP 400 (USB/BBP parsed
  them fine). Replaced with a digit-skipping loop. Fixes
  `test_gpio_all_pins_output_mode[http]`.
- **Bad JSON POST bodies returned HTTP 200 with an error payload instead of 400.**
  Body-required POST handlers that delegate to `api_core_handle` (channel function,
  dac, adc/config, vout/range, gpio/config, gpio/set, rtd/config) passed a NULL body
  from `recv_json_body` straight through, yielding `{"ok":false,...}` at HTTP 200.
  Added an `if (!body) return send_error(req, 400, "Invalid JSON")` guard to each.
  Fixes `test_bad_json_returns_400`.
- **`device.reset()` state not visible to the host on return (async SPI race).**
  Both reset paths (`handler_device_reset`, `api_device_reset`) enqueued the four
  HIGH_IMP channel writes + clear-alerts via the async command queue and returned
  before the SPI writes to the AD74416H committed. Added an internal
  `CMD_SYNC_BARRIER` queue command and `tasks_drain_command_queue()` helper; the
  reset handlers now block until the queue drains before responding. Fixes
  `test_reset_clears_state[usb]`.
- **Streaming reader thread crashed on Windows disconnect (pyserial ctypes race).**
  `_reader_loop` in `transport/usb.py` read `self._serial` without a local snapshot;
  when `disconnect()` NULLed it mid-read, Windows pyserial raised
  `TypeError: byref() argument must be a ctypes instance` and killed the reader
  thread, leaving subsequent streaming tests with zero events. Now snapshots
  `self._serial` at the top of the loop (breaks if `None`) and treats `TypeError`
  as a fatal read error. Fixes the ADC/scope streaming suite on Windows.
- **HAT cal-point export returned 0 points (frame-size overflow).** The HAT-UART
  frame payload is capped at `HAT_FRAME_MAX_LEN = 32` bytes on **both** MCUs, but
  the RP2040 `handle_calibrate_export` built a 244-byte response (48 points × 5 +
  4). `hat_build_frame` silently truncated it to 32 bytes while still reporting
  `n = 48`, so the ESP32 saw `4 + 48*5 > rsp_len` and rejected every export frame
  → `pointsCount: 0` for every rail even right after a successful sweep. Reduced
  the export chunk to **5 points/frame** (29 bytes, within the cap) and raised the
  ESP32 page-loop guard so a full 168-point table reassembles across ~34 frames.
  HAT firmware bumped to **bb-hat-3.5**. iOS now feeds HAT cal points into the
  same linearity chart + analysis used by the mainboard supplies.
  every channel's ADC range to `1` = ±12 V bipolar. For a single-ended VIN at
  0 V the AD74416H sits at code ≈ 0, which the bipolar conversion
  (`v_offset=-12 + code/FS·24`) maps to −12 V. Defaulted to `0` = 0–12 V
  unipolar (code 0 → 0 V); ±12 V remains selectable in the per-channel dropdown
  (`src/tabs/scope.rs`).
- **Desktop Scope never started (stalled on every channel).** The frontend
  invoked the `start_scope_stream` Tauri command with a snake_case `ch_mask`
  argument, but Tauri v2 expects camelCase `chMask`, so the command was rejected
  (`missing required key chMask`, `result=None`) and the backend stream task was
  never launched → 0 SPS → "Scope stalled / VIN can't be streamed". Added
  `#[serde(rename_all = "camelCase")]` to both `StartScopeArgs` in
  `src/tabs/scope.rs`. Also fixed a 20–30 Hz console-spam warning from a
  `canvas_ref.get()` read in the render loop (now `.get_untracked()`).
- **Scope socket starvation over HTTP.** `webserver.cpp` set
  `max_open_sockets = 7` while `sdkconfig.defaults` had already raised
  `CONFIG_LWIP_MAX_SOCKETS=16` "so the httpd can open max_open_sockets=10". With
  only 7, the scope SSE connection plus the desktop's concurrent status/overview/
  hat/rails polls overflowed the budget and `lru_purge_enable` evicted the scope
  connection (`httpd_sock_err send : 104` → scope mode exit → reconnect loop).
  Raised to the intended `10`.
- **Desktop HTTP scope now consumes the `/api/scope/stream` SSE** (the same path
  the on-device web UI uses) instead of 10 Hz `/api/scope` polling, so it enters
  firmware scope-sampling mode over a single long-lived connection
  (`http_transport.rs`).
- **BLE large API responses were truncated → iOS showed stale/empty data.** The
  BLE API tunnel (`ble_service.cpp`) ran the whole dispatch *and* blasted every
  MTU-sized notify frame on the NimBLE host task. For multi-frame responses
  (e.g. `/api/status` ~2.3 KB) the mbuf pool filled, `ble_gatts_notify_custom`
  returned `ENOMEM`, and the send bailed mid-stream — so the client never saw
  the final frame and timed out, falling back to defaults (channels showed
  `HIGH_IMP`, Overview blank). Now the request is dispatched on a worker task
  (off the host task, like the WiFi-provision path) and the chunked notify has
  backpressure (retry-on-`ENOMEM` with a short yield) so the full JSON always
  arrives. Single-frame writes/reads were unaffected, which is why controls
  worked but reads didn't.
- **Desktop HTTP scope showed 0 SPS / `httpd recv 104`.** The desktop polled
  `/api/scope` at 10 Hz with fresh connections, churning the ESP's small httpd
  socket pool. It now consumes the `/api/scope/stream` SSE (the same path the
  on-device web UI uses) over a single long-lived connection, which also makes
  the firmware enter scope-sampling mode (`http_transport.rs`).
- **IDAC `calPoly` shipped empty over HTTP/BLE JSON.** `/api/idac` and
  `/api/overview` built the `calPoly` array with `cJSON_AddNumberToObject(arr,
  NULL, ...)` (the object API), which silently no-ops on a JSON array — so the
  response carried `"polyValid":true,"calPoly":[]`. The desktop app then decoded
  4 zero coefficients, computed 0 V, and clamped to vMin (showed a flat 3 V for
  every IDAC code). Switched to `cJSON_AddItemToArray(arr, cJSON_CreateNumber())`
  in `webserver.cpp` (`handle_get_idac`) and `api_core.cpp` (`api_overview`).
  Pre-existing bug; only visible over HTTP/BLE (USB sends the poly as binary).
  The desktop HTTP transport now also treats a short/empty `calPoly` as
  poly-invalid and falls back to the linear code→voltage estimate.

### BLE control + unified `api_core` surface (ESP32-S3 + iOS) — NEW
The iOS app can now discover, pair with, and fully control the device over
**Bluetooth LE** in addition to WiFi, and the firmware serves one
transport-agnostic JSON API to both transports.
- **Unified `api_core`** (`Firmware/ESP32/src/net/api_core.cpp`): a single
  `api_core_handle(method, path, body)` dispatcher returns the canonical JSON for
  every REST endpoint. The HTTP webserver handlers and the BLE GATT tunnel both
  delegate to it, eliminating duplicated handler logic (firmware flash shrank as
  endpoints were de-duplicated). Streaming/binary surfaces (scope SSE, scripts
  WS, static files, OTA binary upload) intentionally remain HTTP-only.
- **BLE GATT tunnel** (`Firmware/ESP32/src/net/ble_service.cpp`, NimBLE):
  `BugBuster-XXYYZZ` peripheral with token auth and a chunked APIREQ/APIRESP JSON
  tunnel. Endpoints reachable over BLE include device/status/hat/usbpd/wifi,
  overview (live rails), GPIO config/set, IDAC voltage + calibration
  (read/point/clear/save, incl. HAT calibration data), HAT rail enable/voltage,
  IO-expander, USB-PD select, level-shift OE, soft device reset, selftest
  run/worker/calibrate, channel signal-path config (function/dac/adc-config/
  vout-range/rtd-config), QuickSetup list/apply/save/delete, and the on-device
  OTA git-release worker (status/releases/check/apply).
- **iOS BLE client** (`iOSApp/Sources/Services/BLETransport.swift`): CoreBluetooth
  scan/connect/auth and a chunk-reassembling JSON tunnel that decodes the **same**
  Codable models as the HTTP path, so every WiFi control is available over BLE.

### DAQ trigger / flag system (ESP32-S3 + ESP32-P4 + Desktop) — NEW
End-to-end trigger and flag support for the high-speed DAQ. The 12 mainboard IOs
(8 LV-digital, 4 HV/analog via the AD74416H) can each be tagged **Off / Flag /
Trigger**; flags drop a vertical marker line at the exact event time, triggers
start the capture window (with pre-trigger). Markers carry the fused-sample index
so they survive multi-resolution decimation untouched (flags preserved through
compression).
- **S3 IO event engine** (`Firmware/ESP32/src/dio/daq_trigger.{cpp,h}`): per-IO
  role/edge (rising/falling/any)/source; digital edges via the ESP32 GPIO poll,
  analog HV thresholds fed from the AD74416H ADC poll (DIN-comparator option);
  OR/AND trigger grouping. Hooked into the ADC poll task in `tasks.cpp`.
- **BBP `DAQ_TRIG` (0xCE)** sub-op multiplexed (`SET_IO`/`GET_IO`/`SET_LOGIC`/
  `ARM`/`STATUS`/`GET_ALL`) in `Firmware/ESP32/src/bbp/cmds/cmd_daq.cpp`; handled
  locally on the S3. Events forwarded to the P4 over the HAT link as
  `HAT_CMD_DAQ_MARK (0x5C)` / `DAQ_ARM (0x5B)`.
- **P4 stream**: new `USB_REC_MARKER (0x05)` (`usb_marker_payload_t`:
  sample_index/timestamp_us/channel/edge/kind) and `USB_CMD_ARM (0x88)`; the P4
  stamps each marker with the live fused-sample index. Precise alignment uses the
  shared bidirectional IRQ line as a hardware event-capture edge.
- **Desktop**: `REC_MARKER` decode (`daq_proto.rs`), non-decimated marker list in
  `daq_store.rs`, Tauri commands `daq_set_io_role`/`daq_set_trig_logic`/`daq_arm`/
  `daq_get_trig_state`/`daq_get_markers`. New **Trigger / Flag panel**
  (`src/tabs/daq_trigger_panel.rs`): 4×3 IO-block layout, HV/LV badges, per-IO
  edge + analog-threshold slider, VLOGIC slider, OR/AND + arm controls. Markers
  render as **pink flag lines / cyan trigger lines** in the plot and timeline.
  The mock DAQ generator emits synthetic flags/triggers for offline preview.
  The FFT, Settings and Trigger side-panels were harmonized to one card/segment
  design; the SMU "Output enabled" is now a glowing red/green toggle.
- **Python / MCP**: `bb.daq_trigger` accessor (`python/bugbuster/daq_config.py`,
  `CmdId.DAQ_TRIG = 0xCE`) with `set_io`/`get_io`/`set_logic`/`arm`/`status`/
  `get_all`; MCP tools `daq_set_io_role`, `daq_set_trigger_logic`, `daq_arm`,
  `daq_get_trigger_state`. BBP wire history → 3.4 (see `Firmware/bbp-protocol.md` §6.20).

### BLE control plane (ESP32-S3 + iOS) — NEW (in progress)
A low-rate Bluetooth LE control plane on the ESP32-S3 mainboard so a phone can
configure and monitor the device without first being on the LAN. NimBLE
peripheral, **single connection**, peripheral-only, bonding/LE-Secure-Connections;
deliberately not a streaming path (scope/LA/DAQ stay on WiFi/USB).
- **GATT service** (`Firmware/ESP32/src/net/ble_service.{cpp,h}`), advertised as
  `BugBuster-XXYYZZ`. Characteristics: Device Info (read identity JSON), Auth
  (write admin token — same token as HTTP; latches per-connection and gates all
  control writes/reads), WiFi Provision (write `{ssid,password}` → async
  `wifi_connect` on a worker task → notify `{ok,ip}`), Supply Control (write IDAC
  voltage / HAT-rail enable+voltage, reusing the existing driver calls), and
  Sensor Snapshot (read compact JSON: die temp, per-channel ADC, USB-PD, HAT
  rails). Wired into `app_main`; `bt` added to the component build.
- **GATT contract** (for the iOS client): 128-bit UUID base
  `427567XX-7573-7465-7200-757374657200`, where `XX` selects the attribute —
  `00` service, `01` Device Info, `02` Auth, `10` WiFi Provision, `11` Supply
  Control, `12` Sensor Snapshot, `20` API Request, `21` API Response. Advertised
  name `BugBuster-XXYYZZ` (last 3 MAC bytes). Pairing uses LE Secure Connections
  bonding; the admin token must be written to the Auth characteristic before any
  control write/read or API request is honoured (per-connection latch).
- **Generic API tunnel** (`Firmware/ESP32/src/net/ble_api.{cpp,h}` + API
  Request/Response characteristics): write a `{id,path,body}` request and receive
  the JSON response as chunked notifications (`[reqId][seq][last]` framed,
  MTU-sized), so the iOS app can issue its full small-data request surface over
  BLE without a characteristic per endpoint. Dispatcher covers `/api/device/info`,
  `/api/status`, `/api/hat`, `/api/usbpd`, `/api/wifi`, `/api/daq`,
  `/api/idac/voltage`, `/api/hat/rail/enable`, `/api/hat/rail/voltage`, and is
  trivially extensible. (Scope/LA/DAQ-waveform streaming stays on WiFi/USB.)
- **DAQ HAT scaffolding**: `/api/daq` returns HAT presence/type, firmware, and
  shared rail state today, plus a `power` block (I/V/P/energy) flagged
  `available:false` — the P4's high-rate power stream is on its own USB-HS port
  and not yet bridged to the S3 over the HAT UART. The BLE/JSON contract is in
  place so the iOS client can be built now and the fields populated when the
  P4→S3 telemetry path lands.
- **MicroPython clean-build fix** (required once Bluetooth forces a full
  reconfigure): replaced the upstream `ports/esp32/modmachine.c` includefile with
  a trimmed, component-local `modmachine_bugbuster.c` (survives micropython
  submodule resets) exposing only `machine.Pin/reset/freq/unique_id/idle/sleep`;
  guarded the `mphalport.h`/`machine_pin.c` gpio stubs to the qstr pass and added
  the `esp_driver_gpio` include path. `machine.I2C` / `machine.SPI` remain exposed
  (aliased to the SoftI2C/SoftSPI implementations in `modmachine_bugbuster.c`), so
  scripts using either the `machine.*` or `bugbuster.*` bus classes keep working;
  only the never-implemented `Timer`/`RTC`/`TouchPad` machine classes are dropped.
- Status: firmware builds clean (Flash ~49%, NimBLE ~+230 KB). Pending on-device
  WiFi+BLE coexistence/heap verification and the iOS CoreBluetooth transport.

### iOS app — BLE transport (NEW)
The iOS app now mirrors its Wi-Fi control plane over Bluetooth LE, talking to the
ESP32-S3 NimBLE peripheral above. The phone can discover, pair, and control a
BugBuster without first being on the LAN; the same admin token gates both paths.
- **CoreBluetooth client** (`iOSApp/Sources/Services/BLETransport.swift`): scans
  for the BugBuster service UUID, connects, discovers the GATT layout, reads the
  pre-auth Device Info characteristic, writes the admin token to the Auth
  characteristic, and drives the generic API tunnel — writing `{id,path,body}`
  requests and reassembling the chunked `[reqId][seq][last]` notification frames
  back into JSON. Also exposes the Sensor, Supply, and WiFi-provision
  characteristics. GATT operations are serialised and the device-discovery list
  is published for the UI.
- **ConnectionManager integration** (`iOSApp/Sources/Services/ConnectionManager.swift`):
  added a `TransportKind` (`wifi`/`ble`) and a `connectBLE(_:token:)` flow that
  resolves the per-MAC saved token (same store as Wi-Fi), authenticates, and
  starts a BLE poll loop. `postAction` and the quick-refresh helpers route through
  the BLE API tunnel when connected over BLE; the compact BLE JSON for
  `/api/status`, `/api/hat`, `/api/usbpd`, `/api/wifi` is adapted onto the existing
  view models so the UI is unchanged. Endpoints the firmware tunnel doesn't expose
  degrade gracefully.
- **Discovery UI** (`iOSApp/Sources/Views/MainTabView.swift`): a new "Bluetooth
  Devices" card on the connection screen lists nearby peripherals (triggering the
  CoreBluetooth permission prompt) and connects on tap, alongside the existing
  Bonjour/Wi-Fi list.
- **Build**: added `CoreBluetooth.framework` and `NSBluetoothAlwaysUsageDescription`
  /`NSBluetoothPeripheralUsageDescription` to `project.yml`/`Info.plist`. Builds
  clean for the iOS Simulator (deployment target iOS 26.0).
- **BLE control/read surface** (firmware `ble_api.cpp` tunnel + iOS routing).
  SUPPORTED over BLE: device info, status, HAT + rails, USB-PD, WiFi, DAQ,
  coalesced Overview (IDAC + IOExp enables/efuses), GPIO list; and writes for
  IDAC voltage, HAT rail enable/voltage, IOExp control (vadj/15v/mux/usb/efuse),
  USB-PD select, level-shifter OE, GPIO config/set, and device reset.
  NOT supported over BLE (USB/WiFi only): scope/LA/DAQ streaming, scripts/REPL,
  IDAC calibration, channel signal-path config, QuickSetup presets, OTA, and
  live supply-rail voltage measurement (rails shown as not-measured).
- **Fixes found during bring-up**: (1) firmware BLE init failed
  (`gatts_count_cfg rc=3`) because the notify-only API-response characteristic had
  a NULL `access_cb` — ESP-IDF NimBLE rejects that; added a stub callback.
  (2) iOS GATT UUIDs were wrong — NimBLE stores `BB_UUID128` little-endian, so the
  device advertises `42756742-7573-7465-7200-0000000000<disc>`; corrected all
  eight. (3) BLE auth/control no longer falls back to the Wi-Fi/`localhost` HTTP
  path. (4) `/api/gpio` decodes as a bare array (was `{gpios:[…]}`, which never
  matched). (5) NimBLE host log level dropped to WARN to stop notify-spam.
- **Unified API core (`Firmware/ESP32/src/net/api_core.{h,cpp}`)** — began folding
  the HTTP and BLE control planes onto one transport-agnostic dispatcher
  `api_core_handle(method, path, body) -> JSON`, so the two surfaces share a
  single implementation and can't drift (the root cause of the `/v2/rail` and
  GPIO-shape mismatches found during bring-up). The BLE tunnel now routes
  through it; `/api/overview` is the first HTTP endpoint converted to delegate
  (its duplicated handler body removed — firmware flash shrank). `update_manager`
  (returns cJSON, already used by both transports) is the model. Remaining
  endpoints migrate in green batches; streaming/binary/WS surfaces stay
  transport-specific.
- **BLE control/read surface (expanded)**: added over BLE — IDAC cal points
  WITH HAT calibration data (`/api/idac/cal/points` → `hat` block via
  `hat_calibrate_status`), live supply-rail measurement in `/api/overview`, and
  the on-device git-release OTA updater (`/api/ota/{status,releases,check,apply}`,
  small JSON — no binary transfer over BLE).

### DAQ HAT (ESP32-P4 IoT Power Analyzer) — NEW
A new ESP32-P4-based DAQ HAT firmware that implements a 24-bit, nA–3A
seamless-autoranging precision **power analyzer** + **source-measure unit**
(Joulescope / Otii / Nordic PPK2 class). Full device documentation in
`Firmware/DAQ_HAT/README.md`; architecture in `Docs/power-analyzer.md`;
netlist-verified hardware map in `Docs/daq-hat-hardware.md`.

- **ADAQ7769-1 driver suite**: complete register map (`adaq7769_regs.h`), low-level
  SPI Mode-3 transport with the part's CRC-8 (`adaq7769_ll`), high-level HAL
  (`adaq7769`) covering clock/power modes, every digital-filter type (Sinc5,
  Sinc3, wideband FIR) and decimation rate with an ODR solver, PGA gain via the
  chip's own GPIOs, AAF input scaling, reference buffers, conversion modes,
  offset/gain calibration, sync, and diagnostics — plus a DRDY-ISR → capture-task
  → PSRAM ring streaming layer (`adaq7769_stream`). 3 ADAQ across 2 SPI buses
  (one dedicated FINE, one shared COARSE + VOLTAGE).
- **Seamless current auto-ranging**: `range_manager` observes the analog SR-latch
  autorange loop (51/2/0.05 Ω bypass lines), can override it, drives the FINE
  input mux, and carries per-range calibration. `current_fusion` stitches the
  FINE and COARSE ADCs into one calibrated, gap-free `i[n]` with a post-switch
  settle blackout (lean on COARSE), COARSE→FINE cross-fade, and boundary
  hysteresis.
- **On-device DSP**: `power_dsp` computes `p = v·i`, accumulates energy (mWh/J)
  and charge (mAh/C) in double-precision (trapezoidal), and tracks
  min/max/mean/RMS/std for I, V, P. `multires` provides Joulescope-style
  multi-resolution min/max/mean zoom tiers. `spectrum` runs a continuous Welch
  power spectrum via esp-dsp (64–4096-pt real FFT, Hann/Blackman-Harris, 50%
  overlap) on current or power.
- **Programmable source / SMU**: `smu` drives the LTM8056 DUT buck-boost via the
  DS4424 IDAC — 0–20 V output and ≤2.5 A current limit — and reads the LTM8056
  input/output current monitors on the ESP32-P4 ADC1.
- **USB High-Speed measurement stream**: a CRC-16-framed vendor-bulk protocol
  (`usb_proto`) with `WAVEFORM`/`STATS`/`ENERGY`/`FFT`/`MARKER`/`STATUS` records
  and `START`/`STOP`/`SET_RATE`/`RANGE_LOCK`/`RESET_*`/`FFT_CONFIG`/`SET_SOURCE`
  control commands. Transport-agnostic framer (`usb_stream`) bound to the
  ESP32-P4 USB-HS OTG via a TinyUSB vendor backend (`usb_backend`).
- **ESP32-S3 mainboard link**: `s3_link` implements the existing BugBuster HAT
  wire protocol (SYNC 0xAA / LEN / CMD / payload / CRC-8) as a slave, so the S3
  drives the DAQ HAT exactly as it drove the RP2040 HAT and dynamically loads the
  DAQ resource set at boot (`HAT_TYPE_DAQ_POWER`, no hotswap). Adds DAQ control
  commands plus a pre/post-acquisition sync epoch for digital-marker correlation.
- **Firmware versioning + robust split OTA**: central `version.h` (semantic
  version + product id); dual-slot A/B partition table (`partitions.csv`);
  streaming OTA (`ota`) that writes each chunk straight to flash (no full-image
  staging — the S3 only relays), with offset-checked resumable transfer,
  product-id + size guards, full-image SHA-256 verification, and rollback-safe
  `PENDING_VERIFY` self-confirmation.
- **Supporting drivers**: `ad741x` (AD7415 I²C temperature sensors), `ds4424_p4`
  (DS4424 quad IDAC port). Board integration (`daq_board`) wires the full
  acquisition → fusion → DSP → USB pipeline and routes S3/USB control commands.
- Builds clean for ESP-IDF 5.5 on the ESP32-P4 (RAM ~24.7%, Flash ~2.6%).

### Notebooks
- **RP2040 firmware build notebook — OTA flash section**: Added section 3 "OTA Flash via ESP32 (WiFi)" with three cells: (A) auto-discovery via mDNS/zeroconf with subnet-scan fallback probing `/api/device/info`, (B) binary upload to `POST /api/ota/upload_rp2040` with KB/s rate display, (C) flash trigger via `POST /api/update/apply` with live progress bar polling `GET /api/update/status` (`state`/`step`/`progressDone`/`progressTotal`). Existing picotool and HAT-demo sections renumbered to 4 and 5.

### Desktop App
- **HS DAQ power-analyzer filter refactor (performance + correctness)**: Reworked how the current/voltage/power display filters (Avg / EMA / Median / HPF + window) are applied to the multi-resolution min/max "pyramid" data.
  - **Stop reprocessing an unchanged viewport**: `refresh_view()` now dedupes on the view key (`view_start`, `view_end`, plus the backend filter params when hi-res is on), so the polling loop no longer re-decimates + re-filters the same viewport ~30×/s while paused or idle. (`src/tabs/daq.rs`)
  - **Envelope-aware filtering (fixes nonsensical output)**: filters now operate on the column *midline* `(min+max)/2` with the band half-width preserved/handled separately, instead of filtering the `min` and `max` envelopes independently (which collapsed the band and, for HPF, drove both edges to zero). (`src-tauri/src/daq_store.rs`, `src/tabs/daq.rs`)
  - **New "Hi-res" toggle** in the FILTERS panel (`raw_filter`, default on; auto-disabled when filter type is Off): when on, the backend filters the **raw signal before min/max decimation** in the raw path (zoom-stable, undistorted — smooths the signal, not the envelope band; dI/dt and source tint stay on the raw signal); when off, a lightweight filter is applied to the ~1800-column decimated envelope in the frontend (cheaper, no backend round-trip). The window is interpreted as a fixed real timescale in samples, so zoom no longer changes the filter strength. (`src-tauri/src/daq_store.rs` `get_view`, `src/tabs/daq.rs` `SettingsPanel`)

### iOS App
- **ADC Channel C greying when self-test active**: In the Overview, Scope, and Signal Path tabs, ADC channel C (index 2 / BLOCK 3 / CH3) is now greyed out and disabled when the self-test worker thread is enabled, preventing conflicting configurations.
- **Supply tab reorganization**: each voltage slider is now paired with its enable toggle directly below it (VADJ1, VADJ2, HAT rails), matching the visual pattern for both main-board and HAT sections
- **HAT voltage slider — always shows DS4424 target**: RP2040 now includes `target_mv` (the persisted DS4424 set-point from flash) in every `HAT_RSP_RAIL_STATUS` response; ESP32 parses and forwards it as `targetVoltageMv` in `GET /api/hat/v2/rails`; iOS `HatRail.configuredVoltageMv` prefers `targetVoltageMv` over measured `voltageMv` for slider init — works correctly even after RP2040 reboot since the target is restored from flash
- **HAT VADJ3/VADJ4 boot default**: first-boot default changed from 3.3V to 12V, persisted to flash immediately; rails remain disabled until explicitly enabled
- **Removed controls**: "Enable Analog 15V", "Enable Signal Mux", and "Enable USB Hub Controller" toggles removed from the supply tab (not user-facing)
- **Fixed duplicate `onAppear`**: removed duplicate quicksetups load call; HAT sliders now also sync on first appear

### ESP32 Firmware
- **Self-test supply rail monitoring fix**: Restored unipolar ADC range (`ADC_RNG_0_12V`) for U23 supply measurements — the bipolar workaround is no longer needed after the hardware PCB trace fix on S4 (ADC_OUT_4). Commented out the `SelftestSupplyDebug` diagnostic struct, its recording functions, and the debug JSON block in `GET /api/selftest/supply/{rail}` (kept as comments for future use).
- **VADJ load compensation**: Added `VADJ_LOAD_COMPENSATION` factor (1.0272) in `config.h` to correct the ~2.7% voltage lift on VADJ1/VADJ2 caused by the U23 measurement divider (R107/R109, R108/R110) loading the regulator feedback node. The correction is applied only in the formula-based fallback path of `ds4424_voltage_to_code()` — when calibration data is present, the cal table handles the mapping directly without extra correction.
- **Auto-calibration 10× speedup**: Reduced per-point measurement from ~8.2 s to ~0.7 s by switching from the 5-sample-median `measure_supply_stable_for_cal()` to a single fast-path `selftest_measure_supply(fast=true)` during calibration sweeps. The MUX path stays constant between points, and the DCDC loop settles within ~5 ms for a 35 mV DAC step, making the full MUX open/close/settle cycle unnecessary. DAC settle reduced from 200 ms to 100 ms (20× margin over the DCDC loop BW), midpoint soak from 10 s to 3 s. Total calibration time drops from ~5 min to ~1.3 min per channel.

### ESP32 Web UI
- **Supply monitor overlay on correct channel**: Fixed the on-device web UI to grey out and disable Channel C (logical index 2, physical AD74416H D) instead of Channel D (logical index 3) when the self-test supply monitor worker is active. Applies to Overview, Analog (ADC/VDAC/IIN cards), and Signal Path tabs. Channel C sparkline now stops updating (pushes NaN) while the monitor is active, preventing stale histogram data.

## [1.4.1] — 2026-06-12

### Added
- **MicroPython device bundle expansion**: Added frozen OneWire support plus `bb_devices` wrappers for DS18B20, DS3231, BH1750, AHT20, SHT31, BME280, INA219, ADS1115, MCP23017, PCF8574, PCA9685, TCS34725, MPU6050, MCP4725, and SSD1306. `MCP23017` now also exposes pin-level helpers for direction and pull-ups (`pin_mode()`, `set_dir()`, `set_pullup()`) so scripts can read like `pinMode()` / `digitalWrite()` examples instead of raw bitmask manipulation. The MicroPython I2C examples now power the rail and logic voltage explicitly, the MicroPython `I2C` object now exposes a shared-controller `close()` method, and the planner now rejects a second active I2C setup on a different pin pair with a clear error instead of silently stealing the only free controller. Updated the on-device Scripts tab docs/autocomplete and expanded the MicroPython example gallery to match.
- **MicroPython SPI bus close & active-use verification**: Added `bugbuster.SPI` close support (`spi.close()`) to release the shared external SPI controller. Added safety active-use checks in the bus planner, which rejects a second active SPI setup on different pins with a clear error instead of silently stealing the controller. Enabled standard `machine.SPI` and `machine.SoftSPI` mapping to the software SPI implementation in the MicroPython core to ensure parity with `machine.SoftI2C` and to prevent missing driver symbols. Updated the Web UI autocomplete and API documentation to cover `SPI.close()`.


### Fixed
- **MicroPython ESP32-S3 link & compile fix**: Guarded the unsupported machine submodules (`Timer`, `RTC`, `TouchPad`) in `modmachine.c` behind preprocessor checks, and disabled them (`DHT`, `Pulse`, `Bitstream`, `Signal`, `Memx`, `Timer`, `RTC`, `TouchPad`) in `mpconfigport.h` and the CMake config targets to avoid unresolved linker symbols. Implemented a minimal custom `machine_pin.c` mapping to HAL GPIO functions and implementing the virtual pin protocol (`ioctl`) to support OneWire/DHT, and mapped `machine.I2C` to the soft I2C protocol in `extmod/machine_i2c.c`. Verified clean build compiles successfully under PlatformIO and passes all 417 unit + 235 simulator tests.
- **iOS glass-shell regression rollback & polish**: Reverted the experimental glass shell refactor in `MainTabView.swift`/`OverviewTab.swift` that removed the tab bar's container pill (making tab labels unreadable over scrolling content), un-pinned the Bench Overview header, dropped the blue/green/yellow/purple channel identity colors, replaced the `safeAreaInset` shell with a `GeometryReader`/`ignoresSafeArea` layout (compressing Scope and breaking REPL keyboard avoidance), and lost the MAC-keyed saved-token restore. On top of the restored layout: removed the full-width `glassEffect` rectangle that drew a visible slab behind the floating tab bar pill, replaced the pinned header's rounded backplate card with an edge-free top gradient scrim, and reduced channel-card glass tint to ~30% accent so white text stays readable on dark tiles. Overview sparkline/histogram retained.
- **iOS shell round 2 (device screenshot feedback)**: Fixed the rectangular slab behind the floating tab bar and `premiumGlassCard` surfaces — `.background(.ultraThinMaterial)` applied to a `RoundedRectangle` fills its rectangular bounding box, so both now use `.fill(.ultraThinMaterial)` with the dark tint overlaid inside the shape. Lowered the tab bar (removed extra bottom padding above the home indicator). Restored `.ignoresSafeArea(.keyboard)` on the connected shell, which ScriptsTab's manual keyboard tracking was designed around: this stops the tab bar from riding up above the keyboard and removes the double keyboard avoidance that crushed the REPL console to ~50pt. Scope now fills the screen (removed the stale 90pt bottom clearance left over from the pre-`safeAreaInset` tab bar, trimmed header/graph padding). REPL header no longer draws an edge-to-edge `Rectangle()` glass slab, and REPL/editor keyboard clearance was recalibrated (`keyboardHeight − 66`, resting padding 90 → 10). Channel-card glass tint further reduced to `accent.opacity(0.12)` since `glassEffect` tint renders far more saturated on device.
- **iOS shell round 3**: Overview pinned header now has a real blur backdrop (`ultraThinMaterial` + dark wash, alpha-masked to fade out at the bottom edge so no box line appears) for readability over scrolled content. Removed stale bottom spacers/padding that left dead space above the tab bar in Overview (`Spacer(minLength: 100)`), Diagnostics (`Spacer(minLength: 100)`), and Signal Path (`Spacer(minLength: 90)`). Diagnostics SPIFFS storage card now retries up to 3× (3s apart) instead of failing silently forever when the first `/api/scripts/storage` fetch races a busy/wedged device. Background poller re-checks `ScopeStreamManager.isStreaming` immediately before the slow-cycle overview/selftest/hat fetches to close the 250ms race that could fire 4 sequential requests into the ESP32 httpd right as an SSE scope stream starts.
- **Bipolar ±12V Range Support**: Restored support for the `±12V` bipolar range on Voltage Input (`CH_FUNC_VIN`) channels in the ESP32 firmware by removing the safety clamp override in `tasks.cpp` and initializing the default Scope tab range back to `1u8` (`±12V`). This resolves the Scope tab streaming freeze when negative voltages are applied.
- **Logic Analyzer blank waveform view**: Guarded the desktop LA tab against zero-width canvas measurements before requesting view data, and made the LA store treat `max_points=0` as an un-decimated viewport instead of dividing by zero. This fixes the case where streaming/capture succeeds but the waveform panel stays empty.

## [1.4.0] — 2026-06-12

### Added
- **iOS Oscilloscope Scope Tab**: Added a new "Scope" tab (waveform icon) to the iOS app bottom navigation. It features a real-time waveform display rendering active channels with distinct colors, a configuration settings modal sheet (for selecting MUX inputs, voltage ranges, and sample rates per channel), pinch-to-zoom scaling for the timebase using magnification gestures, and auto-fit Y-axis scaling. Implemented URLSession response disposition handling to enable chunked stream delivery, added Play/Pause toolbar buttons, and added connection error state overlays with retry flows.
- **iOS Oscilloscope sample counter**: Displays the count of all received samples next to the streaming state in the header.
- **iOS Oscilloscope touch cursor & tooltip**: Added interactive single-finger touch/drag gesture tracking on the graph displaying a vertical dashed cursor line, a highlighted dot on the closest channel, and a floating voltage tooltip bubble clamped inside the graph bounds.
- **Scope ADC mode (BBP proto v8 → v9)**: while the desktop scope stream is active, the firmware now drops the 4 always-on ADC diagnostic conversion slots (each 20 SPS, ~200 ms per sequencer pass) and converts only the channels enabled on the scope, raising the effective per-channel sample rate from ~5 SPS to the configured channel rate. `BBP_CMD_START_SCOPE_STREAM` (0x62) accepts an optional 1-byte logical channel mask (0/absent → all channels); stopping the stream restores diagnostics and all active channels on every stop path. Supply-monitor safety interlock keeps CH D converting regardless of the scope mask. Desktop scope passes its channel-enable toggles as the mask, restarts the stream on toggle changes, and shows a "Diagnostics paused during capture" badge; Python `on_scope_data()` gains a `ch_mask` parameter. New `tasks_rebuild_adc_conv_ctrl()` helper also keeps the restricted mask/diag-off state across ADC rate changes, shrinking the SPI bus-mutex hold that previously froze the wavegen DAC output for up to ~700 ms when changing the ADC sample rate.
- iOS app: MicroPython Scripts tab (🐍) with three sub-panels — File Manager (list/create/edit/delete/run scripts, storage quota bar), Editor (glass monospace textarea, dirty indicator, Save/Run/Stop/Reset VM), and REPL (WebSocket live terminal with Connect/Disconnect, log polling fallback, auto-scroll). Backend adds 13 new Tauri commands covering the ESP32 `/api/scripts/*` HTTP REST API and WebSocket REPL proxy via `tokio-tungstenite`.
- **MicroPython on-device syntax linter**: Added a new `POST /api/scripts/lint` HTTP endpoint to the ESP32 firmware that compiles Python strings using the MicroPython compiler in a thread-safe context inside the `uPython` FreeRTOS task, returning syntax correctness and detailed compilation tracebacks without executing the script.
- **iOS custom code editor & selectable console**: Replaced SwiftUI `TextEditor` and `TextSelection` views in `ScriptsTab` with native UIKit `UITextView` wrappers (`SelectableCodeEditor` and `SelectableConsoleView`). This fixes iOS copy-paste pasteboard hangs, disables smart quote/dash conversions, intercepts hardware/accessory Tab keys to insert 4 spaces, and introduces a horizontally scrollable custom-styled keyboard accessory bar.
- **iOS script editor "Check" tool**: Added a "Check" button and syntax success/error badge directly above the script editor on iOS to trigger the on-device linter.

### Fixed
- **ESP32 SPI bus lock optimization**: Released the recursive SPI bus mutex during the `ADC_BUSY` check wait inside `startAdcConversion` in `ad74416h.cpp`, preventing socket and SPI bus starvation when modifying channel functions or VOUT/DAC voltages.
- **ESP32 true 20+ SPS polling rate**: Configured `adcRateToPollMs()` in `tasks.cpp` to poll at approximately twice the conversion rate (twice Nyquist) relative to the hardware period (e.g., 20 ms for 20 SPS), resolving Nyquist alignment misses that capped the scope updates to 10 SPS.
- **iOS Snappy VOUT/Channel Settings**: Updated `saveSettings()` in `OverviewTab.swift` to close the settings sheet instantly, executing sequential POST calls asynchronously in the background instead of freezing the UI.
- **iOS Scope background persistence & disabled by default**: Promoted `ScopeStreamManager` to a singleton to persist background streaming when switching tabs, and removed auto-start/stop on view lifecycle to start disabled by default.
- **iOS connection timeouts during streaming**: Increased `httpMaximumConnectionsPerHost` to 5 in `ConnectionManager` and paused diagnostics polling while the scope stream is active to prevent socket starvation and configuration timeouts.
- **iOS Scope negative array index bounds check**: resolved a fatal Swift/Array index out-of-range crash in `ScopeTab.swift` by enforcing bounds checks (`max(0, ...)`) on the waveform display slicing range when the rolling sample buffer contains fewer than 10 samples.
- **ESP32 httpd stack overflow coredump**: dynamically heap-allocated the 1.7 KB `snapshot` array inside `handle_get_scope_stream` in `webserver.cpp` instead of using the stack. This reduces the stack usage of the `httpd` worker task by ~40%, preventing stack overflows when the SSE scope stream is active under the default 4 KB stack configuration.
- **iOS Signal Path I/O layout ordering**: Reordered the block GPIO indicators from reverse sequence (`IO3, IO2, IO1`) to ascending sequence (`IO1, IO2, IO3`), aligning the physical board ADC/External routing capabilities with the primary `IO1`/`IO4`/`IO7`/`IO10` channel indexes in the UI.
- **ESP32 wavegen logical-channel routing**: Waveform generation now keeps host/app channel numbers logical and remaps only at the final AD74416H HAL write boundary, so logical CH C/CH D drive the physically swapped board channels correctly. No BBP or HTTP payload shape changed.
- **ESP32 DAC range-change and ADC readback recovery**: VOUT range changes now follow the AD74416H datasheet sequence: put the channel in `HIGH_IMP`, program `VOUT_RANGE` plus the equivalent DAC code, re-enable VOUT, and restart ADC readback with a matching `0..12V` or `±12V` ADC range. This prevents `VIOUT_SHUTDOWN`/`ADC_ERR` after switching to `±12V`.
- **ESP32 reset and ADC sequencing edge cases**: Device reset now stops any active wavegen on both BBP and HTTP paths; `enableAdcChannel()` restores the previous conversion mode instead of forcing continuous conversions; ADC stream/DSP pushes are gated on `ADC_DATA_RDY` to avoid duplicate samples.
- **ESP32 HTTP API hardening**: `POST /api/hat/detect` now requires the existing admin token, `POST /api/channel/N/adc/config` validates ADC enum values, and `POST /api/wavegen/start` rejects invalid channel/waveform/mode/amplitude/offset values before caching state.
- **On-device scope stream performance**: SSE and WebSocket scope streams now enter refcounted scope ADC mode, and SSE snapshots scope buckets under `g_stateMutex` before building JSON so heap work no longer stalls ADC/wavegen state access.
- **Python HTTP wavegen channel key**: `BugBuster.start_waveform()` now sends the canonical `channel` JSON key over HTTP and validates wavegen/ADC enum arguments before dispatch.
- **Desktop WaveGen IO ownership**: opening the WaveGen tab no longer claims all four analog logical channel slots. The tab now claims only the selected logical channel when the generator starts, releases that slot on stop/start failure, and the Tauri keep-alive set records only slots the device actually granted.
- **Desktop digital IO ownership**: opening GPIO, DIN, or DOUT no longer claims IO1-IO12 up front. GPIO writes now claim only the edited IO slot, DIN/DOUT writes claim only the edited logical channel slot, and Tauri keep-alive renewals prune slots the device rejects so stale leases do not keep showing startup blockers.
- **Desktop analog tab ownership**: opening ADC, VDAC, Scope, Signal Path, or Voltages no longer claims CH0-CH3 up front. These tabs are view-only on entry, so another interface holding an analog channel no longer blocks basic navigation.
- **eFuse status updates**: Added the `efuses` array to the `/api/overview` endpoint response on the ESP32 web server firmware and merged it in the iOS App's `ConnectionManager` to keep the eFuse ON/OFF switches in sync.
- **eFuse physical output synchronization**: Updated the ESP32 PCA9535 driver to read output registers `PCA9535_REG_OUTPUT0` and `PCA9535_REG_OUTPUT1` from the physical I/O expander during `pca9535_update()`, ensuring the eFuse status cache matches physical device pin configurations.
- **REPL double prompt**: Changed REPL command submission to append `\r` instead of `\r\n` to prevent extra empty `>>> ` prompts in MicroPython.
- **WebSocket REPL connection timing**: Fixed a race condition in `WebSocketREPL.swift` where state checks aborted the read loop immediately after connection init.
- **VADJ rails range**: Expanded the HAT VADJ3 and VADJ4 sliders on the iOS App's Overview tab to cover `0.0V` to `36.0V` instead of `1.7...5.0V`.
- Desktop app update check now detects newer stable releases for nightly users (e.g. `1.1.2-25` correctly sees `1.1.3` available via manual stable-manifest fallback when tauri-plugin-updater returns no update)
- `published_at` deserialization no longer crashes on GitHub releases with `null` date (`#[serde(default)]` + safe `.get(..10)` slice in browse-releases list)

### Improved
- **Token persistence & Auto-connect on iOS**: Loaded and pre-filled the token and manual IP from `UserDefaults` on startup, and automatically reused saved pairing tokens when discovering devices on new IPs.
- **REPL quote sanitization and Copy support**: Configured keyboard type to `.asciiCapable` and attached on-change handlers to sanitize smart quotes/dashes to ASCII equivalents. Added a copy button to copy the entire console log output.
- OTA upload progress bar animates with a green shimmer during HTTP upload stages instead of freezing at 40% (SPIFFS) or 90% (ESP32); backend emits intermediate progress ticks every 250 ms via `http_upload_with_progress` helper
- iOS app device scan is now ~5× faster: HTTP probe timeout reduced from 800 ms to 300 ms and concurrent chunk size increased from 30 to 64 hosts, bringing worst-case scan time from ~30 s down to ~5 s per subnet

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
- **`Docs/simulated-device.md`** — full reference for the BugBuster Python simulator: handler coverage table (144 commands across 15 files), test file → feature matrix, state field inventory, known gaps, and a reliability improvement roadmap.
- **`Docs/memory-testing.md`** — ESP32-S3 memory safety guide: SRAM layout diagram, pre-ship checklist (binary gate, stack HWM, heap fragmentation, canary), common failure modes and fixes, and CI integration notes.

### Fixed

#### ESP32 firmware
- **Removed unused `ADMIN_TOKEN` fallback**: The static `#define ADMIN_TOKEN "000..."` in `config.h` was a Phase-1 placeholder never referenced by any code path — the live token always comes from `auth_get_admin_token()`. Removed to eliminate dead-code confusion. (`config.h`)

#### Python client / simulator
- **`BBP_CMD_PCA_GET_STATUS` (0xB0) payload truncation**: `_parse_pca_status()` previously read only 12 bytes, silently dropping the 9 trailing enable fields that firmware has always written (`vadj1_en`, `vadj2_en`, `en_15v`, `en_mux`, `en_usb_hub`, `efuse_en[0..3]`). Parser now reads all 21 bytes and exposes the enable flags in the returned dict; older firmware returning 12 bytes degrades gracefully. (`python/bugbuster/client.py`)
- **Simulator `PCA_GET_STATUS` returns full 21-byte payload**: `_pca_get_status` handler in `tests/mock/handlers/power.py` now appends the 9 enable bytes, keyed by the `PcaControl` integer enum (0–8), so round-trip tests against the simulator match the real firmware. (`tests/mock/handlers/power.py`)
- **0x07 self-test alignment**: `SELFTEST_SUPPLY_VOLTAGES_CACHED` is now the canonical BBP name for command `0x07`; the Python client exposes `selftest_supplies_cached()` and keeps `selftest_efuse_currents()` as a compatibility alias. The simulator and HTTP mock now return cached rail voltages from `/api/selftest/supplies/cached`, and the MCP discovery tool reports the cached supply snapshot under `supply_voltages_cached`. (`python/bugbuster/constants.py`, `python/bugbuster/client.py`, `tests/mock/*`, `python/bugbuster_mcp/tools/discovery.py`)

#### Desktop app
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
- **WIFI_FORGET command (0x0A)**: New BBP command erases saved STA credentials (`sta_ssid` / `sta_pass`) from NVS and disconnects. Implemented in `wifi_manager.cpp` (`wifi_forget_credentials()`), registered in `cmd_wifi.cpp`, documented in `bbp-protocol.md`. Returns a bool indicating NVS erase success.
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
- Docs: stale version badge in `README.md`; `release-checklist.md`
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
- New `Docs/quick-setup.md` — user guide for the 4-slot NVS-backed preset
  system (HTTP CRUD, BBP `0xF0..0xF4`, desktop tile UI, 1000-byte limit).
- New `Docs/micropython-troubleshooting.md` — 10-entry FAQ covering eval
  queue, heap watermark, REPL close code 4002, persistence vs ephemeral,
  `vfs_posix` stdout routing, log cursor (`?since=`), upload + autorun.
- New `Docs/bugs-improvements-2026-05-04.md` — backlog snapshot from the
  sweep (now resolved or rescoped).
- New host-stack Mermaid diagram in root `README.md` (Desktop + MCP +
  Python lib ↔ ESP32 BBP v4 ↔ RP2040 HAT).
- `Firmware/ESP32/README.md` gained MicroPython Scripting, Board Profiles,
  and External Bus subsections; `README_CALIBRATION.md` notes board-profile
  scoping and on-device script access.
- `Firmware/la-hat-architecture.md` §6 BBP table extended.
- `Firmware/bbp-protocol.md` Appendix A: ext-bus `0x75/0x76` and `0xB8–0xBE`, QuickSetup `0xF0–0xF4`,
  `WIFI_SET_AP_PASSWORD 0xEF`.
- `Firmware/firmware-structure.md` updated for Vite/Preact `web/` build,
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
- `Docs/release-checklist.md`: combined-release subsection added; manual
  PROTO_VERSION grep replaced with a pointer to the new CI gate; RP2040
  two-file warning replaced with the single-source rule.
- `Docs/external-bus.md` pending-items list reformatted as `## Roadmap`
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
- MUX / EFUSE refinements; PCA9535 fault-response path
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
- Error codes `0x0B`..`0x11` — `ERR_TIMEOUT`, `ERR_UNSAFE_REGISTER`.
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

#### Board profiles
- JSON schema (`Docs/board-profiles.md`) with nested `{value, locked}` for
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

- `bbp-protocol.md` bumped to v3.0.0 (BBP wire v4).
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
