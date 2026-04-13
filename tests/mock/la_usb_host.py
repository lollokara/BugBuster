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
INFO_NONE           = 0x00
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
            dev.set_configuration()
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

    def send_command(self, cmd: int) -> None:
        """Send a single-byte command to the bulk OUT endpoint (EP_OUT=0x06)."""
        if self._dev is None:
            raise RuntimeError("Not connected — call connect() first")
        try:
            self._dev.write(EP_OUT, bytes([cmd]))
        except Exception:
            try:
                import usb.util
                usb.util.clear_halt(self._dev, EP_OUT)
            except Exception:
                pass
            raise

    def read_raw(self, timeout_ms: int = 1000) -> bytes:
        """Read up to 16384 bytes from the bulk IN endpoint (EP_IN=0x87).

        A large read size amortises Python/libusb call overhead across many
        USB packets, which is critical for sustaining >500 KB/s on Full Speed
        bulk where each 64-byte packet requires a separate IN token.
        """
        if self._dev is None:
            raise RuntimeError("Not connected")
        try:
            return bytes(self._dev.read(EP_IN, 16384, timeout=timeout_ms))
        except Exception:
            try:
                import usb.util
                usb.util.clear_halt(self._dev, EP_IN)
            except Exception:
                pass
            raise

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

    def stream_capture(self, duration_s: float = 1.0) -> StreamResult:
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

        self.send_command(STREAM_CMD_START)

        # Expect START packet — skip stale DATA/STOP left over from a
        # previous stream (the firmware no longer calls write_clear on
        # START so residual packets may still be in the USB FIFO).
        start_pkt = None
        try:
            for _ in range(512):
                pkt = self.read_packet(timeout_ms=2000)
                if pkt.pkt_type == PKT_START:
                    start_pkt = pkt
                    break
                elif pkt.pkt_type == PKT_ERROR:
                    result.errors.append(
                        f"Received PKT_ERROR while waiting for START: info={pkt.info:#02x}"
                    )
                    return result
        except Exception as e:
            result.errors.append(f"Failed to receive START packet: {e}")
            return result

        if start_pkt is None:
            result.errors.append("Did not receive PKT_START within 512 packets")
            return result

        if start_pkt.info == INFO_START_REJECTED:
            result.stop_reason = "start_rejected"
            return result

        # Collect DATA packets
        expected_seq = 0
        t0 = time.monotonic()
        while time.monotonic() - t0 < duration_s:
            try:
                pkt = self.read_packet(timeout_ms=200)
            except Exception as e:
                result.errors.append(str(e))
                break

            if pkt.pkt_type == PKT_DATA:
                if pkt.seq != expected_seq:
                    result.sequence_mismatches += 1
                expected_seq = (pkt.seq + 1) & 0xFF
                result.bytes_received += pkt.payload_len
                result.payload.extend(pkt.payload)
                result.packets_received += 1

            elif pkt.pkt_type == PKT_ERROR:
                result.stop_reason = "error"
                result.errors.append(f"Received PKT_ERROR: info={pkt.info:#02x}")
                return result

            elif pkt.pkt_type == PKT_STOP:
                result.stop_reason = _STOP_REASON_MAP.get(pkt.info, "normal")
                return result

        # Duration elapsed — send STOP, then drain any DATA the firmware had
        # already queued so bytes_received is accurate for duration-truth tests.
        self.send_command(STREAM_CMD_STOP)
        result.stop_reason = "host_stop"
        for _ in range(8192):
            try:
                pkt = self.read_packet(timeout_ms=300)
            except Exception:
                break  # timeout = no more packets
            if pkt.pkt_type == PKT_DATA:
                if pkt.seq != expected_seq:
                    result.sequence_mismatches += 1
                expected_seq = (pkt.seq + 1) & 0xFF
                result.bytes_received += pkt.payload_len
                result.payload.extend(pkt.payload)
                result.packets_received += 1
            elif pkt.pkt_type in (PKT_STOP, PKT_ERROR):
                result.stop_reason = _STOP_REASON_MAP.get(pkt.info, "normal")
                break
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

    def close(self) -> None:
        """Release the USB interface and dispose resources."""
        if self._dev is not None and self._claimed:
            try:
                import usb.util
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
