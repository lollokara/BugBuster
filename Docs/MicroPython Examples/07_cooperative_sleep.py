# 07_cooperative_sleep.py — Demonstrate cooperative cancellation with script_stop.
#
# Purpose: show how bugbuster.sleep() allows graceful interruption.
#
# Run: POST /api/scripts/eval, then within 5 seconds call POST /api/scripts/stop
#   (or use Python client: bb.script_stop() after a short delay).
# Expect: loop runs for up to 50 iterations (5 seconds total at 100 ms/iter).
#   When you call stop, the next bugbuster.sleep() raises KeyboardInterrupt,
#   which is caught and prints "Stopped by user". Without stop, all 50 complete.

import bugbuster

print('Starting 50-iteration loop (100 ms each = 5 sec max)...')
print('Call /api/scripts/stop to interrupt (KeyboardInterrupt will be caught).')

try:
    for i in range(50):
        bugbuster.sleep(100)
        print('iter %d' % i)
    print('Loop completed all 50 iterations.')

except KeyboardInterrupt:
    print('Stopped by user (KeyboardInterrupt caught).')
