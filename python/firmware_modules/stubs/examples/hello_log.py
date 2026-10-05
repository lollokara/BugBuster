"""Hello BugBuster: print, log levels and the tick counter.

Shows the three ways a script talks to the console: print(), bugbuster.log()
and the timestamped bb_logging helpers. Touches no hardware.
"""
import bugbuster
import bb_logging

t0 = bugbuster.ticks_ms()
print("hello from BugBuster")
bugbuster.log("I", "plain log line")
bb_logging.info("timestamped info line")
bb_logging.warn("timestamped warning")
bugbuster.sleep(100)
print("elapsed: %d ms" % (bugbuster.ticks_ms() - t0))
