"""P4 serial-console driver.

The P4 exposes a CLI on its USB-serial-JTAG console. Some capabilities are
reachable ONLY here -- notably `odr` / `voltodr`, which reprogram both ADAQs
over SPI and therefore require acquisition to be stopped first, since the
capture task holds the bus.
"""
from __future__ import annotations

import time

PROMPT = "p4> "


class P4ConsoleUnavailable(Exception):
    """No P4 serial console could be opened."""


class FakeSerial:
    """Test double for pyserial.Serial."""

    def __init__(self, responses=None):
        self.written = []
        self.responses = list(responses or [])

    def write(self, data):
        self.written.append(bytes(data))

    def read(self, n=1):
        if not self.responses:
            return b""
        chunk = self.responses.pop(0)
        return chunk.encode() if isinstance(chunk, str) else chunk

    def close(self):
        pass


def find_p4_port():
    """Best-effort autodetect of the P4 console port."""
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    for p in list_ports.comports():
        # The P4 enumerates its console under the Espressif USB-serial-JTAG VID.
        if p.vid == 0x303A and "usbmodem" in (p.device or ""):
            return p.device
    return None


class P4Console:
    def __init__(self, port, baud: int = 115200, timeout: float = 1.0, _serial=None):
        if _serial is not None:
            self.ser = _serial
            return
        try:
            import serial
        except ImportError as exc:
            raise P4ConsoleUnavailable("pyserial is required") from exc
        if not port:
            raise P4ConsoleUnavailable("no P4 console port given or detected")
        try:
            self.ser = serial.Serial(port, baud, timeout=timeout)
        except Exception as exc:
            raise P4ConsoleUnavailable("cannot open %s — %s" % (port, exc)) from exc

    def cmd(self, text: str, deadline: float = 5.0) -> str:
        """Send one command, return everything up to the next prompt."""
        self.ser.write((text + "\r\n").encode())
        buf = ""
        end = time.monotonic() + deadline
        while time.monotonic() < end:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk.decode(errors="replace")
                if PROMPT in buf:
                    out = buf.split(PROMPT)[0]
                    # Drop the device's echo of the command we just sent.
                    lines = out.splitlines()
                    if lines and lines[0].strip() == text.strip():
                        lines = lines[1:]
                    return "\n".join(lines)
            else:
                time.sleep(0.01)
        raise TimeoutError("no prompt within %.1fs after %r" % (deadline, text))

    def set_odr(self, ratio: int, deadline: float = 10.0) -> None:
        """Reprogram the ADC oversampling ratio.

        `odr` is an oversampling RATIO, not a rate: per-channel SPS is
        8192000 / ratio. Acquisition must be stopped first because the capture
        task holds the SPI bus.
        """
        self.cmd("fast off", deadline=deadline)
        self.cmd("odr %d" % ratio, deadline=deadline)
        self.cmd("fast on", deadline=deadline)

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass

    def __enter__(self) -> "P4Console":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()
