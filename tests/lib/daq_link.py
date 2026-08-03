"""DaqLink — one DAQ stream client, two transports.

USB bulk and the WiFi TCP socket carry the IDENTICAL v2 frame format:
tcp_backend.c and usb_backend.c are two usb_transport_t implementations behind
one usb_stream.c. So DaqLink.usb() and DaqLink.tcp() return the same object and
every decoder, assertion and benchmark works unchanged across both. A
wire-format regression therefore cannot pass on USB while silently breaking
WiFi.

Safety: safe_state() is the cleanup contract. A test run that dies with V_DUT
enabled is a hazard to whatever is wired to the DUT terminals, so safe_state()
is best-effort and MUST NOT raise, even on a half-dead link.
"""
from __future__ import annotations

import socket
import struct
import time

from tests.lib import daq_proto as P
from tests.lib.daq_capture import Capture, FrameParser


class DaqLinkUnavailable(Exception):
    """The P4 DAQ HAT is not enumerated / not reachable."""


class UsbIO:
    """pyusb bulk transport for the P4's vendor interface."""

    def __init__(self, timeout_ms: int = 1000):
        try:
            import usb.core
            import usb.util
        except ImportError as exc:
            raise DaqLinkUnavailable(
                "pyusb is required: pip3 install pyusb (and: brew install libusb)"
            ) from exc

        dev = usb.core.find(idVendor=P.VID, idProduct=P.PID)
        if dev is None:
            raise DaqLinkUnavailable(
                "DAQ HAT not found on USB (%04X:%04X)" % (P.VID, P.PID))
        try:
            dev.set_configuration()
        except Exception:
            # Already configured, or macOS has it configured for us.
            pass
        self.dev = dev
        self.timeout_ms = timeout_ms
        self.closed = False

    def write(self, data: bytes) -> None:
        self.dev.write(P.EP_OUT, data, self.timeout_ms)

    def read(self, nbytes: int, timeout_ms: int) -> bytes:
        """Read up to nbytes. A timeout returns b"" rather than raising.

        A read timeout is NORMAL here -- it just means the device had nothing
        queued in that window -- so collect() must be able to spin through them.
        pyusb raises USBTimeoutError (a USBError subclass) for this since 1.0.2.
        Do NOT substring-match the message: libusb's text is "Operation timed
        out", which does not contain "timeout".
        """
        import usb.core
        try:
            return bytes(self.dev.read(P.EP_IN, nbytes, timeout_ms))
        except getattr(usb.core, "USBTimeoutError", ()):
            return b""
        except usb.core.USBError as exc:
            # Fallback for pyusb < 1.0.2, which has no USBTimeoutError. Cover
            # both spellings libusb backends use.
            text = str(exc).lower()
            if "timed out" in text or "timeout" in text:
                return b""
            raise

    def close(self) -> None:
        if self.closed:
            return
        try:
            import usb.util
            usb.util.dispose_resources(self.dev)
        except Exception:
            pass
        self.closed = True


class TcpIO:
    """TCP transport for the P4 softAP WiFi stream (same frame format)."""

    def __init__(self, host: str, port: int, timeout_ms: int = 1000):
        try:
            self.sock = socket.create_connection((host, port), timeout=timeout_ms / 1000.0)
        except OSError as exc:
            raise DaqLinkUnavailable("cannot reach %s:%d — %s" % (host, port, exc)) from exc
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.closed = False

    def write(self, data: bytes) -> None:
        self.sock.sendall(data)

    def read(self, nbytes: int, timeout_ms: int) -> bytes:
        self.sock.settimeout(timeout_ms / 1000.0)
        try:
            return self.sock.recv(nbytes)
        except socket.timeout:
            return b""

    def close(self) -> None:
        if self.closed:
            return
        try:
            self.sock.close()
        except OSError:
            pass
        self.closed = True


class FakeIO:
    """Test double: records writes, replays a canned list of reads."""

    def __init__(self, reads=None):
        self.writes = []
        self.reads = list(reads or [])
        self.closed = False

    def write(self, data: bytes) -> None:
        self.writes.append(bytes(data))

    def read(self, nbytes: int, timeout_ms: int) -> bytes:
        return self.reads.pop(0) if self.reads else b""

    def close(self) -> None:
        self.closed = True


# Lowest current limit the SMU accepts (SMU_ILIMIT_MIN_A in P4 config.h).
# safe_state() programs this alongside enable=0 so a later enable cannot
# inherit a high limit from whatever the last test set.
ILIMIT_SAFE_A = 0.05


class DaqLink:
    """Command + capture client for the DAQ HAT stream."""

    def __init__(self, io):
        self.io = io

    # -- construction --------------------------------------------------------

    @classmethod
    def usb(cls, timeout_ms: int = 1000) -> "DaqLink":
        return cls(UsbIO(timeout_ms))

    @classmethod
    def tcp(cls, host: str, port: int, timeout_ms: int = 1000) -> "DaqLink":
        return cls(TcpIO(host, port, timeout_ms))

    # -- raw ----------------------------------------------------------------

    def send(self, cmd: int, payload: bytes = b"") -> None:
        self.io.write(P.build_control_frame(cmd, payload))

    def read(self, chunk: int = 65536, timeout_ms: int = 1000) -> bytes:
        return self.io.read(chunk, timeout_ms)

    # -- commands ------------------------------------------------------------

    def start(self) -> None:
        self.send(P.CMD_START)

    def stop(self) -> None:
        self.send(P.CMD_STOP)

    def set_rate(self, current_sps: int, voltage_sps: int, decimation: int) -> None:
        self.send(P.CMD_SET_RATE,
                  struct.pack("<IIB3x", current_sps, voltage_sps, decimation))

    def set_range_lock(self, range_id: int) -> None:
        self.send(P.CMD_RANGE_LOCK, bytes([range_id]))

    def set_source(self, vdut: float, ilimit: float, enable: bool) -> None:
        self.send(P.CMD_SET_SOURCE,
                  struct.pack("<ffB3x", float(vdut), float(ilimit), 1 if enable else 0))

    def set_fft(self, nbins: int, source: int, window: int, enabled: bool) -> None:
        self.send(P.CMD_FFT_CONFIG,
                  struct.pack("<HBBB3x", nbins, source, window, 1 if enabled else 0))

    def arm(self, armed: bool, trig_logic: int = 0, pre_samples: int = 0) -> None:
        self.send(P.CMD_ARM,
                  struct.pack("<BBHI", 1 if armed else 0, trig_logic, 0, pre_samples))

    def reset_energy(self) -> None:
        self.send(P.CMD_RESET_ENERGY)

    def reset_stats(self) -> None:
        self.send(P.CMD_RESET_STATS)

    # -- capture -------------------------------------------------------------

    def drain(self, timeout_ms: int = 200) -> None:
        """Discard whatever is already in flight, so a capture starts clean."""
        deadline = time.monotonic() + timeout_ms / 1000.0
        while time.monotonic() < deadline:
            if not self.io.read(65536, 50):
                break

    def collect(self, seconds: float, chunk: int = 65536,
                timeout_ms: int = 1000) -> Capture:
        """Read for `seconds` and return the decoded Capture.

        seconds=0.0 drains whatever the transport has queued and returns — used
        by unit tests against FakeIO, where there is no real clock.
        """
        cap = Capture()
        parser = FrameParser(cap.on_record)
        t0 = time.monotonic()
        while True:
            data = self.io.read(chunk, timeout_ms)
            if data:
                cap.bytes_rx += len(data)
                parser.feed(data)
            elif seconds <= 0.0:
                break
            if seconds > 0.0 and time.monotonic() - t0 >= seconds:
                break
            if seconds <= 0.0 and not data:
                break
        cap.seconds = time.monotonic() - t0 if seconds > 0.0 else 0.0
        cap.resyncs = parser.resyncs
        return cap

    # -- safety --------------------------------------------------------------

    def safe_state(self) -> None:
        """Best-effort return to a safe state. Never raises.

        Order matters: stop the stream first so the device is not mid-emit,
        then kill the DUT supply, then release the range lock.
        """
        for fn in (self.stop,
                   lambda: self.set_source(0.0, ILIMIT_SAFE_A, False),
                   lambda: self.set_range_lock(P.RANGE_AUTO)):
            try:
                fn()
            except Exception:
                pass

    def close(self) -> None:
        try:
            self.io.close()
        except Exception:
            pass

    def __enter__(self) -> "DaqLink":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.safe_state()
        self.close()
