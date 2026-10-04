"""Pulse channel 0 to 1.0 V three times while holding an exclusive claim.

The claim keeps the app, HTTP and CLI from touching CH0 mid-sequence; it is
released automatically when the with-block ends, even after an error.
"""
import bugbuster

try:
    with bugbuster.claim([12], purpose="pulse demo"):
        ch = bugbuster.Channel(0)
        ch.set_function(bugbuster.FUNC_VOUT)
        for n in range(3):
            ch.set_voltage(1.0)
            bugbuster.sleep(200)
            ch.set_voltage(0.0)
            bugbuster.sleep(200)
            print("pulse", n + 1)
        ch.set_function(bugbuster.FUNC_HIGH_IMP)
except OSError as e:
    print("CH0 is busy or the claim failed:", e)
