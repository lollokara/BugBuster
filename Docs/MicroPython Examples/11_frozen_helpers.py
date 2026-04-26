# 11_frozen_helpers.py — On-device script using frozen modules bb_helpers and bb_logging.
#
# Purpose: demonstrate that frozen helper modules (bb_helpers, bb_logging) are
#   available on-device without any upload. Import them directly; the firmware
#   includes them in the MicroPython frozen bytecode image.
#
# Run: POST /api/scripts/eval  (no upload needed — modules are frozen)
#   curl -X POST http://<ip>/api/scripts/eval \
#        -H "Content-Type: text/plain" \
#        -H "X-BugBuster-Admin-Token: TOKEN" \
#        --data-binary @11_frozen_helpers.py
#
#   or via the Python client:
#     python -m bugbuster.script --host <ip> --token TOKEN run 11_frozen_helpers.py
#
# Expect: log lines of the form:
#   [INFO] starting ramp
#   v=0.00  readback=0.00000
#   v=1.00  readback=1.00000
#   ...
#   v=5.00  readback=5.00000
#   [INFO] done
#   Exact readback values depend on hardware calibration; on a bench with no
#   load they should be within ±50 mV of the target.
#
# Prerequisites: none — bb_helpers and bb_logging are frozen into the firmware.
#   Channel 0 is driven by the DAC; no external wiring required for a basic run.

import bb_helpers
import bb_logging

bb_logging.info("starting ramp")

try:
    results = bb_helpers.dac_ramp(0, 0.0, 5.0, 1.0)
    for v, readback in results:
        print('v=%.2f  readback=%.5f' % (v, readback))
except KeyboardInterrupt:
    print("interrupted")

bb_logging.info("done")
