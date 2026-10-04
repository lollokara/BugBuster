"""Timestamped log helpers (``bb_logging.py`` on the device).

Each call writes one line to the script log and console through
``bugbuster.log``, prefixed with the tick timestamp and level, for example
``[     12345] INFO  starting``. Frozen into the firmware: ``import bb_logging``.

Example:
    import bb_logging
    bb_logging.info("starting sweep")
    bb_logging.warn("supply sagging")
"""

def debug(msg: object) -> None:
    """Log a DEBUG-level line (level 'D') with a timestamp.

    Args:
        msg: Text to log.

    Example:
        bb_logging.debug("raw=%d" % raw)
    """

def info(msg: object) -> None:
    """Log an INFO-level line (level 'I') with a timestamp.

    Args:
        msg: Text to log.

    Example:
        bb_logging.info("ch0 = %.3f V" % volts)
    """

def warn(msg: object) -> None:
    """Log a WARN-level line (level 'W') with a timestamp.

    Args:
        msg: Text to log.

    Example:
        bb_logging.warn("voltage above threshold")
    """

def error(msg: object) -> None:
    """Log an ERROR-level line (level 'E') with a timestamp.

    Args:
        msg: Text to log.

    Example:
        bb_logging.error("sensor not responding")
    """
