# bb_helpers.py — BugBuster channel helper utilities (frozen module)
#
# Provides high-level helpers built on top of the bugbuster module.
# Import with: import bb_helpers
#
# These run on-device without VFS — compiled as frozen .mpy bytecode.

import bugbuster


def settle(ms):
    """Sleep for ms milliseconds (cooperative)."""
    bugbuster.sleep(ms)


def dac_ramp(channel, lo, hi, step, settle_ms=50):
    """Ramp DAC output on `channel` from `lo` V to `hi` V in `step` V increments.

    Each step waits settle_ms milliseconds before proceeding.
    Returns a list of (voltage, readback) tuples.

    Example:
        results = dac_ramp(0, 0.0, 5.0, 1.0)
        for v, rb in results:
            print('set=%.2f  read=%.5f' % (v, rb))
    """
    ch = bugbuster.Channel(channel)
    ch.set_function(bugbuster.FUNC_VOUT)

    results = []
    v = lo
    while v <= hi + 1e-9:
        ch.set_voltage(v)
        bugbuster.sleep(settle_ms)
        readback = ch.read_voltage()
        results.append((v, readback))
        v = v + step

    ch.set_function(bugbuster.FUNC_HIGH_IMP)
    return results


def channel_sweep(channel, voltages, settle_ms=100, readback=True):
    """Drive `channel` through an explicit list of voltages.

    Args:
        channel:    Channel index (0-based).
        voltages:   Iterable of float voltage values (V).
        settle_ms:  Settle time between steps (ms).
        readback:   If True, read back ADC after each step.

    Returns a list of (target_v, readback_v) tuples if readback=True,
    else a list of target_v values.

    Example:
        channel_sweep(0, [0.0, 2.5, 5.0], settle_ms=200)
    """
    ch = bugbuster.Channel(channel)
    ch.set_function(bugbuster.FUNC_VOUT)

    results = []
    for v in voltages:
        ch.set_voltage(v)
        bugbuster.sleep(settle_ms)
        if readback:
            rb = ch.read_voltage()
            results.append((v, rb))
        else:
            results.append(v)

    ch.set_function(bugbuster.FUNC_HIGH_IMP)
    return results
