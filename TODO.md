# BugBuster TODO

> Legend: 🔴 Bug/broken · 🟡 Missing/incomplete · 🟢 Refactor/cleanup · ⏳ Explicitly deferred

---

## Firmware — RP2040 HAT

### 🔴 Bugs

*(none — `uint8_t` truncation in `bb_la_usb.c` was already fixed; both LIVE STREAM and ONE-SHOT paths use `uint32_t` intermediates with safe post-clamp cast. Verified 2026-06-06.)*

### 🟡 Missing / Pending


---

## Firmware — ESP32

### 🔴 Bugs

*(none)*

### 🟡 Missing / Pending

- **`startAdcConversion()` holds the SPI bus mutex during the ADC_BUSY busy-wait** — `ad74416h.cpp:259–271` stops the sequencer and polls `LIVE_STATUS` for up to 500 ms *while holding `g_spi_bus_mutex`*. The normal-mode diagnostic rate has been shortened, reducing the expected wait, but the driver still holds the mutex through the busy-wait. Consider releasing the mutex between `LIVE_STATUS` poll reads after auditing the register-update race. Found 2026-06-11; partially mitigated 2026-06-11.


### 🟢 Refactor / Cleanup

*(none)*

### 🟢 Forward-looking note

- **AD74416H coordinated multi-channel DAC latch** — `WAIT_LDAC_CMD=0` (reset default) means each `setDacCode()` takes effect immediately. A `// TODO` in `ad74416h.cpp:185` notes that if `WAIT_LDAC_CMD` is ever enabled for glitch-free simultaneous updates, the CMD_KEY write must move to a dedicated `latchAllDacs()` rather than firing per single-channel write.  
  File: `Firmware/ESP32/src/hal/ad74416h.cpp:185`  
  Priority: low — only relevant when/if coordinated multi-channel mode is adopted.

---

## Desktop App

### 🔴 Bugs

### 🟡 Missing / Incomplete

- **Tauri `time` / `tauri-utils` build regression** — GitHub CI can resolve `tauri-utils 2.9.2` against `time 0.3.48`, which triggers `E0119` in `tauri-utils` during `cargo tauri build`. Temporarily pinned locally with `time = "=0.3.47"` in `DesktopApp/BugBuster/src-tauri/Cargo.toml` until the upstream fix lands.  
  Upstream issue: https://github.com/tauri-apps/tauri/issues/15525

- **macOS unsigned app — bypass Gatekeeper for distribution** — Without an Apple Developer account the app is blocked by Gatekeeper on first launch ("cannot be opened because the developer cannot be verified"). Options to investigate: (1) `xattr -cr` post-install script / DMG install helper that strips the quarantine bit; (2) ship a notarised ad-hoc-signed build using a free Apple ID + `codesign --deep --force --sign -` (ad-hoc) which satisfies Gatekeeper for local installs without a paid account; (3) document the one-time right-click → Open workaround in README; (4) evaluate distributing via Homebrew cask (users `brew install` and Homebrew handles the quarantine strip). The token-persistence bug (keychain blocked on unsigned binaries) is already fixed by switching to `tokens.json`.  
  Related files: `.github/workflows/desktop-release.yml`, `DesktopApp/README.md`

- **Scope recording semantics** — web Scope tab is Partial vs desktop: not all streaming/recording controls are mirrored over HTTP.  
  Ref: `Firmware/ESP32/web/docs/desktop-parity-matrix.md`

- **Calibration deep flows** — HAT calibration telemetry is shown in the web HAT card, but desktop-specific import flows and deep cal workflows have no web equivalent.  
  Ref: parity matrix row "Calibration tab deep flows"

### 🟢 Stubs awaiting firmware wiring

- **`set_pin_drive_strength`** — Tauri command only logs the request (`pin`, `drive`); no BBP command or firmware handler exists yet. The comment says hardware wiring is "pending firmware support" (likely via PCA GPIO expander controlling series-resistor bypass switches).  
  File: `DesktopApp/BugBuster/src-tauri/src/commands.rs:2249`

- **`set_efuse_config`** — Tauri command only logs `efuse`, `sw_limit_ma`, `enabled`; no BBP command or firmware handler exists. UI in `board.rs` calls this for per-efuse software current-limit configuration.  
  File: `DesktopApp/BugBuster/src-tauri/src/commands.rs:2264`

---

## ESP32 Web UI

### ⏳ Deferred

- **Logic Analyzer streaming** — USB vendor-bulk path; no HTTP stream parity is architecturally possible. Needs explicit "USB only" messaging in the web UI.  
  Ref: parity matrix row "Logic Analyzer stream"

---

## Python Client

### 🔴 Bugs

*(none)*

### 🟢 Refactor

- **Batch IO-owner claim+write** — `client.py:4072` has `TODO: batch claim+write into one frame in v6`. Currently does two round-trips when it could be one.  
  File: `python/bugbuster/client.py:4072`

---

## Simulator / Tests — `SimulatedDevice` Refactor

### 🔴 Simulator Bugs

*(none)*

*(none)*

---

## Version / Protocol Hygiene

*(none — CI proto-version check now runs on all pushes/PRs, path filter removed 2026-06-09)*
