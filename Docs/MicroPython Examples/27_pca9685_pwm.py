import bb_devices
import bugbuster

i2c = bugbuster.I2C(1, 2, freq=400000, pullups='external', supply=3.3, vlogic=3.3)
pwm = bb_devices.PCA9685(i2c, addr=0x40)
pwm.set_pwm_freq(50)

# Servo-style PWM: channel 0 at 10% duty cycle.
pwm.set_duty(0, 0.10)
print("Channel 0 duty set to 10%")
