"""Inject the unsolicited BBP events the firmware pushes to the host.

Payload builders mirror the firmware byte for byte:
  ALERT_EVT      0x82  Firmware/ESP32/src/tasks.cpp     (fault monitor, payload[8])
  DIN_EVT        0x83  Firmware/bbp-protocol.md 7.4     (defined in bbp.h, never emitted)
  PCA_FAULT_EVT  0x84  Firmware/ESP32/src/main.cpp      (pcaFaultCallback, payload[6])
  HAT_LA_LOG_EVT 0xEC  Firmware/ESP32/src/hat/hat.cpp   (raw RP2040 log bytes, no NUL)

``EventInjector`` delivers them through ``SimulatedDevice.emit_event``;
``encode_event_frame`` produces the COBS wire frame for driving a real
``USBTransport`` reader.
"""

from __future__ import annotations

import itertools
import struct
from typing import Sequence, Union

from bugbuster.constants import CmdId, MsgType
from bugbuster.protocol import build_frame

LA_LOG_MAX = 255  # HAT UART frame length is a u8


def alert_payload(alert_status: int, supply_alert_status: int,
                  chan_alert: Sequence[int] = (0, 0, 0, 0)) -> bytes:
    if len(chan_alert) != 4:
        raise ValueError("chan_alert needs one entry per channel (4)")
    return struct.pack("<HH4B", alert_status & 0xFFFF, supply_alert_status & 0xFFFF,
                       *(c & 0xFF for c in chan_alert))


def din_payload(channel: int, state: bool, counter: int) -> bytes:
    return struct.pack("<BBI", channel & 0xFF, 1 if state else 0, counter & 0xFFFFFFFF)


def pca_fault_payload(fault_type: int, channel: int, timestamp_ms: int) -> bytes:
    return struct.pack("<BBI", fault_type & 0xFF, channel & 0xFF, timestamp_ms & 0xFFFFFFFF)


def la_log_payload(text: Union[str, bytes]) -> bytes:
    raw = text.encode("utf-8") if isinstance(text, str) else bytes(text)
    if not 0 < len(raw) <= LA_LOG_MAX:
        raise ValueError(f"LA log payload must be 1..{LA_LOG_MAX} bytes")
    return raw


def encode_event_frame(evt_id: int, payload: bytes, seq: int = 0) -> bytes:
    """COBS-encoded EVT frame with its 0x00 delimiter, as bbpSendEvent() writes it."""
    return build_frame(seq, int(evt_id), payload, msg_type=MsgType.EVT)


class EventInjector:
    """Push firmware-layout events from a SimulatedDevice to its transport."""

    def __init__(self, device):
        self._device = device
        self._seq = itertools.count()

    def raw(self, evt_id: int, payload: bytes) -> None:
        self._device.emit_event(int(evt_id), bytes(payload))

    def alert(self, alert_status: int, supply_alert_status: int,
              chan_alert: Sequence[int] = (0, 0, 0, 0)) -> None:
        self.raw(CmdId.ALERT_EVT, alert_payload(alert_status, supply_alert_status, chan_alert))

    def din(self, channel: int, state: bool, counter: int) -> None:
        self.raw(CmdId.DIN_EVT, din_payload(channel, state, counter))

    def pca_fault(self, fault_type: int, channel: int, timestamp_ms: int) -> None:
        self.raw(CmdId.PCA_FAULT_EVT, pca_fault_payload(fault_type, channel, timestamp_ms))

    def la_log(self, text: Union[str, bytes]) -> None:
        self.raw(CmdId.HAT_LA_LOG_EVT, la_log_payload(text))

    def frame(self, evt_id: int, payload: bytes) -> bytes:
        return encode_event_frame(evt_id, payload, next(self._seq))
