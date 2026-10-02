"""Unsolicited BBP events: ALERT_EVT, DIN_EVT, PCA_FAULT_EVT, HAT_LA_LOG_EVT.

Payload layouts are pinned against the firmware senders, then the client
callbacks (on_alert, on_fault, on_din_event, on_la_log) are driven through
the simulator and through a real USBTransport reader thread fed with wire
frames, including malformed, short and unregistered events.
"""

import queue
import re
import struct
import threading
import time

import pytest

import bugbuster as bb
from bugbuster.constants import CmdId, MsgType
from bugbuster.protocol import build_frame, cobs_encode, crc16_ccitt, parse_frame
from bugbuster.transport.usb import USBTransport
from tests.lib.srcread import REPO_ROOT, read_source
from tests.mock import EventInjector, SimulatedDevice, SimulatedHTTPTransport, SimulatedUSBTransport
from tests.mock.bbp_events import (
    alert_payload, din_payload, encode_event_frame, la_log_payload, pca_fault_payload,
)

BBP_H = read_source("Firmware/ESP32/src/bbp/bbp.h")


def _norm(s: str) -> str:
    return re.sub(r"\s+", "", s)


def _payload_assignments(src: str, start: str, end: str) -> dict[int, str]:
    i = src.index(start)
    block = src[i:src.index(end, i)]
    return {int(m.group(1)): _norm(m.group(2))
            for m in re.finditer(r"payload\[(\d+)\]\s*=\s*([^;]+);", block)}


# ---------------------------------------------------------------------------
# Firmware contract
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("define,member", [
    ("BBP_EVT_ALERT", CmdId.ALERT_EVT),
    ("BBP_EVT_DIN", CmdId.DIN_EVT),
    ("BBP_EVT_PCA_FAULT", CmdId.PCA_FAULT_EVT),
    ("BBP_EVT_LA_LOG", CmdId.HAT_LA_LOG_EVT),
])
def test_event_ids_match_bbp_h(define, member):
    m = re.search(rf"#define\s+{define}\s+0x([0-9A-Fa-f]+)", BBP_H)
    assert m, f"{define} missing from bbp.h"
    assert int(m.group(1), 16) == int(member)


def test_alert_payload_matches_tasks_cpp_byte_for_byte():
    src = read_source("Firmware/ESP32/src/tasks.cpp")
    got = _payload_assignments(src, "uint8_t payload[8];", "bbpSendEvent(BBP_EVT_ALERT")
    assert got == {
        0: "(uint8_t)(alertStatus&0xFF)", 1: "(uint8_t)(alertStatus>>8)",
        2: "(uint8_t)(supplyAlertStatus&0xFF)", 3: "(uint8_t)(supplyAlertStatus>>8)",
        4: "(uint8_t)(chanAlert[0]&0xFF)", 5: "(uint8_t)(chanAlert[1]&0xFF)",
        6: "(uint8_t)(chanAlert[2]&0xFF)", 7: "(uint8_t)(chanAlert[3]&0xFF)",
    }
    assert alert_payload(0xA1B2, 0xC3D4, (0x1E5, 0x06, 0x07, 0x08)) == bytes(
        [0xB2, 0xA1, 0xD4, 0xC3, 0xE5, 0x06, 0x07, 0x08])


def test_pca_fault_payload_matches_main_cpp_byte_for_byte():
    src = read_source("Firmware/ESP32/src/main.cpp")
    got = _payload_assignments(src, "uint8_t payload[6];", "bbpSendEvent(BBP_EVT_PCA_FAULT")
    assert got == {
        0: "(uint8_t)event->type", 1: "event->channel",
        2: "(uint8_t)(event->timestamp_ms&0xFF)",
        3: "(uint8_t)((event->timestamp_ms>>8)&0xFF)",
        4: "(uint8_t)((event->timestamp_ms>>16)&0xFF)",
        5: "(uint8_t)((event->timestamp_ms>>24)&0xFF)",
    }
    assert pca_fault_payload(2, 1, 0x11223344) == bytes([2, 1, 0x44, 0x33, 0x22, 0x11])


def test_pca_fault_types_match_firmware_enum():
    src = read_source("Firmware/ESP32/src/hal/pca9535.h")
    body = src[src.index("PCA_FAULT_EFUSE_TRIP"):src.index("} PcaFaultType;")]
    names = re.findall(r"\b(PCA_FAULT_[A-Z_]+)\b", body)
    assert names == ["PCA_FAULT_EFUSE_TRIP", "PCA_FAULT_EFUSE_CLEAR",
                     "PCA_FAULT_PG_LOST", "PCA_FAULT_PG_RESTORED"]


def test_la_log_is_relayed_as_raw_bytes():
    src = read_source("Firmware/ESP32/src/hat/hat.cpp")
    assert "bbpSendEvent(BBP_EVT_LA_LOG, rsp, rsp_len);" in src
    assert la_log_payload("boot ok") == b"boot ok"
    with pytest.raises(ValueError):
        la_log_payload(b"")
    with pytest.raises(ValueError):
        la_log_payload(b"x" * 256)


def test_din_payload_matches_protocol_doc():
    doc = read_source("Firmware/bbp-protocol.md")
    sec = doc[doc.index("#### 0x83 DIN_EVENT"):doc.index("#### 0x84 PCA_FAULT_EVENT")]
    rows = re.findall(r"^(\d+)\s+(\w+)\s+(u8|bool|u32)", sec, re.M)
    assert rows == [("0", "channel", "u8"), ("1", "state", "bool"), ("2", "counter", "u32")]
    assert din_payload(3, True, 0x01020304) == bytes([3, 1, 4, 3, 2, 1])


def test_din_event_is_not_emitted_by_firmware():
    # Pins a gap: on_din_event() waits for an event nothing sends. If this
    # starts failing, re-verify din_payload() against the new sender.
    senders = [p for p in (REPO_ROOT / "Firmware/ESP32/src").rglob("*.cpp")
               if "bbpSendEvent(BBP_EVT_DIN" in read_source(p)]
    assert senders == []


def test_encode_event_frame_round_trips_as_evt():
    frame = encode_event_frame(CmdId.ALERT_EVT, alert_payload(1, 2), seq=7)
    assert frame.endswith(b"\x00")
    msg_type, seq, cmd, payload = parse_frame(frame[:-1])
    assert (msg_type, seq, cmd, payload) == (MsgType.EVT, 7, CmdId.ALERT_EVT, alert_payload(1, 2))


# ---------------------------------------------------------------------------
# Simulator delivery
# ---------------------------------------------------------------------------

@pytest.fixture
def sim():
    device = SimulatedDevice()
    client = bb.BugBuster(SimulatedUSBTransport(device, hat=True))
    client.connect()
    yield client, device, EventInjector(device)
    client.disconnect()


def test_on_alert_decodes_status_words(sim):
    client, _, inject = sim
    got = []
    client.on_alert(got.append)
    inject.alert(0x8040, 0x0003, (1, 2, 3, 4))
    assert got == [{"alert_status": 0x8040, "supply_alert_status": 0x0003}]


def test_on_din_event_decodes_channel_state_counter(sim):
    client, _, inject = sim
    got = []
    client.on_din_event(lambda ch, st, n: got.append((ch, st, n)))
    inject.din(2, True, 0xDEADBEEF)
    inject.din(0, False, 1)
    assert got == [(2, True, 0xDEADBEEF), (0, False, 1)]
    assert isinstance(got[0][1], bool)


def test_on_la_log_decodes_utf8_and_replaces_invalid(sim):
    client, _, inject = sim
    got = []
    client.on_la_log(got.append)
    inject.la_log("la: armed \u00b5s")
    inject.la_log(b"bad \xff byte")
    assert got == ["la: armed \u00b5s", "bad \ufffd byte"]


def test_on_la_log_none_unregisters(sim):
    client, _, inject = sim
    got = []
    client.on_la_log(got.append)
    client.on_la_log(None)
    inject.la_log("dropped")
    assert got == []


def test_on_fault_fetches_breakdown_when_alert_fires(sim):
    client, device, inject = sim
    got = []
    client.on_fault(got.append)
    device.alert_status = 1 << 6
    inject.alert(1 << 6, 0)
    assert len(got) == 1
    assert any(f["code"] == 6 and f["channel"] is None for f in got[0])


def test_on_fault_skips_callback_when_no_fault_active(sim):
    client, device, inject = sim
    got = []
    client.on_fault(got.append)
    device.alert_status = 0
    inject.alert(0, 0)
    assert got == []


def test_pca_fault_event_without_handler_is_ignored(sim):
    client, _, inject = sim
    got = []
    client.on_alert(got.append)
    inject.pca_fault(0, 1, 1234)
    inject.alert(0x0002, 0)
    assert got == [{"alert_status": 2, "supply_alert_status": 0}]


def test_pca_fault_event_reaches_a_transport_handler(sim):
    # No client-level API exists for PCA_FAULT_EVT; the raw transport hook works.
    client, _, inject = sim
    got = []
    client._t.on_event(CmdId.PCA_FAULT_EVT,
                       lambda p: got.append(struct.unpack("<BBI", p)))
    inject.pca_fault(2, 1, 0x0001E240)
    assert got == [(2, 1, 0x0001E240)]


@pytest.mark.parametrize("register,evt_id,short", [
    ("on_alert", CmdId.ALERT_EVT, b"\x01\x02\x03"),
    ("on_din_event", CmdId.DIN_EVT, b"\x01\x01"),
])
def test_short_payload_does_not_invoke_callback(sim, register, evt_id, short):
    client, _, inject = sim
    got = []
    getattr(client, register)(lambda *a: got.append(a))
    inject.raw(evt_id, short)
    inject.raw(evt_id, b"")
    assert got == []


@pytest.mark.parametrize("method", ["on_alert", "on_din_event", "on_la_log", "on_fault"])
def test_event_callbacks_are_usb_only(method):
    client = bb.BugBuster(SimulatedHTTPTransport(SimulatedDevice()))
    with pytest.raises(NotImplementedError):
        getattr(client, method)(lambda *a: None)


# ---------------------------------------------------------------------------
# Real USBTransport reader thread fed with wire frames
# ---------------------------------------------------------------------------

class _FeedSerial:
    def __init__(self):
        self.is_open = True
        self._buf = bytearray()
        self._lock = threading.Lock()

    def feed(self, data: bytes) -> None:
        with self._lock:
            self._buf.extend(data)

    @property
    def in_waiting(self):
        with self._lock:
            return len(self._buf)

    def read(self, n):
        with self._lock:
            out = bytes(self._buf[:n])
            del self._buf[:n]
        if not out:
            time.sleep(0.002)
        return out

    def write(self, d):
        return len(d)

    def flush(self):
        pass

    def close(self):
        self.is_open = False


@pytest.fixture
def wire():
    port = _FeedSerial()
    t = USBTransport("COM_TEST")
    t._serial = port
    t._running = True
    t.auto_reconnect = False
    th = threading.Thread(target=t._reader_loop, daemon=True)
    t._reader_thread = th
    th.start()
    yield bb.BugBuster(t), t, port
    t._running = False
    th.join(2.0)


def test_reader_dispatches_every_event_type(wire):
    client, _, port = wire
    q: queue.Queue = queue.Queue()
    client.on_alert(lambda d: q.put(("alert", d)))
    client.on_din_event(lambda c, s, n: q.put(("din", (c, s, n))))
    client.on_la_log(lambda s: q.put(("log", s)))
    port.feed(encode_event_frame(CmdId.ALERT_EVT, alert_payload(0x10, 0x20, (9, 9, 9, 9)), 1)
              + encode_event_frame(CmdId.DIN_EVT, din_payload(1, False, 77), 2)
              + encode_event_frame(CmdId.HAT_LA_LOG_EVT, la_log_payload("hello"), 3))
    got = [q.get(timeout=2.0) for _ in range(3)]
    assert got == [("alert", {"alert_status": 0x10, "supply_alert_status": 0x20}),
                   ("din", (1, False, 77)), ("log", "hello")]


def test_reader_survives_malformed_short_and_unregistered_events(wire):
    client, t, port = wire
    q: queue.Queue = queue.Queue()
    calls = []
    client.on_alert(lambda d: (calls.append(d), q.put(d)))
    client.on_din_event(lambda *a: calls.append(a))

    def _boom(_text):
        raise RuntimeError("user callback bug")
    client.on_la_log(_boom)

    good = encode_event_frame(CmdId.ALERT_EVT, alert_payload(0x0004, 0), 9)
    msg = struct.pack("<BHB", MsgType.EVT, 4, CmdId.ALERT_EVT) + alert_payload(1, 1)
    bad_crc = cobs_encode(msg + struct.pack("<H", crc16_ccitt(msg) ^ 0x5A5A)) + b"\x00"
    port.feed(
        encode_event_frame(CmdId.ALERT_EVT, b"\x01\x02\x03", 1)        # short alert
        + encode_event_frame(CmdId.DIN_EVT, b"\x00", 2)                # short din
        + encode_event_frame(CmdId.PCA_FAULT_EVT, pca_fault_payload(0, 0, 5), 3)  # no handler
        + bad_crc                                                      # CRC mismatch
        + encode_event_frame(0x99, b"\xaa", 5)                         # unknown event id
        + b"\x02\x01\x00"                                              # sub-minimum frame
        + encode_event_frame(CmdId.HAT_LA_LOG_EVT, b"raise", 6)        # callback raises
        + build_frame(0x4321, 0x01, b"", msg_type=MsgType.RSP)         # RSP nobody awaits
        + good
    )
    assert q.get(timeout=2.0) == {"alert_status": 4, "supply_alert_status": 0}
    assert calls == [{"alert_status": 4, "supply_alert_status": 0}]
    assert t._reader_thread.is_alive()
    assert t._link_error is None
    assert t.is_healthy()


def test_reader_reassembles_event_split_across_reads(wire):
    client, _, port = wire
    q: queue.Queue = queue.Queue()
    client.on_din_event(lambda c, s, n: q.put((c, s, n)))
    frame = encode_event_frame(CmdId.DIN_EVT, din_payload(3, True, 123456), 1)
    for b in frame:
        port.feed(bytes([b]))
        time.sleep(0.001)
    assert q.get(timeout=2.0) == (3, True, 123456)


def test_reader_unregistered_event_does_not_block_command_response(wire):
    _, t, port = wire
    waiter: queue.Queue = queue.Queue()
    with t._pending_lock:
        t._pending[0x2222] = waiter
    port.feed(encode_event_frame(CmdId.PCA_FAULT_EVT, pca_fault_payload(1, 2, 3), 1)
              + build_frame(0x2222, 0xFE, b"\x01", msg_type=MsgType.RSP))
    assert waiter.get(timeout=2.0) == b"\x01"
