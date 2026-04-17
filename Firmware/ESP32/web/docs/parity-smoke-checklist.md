# Web Desktop-Parity Smoke Checklist

Date: 2026-04-17

## Preconditions
1. Flash firmware and upload web assets:
```bash
cd /Users/lorenzo/Documents/Sviluppo/BugBuster/Firmware/ESP32
pio run -e esp32s3 -t upload
pio run -e esp32s3 -t uploadfs
```
2. Obtain admin token (same token used by web pairing).
3. Ensure device is reachable (`http://192.168.4.1` by default).

## Scripted API Smoke
Read-only smoke:
```bash
BASE_URL=http://192.168.4.1 TOKEN=<admin-token> \
  /Users/lorenzo/Documents/Sviluppo/BugBuster/Firmware/ESP32/tools/web_parity_smoke.sh
```

Mutating smoke:
```bash
BASE_URL=http://192.168.4.1 TOKEN=<admin-token> RUN_MUTATING=1 \
  /Users/lorenzo/Documents/Sviluppo/BugBuster/Firmware/ESP32/tools/web_parity_smoke.sh
```

## Browser Smoke (UI)
1. `Overview`: change channel function and confirm status reflection.
2. `Analog`: apply ADC config, DAC voltage/code, diagnostics source.
3. `Digital`: apply DIN/DOUT settings, toggle DOUT state.
4. `Scope`: start/stop wavegen with custom parameters.
5. `Signal Path`: toggle MUX/rails/e-fuse and confirm readback.
6. `System`: verify UART apply, USB-PD select, IOExp toggles, faults clear/mask, HAT actions.

## Expected Deferred Surfaces
- Logic analyzer live stream: desktop-only (USB vendor-bulk transport).
- Scope recording/export workflow: desktop-only path.
