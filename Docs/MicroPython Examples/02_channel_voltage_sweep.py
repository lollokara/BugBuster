# 02_channel_voltage_sweep.py — Drive DAC output and read ADC readback.
#
# Purpose: demonstrate analog channel voltage output and ADC reading.
#
# Wiring: none required for bench test. If you wire an external load,
#   connect between IO1 (channel 0) and GND; the DAC will drive it.
#
# Run: POST /api/scripts/eval or upload + run-file.
# Expect: sweep from 0.0 V to 5.0 V in 1.0 V steps, each step sleeps 100 ms,
#   and reads back the ADC. On a properly configured bench, readback should
#   track within ±50 mV of the target. Without hardware, readback may be
#   near-zero (expected). Each iteration prints target, readback, and delta.

import bugbuster

ch = bugbuster.Channel(0)
ch.set_function(bugbuster.FUNC_VOUT)

print("Sweeping channel 0 from 0 V to 5 V (1 V steps)...")
for target_v in [0.0, 1.0, 2.0, 3.0, 4.0, 5.0]:
    ch.set_voltage(target_v)
    bugbuster.sleep(100)
    readback_v = ch.read_voltage()
    delta_v = readback_v - target_v
    print('target=%.2f V  readback=%.5f V  delta=%.5f V' % (target_v, readback_v, delta_v))

ch.set_function(bugbuster.FUNC_HIGH_IMP)
print("Done. Channel 0 returned to HIGH_IMP.")
