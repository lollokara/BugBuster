# bb_logging.py — BugBuster on-device logging helpers (frozen module)
#
# Provides formatted log output using print() with level prefix.
# Import with: import bb_logging
#
# These run on-device without VFS — compiled as frozen .mpy bytecode.
#
# Usage:
#   import bb_logging
#   bb_logging.info('Starting sweep')
#   bb_logging.warn('Voltage above threshold')
#   bb_logging.error('Sensor not responding')

import bugbuster


def _ts():
    """Return a timestamp prefix string using the bugbuster tick counter."""
    try:
        ms = bugbuster.ticks_ms()
        return '[%10d]' % ms
    except AttributeError:
        return '[----------]'


def info(msg):
    """Print an INFO-level message with timestamp."""
    print('%s INFO  %s' % (_ts(), msg))


def warn(msg):
    """Print a WARN-level message with timestamp."""
    print('%s WARN  %s' % (_ts(), msg))


def error(msg):
    """Print an ERROR-level message with timestamp."""
    print('%s ERROR %s' % (_ts(), msg))
