# 01_hello.py — Simplest possible smoke test.
#
# Purpose: verify the scripting engine runs at all.
#
# Run: POST /api/scripts/eval or upload + run-file.
# Expect: see "hello bugbuster" and "goodbye" in the log output.

import bugbuster

print('hello bugbuster')
bugbuster.sleep(100)
print('goodbye')
