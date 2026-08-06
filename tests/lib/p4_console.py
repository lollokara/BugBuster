"""P4 serial-console driver.

The P4 exposes a CLI on its USB-serial-JTAG console. Some capabilities are
reachable ONLY here -- notably `odr` / `voltodr`, which reprogram both ADAQs
over SPI and therefore require acquisition to be stopped first, since the
capture task holds the bus.

Two details that are easy to get wrong and cost a bench session each:

* The prompt is "daq> ", not "p4> " -- see
  Firmware/DAQ_HAT/ESP32P4/src/cli/cli.c:3460.
* The console port CANNOT be identified by VID/PID or by name. The S3
  mainboard and the P4 both enumerate as 0x303A CDC devices with
  indistinguishable descriptions, so the only reliable discovery is to open
  each candidate and ask which one answers with the prompt. That is what
  find_p4_port() does, and it is why the original bench tool probed rather
  than matched.
"""
from __future__ import annotations

import glob
import time

# The P4 REPL prints "daq> " (Firmware/DAQ_HAT/ESP32P4/src/cli/cli.c:3460),
# NOT "p4> ". Getting this wrong makes every cmd() time out.
PROMPT = "daq> "


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

    def reset_input_buffer(self):
        pass

    def close(self):
        pass


class P4Console:
    def __init__(self, port, baud: int = 115200, timeout: float = 1.0, _serial=None):
        self.port = port
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
            # Do NOT let pyserial assert DTR/RTS. On the P4's USB-Serial-JTAG
            # those lines drive the reset/boot straps, so a plain
            # serial.Serial(port, baud) REBOOTS THE BOARD on open (and again on
            # close). That silently kills any in-flight USB vendor-bulk stream
            # and resets the DSP integrators, which makes back-to-back
            # measurements disagree for no visible reason. They must be cleared
            # before open(), hence the unconfigured-then-open dance.
            self.ser = serial.Serial()
            self.ser.port = port
            self.ser.baudrate = baud
            self.ser.timeout = timeout
            self.ser.dtr = False
            self.ser.rts = False
            self.ser.open()
        except Exception as exc:
            raise P4ConsoleUnavailable("cannot open %s — %s" % (port, exc)) from exc
        # The USB-serial-JTAG link needs a moment after open, and whatever the
        # device printed before we attached is not ours to parse.
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def cmd(self, text: str, deadline: float = 8.0) -> str:
        """Send one command, return everything up to the next prompt.

        Raises TimeoutError only when NOTHING arrived. If output arrived but no
        prompt did, the output is returned: a previous read may already have
        consumed the prompt, and failing there would be a false alarm.
        """
        self.ser.reset_input_buffer()
        self.ser.write((text + "\r\n").encode())
        buf = ""
        end = time.monotonic() + deadline
        while time.monotonic() < end:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk.decode("utf8", "replace")
                if PROMPT.rstrip() in buf:
                    break
            elif buf:
                # Quiet for a full read timeout with data already in hand.
                break
            else:
                time.sleep(0.01)

        if not buf:
            raise TimeoutError("no prompt within %.1fs after %r" % (deadline, text))

        out = buf.split(PROMPT)[0]
        lines = out.splitlines()
        # Drop the device's echo of the command we just sent.
        if lines and lines[0].strip() == text.strip():
            lines = lines[1:]
        return "\n".join(lines)

    def alive(self) -> bool:
        """True when the device answers a bare newline with its prompt."""
        try:
            self.ser.reset_input_buffer()
            self.ser.write(b"\r\n")
            end = time.monotonic() + 2.0
            buf = ""
            while time.monotonic() < end:
                chunk = self.ser.read(4096)
                if chunk:
                    buf += chunk.decode("utf8", "replace")
                    if PROMPT.rstrip() in buf:
                        return True
                else:
                    time.sleep(0.01)
            return False
        except Exception:
            return False

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


def candidate_ports():
    """Every port that could plausibly be the P4 console, in probe order.

    POSIX exposes usable device nodes by glob; Windows has no such namespace,
    so COM ports have to come from pyserial's enumeration. Returning [] rather
    than raising keeps find_p4_port() a pure "not found" on a machine without
    pyserial.
    """
    posix = sorted(glob.glob("/dev/cu.usbmodem*")) + sorted(glob.glob("/dev/ttyACM*"))
    if posix:
        return posix
    try:
        from serial.tools import list_ports
    except ImportError:
        return []
    # 0x303A is Espressif; the S3 and the P4 are indistinguishable by descriptor
    # (see module docstring), so VID only orders the probe, it cannot select.
    ports = list(list_ports.comports())
    ports.sort(key=lambda p: (p.vid != 0x303A, p.device))
    return [p.device for p in ports]


def find_p4_port():
    """Return the port whose device answers with the P4 prompt, else None.

    Probing is the ONLY reliable discovery -- see the module docstring.
    """
    for port in candidate_ports():
        try:
            con = P4Console(port, timeout=0.6)
        except Exception:
            continue
        try:
            if con.alive():
                return port
        finally:
            con.close()
    return None
