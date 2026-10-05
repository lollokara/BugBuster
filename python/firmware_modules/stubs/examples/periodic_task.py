"""Run a task at a fixed period without drifting, until stopped.

Uses ticks_ms to schedule each tick from the previous deadline, so slow work
does not stretch the period. Press Stop to end it; the finally block runs.
"""
import bugbuster

PERIOD_MS = 500
count = 0
deadline = bugbuster.ticks_ms() + PERIOD_MS
try:
    while True:
        wait = deadline - bugbuster.ticks_ms()
        if wait > 0:
            bugbuster.sleep(wait)
        count += 1
        print("tick", count)
        deadline += PERIOD_MS
finally:
    print("stopped after", count, "ticks")
