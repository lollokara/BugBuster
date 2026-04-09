"""
USB Binary Transport for the BugBuster device.

Protocol overview:
  1. Open the CDC serial port.
  2. Send the 4-byte magic handshake (0xBB 0x42 0x55 0x47).
  3. Wait for the 8-byte response (magic + proto_ver + fw_major/minor/patch).
  4. All subsequent traffic is COBS-framed with a 0x00 delimiter.
  5. Each CMD frame carries a monotonically increasing 16-bit SEQ number.
     The device echoes SEQ in its RSP/ERR response so the host can match them.
  6. Unsolicited EVT frames are dispatched to registered callbacks.

See BugBusterProtocol.md §2–§5 for the complete wire specification.
"""

import threading
import queue
import struct
import logging
from typing import Callable, Optional

import serial  # pyserial

from ..protocol import HANDSHAKE_MAGIC, build_frame, parse_frame, ProtocolError
from ..constants import MsgType, ErrorCode

log = logging.getLogger(__name__)


class DeviceError(Exception):
    """Raised when the device returns an ERR message."""
    def __init__(self, code: int, seq: int):
        self.code = code
        self.seq  = seq
        name = ErrorCode(code).name if code in ErrorCode._value2member_map_ else f"0x{code:02X}"
        super().__init__(f"Device error {name} (seq={seq})")


class USBTransport:
    """
    Manages the USB CDC binary link to a BugBuster device.

    Usage::

        with USBTransport("/dev/ttyACM0") as t:
            proto_ver, fw_ver = t.firmware_version
            payload = t.send_command(CmdId.PING, struct.pack('<I', 0xDEAD))
    """

    DEFAULT_BAUDRATE = 921600
    DEFAULT_TIMEOUT  = 5.0       # seconds to wait for a response
    READ_CHUNK       = 512       # bytes to read per serial.read() call

    def __init__(
        self,
        port:     str,
        baudrate: int   = DEFAULT_BAUDRATE,
        timeout:  float = DEFAULT_TIMEOUT,
    ):
        self._port     = port
        self._baudrate = baudrate
        self._timeout  = timeout

        self._serial: Optional[serial.Serial] = None
        self._seq   = 0
        self._seq_lock = threading.Lock()

        # seq -> queue.Queue that receives the response payload (bytes) or an exception
        self._pending: dict[int, queue.Queue] = {}
        self._pending_lock = threading.Lock()

        # evt_id -> callable(payload: bytes)
        self._event_handlers: dict[int, Callable[[bytes], None]] = {}

        self._reader_thread: Optional[threading.Thread] = None
        self._running = False

        # Firmware info filled in after connect()
        self.proto_version = None
        self.fw_version    = None   # (major, minor, patch)

    # ------------------------------------------------------------------
    # Connection management
    # ------------------------------------------------------------------

    def connect(self) -> tuple[int, tuple[int, int, int]]:
        """
        Open the serial port, perform the BBP handshake, and start the
        background reader thread.

        Returns ``(proto_version, (fw_major, fw_minor, fw_patch))``.
        """
        self._serial = serial.Serial(
            self._port,
            self._baudrate,
            timeout=0.1,
            dsrdtr=False,
            rtscts=False,
            xonxoff=False,
        )
        # ESP32-S3 native USB CDC requires DTR high before it will send data.
        # Setting it explicitly after open avoids the macOS DTR-toggle issue
        # that caused the device to exit binary mode on previous designs.
        self._serial.dtr = True

        # Handshake strategy:
        #   0. Drain startup / prompt text after the DTR transition.
        #   1. Try a clean magic handshake first.
        #   2. If that fails, perform a binary-mode recovery (COBS delimiters +
        #      DISCONNECT) and retry.
        # We never call reset_input_buffer() because on macOS CDC devices
        # tcflush() does not reliably flush the kernel USB receive buffer.
        import time as _time

        def _attempt_handshake(settle_timeout: float) -> bytes:
            buf             = bytearray()
            magic_sent      = False
            last_rx         = _time.time()
            settle_deadline = _time.time() + settle_timeout
            deadline        = None
            settle_quiet_s  = 0.20

            while True:
                now = _time.time()

                if deadline is not None and now > deadline:
                    raise TimeoutError(buf.hex() if buf else "nothing")

                try:
                    chunk = self._serial.read(64)
                except serial.SerialException:
                    _time.sleep(0.02)
                    continue

                if chunk:
                    buf.extend(chunk)
                    last_rx = now

                idx = buf.find(HANDSHAKE_MAGIC)
                if idx != -1 and len(buf) >= idx + 8:
                    return bytes(buf[idx:idx + 8])

                if not magic_sent:
                    quiet_for = now - last_rx
                    if quiet_for >= settle_quiet_s or now > settle_deadline:
                        self._serial.write(HANDSHAKE_MAGIC)
                        self._serial.flush()
                        magic_sent = True
                        deadline   = _time.time() + self._timeout

        # Drain any startup noise after DTR rises.
        startup = bytearray()
        settle_until = _time.time() + 0.5
        while _time.time() < settle_until:
            try:
                chunk = self._serial.read(128)
            except serial.SerialException:
                break
            if chunk:
                startup.extend(chunk)
            else:
                _time.sleep(0.02)

        last_handshake_buf = startup.hex() if startup else "nothing"
        resp = None
        for attempt in range(2):
            try:
                resp = _attempt_handshake(settle_timeout=1.0 if attempt == 0 else 0.4)
                break
            except TimeoutError as exc:
                last_handshake_buf = str(exc)
                if attempt == 0:
                    # Recovery for a stale binary session: terminate any partial
                    # COBS frame, request DISCONNECT, then retry a clean magic.
                    try:
                        self._serial.write(b'\x00' * 4)
                        self._serial.write(build_frame(0, 0xFF))
                        self._serial.flush()
                        _time.sleep(0.3)
                        self._serial.read(256)
                        log.debug("Sent recovery DISCONNECT after failed handshake attempt")
                    except Exception:
                        pass
                else:
                    self._serial.close()
                    raise ConnectionError(
                        f"BBP handshake timed out — received: {last_handshake_buf}"
                    )

        self.proto_version = resp[4]
        self.fw_version    = (resp[5], resp[6], resp[7])
        log.info(
            "Connected to BugBuster fw=%d.%d.%d protocol=%d via %s",
            *self.fw_version, self.proto_version, self._port,
        )

        self._running = True
        self._reader_thread = threading.Thread(
            target=self._reader_loop, name="bbp-reader", daemon=True
        )
        self._reader_thread.start()

        return self.proto_version, self.fw_version

    def disconnect(self) -> None:
        """Gracefully exit binary mode and close the serial port."""
        self._running = False
        if self._serial and self._serial.is_open:
            try:
                # Send DISCONNECT command (0xFF) so device returns to CLI
                with self._seq_lock:
                    seq = self._seq
                    self._seq = (self._seq + 1) & 0xFFFF
                self._serial.write(build_frame(seq, 0xFF))
                self._serial.flush()
            except Exception:
                pass
            self._serial.close()

        if self._reader_thread and self._reader_thread.is_alive():
            self._reader_thread.join(timeout=2.0)

    # ------------------------------------------------------------------
    # Command execution
    # ------------------------------------------------------------------

    def send_command(self, cmd_id: int, payload: bytes = b'') -> bytes:
        """
        Send a CMD frame and block until the matching RSP arrives (or timeout).

        Returns the raw response payload bytes.
        Raises :class:`DeviceError` on ERR response, :class:`TimeoutError` on timeout.
        """
        with self._seq_lock:
            seq = self._seq
            self._seq = (self._seq + 1) & 0xFFFF

        resp_queue: queue.Queue = queue.Queue()
        with self._pending_lock:
            self._pending[seq] = resp_queue

        frame = build_frame(seq, cmd_id, payload)
        self._serial.write(frame)
        self._serial.flush()

        try:
            result = resp_queue.get(timeout=self._timeout)
        except queue.Empty:
            with self._pending_lock:
                self._pending.pop(seq, None)
            raise TimeoutError(
                f"No response for cmd=0x{cmd_id:02X} seq={seq} within {self._timeout}s"
            )

        if isinstance(result, Exception):
            raise result
        return result

    # ------------------------------------------------------------------
    # Event subscription
    # ------------------------------------------------------------------

    def on_event(self, evt_id: int, callback: Callable[[bytes], None]) -> None:
        """
        Register a callback for an unsolicited EVT frame.

        *evt_id* is the CMD_ID field in the EVT frame (e.g. CmdId.ADC_DATA_EVT).
        The callback receives the raw payload bytes and is called from the
        reader thread — keep it short or hand work off to another thread.
        """
        self._event_handlers[evt_id] = callback

    def remove_event(self, evt_id: int) -> None:
        self._event_handlers.pop(evt_id, None)

    # ------------------------------------------------------------------
    # Context-manager support
    # ------------------------------------------------------------------

    def __enter__(self) -> "USBTransport":
        self.connect()
        return self

    def __exit__(self, *_) -> None:
        self.disconnect()

    # ------------------------------------------------------------------
    # Background reader thread
    # ------------------------------------------------------------------

    def _reader_loop(self) -> None:
        """
        Continuously read bytes from the serial port, accumulate them into a
        buffer, and dispatch complete COBS frames as they arrive.
        """
        buf = bytearray()
        while self._running:
            try:
                chunk = self._serial.read(self.READ_CHUNK)
            except serial.SerialException as exc:
                # Spurious macOS "no data" exception — keep going
                log.debug("Serial transient: %s", exc)
                __import__('time').sleep(0.01)
                continue
            except Exception as exc:
                log.error("Serial read error: %s", exc)
                break

            if not chunk:
                continue

            buf.extend(chunk)

            # Each complete frame ends with 0x00
            if b'\x00' in buf:
                # Split buffer into complete frames and the remaining partial frame
                parts = buf.split(b'\x00')
                # The last part is the new buffer (stale data after the last 0x00)
                buf = bytearray(parts.pop())
                
                for frame_bytes in parts:
                    if frame_bytes:
                        self._dispatch_frame(bytes(frame_bytes))

    def _dispatch_frame(self, raw: bytes) -> None:
        try:
            msg_type, seq, cmd_id, payload = parse_frame(raw)
        except ProtocolError as exc:
            log.warning("Dropping malformed frame: %s", exc)
            return

        if msg_type in (MsgType.RSP, MsgType.ERR):
            with self._pending_lock:
                resp_queue = self._pending.pop(seq, None)
            if resp_queue is None:
                log.warning("Received unexpected RSP/ERR seq=%d cmd=0x%02X", seq, cmd_id)
                return
            if msg_type == MsgType.ERR:
                code = payload[0] if payload else 0xFF
                resp_queue.put(DeviceError(code, seq))
            else:
                resp_queue.put(payload)

        elif msg_type == MsgType.EVT:
            handler = self._event_handlers.get(cmd_id)
            if handler:
                try:
                    handler(payload)
                except Exception as exc:
                    log.error("Event handler for 0x%02X raised: %s", cmd_id, exc)

        else:
            log.debug("Unhandled msg_type=0x%02X seq=%d cmd=0x%02X", msg_type, seq, cmd_id)
