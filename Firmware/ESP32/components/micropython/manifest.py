# MicroPython frozen manifest for BugBuster (V2-C)
# Freezes bb_helpers, bb_devices, bb_logging as pre-compiled .mpy bytecode.
# PORT_DIR = Firmware/ESP32/components/micropython
# Four levels up from PORT_DIR reaches the repo root; python/firmware_modules is there.
freeze("$(PORT_DIR)/../../../../python/firmware_modules")
