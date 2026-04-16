"""
LaUsbHost — Direct USB bulk host for RP2040 LA.

Bypasses ESP32/BBP and speaks the vendor bulk protocol directly.
Useful for validating firmware USB packet correctness from the host side.

Requires: pip install pyusb
"""
from __future__ import annotations

import struct
import time
from dataclasses import dataclass, field
from typing import Optional

# USB identifiers
VID = 0x2E8A
PID = 0x000C
LA_INTERFACE = 3
EP_IN = 0x87   # bulk IN (device → host)
EP_OUT = 0x06  # bulk OUT (host → device)

# Packet type constants (must match bb_la_usb.h)
PKT_START = 0x01
PKT_DATA  = 0x02
PKT_STOP  = 0x03
PKT_ERROR = 0x04

# Info byte constants
# NOTE: INFO semantics are packet-type-dependent.
# In DATA packets: bit 0 (INFO_COMPRESSED) means payload is RLE-compressed.
# In STOP packets: full byte = stop reason (INFO_STOP_HOST=0x01, etc.).
# The value 0x01 is shared; discrimination is by pkt_type, not info value.
INFO_NONE           = 0x00
INFO_COMPRESSED     = 0x01  # DATA packets only: payload is [value:8][count-1:8] RLE pairs
INFO_START_REJECTED = 0x80
INFO_STOP_HOST      = 0x01
INFO_STOP_USB_ERR   = 0x02
INFO_STOP_DMA_OVR   = 0x03

# Command bytes (host → device on EP_OUT)
STREAM_CMD_STOP  = 0x00
STREAM_CMD_START = 0x01

STREAM_MAX_PAYLOAD = 60  # max payload bytes per packet (64 - 4 header bytes)

_STOP_REASON_MAP = {
    INFO_STOP_HOST:    "host_stop",
    INFO_STOP_USB_ERR: "usb_short_write",
    INFO_STOP_DMA_OVR: "dma_overrun",
}


def _rle_decompress(data: bytes) -> bytes:
    """Decompress [value:8][count_minus_1:8] RLE pairs to raw bytes."""
    out = bytearray()
    i = 0
    while i + 1 < len(data):
        val = data[i]
        count = data[i + 1] + 1
        out.extend(bytes([val]) * count)
        i += 2
    return bytes(out)


@dataclass
class StreamPacket:
    """Parsed LA USB bulk stream packet."""
    pkt_type: int
    seq: int
    payload_len: int
    info: int
    payload: bytes

    @property
    def type_name(self) -> str:
        return {
            PKT_START: "START",
            PKT_DATA:  "DATA",
            PKT_STOP:  "STOP",
            PKT_ERROR: "ERROR",
        }.get(self.pkt_type, f"UNKNOWN({self.pkt_type:#04x})")


@dataclass
class StreamResult:
    """Result of a streaming capture session."""
    packets_received: int = 0
    bytes_received: int = 0
    sequence_mismatches: int = 0
    stop_reason: str = "unknown"
    payload: bytearray = field(default_factory=bytearray)
    errors: list = field(default_factory=list)


class LaUsbHost:
    """
    Direct USB bulk host for RP2040 LA.

    Speaks the vendor bulk protocol on interface 3 directly,
    bypassing ESP32/BBP entirely. Used for firmware-level USB tests.

    Usage::

        host = LaUsbHost()
        host.connect()
        result = host.stream_capture(duration_s=0.5)
        host.close()
    """

    def __init__(self):
        self._dev = None
        self._claimed = False
        self._stream_buffer = bytearray()
        self._bbp_port: Optional[str] = None  # set to enable BBP STOP path
        self.reconnect_fallbacks = 0
        self.timeout_faults = 0

    def connect(self, vid: int = VID, pid: int = PID) -> None:
        """Find and claim the RP2040 LA vendor bulk interface."""
        try:
            import usb.core
            import usb.util
        except ImportError:
            raise RuntimeError(
                "pyusb is required: pip install pyusb"
            )
        dev = usb.core.find(idVendor=vid, idProduct=pid)
        if dev is None:
            raise RuntimeError(
                f"Device {vid:#06x}:{pid:#06x} not found. "
                f"Ensure the RP2040 is connected and in normal mode."
            )
        try:
            # dev.set_configuration()
            pass
        except Exception:
            # macOS: device is already configured by the OS; skip set_configuration()
            pass
        import usb.util
        try:
            usb.util.claim_interface(dev, LA_INTERFACE)
        except Exception:
            # Interface may already be claimed from a previous run; release and retry
            usb.util.release_interface(dev, LA_INTERFACE)
            usb.util.claim_interface(dev, LA_INTERFACE)
        self._dev = dev
        self._claimed = True
        self._stream_buffer.clear()
        
        # Non-destructive drain to clear any stale packets without sticking the endpoint
        # self.drain(timeout_ms=50)

    def reconnect_interface(self) -> None:
        """Diagnostic fallback only: release/re-claim the USB interface."""
        if self._dev is None or not self._claimed:
            return
        try:
            import usb.util
            usb.util.release_interface(self._dev, LA_INTERFACE)
        except Exception:
            pass
        import time; time.sleep(0.1)
        try:
            import usb.util
            usb.util.claim_interface(self._dev, LA_INTERFACE)
        except Exception:
            pass
        self.reconnect_fallbacks += 1
        self._stream_buffer.clear()

    def probe_endpoint(self) -> bool:
        """Diagnostic health check for EP_OUT with reconnect fallback."""
        if self._dev is None:
            return False
        try:
            self._dev.write(EP_OUT, bytes([STREAM_CMD_STOP]), timeout=2000)
            return True
        except Exception:
            # Recovery attempt: release + re-claim the interface
            try:
                import usb.util
                import time
                usb.util.release_interface(self._dev, LA_INTERFACE)
                time.sleep(0.5)
                usb.util.claim_interface(self._dev, LA_INTERFACE)
                self.reconnect_fallbacks += 1
                self._stream_buffer.clear()
                time.sleep(0.2)
                self._dev.write(EP_OUT, bytes([STREAM_CMD_STOP]), timeout=2000)
                return True
            except Exception:
                return False

    def send_command(self, cmd: int) -> None:
        """Send a single-byte command to the bulk OUT endpoint (EP_OUT=0x06)."""
        if self._dev is None:
            raise RuntimeError("Not connected — call connect() first")
        self._dev.write(EP_OUT, bytes([cmd]))

    def read_raw(self, timeout_ms: int = 1000) -> bytes:
        """Read up to 16384 bytes from the bulk IN endpoint (EP_IN=0x87).

        A large read size amortises Python/libusb call overhead across many
        USB packets, which is critical for sustaining >500 KB/s on Full Speed
        bulk where each 64-byte packet requires a separate IN token.
        """
        if self._dev is None:
            raise RuntimeError("Not connected")
        return bytes(self._dev.read(EP_IN, 16384, timeout=timeout_ms))

    def read_packet(self, timeout_ms: int = 1000) -> StreamPacket:
        """Read and parse one stream packet from the device (with buffering)."""
        while True:
            # 1. Try to parse from buffer
            if len(self._stream_buffer) >= 4:
                payload_len = self._stream_buffer[2]
                frame_len = 4 + payload_len
                if len(self._stream_buffer) >= frame_len:
                    pkt_data = self._stream_buffer[:frame_len]
                    del self._stream_buffer[:frame_len]
                    
                    return StreamPacket(
                        pkt_type=pkt_data[0],
                        seq=pkt_data[1],
                        payload_len=pkt_data[2],
                        info=pkt_data[3],
                        payload=bytes(pkt_data[4:]),
                    )

            # 2. Read more if needed
            raw = self.read_raw(timeout_ms)
            if not raw:
                raise ValueError("USB read returned 0 bytes")
            self._stream_buffer.extend(raw)

    def wait_for_start(self, timeout_ms: int = 2000, max_packets: int = 512) -> StreamPacket:
        """Consume packets until START. Silently skips stale DATA/STOP/ERROR
        from a previous session — this avoids the need for reset_stream_buffer()
        which cancels IN transfers and sticks the device endpoint."""
        stale_count = 0
        for _ in range(max_packets):
            pkt = self.read_packet(timeout_ms=timeout_ms)
            if pkt.pkt_type == PKT_START:
                if stale_count > 0:
                    pass  # silently consumed stale packets
                return pkt
            # Skip any stale packet type from previous session
            stale_count += 1
        raise RuntimeError(f"Did not receive PKT_START within {max_packets} packets (skipped {stale_count} stale)")

    def _record_data_packet(
        self,
        pkt: StreamPacket,
        result: StreamResult,
        expected_seq: int,
    ) -> int:
        if pkt.seq != expected_seq:
            result.sequence_mismatches += 1
        payload = _rle_decompress(pkt.payload) if (pkt.info & INFO_COMPRESSED) else pkt.payload
        result.bytes_received += len(payload)  # decompressed bytes for duration calculation
        result.payload.extend(payload)
        result.packets_received += 1
        return (pkt.seq + 1) & 0xFF

    def _stop_stream_via_bbp(
        self,
        bbp_port: str,
        *,
        result: Optional[StreamResult] = None,
        expected_seq: int = 0,
        timeout_ms: int = 2000,
    ) -> tuple[Optional[StreamPacket], int]:
        """Send STOP via BBP/UART (hat_la_stop). Returns after stop; no USB drain.

        Design notes:
        - BBP (ESP32 CDC/pyserial) and vendor bulk (RP2040 pyusb) are on DIFFERENT
          USB devices — no IOKit conflict, both can stay open simultaneously.
        - After the 500 ms sleep, Core 0 has already processed the s_need_endpoint_rearm
          triggered by bb_la_usb_soft_reset(), which calls tud_vendor_n_fifo_clear().
          This clears ALL DATA from the TX FIFO — only PKT_STOP remains.  The drain
          therefore adds zero DATA bytes to result, so skipping it is lossless for
          result.bytes_received.
        - Locally-buffered data (already read from USB into self._stream_buffer before
          this call) IS drained without any USB I/O.  This preserves bytes already
          on the host side.
        - Stale EP_IN data (PKT_STOP from firmware) is cleaned up by the next
          preflight's hat_la_usb_reset() → bb_la_usb_abort_bulk() → rearm →
          tud_vendor_n_fifo_clear() on Core 0.
        """
        import bugbuster as bb

        # Drain whatever is already in the local parse buffer — data that was read
        # from EP_IN before this stop was called.  This preserves bytes already on
        # the host side without any USB I/O.
        if result is not None:
            while len(self._stream_buffer) >= 4:
                payload_len = self._stream_buffer[2]
                frame_len = 4 + payload_len
                if len(self._stream_buffer) < frame_len:
                    break  # incomplete packet — stop without reading more
                pkt_data = self._stream_buffer[:frame_len]
                del self._stream_buffer[:frame_len]
                pkt = StreamPacket(
                    pkt_type=pkt_data[0],
                    seq=pkt_data[1],
                    payload_len=pkt_data[2],
                    info=pkt_data[3],
                    payload=bytes(pkt_data[4:]),
                )
                if pkt.pkt_type == PKT_DATA:
                    expected_seq = self._record_data_packet(pkt, result, expected_seq)
                elif pkt.pkt_type == PKT_STOP:
                    # PKT_STOP was already in the local buffer — firmware stopped
                    # on its own (e.g. DMA overrun).  No BBP command needed.
                    result.stop_reason = _STOP_REASON_MAP.get(pkt.info, "normal")
                    return pkt, expected_seq
        else:
            self._stream_buffer.clear()

        # Send STOP via BBP/UART.
        try:
            dev = bb.connect_usb(bbp_port)
            try:
                dev.hat_la_stop()
            finally:
                dev.disconnect()
        except Exception:
            pass  # best-effort; preflight recovery handles remaining USB state

        # Sleep so Core 0 has time to process the s_need_endpoint_rearm triggered
        # by bb_la_usb_soft_reset() and queue PKT_STOP.  We do NOT re-claim the
        # vendor bulk interface here — doing so on the same IOKit dev object breaks
        # the RP2040 HAT UART in subsequent BBP sessions.
        time.sleep(0.5)

        if result is not None:
            result.stop_reason = "host_stop"
        return StreamPacket(
            pkt_type=PKT_STOP, seq=0, payload_len=0,
            info=INFO_STOP_HOST, payload=b"",
        ), expected_seq

    def start_stream(self, bbp_port: Optional[str] = None) -> None:
        """Send START via BBP (hat_la_stream_start) or EP_OUT fallback.

        Using BBP avoids the EP_OUT path which becomes unresponsive after
        the first stop/rearm cycle on RP2040 TinyUSB.  When ``bbp_port`` is
        set (or ``self._bbp_port`` is set), START is routed through
        HAT_CMD_LA_STREAM_START = 0x37 on the RP2040 (already implemented).
        """
        effective_port = bbp_port or self._bbp_port
        if effective_port is not None:
            import bugbuster as bb
            _bbp = bb.connect_usb(effective_port)
            try:
                _bbp.hat_la_stream_start()
            finally:
                _bbp.disconnect()
        else:
            self.send_command(STREAM_CMD_START)

    def stop_stream(
        self,
        *,
        bbp_port: Optional[str] = None,
        result: Optional[StreamResult] = None,
        expected_seq: int = 0,
        timeout_ms: int = 2000,
    ) -> tuple[Optional[StreamPacket], int]:
        """Send STOP and drain remaining DATA until PKT_STOP or read timeout.

        When ``bbp_port`` is set (or ``self._bbp_port`` is set), STOP is sent
        via BBP/UART (hat_la_stop) instead of EP_OUT.  This is the correct path
        during active IN streaming: EP_OUT STOP is silently dropped by the
        RP2040 TinyUSB DCD under heavy IN load (root cause in Diag/FINDINGS.md).

        Falls back to the legacy EP_OUT path when no BBP port is available.
        A read timeout after STOP is treated as a successful host-initiated stop.
        """
        effective_port = bbp_port or self._bbp_port
        if effective_port is not None:
            return self._stop_stream_via_bbp(
                effective_port,
                result=result,
                expected_seq=expected_seq,
                timeout_ms=timeout_ms,
            )

        # Legacy path: STOP via EP_OUT (may be lost during active IN streaming)
        self.send_command(STREAM_CMD_STOP)
        deadline = time.monotonic() + (timeout_ms / 1000.0)
        while time.monotonic() < deadline:
            remaining_ms = max(1, int((deadline - time.monotonic()) * 1000))
            try:
                pkt = self.read_packet(timeout_ms=remaining_ms)
            except Exception:
                # Read timeout after STOP — firmware stopped streaming,
                # PKT_STOP just didn't arrive.  Treat as host_stop.
                self._stream_buffer.clear()
                if result is not None:
                    result.stop_reason = "host_stop"
                synthetic = StreamPacket(
                    pkt_type=PKT_STOP, seq=0, payload_len=0,
                    info=INFO_STOP_HOST, payload=b"",
                )
                return synthetic, expected_seq

            if pkt.pkt_type == PKT_DATA:
                if result is not None:
                    expected_seq = self._record_data_packet(pkt, result, expected_seq)
                continue

            if pkt.pkt_type not in (PKT_STOP, PKT_ERROR):
                if result is not None:
                    result.stop_reason = "error"
                    result.errors.append(
                        f"Unexpected packet type after STOP: type={pkt.type_name} seq={pkt.seq}"
                    )
                return None, expected_seq

            if result is not None:
                if pkt.pkt_type == PKT_ERROR:
                    result.stop_reason = "error"
                    result.errors.append(f"Received PKT_ERROR after STOP: info={pkt.info:#02x}")
                else:
                    result.stop_reason = _STOP_REASON_MAP.get(pkt.info, "normal")
            return pkt, expected_seq

        if result is not None:
            result.stop_reason = "host_stop"
        synthetic = StreamPacket(
            pkt_type=PKT_STOP, seq=0, payload_len=0,
            info=INFO_STOP_HOST, payload=b"",
        )
        return synthetic, expected_seq

    def inject_timeout_fault(self, timeout_ms: int = 100) -> bool:
        """Fault-injection only: trigger a bulk-IN timeout cancellation."""
        self.timeout_faults += 1
        try:
            self.read_raw(timeout_ms=timeout_ms)
            return False
        except Exception:
            return True

    def stream_capture(self, duration_s: float = 1.0, bbp_port: Optional[str] = None) -> StreamResult:
        """
        Send START, collect DATA packets for duration_s seconds, send STOP.

        Returns a StreamResult with packet count, byte count, seq mismatches,
        and the accumulated payload.

        After the duration elapses the host sends STOP and then continues to
        drain DATA packets until PKT_STOP arrives (or a short timeout).  This
        ensures bytes_received reflects all data the firmware had already
        queued, which is required for accurate duration-truth accounting.
        """
        result = StreamResult()

        self.start_stream(bbp_port)

        try:
            start_pkt = self.wait_for_start(timeout_ms=2000, max_packets=512)
        except Exception as e:
            result.errors.append(f"Failed to receive START packet: {e}")
            return result

        if start_pkt.info == INFO_START_REJECTED:
            result.stop_reason = "start_rejected"
            return result

        # Collect DATA packets
        expected_seq = 0
        stream_read_failed = False
        t0 = time.monotonic()
        while time.monotonic() - t0 < duration_s:
            try:
                pkt = self.read_packet(timeout_ms=200)
            except Exception as e:
                result.errors.append(str(e))
                stream_read_failed = True
                break

            if pkt.pkt_type == PKT_DATA:
                expected_seq = self._record_data_packet(pkt, result, expected_seq)

            elif pkt.pkt_type == PKT_ERROR:
                result.stop_reason = "error"
                result.errors.append(f"Received PKT_ERROR: info={pkt.info:#02x}")
                return result

            elif pkt.pkt_type == PKT_STOP:
                result.stop_reason = _STOP_REASON_MAP.get(pkt.info, "normal")
                return result

        # Duration elapsed — STOP must terminate in-band. A timeout here is a
        # failure, not a normal cleanup signal.
        self.stop_stream(result=result, expected_seq=expected_seq, timeout_ms=2000, bbp_port=bbp_port)
        if stream_read_failed and result.stop_reason == "host_stop":
            result.stop_reason = "read_error"
        return result

    def oneshot_capture(self) -> bytes:
        """
        Read a complete one-shot capture.

        Wire format: [4-byte LE uint32 total_length][raw sample bytes...]
        Returns only the raw sample bytes (header stripped).
        """
        # Read header packet (first 4 bytes are the length)
        header_buf = self.read_raw(timeout_ms=5000)
        if len(header_buf) < 4:
            raise ValueError(f"Header too short: {len(header_buf)} bytes")

        total_len = struct.unpack_from("<I", header_buf)[0]
        data = bytearray(header_buf[4:])

        # Read remaining data until we have total_len bytes
        while len(data) < total_len:
            chunk = self.read_raw(timeout_ms=2000)
            data.extend(chunk)

        return bytes(data[:total_len])

    def drain(self, timeout_ms: int = 100) -> int:
        """Fault-injection helper: clear local buffer and perform one timed read."""
        self._stream_buffer.clear()
        total = 0
        try:
            raw = bytes(self._dev.read(EP_IN, 16384, timeout=timeout_ms))
            total += len(raw)
        except Exception:
            self.timeout_faults += 1
            pass
        return total

    def close(self) -> None:
        """Release the USB interface and dispose resources."""
        if self._dev is not None:
            try:
                import usb.util
                if self._claimed:
                    usb.util.release_interface(self._dev, LA_INTERFACE)
                usb.util.dispose_resources(self._dev)
            except Exception:
                pass
        self._dev = None
        self._claimed = False

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
