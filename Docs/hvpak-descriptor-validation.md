# HVPAK Descriptor Validation

This document defines the bench procedure for validating the descriptor table in
`Firmware/RP2040/src/bb_hvpak.c` against the programmed GreenPAK image on real
hardware.

## Preconditions

- HAT connected and reachable over USB
- Programmed GreenPAK image present and reporting `hvpak_ready=true`
- Run from the repository root with the Python environment active

## Baseline

```bash
pytest tests/device/test_11_hat.py -q --hat --device-usb
```

Confirm:
- `hat_get_hvpak_info()` returns `ready=true`
- `hat_get_hvpak_caps()` reports the expected part family

For blank/unprovisioned PAK checks:
- `hat_get_hvpak_info()` may return `ready=false` and `factory_virgin=true`
- this means the chip is responding through the native service window, but the
  custom mailbox image has not been recognized yet

## Representative validation set

Run at least one check from each domain below.

### 1. LUT validation

- Read one LUT through `hat_get_hvpak_lut(kind, index)`
- Write the same truth table back with `hat_set_hvpak_lut(kind, index, truth_table)`
- Re-read and confirm the value is unchanged

Suggested priority:
- first available `lut2`
- else first available `lut3`
- else first available `lut4`

## 2. Bridge validation

- Read bridge state with `hat_get_hvpak_bridge()`
- Write the same values back with `hat_set_hvpak_bridge(...)`
- Re-read and confirm the fields match

Fields to note in the evidence:
- output mode
- OCP retry
- predriver/full-bridge mode
- control selection
- UVLO / deglitch

## 3. Analog or PWM validation

Choose one:

### Analog
- Read with `hat_get_hvpak_analog()`
- Write the same values back with `hat_set_hvpak_analog(**before)`
- Re-read and confirm the fields match

### PWM
- Read one PWM block with `hat_get_hvpak_pwm(index)`
- Write the same values back with `hat_set_hvpak_pwm(index, ...)`
- Re-read and confirm the fields match

## 4. Guarded raw-register validation

- Attempt a masked write to `0x48`
- Confirm it fails with `HVPAK_UNSAFE_REGISTER`

This proves the raw-register guard is live on hardware.

## Evidence format

Record:
- detected part (`SLG47104` or `SLG47115-E`)
- which domain checks were run
- before/after values
- any skipped checks and why

Recommended note template:

```text
Part: SLG47115-E
LUT check: PASS (kind=lut3 index=0 truth=0x96)
Bridge check: PASS (output_mode=[1,3], uvlo=true)
Analog/PWM check: PASS (PWM0 initial=20 deadband=2)
Unsafe raw write guard: PASS (HVPAK_UNSAFE_REGISTER)
Gaps: ACMP1 not validated on SLG47104 because hardware not available
```

## Expected outcomes

- If any round-trip value mismatches, the descriptor table is suspect and the
  mismatch should be fixed in a dedicated descriptor patch.
- If the part is unavailable, explicitly record the gap rather than inferring
  success from the other part.
