"""Check which VADJ voltages the USB-C power source can deliver (read-only).

Asks the PD guard about a few voltages on both rails; nothing is switched on.
"""
import bugbuster

for rail in (1, 2):
    for volts in (3.3, 5.0, 9.0, 12.0):
        warning = bugbuster.vadj_pd_warning(rail, volts)
        print("VADJ%d %.1f V: %s" % (rail, volts, "ok" if warning is None else "NOT available"))
