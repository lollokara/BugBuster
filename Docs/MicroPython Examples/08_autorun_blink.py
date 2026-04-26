# 08_autorun_blink.py — Autorun boot indicator (toggle DO every 500 ms).
#
# Purpose: serve as a visible "device has booted" indicator by toggling
#   channel 0 digital output at 1 Hz. Useful for autorun testing.
#
# Wiring: optional. Wire a logic analyzer or LED to IO1 (channel 0 DO) and GND
#   to see 0.5 s HIGH / 0.5 s LOW cycles. Without wiring, script still runs fine.
#
# Autorun setup:
#   1. Upload this script:
#        curl -X POST http://device.local/api/scripts/files?name=boot_blink.py \
#          -H "X-BugBuster-Admin-Token: $TOKEN" \
#          --data-binary @08_autorun_blink.py
#   2. Enable autorun:
#        curl -X POST http://device.local/api/scripts/autorun/enable?name=boot_blink.py \
#          -H "X-BugBuster-Admin-Token: $TOKEN"
#   3. Reboot the device (power cycle or reset button).
#   4. If IO12 is floating (default, internal pull-up = HIGH), autorun runs.
#   5. Check /api/scripts/autorun/status to confirm enabled & last_run_ok.
#
# Recovery: pull IO12 LOW (10 kΩ pull-down to GND, or button to GND) at boot
#   to skip autorun. Or call /api/scripts/autorun/disable to remove the sentinel.
#
# Run: direct eval or upload + run-file for one-shot. Or enable as autorun (above).
# Expect: toggle channel 0 DO on/off 30 times (15 seconds). Log prints each toggle.
#   If autorun is enabled, this runs at every boot (before the grace window expires).

import bugbuster

ch = bugbuster.Channel(0)
ch.set_function(bugbuster.FUNC_VOUT)

print('Boot blink starting (30 toggles, 500 ms each)...')

for i in range(30):
    ch.set_do(True)
    bugbuster.sleep(500)
    ch.set_do(False)
    bugbuster.sleep(500)
    if (i + 1) % 10 == 0:
        print('  %d toggles done' % (i + 1))

ch.set_function(bugbuster.FUNC_HIGH_IMP)
print('Boot blink complete. Device booted successfully.')
