# BugBuster Desktop App

Tauri v2 + Leptos 0.7 desktop application for controlling the BugBuster hardware.

## Stack

- **Backend:** Rust (Tauri v2) -- serial/HTTP transport, BBP protocol, state management
- **Frontend:** Leptos 0.7 (WASM) -- reactive UI compiled via Trunk
- **Build:** Trunk (WASM) + Cargo (Tauri)

## Development

```bash
# Install dependencies (first time)
cargo install trunk
cargo install tauri-cli

# Dev mode (hot-reload frontend + backend)
cargo tauri dev

# Production build
cargo tauri build
```

## Project Structure

```
src/                    Leptos frontend (WASM)
  app.rs                Main app shell, routing, particle background
  tauri_bridge.rs       Tauri invoke wrappers, type definitions
  tabs/                 18 tab components (overview, adc, scope, etc.)
  components/           Shared UI components

src-tauri/              Tauri backend (Rust)
  src/
    lib.rs              Tauri plugin setup, command registration
    commands.rs         40+ Tauri commands (device control, OTA, etc.)
    connection_manager.rs  Transport lifecycle, state polling
    usb_transport.rs    BBP over USB CDC (COBS framing)
    http_transport.rs   REST API over WiFi (JSON re-encoding to BBP binary)
    discovery.rs        Device discovery (USB enumeration + subnet scan)
    bbp.rs              Protocol constants, frame builder, payload helpers
    state.rs            DeviceState, ChannelState, connection types
    transport.rs        Transport trait abstraction

styles.css              Global glass UI theme
index.html              Trunk entry point
```

## Connecting

The app auto-discovers devices on:
- **USB:** Scans for Espressif VID serial ports, probes with BBP handshake
- **WiFi:** Scans `192.168.4.1` (AP) + all local subnet IPs

USB is preferred for low-latency streaming. WiFi works for all features except real-time scope streaming and IDAC calibration.
