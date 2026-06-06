# HAT v2 status, rails, LEDs, and calibration telemetry
# Run on-device through the Scripts tab, script_eval, or script run-file.

import bugbuster

status = bugbuster.hat_status()
print('HAT detected:', status['detected'])
print('HAT connected:', status['connected'])

if not status['connected']:
    print('No HAT UART connection; skipping HAT commands')
else:
    caps = bugbuster.hat_caps()
    print('HAT fw: %d.%d' % (caps['fw_major'], caps['fw_minor']))
    print('rails=%d leds=%d shifted_io=%d' % (
        caps['rail_count'], caps['led_count'], caps['shifted_io_count']))

    print('Rail status:')
    for rail in bugbuster.hat_rails():
        print('  id=%d enabled=%s voltage=%dmV current=%dmA status=%d' % (
            rail['rail_id'], rail['enabled'], rail['voltage_mv'],
            rail['current_ma'], rail['status']))

    # Blink LED 1 green briefly, then turn it off.
    bugbuster.hat_led(1, bugbuster.HAT_LED_GREEN)
    bugbuster.sleep(250)
    bugbuster.hat_led(1, bugbuster.HAT_LED_OFF)

    # Read calibration state without starting a sweep.
    cal = bugbuster.hat_calibrate_status()
    print('cal state=%d progress=%d rail=%d err=%d' % (
        cal['state'], cal['progress'], cal['rail_id'], cal['last_error']))
