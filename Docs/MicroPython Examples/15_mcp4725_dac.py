import bugbuster

# 1. Initialize I2C for MCP4725
# sda_io=1, scl_io=2 are in the VADJ1 block (IO 1-6).
# supply=5.0  -> Energizes VADJ1 rail to 5.0 V.
i2c = bugbuster.I2C(
    sda_io=1, 
    scl_io=2, 
    freq=100000, 
    pullups='external', 
    supply=5.0, 
    vlogic=5.0
)

# 2. Configure IO 6 for Current Output
# IO 6 maps to AD74416H Channel 1.
# backend now automatically closes the MUX switch when function is set.
led_ch = bugbuster.Channel(1)
led_ch.set_function(bugbuster.FUNC_IOUT)

# Scan for MCP4725
print("Scanning I2C bus...")
addrs = i2c.scan()
print("Found addresses:", [hex(a) for a in addrs])

ADDR = 0x62

def set_dac_voltage(value):
    """Sets MCP4725 12-bit DAC output (0-4095)."""
    value = max(0, min(4095, int(value)))
    buf = bytes([0x40, (value >> 4) & 0xFF, (value << 4) & 0xF0])
    try:
        i2c.writeto(ADDR, buf)
    except OSError:
        pass

# Main Loop: Blink LED on IO6 while ramping MCP DAC
if ADDR in addrs:
    print("Starting concurrent LED blink (IO6, 10mA) and DAC ramp (0x62)...")
    try:
        dac_val = 0
        led_on = False
        while True:
            led_on = not led_on
            # 10mA = 2.5V setting on the 0-20mA channel
            current_v = 2.5 if led_on else 0.0
            led_ch.set_voltage(current_v)
            
            set_dac_voltage(dac_val)
            print("LED: %s | DAC: %d" % ("ON " if led_on else "OFF", dac_val))
            
            dac_val = (dac_val + 512) % 4096
            bugbuster.sleep(500)
    except KeyboardInterrupt:
        print("Script stopped by user")
        led_ch.set_voltage(0.0)
        led_ch.set_function(bugbuster.FUNC_HIGH_IMP)
else:
    print("MCP4725 not detected at 0x62.")
