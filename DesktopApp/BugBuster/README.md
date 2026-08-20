# BugBuster desktop app

Cross-platform GUI for the BugBuster mainboard and both HATs. Tauri v2 backend
(Rust) with a Leptos 0.7 frontend compiled to WASM.

Version `2.1.0`. Connects over USB CDC (BBP v10) or WiFi (HTTP REST).

## Build

```bash
rustup target add wasm32-unknown-unknown
cargo install trunk tauri-cli

cd DesktopApp/BugBuster
cargo tauri dev      # hot-reload frontend and backend
cargo tauri build    # release bundle
```

## Tabs

22 tabs in five categories.

| Category | Tabs |
|---|---|
| **Overview** | Dashboard · Board Map · Voltages & Cal · Faults · Diagnostics |
| **Analog** | ADC · VDAC · IDAC · IIN |
| **Digital** | GPIO · DIN · DOUT · HV IO · IO Expander |
| **Instruments** | Scope · Logic Analyzer · HS DAQ · WaveGen · Signal Path |
| **System** | HAT · USB PD · UART |

<table>
  <tr>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_dashboard.png" alt="Dashboard" width="300"/><br/><sub><b>Dashboard</b></sub></td>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_adc.png" alt="ADC" width="300"/><br/><sub><b>ADC</b></sub></td>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_scope.png" alt="Scope" width="300"/><br/><sub><b>Scope</b></sub></td>
  </tr>
  <tr>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_logic_analyzer.png" alt="Logic Analyzer" width="300"/><br/><sub><b>Logic Analyzer</b></sub></td>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_wavegen.png" alt="WaveGen" width="300"/><br/><sub><b>WaveGen</b></sub></td>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_signal_path.png" alt="Signal Path" width="300"/><br/><sub><b>Signal Path</b></sub></td>
  </tr>
  <tr>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_vdac.png" alt="VDAC" width="300"/><br/><sub><b>VDAC</b></sub></td>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_idac.png" alt="IDAC" width="300"/><br/><sub><b>IDAC</b></sub></td>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_iin.png" alt="IIN" width="300"/><br/><sub><b>IIN</b></sub></td>
  </tr>
  <tr>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_din.png" alt="DIN" width="300"/><br/><sub><b>DIN</b></sub></td>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_dout.png" alt="DOUT" width="300"/><br/><sub><b>DOUT</b></sub></td>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_gpio.png" alt="GPIO" width="300"/><br/><sub><b>GPIO</b></sub></td>
  </tr>
  <tr>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_usb_pd.png" alt="USB-PD" width="300"/><br/><sub><b>USB-PD</b></sub></td>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_voltages___cal.png" alt="Voltages and calibration" width="300"/><br/><sub><b>Voltages + Cal</b></sub></td>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_faults.png" alt="Faults" width="300"/><br/><sub><b>Faults</b></sub></td>
  </tr>
  <tr>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_uart.png" alt="UART" width="300"/><br/><sub><b>UART</b></sub></td>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_hv_io.png" alt="HV IO" width="300"/><br/><sub><b>HV IO</b></sub></td>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_io_expander.png" alt="IO Expander" width="300"/><br/><sub><b>IO Expander</b></sub></td>
  </tr>
  <tr>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_board_map.png" alt="Board Map" width="300"/><br/><sub><b>Board Map</b></sub></td>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_hat.png" alt="HAT" width="300"/><br/><sub><b>HAT</b></sub></td>
    <td align="center"><img src="../../Docs/Images/screenshots/screenshot_diagnostics.png" alt="Diagnostics" width="300"/><br/><sub><b>Diagnostics</b></sub></td>
  </tr>
</table>

## Layout

```
src/                       Leptos frontend (WASM)
  app.rs                   App shell, tab categories, routing
  tauri_bridge.rs          invoke() wrappers and shared types
  tabs/                    One module per tab
  components/              Shared UI components

src-tauri/src/             Tauri backend (Rust)
  lib.rs                   Plugin setup, command registration
  commands.rs              173 Tauri commands
  connection_manager.rs    Transport lifecycle, state polling
  usb_transport.rs         BBP over USB CDC (COBS framing)
  http_transport.rs        REST over WiFi, re-encoded to BBP binary
  discovery.rs             USB enumeration + subnet scan
  bbp.rs                   Protocol constants, frame builder, payload helpers
  state.rs                 DeviceState, ChannelState, connection types
  transport.rs             Transport trait abstraction

styles.css                 Global glass UI theme
index.html                 Trunk entry point
```

`bbp.rs` holds `PROTO_VERSION`, which must stay in lockstep with
`Firmware/ESP32/src/bbp/bbp.h` and `python/bugbuster/protocol.py`.

## Connecting

The app auto-discovers devices:

- **USB** - scans for Espressif VID serial ports and probes each with a BBP
  handshake.
- **WiFi** - scans `192.168.4.1` (the device AP) plus every local subnet IP.

USB is preferred. WiFi covers everything except real-time scope streaming and
IDAC calibration.

Mutating HTTP requests need an admin token. On the first USB connection the app
persists a token keyed by the device MAC, and HTTP sessions reuse it
automatically. If `/api/device/info` reports no MAC, the app raises a
"firmware too old" error rather than looping on pairing.

## Board profiles

The **Board Map** tab imports and exports board profiles (`.json`) that lock the
VLOGIC / VADJ1 / VADJ2 rails and declare per-pin names and directions. The MCP
server reads the same profiles through `list_boards` / `set_board` and enforces
the rail lock in `bugbuster_mcp/safety.py`.

Schema: [Docs/board-profiles.md](../../Docs/board-profiles.md)

## Releasing

The version lives in three files. Never edit them by hand:

```bash
python scripts/desktop_version.py 2.2.0        # set
python scripts/desktop_version.py --check      # verify lockstep
```

```bash
git add Cargo.toml src-tauri/Cargo.toml src-tauri/tauri.conf.json
git commit -m "desktop: release 2.2.0"
git tag desktop-v2.2.0
git push origin main --tags
```

`.github/workflows/desktop-release.yml` then builds on `windows-latest`,
`ubuntu-22.04` and `macos-latest`, uploads the bundles to a draft GitHub
Release, and rejects the tag if it does not match the app version.

**Known gaps:** macOS builds are not notarized, Windows builds are not
code-signed, and the workflow produces a single macOS artifact - extend the
matrix with explicit targets if you need separate Intel and Apple Silicon
builds.
