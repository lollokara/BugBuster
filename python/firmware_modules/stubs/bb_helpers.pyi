"""Small helpers for channel ramps and sweeps (``bb_helpers.py`` on the device)."""

def settle(ms) -> None:
    """Sleep for ms milliseconds (cooperative)."""

def dac_ramp(channel, lo, hi, step, settle_ms=50) -> None:
    """Ramp DAC output on `channel` from `lo` V to `hi` V in `step` V increments."""

def channel_sweep(channel, voltages, settle_ms=100, readback=True) -> list:
    """Drive `channel` through an explicit list of voltages."""
