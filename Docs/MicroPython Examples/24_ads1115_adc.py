import bb_devices
import bugbuster

i2c = bugbuster.I2C(1, 2, freq=400000, pullups='external', supply=3.3, vlogic=3.3)
adc = bb_devices.ADS1115(i2c, addr=0x48)
raw = adc.read_raw(channel=0, pga=4.096, data_rate=128)
volts = adc.read_voltage(channel=0, pga=4.096, data_rate=128)
print("Raw:", raw)
print("Voltage: %.4f V" % volts)
