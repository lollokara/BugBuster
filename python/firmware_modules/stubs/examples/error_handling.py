"""Handle the common script errors: busy channels, missing hardware, bad values.

Shows which exceptions the API raises and how to recover from each.
"""
import bugbuster
import daq

try:
    ch = bugbuster.Channel(9)
except ValueError as e:
    print("bad channel:", e)

try:
    bugbuster.Channel(0).set_function(bugbuster.FUNC_HIGH_IMP)
except OSError as e:
    print("CH0 busy or hardware refused:", e)

try:
    print(daq.read())
except OSError as e:
    print("DAQ HAT not available:", e)
