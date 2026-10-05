"""Print the board's identity: MAC, CPU clock and last reset cause.

Read-only; changes nothing on the board.
"""
import machine

causes = {1: "power-on", 2: "external", 3: "software", 4: "panic", 5: "interrupt watchdog",
          6: "task watchdog", 7: "watchdog", 8: "deep sleep", 9: "brown-out", 10: "SDIO"}
print("unique id :", machine.unique_id().hex())
print("cpu clock : %d MHz" % (machine.freq() // 1000000))
cause = machine.reset_cause()
print("last reset:", causes.get(cause, "unknown (%d)" % cause))
