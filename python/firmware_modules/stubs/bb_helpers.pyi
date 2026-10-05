"""Small helpers for channel ramps and sweeps (``bb_helpers.py`` on the device).

Frozen into the firmware: ``import bb_helpers``. The ramp and sweep helpers
switch the channel to voltage output and return it to high impedance when done.

Example:
    import bb_helpers
    for volts, readback in bb_helpers.dac_ramp(0, 0.0, 3.0, 1.0):
        print(volts, readback)
"""

def settle(ms: int) -> None:
    """Wait ``ms`` milliseconds (a thin wrapper over ``bugbuster.sleep``).

    Args:
        ms: Delay in milliseconds, 0 or more.

    Raises:
        ValueError: ``ms`` is negative.
        KeyboardInterrupt: the script was stopped during the wait.

    Example:
        bb_helpers.settle(200)
    """

def dac_ramp(channel: int, lo: float, hi: float, step: float, settle_ms: int = 50) -> list[tuple[float, float]]:
    """Ramp a channel's DAC from ``lo`` to ``hi`` volts and read each step back.

    Sets the channel to ``FUNC_VOUT``, then for each voltage drives it, waits
    ``settle_ms`` and reads the ADC. Finishes by returning the channel to
    ``FUNC_HIGH_IMP``.

    Args:
        channel: Channel index, 0-3.
        lo: Start voltage in volts (0 to 12 V unipolar).
        hi: End voltage in volts, included.
        step: Increment in volts; must be greater than 0 (a zero or negative step never ends).
        settle_ms: Wait after each step in milliseconds. Default 50.

    Returns:
        A list of ``(set_volts, readback_volts)`` tuples, one per step.

    Raises:
        ValueError: ``channel`` is outside 0-3.
        OSError: the channel is owned by another client, or the hardware refuses.

    Safety:
        Drives the IO terminal of the channel across the whole range.

    Example:
        results = bb_helpers.dac_ramp(0, 0.0, 5.0, 1.0)
        for v, rb in results:
            print("set=%.2f  read=%.5f" % (v, rb))
    """

def channel_sweep(channel: int, voltages: list[float], settle_ms: int = 100, readback: bool = True) -> list:
    """Drive a channel through an explicit list of voltages.

    Sets the channel to ``FUNC_VOUT``, steps through ``voltages`` and returns the
    channel to ``FUNC_HIGH_IMP`` at the end.

    Args:
        channel: Channel index, 0-3.
        voltages: Iterable of voltages in volts.
        settle_ms: Wait after each step in milliseconds. Default 100.
        readback: True (default) reads the ADC after each step.

    Returns:
        With ``readback=True`` a list of ``(target_volts, readback_volts)``
        tuples; otherwise a list of the target voltages.

    Raises:
        ValueError: ``channel`` is outside 0-3.
        OSError: the channel is owned by another client, or the hardware refuses.

    Safety:
        Drives the IO terminal of the channel.

    Example:
        points = bb_helpers.channel_sweep(0, [0.0, 2.5, 5.0], settle_ms=200)
    """
