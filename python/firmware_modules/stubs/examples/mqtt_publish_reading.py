"""Publish channel 0's voltage to an MQTT broker every 5 seconds, three times.

Edit BROKER for your network. Each call opens a short plain-TCP connection (QoS 0).
"""
import bugbuster

BROKER = "192.168.1.10"

ch = bugbuster.Channel(0)
ch.set_function(bugbuster.FUNC_VIN)
try:
    for _ in range(3):
        bugbuster.sleep(100)
        payload = "%.4f" % ch.read_voltage()
        try:
            bugbuster.mqtt_publish("bugbuster/ch0", payload, BROKER)
            print("published", payload)
        except OSError as e:
            print("publish failed:", e)
        bugbuster.sleep(5000)
finally:
    ch.set_function(bugbuster.FUNC_HIGH_IMP)
