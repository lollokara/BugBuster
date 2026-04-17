# Desktop → Web Settings Parity Matrix

Last updated: 2026-04-17
Scope: `Firmware/ESP32/web`

## Legend
- `Full`: web exposes equivalent granular control path already available over HTTP.
- `Partial`: some controls exposed, but not full desktop granularity yet.
- `Deferred`: desktop feature is USB/Tauri-specific or missing HTTP endpoint.

## Per-surface status

| Desktop surface | Web placement | Status | Notes |
|---|---|---|---|
| Overview channel function | Overview | Full | `/api/channel/{ch}/function` wired |
| ADC (range/rate/mux) | Analog | Full | `/api/channel/{ch}/adc/config` wired |
| VDAC code/voltage/range | Analog | Full | `/api/channel/{ch}/dac`, `/vout/range` wired |
| IIN readback | Analog | Full | Read-only parity from `/api/status` |
| IDAC set/calibration | Analog | Full | `/api/idac/*` wired |
| Diagnostics source | Analog | Full | `/api/diagnostics/config` wired |
| DIN config/state | Digital | Full | `/api/channel/{ch}/din/config` wired |
| DOUT config/state | Digital | Full | `/api/channel/{ch}/do/*` wired |
| GPIO/DIO config & set | Digital | Full | `/api/gpio/*` and `/api/dio/*` wired |
| Signal path/mux/rails/efuse | Signal Path + System IOExp | Full | `/api/mux/*`, `/api/ioexp/*`, `/api/lshift/oe` |
| Board profile select | System | Full | `/api/board`, `/api/board/select` |
| USB-PD select/caps | System | Full | `/api/usbpd`, `/api/usbpd/select`, `/api/usbpd/caps` |
| UART bridge config | System | Full | `/api/uart/config`, `/api/uart/pins`, `/api/uart/{id}/config` |
| Fault clear/mask (global+channel) | System | Full | `/api/faults/clear*`, `/api/faults/mask*` |
| HAT detect/reset/pin config | System | Full | `/api/hat/*` wired |
| Scope view controls | Scope | Partial | UI controls present; not all desktop streaming/recording semantics |
| Wavegen advanced controls | Scope | Full | Channel/mode/waveform/freq/amplitude/offset + start/stop wired |
| Logic Analyzer stream | N/A | Deferred | USB vendor-bulk desktop path; no HTTP stream parity |
| Calibration tab deep flows | N/A | Deferred | Desktop workflow not fully represented in HTTP endpoints |
| Voltages tab dedicated panel | N/A | Partial | Values shown via Overview/System, no standalone tab yet |

## Next closure targets
1. Scope + wavegen advanced control parity in Scope tab (desktop-equivalent parameter panel).
2. Explicit deferred messaging blocks for LA/USB-only surfaces.
3. Browser parity smoke checklist per row (set/read roundtrip evidence).

## Verification Commands
Read-only API smoke:
```bash
BASE_URL=http://192.168.4.1 TOKEN=<admin-token> \
  /Users/lorenzo/Documents/Sviluppo/BugBuster/Firmware/ESP32/tools/web_parity_smoke.sh
```

Mutating API smoke:
```bash
BASE_URL=http://192.168.4.1 TOKEN=<admin-token> RUN_MUTATING=1 \
  /Users/lorenzo/Documents/Sviluppo/BugBuster/Firmware/ESP32/tools/web_parity_smoke.sh
```

Operator checklist:
- `/Users/lorenzo/Documents/Sviluppo/BugBuster/Firmware/ESP32/web/docs/parity-smoke-checklist.md`
