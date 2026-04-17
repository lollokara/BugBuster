# BugBuster IDAC Calibration Guide

The BugBuster board uses a DS4424 4-channel I2C IDAC to adjust the output voltages of three regulators:
- **IDAC0**: Level shifter voltage (`V_LSHIFT`), typically 3.3V midpoint.
- **IDAC1**: Regulator 1 output voltage (`V_ADJ1`), typically 5.0V midpoint.
- **IDAC2**: Regulator 2 output voltage (`V_ADJ2`), typically 5.0V midpoint.

Each IDAC channel can be auto-calibrated using the AD74416H's Channel D through a dedicated self-test mux (U23).

## Calibration Procedure

### 1. Manual Calibration via CLI
You can manually sweep an IDAC channel and read the resulting voltage:
1. Set the IDAC code: `idac <ch> code <n>` (where n is -127 to 127).
2. Read the voltage using the ADC (requires manual mux and ADC setup).

### 2. Auto-Calibration via CLI
The `idac_cal <ch>` command automates the sweep and measurement process:
```bash
idac_cal 1
```
Or via the `idac` command:
```bash
idac 1 cal
```

This will:
1. Snapshot the current hardware state (MUX, ADC).
2. Configure U23 to route the requested voltage rail to AD74416H Channel D.
3. Set Channel D to `VIN` mode.
4. Sweep the IDAC through its full range (sink and source).
5. Record ADC measurements at each step.
6. Interpolate missing points and store the calibration table in NVS.
7. Restore the hardware state.

### 3. Verification
After calibration, use the `idac <ch> v <volts>` command to set a specific voltage and verify it with a multimeter or the `adc` command.

## Persistence
Calibration data is stored in the ESP32's Non-Volatile Storage (NVS) under the `ds4424_cal` namespace. It is loaded during `ds4424_init()` on every boot.

If the calibration data is invalid or missing, the system falls back to theoretical formulas based on resistor values.

## Advanced Usage
`idac_cal <ch> [step] [settle_ms]`
- `step`: Code increment between measurements (default: 8).
- `settle_ms`: Wait time after each DAC change (default: 100ms).
