"""Log five BME280 readings (temperature, pressure, humidity) once a second.

BME280 on IO1 (SDA) / IO2 (SCL) at 3.3 V, address 0x76.
"""
import bugbuster
import bb_devices
import bb_logging

bus = bugbuster.I2C(1, 2, freq=100000, supply=3.3, vlogic=3.3)
try:
    sensor = bb_devices.BME280(bus, addr=0x76)
    for _ in range(5):
        temp_c, press_pa, rh = sensor.read()
        bb_logging.info("%.2f C  %.1f hPa  %.1f %%RH" % (temp_c, press_pa / 100, rh))
        bugbuster.sleep(1000)
except OSError as e:
    bb_logging.error("BME280 not answering: %s" % e)
finally:
    bus.close()
