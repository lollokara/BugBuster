"""Report the HAT link, capabilities and rails (read-only).

Works with no HAT connected: it then just says so.
"""
import bugbuster

st = bugbuster.hat_status()
print("detected %s, connected %s, type %d, fw %d.%d" % (
    st["detected"], st["connected"], st["type"], st["fw_major"], st["fw_minor"]))
if st["connected"] and st["type"] == 1:
    try:
        caps = bugbuster.hat_caps()
        print("%d rails, %d LEDs" % (caps["rail_count"], caps["led_count"]))
        for rail in bugbuster.hat_rails():
            print("rail %d: %s %d mV %d mA" % (rail["rail_id"], "on" if rail["enabled"] else "off",
                                              rail["voltage_mv"], rail["current_ma"]))
    except OSError as e:
        print("HAT did not answer:", e)
else:
    print("no SWD/GPIO HAT connected")
