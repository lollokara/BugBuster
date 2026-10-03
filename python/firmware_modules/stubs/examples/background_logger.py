"""Background job: log channel 0's voltage once a second until stopped.

Run it with "Run in background"; its output goes to the log console and the
Stop button (or a replace) ends it.
"""
import bugbuster
import bb_logging

ch = bugbuster.Channel(0)
ch.set_function(bugbuster.FUNC_VIN)
while True:
    bb_logging.info("ch0 = %.4f V" % ch.read_voltage())
    bugbuster.sleep(1000)
