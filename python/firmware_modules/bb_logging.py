# bb_logging.py — BugBuster on-device logging helpers (frozen module)
#
# Provides formatted log output using bugbuster.log() with level prefix.
# Import with: import bb_logging
#
# These run on-device without VFS — compiled as frozen .mpy bytecode.
#
# Usage:
#   import bb_logging
#   bb_logging.debug('Reading raw samples')
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


def _emit(level, text):
    try:
        bugbuster.log(level, text)
    except AttributeError:
        print(text)


def debug(msg):
    """Log a DEBUG-level message with timestamp."""
    _emit('D', '%s DEBUG %s' % (_ts(), msg))


def info(msg):
    """Log an INFO-level message with timestamp."""
    _emit('I', '%s INFO  %s' % (_ts(), msg))


def warn(msg):
    """Log a WARN-level message with timestamp."""
    _emit('W', '%s WARN  %s' % (_ts(), msg))


def error(msg):
    """Log an ERROR-level message with timestamp."""
    _emit('E', '%s ERROR %s' % (_ts(), msg))
