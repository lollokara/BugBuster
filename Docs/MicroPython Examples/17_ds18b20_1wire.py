import bb_devices
import bugbuster

# Wire the data line to an ESP32 GPIO and add a 4.7 kOhm pull-up to 3.3 V.
sensor = bb_devices.DS18B20(2)
roms = sensor.scan()
print("Found:", roms)
if roms:
    for rom, temp_c in sensor.read_all():
        print("%s -> %.2f C" % (rom, temp_c))
