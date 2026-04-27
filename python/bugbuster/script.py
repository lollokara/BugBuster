"""
BugBuster — Host-side scripting ergonomics (V2-H).

Provides:
- ``ScriptSession`` — context manager for a persistent MicroPython VM session.
- ``on_device`` — decorator that captures host-defined function source and posts
  it to the device as a script eval. Return values are communicated via
  ``print(repr(value))`` in the device function body; the decorator parses the
  last log line.
- ``bb.script_session()`` — factory that returns ``ScriptSession(client=bb)``.

CLI entry point (``python -m bugbuster.script``):

    python -m bugbuster.script run <path.py> [--persist]
    python -m bugbuster.script logs [--tail]
    python -m bugbuster.script autorun-set <name>
    python -m bugbuster.script autorun-disable
    python -m bugbuster.script reset
    python -m bugbuster.script status

Return-channel convention
-------------------------
When using ``@on_device``, the device function must return its value via
``print(repr(value))``.  The decorator drains the device log and applies
``ast.literal_eval`` to the **last non-empty line** to reconstruct the
Python object on the host.  This works for any type that ``repr()`` round-
trips safely: str, int, float, bool, None, list, dict, tuple.

Example::

    @on_device
    def read_temp():
        # runs on-device
        t = sensor.temperature()
        print(repr(t))

    temp = read_temp()   # returns float from device

Limitations:
- ``inspect.getsource()`` requires the function to be defined in a real
  source file — it does not work on lambdas or in interactive (REPL) sessions.
- The simulated test device logs the source snippet, not execution output;
  only a real device will execute the code and produce ``print()`` output.
"""

from __future__ import annotations

import ast
import inspect
import textwrap
import time
import warnings
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .client import BugBuster, ScriptStatusResult

# BugBusterWarning lives in client.py; import it here for re-export.
from .client import BugBusterWarning  # noqa: F401


# ---------------------------------------------------------------------------
# ScriptSession
# ---------------------------------------------------------------------------

class ScriptSession:
    """
    Context manager for a persistent on-device MicroPython VM session.

    Opens a persistent interpreter context on ``__enter__`` (by sending an
    empty ``script_eval(persist=True)`` to warm the VM) and tears it down
    on ``__exit__`` via ``script_reset()``.

    Typical usage::

        with bb.script_session() as s:
            s.eval("x = 5")
            logs = s.eval("print(x)")
            print(logs)     # "5\\n"
    """

    def __init__(self, client: "BugBuster"):
        self._client = client

    # ------------------------------------------------------------------
    # Context manager
    # ------------------------------------------------------------------

    def __enter__(self) -> "ScriptSession":
        # Warm the persistent VM with an empty eval
        self._client.script_eval("", persist=True)
        return self

    def __exit__(self, *_) -> None:
        self._client.script_reset()

    # ------------------------------------------------------------------
    # Public methods
    # ------------------------------------------------------------------

    def eval(self, src: str, log_drain: bool = True) -> str:
        """
        Submit *src* to the persistent VM and return log output.

        Parameters
        ----------
        src:
            Python source string to evaluate on-device.
        log_drain:
            If True (default), drain and return all pending log output after
            the eval.  Set False to skip draining if you only care about side
            effects.

        Returns
        -------
        str
            Concatenated log output drained from the device ring buffer.
            Returns an empty string if ``log_drain=False``.
        """
        self._client.script_eval(src, persist=True)
        if not log_drain:
            return ""
        return _drain_logs(self._client)

    def reset(self) -> None:
        """Tear down and re-initialise the persistent VM."""
        self._client.script_reset()
        # Re-warm the VM so further evals work
        self._client.script_eval("", persist=True)

    def status(self) -> "ScriptStatusResult":
        """Return the current device script engine status."""
        return self._client.script_status()


def _drain_logs(client: "BugBuster") -> str:
    """Drain the full log ring (may require multiple reads for long output)."""
    chunks = []
    while True:
        chunk = client.script_logs()
        if not chunk:
            break
        chunks.append(chunk)
    return "".join(chunks)


# ---------------------------------------------------------------------------
# @on_device decorator
# ---------------------------------------------------------------------------

def on_device(func):
    """
    Decorator that posts the decorated function's source to the device for
    on-device execution and parses the return value from the log output.

    The decorated function must output its return value by calling
    ``print(repr(value))`` as the **last statement**.  The decorator drains
    the device log and reconstructs the Python object via
    ``ast.literal_eval`` applied to the last non-empty log line.

    Example::

        @on_device
        def measure_adc(channel: int):
            import bb_devices
            v = bb_devices.adc_read(channel)
            print(repr(v))

        voltage = measure_adc(0)   # returns float

    Limitations:
    - The function must be defined in a real ``.py`` file (not interactively).
    - Only ``ast.literal_eval``-safe types can be returned (str, int, float,
      bool, None, list, dict, tuple).
    - The simulated test device does not execute code; it only logs the source.
    """
    # Capture source at decoration time (requires a real source file)
    raw_lines = inspect.getsource(func).splitlines(keepends=True)

    # Strip leading @on_device line
    stripped = []
    skip_decorator = True
    for line in raw_lines:
        if skip_decorator and line.lstrip().startswith("@on_device"):
            skip_decorator = False
            continue
        stripped.append(line)

    src = textwrap.dedent("".join(stripped))

    def wrapper(*args, **kwargs):
        # Retrieve the bound client — requires the decorated function to be
        # called on a namespace that provides _bb_client, or fall back to
        # the session singleton.
        client = _get_session_client()
        client.script_eval(src, persist=True)
        logs = _drain_logs(client)
        # Parse last non-empty line as the return value
        lines = [ln for ln in logs.splitlines() if ln.strip()]
        if not lines:
            return None
        try:
            return ast.literal_eval(lines[-1])
        except (ValueError, SyntaxError):
            return None

    wrapper.__name__ = func.__name__
    wrapper.__doc__  = func.__doc__
    return wrapper


def _get_session_client():
    """
    Return the active session client.

    Tries the MCP session singleton first; falls back gracefully.
    Raises ``RuntimeError`` if no client is reachable.
    """
    try:
        from bugbuster_mcp import session as _mcp_session
        return _mcp_session.get_client()
    except Exception:
        pass
    raise RuntimeError(
        "@on_device requires an active BugBuster session. "
        "Use within a ScriptSession context or configure an MCP session."
    )


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _build_parser():
    import argparse

    parser = argparse.ArgumentParser(
        prog="python -m bugbuster.script",
        description="BugBuster on-device scripting CLI (V2-H)",
    )

    # --- Common connection flags -------------------------------------------
    conn = parser.add_argument_group("connection (mutually exclusive)")
    mutex = conn.add_mutually_exclusive_group()
    mutex.add_argument("--host",  metavar="IP",   help="HTTP transport host IP")
    mutex.add_argument("--usb",   metavar="PORT", help="USB serial port path")

    parser.add_argument(
        "--token", metavar="TOKEN",
        help="Admin token (used with --host; auto-fetched over USB)",
    )

    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    # --- run ---------------------------------------------------------------
    sp_run = subparsers.add_parser("run", help="Eval a Python file on-device")
    sp_run.add_argument("path", help="Path to Python source file")
    sp_run.add_argument(
        "--persist", action="store_true",
        help="Eval in persistent mode (globals persist across evals)",
    )

    # --- logs --------------------------------------------------------------
    sp_logs = subparsers.add_parser("logs", help="Drain on-device script logs")
    sp_logs.add_argument(
        "--tail", action="store_true",
        help="Follow logs (poll every 250 ms); press Ctrl-C to stop",
    )

    # --- autorun-set -------------------------------------------------------
    sp_ar_set = subparsers.add_parser("autorun-set", help="Set the autorun script")
    sp_ar_set.add_argument("name", help="Script name (must already be uploaded)")

    # --- autorun-disable ---------------------------------------------------
    subparsers.add_parser("autorun-disable", help="Disable autorun")

    # --- reset -------------------------------------------------------------
    subparsers.add_parser("reset", help="Reset the persistent VM")

    # --- status ------------------------------------------------------------
    subparsers.add_parser("status", help="Pretty-print script engine status")

    return parser


def _make_client(args):
    """Construct and return a connected BugBuster client from CLI args."""
    from bugbuster.client import BugBuster
    from bugbuster.transport.usb  import USBTransport
    from bugbuster.transport.http import HTTPTransport

    if args.host:
        t = HTTPTransport(args.host)
        bb = BugBuster(t)
        bb.connect()
        if args.token:
            bb._admin_token = args.token
        return bb
    elif args.usb:
        t = USBTransport(args.usb)
        bb = BugBuster(t)
        bb.connect()
        return bb
    else:
        raise SystemExit(
            "error: specify --host <ip> or --usb <port> to connect to a device"
        )


def _cmd_run(bb, args) -> int:
    with open(args.path, "r", encoding="utf-8") as fh:
        src = fh.read()
    bb.script_eval(src, persist=args.persist)
    logs = _drain_logs(bb)
    if logs:
        print(logs, end="")
    return 0


def _cmd_logs(bb, args) -> int:
    if args.tail:
        try:
            while True:
                chunk = bb.script_logs()
                if chunk:
                    print(chunk, end="", flush=True)
                else:
                    time.sleep(0.25)
        except KeyboardInterrupt:
            return 0
    else:
        logs = _drain_logs(bb)
        if logs:
            print(logs, end="")
    return 0


def _cmd_autorun_set(bb, args) -> int:
    bb.script_autorun_enable(args.name)
    print(f"Autorun set to: {args.name}")
    return 0


def _cmd_autorun_disable(bb, args) -> int:
    bb.script_autorun_disable()
    print("Autorun disabled.")
    return 0


def _cmd_reset(bb, args) -> int:
    bb.script_reset()
    print("VM reset.")
    return 0


def _cmd_status(bb, args) -> int:
    st = bb.script_status()
    print(f"is_running:        {st.is_running}")
    print(f"script_id:         {st.script_id}")
    print(f"total_runs:        {st.total_runs}")
    print(f"total_errors:      {st.total_errors}")
    print(f"last_error:        {st.last_error!r}")
    print(f"mode:              {'PERSISTENT' if st.mode else 'EPHEMERAL'}")
    print(f"globals_bytes_est: {st.globals_bytes_est}")
    print(f"globals_count:     {st.globals_count}")
    print(f"auto_reset_count:  {st.auto_reset_count}")
    print(f"last_eval_at_ms:   {st.last_eval_at_ms}")
    print(f"idle_for_ms:       {st.idle_for_ms}")
    print(f"watermark_soft_hit:{st.watermark_soft_hit}")
    return 0


def main(argv=None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    bb = _make_client(args)
    try:
        dispatch = {
            "run":              _cmd_run,
            "logs":             _cmd_logs,
            "autorun-set":      _cmd_autorun_set,
            "autorun-disable":  _cmd_autorun_disable,
            "reset":            _cmd_reset,
            "status":           _cmd_status,
        }
        handler = dispatch.get(args.subcommand)
        if handler is None:
            parser.print_help()
            return 1
        return handler(bb, args)
    finally:
        bb.disconnect()


if __name__ == "__main__":
    import sys
    sys.exit(main())
