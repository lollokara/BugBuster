"""Per-record-type TX counters for the DAQ stream (voltage-loss diagnosis)."""
from pathlib import Path

PROTO = Path("Firmware/DAQ_HAT/ESP32P4/src/stream/usb_proto.h").read_text()
STREAM_H = Path("Firmware/DAQ_HAT/ESP32P4/src/stream/usb_stream.h").read_text()
STREAM_C = Path("Firmware/DAQ_HAT/ESP32P4/src/stream/usb_stream.c").read_text()
BOARD = Path("Firmware/DAQ_HAT/ESP32P4/src/board/daq_board.c").read_text()


def test_status_payload_appends_per_type_counters_without_reordering():
    """Extension v5 must be appended AFTER the v4 relay fields, never inserted."""
    for field in ("wave_i_frames", "wave_v_frames", "wave_i_drops", "wave_v_drops"):
        assert field in PROTO, f"{field} missing from usb_status_payload_t"
    # Append-only: every new field comes after the last v4 field.
    last_v4 = PROTO.index("relay_pushed_bytes")
    for field in ("wave_i_frames", "wave_v_frames", "wave_i_drops", "wave_v_drops"):
        assert PROTO.index(field) > last_v4, f"{field} inserted before end of v4 block"


def test_proto_version_unchanged_appending_is_forward_compatible():
    """Desktop must keep parsing v2; appending trailing bytes is the contract."""
    assert "#define USB_PROTO_VERSION    2u" in PROTO


def test_stream_counts_wave_v_separately_from_wave_i():
    assert "usb_stream_get_type_counters" in STREAM_H
    assert "wv_frames" in STREAM_C and "wi_frames" in STREAM_C


def test_stream_counts_drops_separately_by_type():
    """A dropped frame must be attributed to its record type, not just totalled."""
    assert "wv_drops" in STREAM_C and "wi_drops" in STREAM_C


def test_board_populates_the_new_status_fields():
    assert "usb_stream_get_type_counters" in BOARD


IOS_MGR = Path("iOSApp/Sources/Services/DaqWifiStreamManager.swift").read_text()
IOS_SCOPE = Path("iOSApp/Sources/Views/ScopeTab.swift").read_text()


def test_ios_parses_extension_v5_counters_at_correct_offsets():
    for off, field in ((72, "waveIFrames"), (76, "waveVFrames"),
                       (80, "waveIDrops"), (84, "waveVDrops")):
        assert field in IOS_MGR, f"{field} not parsed on iOS"
        assert f"u32({off})" in IOS_MGR, f"offset {off} ({field}) not read"


def test_ios_exposes_volt_adc_health_already_on_the_wire():
    """adaq_ok_bits bit2 = VOLT ok. Sent since extension v2, never surfaced."""
    assert "adaqOkBits" in IOS_MGR
    assert "voltAdcOK" in IOS_MGR
    assert "0x04" in IOS_MGR


def test_ios_counts_received_frames_by_type():
    assert "rxWaveI" in IOS_MGR and "rxWaveV" in IOS_MGR


def test_scope_tab_shows_the_diagnostic_readout():
    assert "rxCounts" in IOS_SCOPE
