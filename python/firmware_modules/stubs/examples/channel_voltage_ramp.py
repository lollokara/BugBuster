"""Ramp channel 0 from 0 V to 3 V in 0.5 V steps and compare the readback.

Drives the CH0 terminal (IO3) up to 3 V; disconnect anything that must not see
that. bb_helpers.dac_ramp returns the channel to high impedance when it ends.
"""
import bb_helpers

for set_v, read_v in bb_helpers.dac_ramp(0, 0.0, 3.0, 0.5, settle_ms=100):
    print("set %.2f V  read %.4f V  error %+.1f mV" % (set_v, read_v, (read_v - set_v) * 1e3))
