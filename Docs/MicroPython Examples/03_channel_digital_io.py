# 03_channel_digital_io.py — Digital output toggle.
#
# Purpose: demonstrate set_do() for digital output control.
#
# Wiring (optional): to verify the output, wire a logic analyzer or LED
#   between IO1 (channel 0 DO) and GND. VOUT will drive 0 V (LOW) or 3.3 V
#   (HIGH) depending on set_do() state.
#
# Run: POST /api/scripts/eval or upload + run-file.
# Expect: toggle channel 0 DO between HIGH (True) and LOW (False) five times,
#   with 200 ms delay between transitions. Print state before and after.

import bugbuster

ch = bugbuster.Channel(0)
ch.set_function(bugbuster.FUNC_VOUT)

print("Toggle channel 0 DO (5 cycles, 200 ms each)...")
for cycle in range(5):
    ch.set_do(True)
    print('Cycle %d: set_do(True)' % cycle)
    bugbuster.sleep(200)

    ch.set_do(False)
    print('Cycle %d: set_do(False)' % cycle)
    bugbuster.sleep(200)

ch.set_function(bugbuster.FUNC_HIGH_IMP)
print("Done. Channel 0 returned to HIGH_IMP.")
