import bb_devices
import bugbuster

i2c = bugbuster.I2C(1, 2, freq=400000, pullups='external', supply=3.3, vlogic=3.3)
imu = bb_devices.MPU6050(i2c)
accel = imu.read_accel()
gyro = imu.read_gyro()
temp_c = imu.read_temperature()
print("Accel:", accel)
print("Gyro:", gyro)
print("Temp: %.2f C" % temp_c)
