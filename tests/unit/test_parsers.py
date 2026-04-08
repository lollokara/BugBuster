"""
Unit tests for BugBuster binary protocol parsers.

These tests run without hardware -- they build binary payloads with struct.pack
and verify the parsers extract every field correctly.
"""

import struct
from typing import Optional, List

import pytest

from bugbuster.client import _parse_status, _parse_faults, BugBuster


# ---------------------------------------------------------------------------
# Helpers to build binary payloads
# ---------------------------------------------------------------------------

def _build_status_payload(
    spi_ok: int = 1,
    die_temp: float = 25.5,
    alert_status: int = 0,
    alert_mask: int = 0,
    supply_alert_status: int = 0,
    supply_alert_mask: int = 0,
    live_status: int = 0,
    channels: Optional[list] = None,
    diagnostics: Optional[list] = None,
) -> bytes:
    """
    Build a complete _parse_status binary payload.

    Header: spi_ok(1) + die_temp(4f) + alert_status(2) + alert_mask(2)
            + supply_alert_status(2) + supply_alert_mask(2) + live_status(2) = 15 bytes
    Channels: 4 x 30 bytes = 120 bytes  (offset 15..134)
    Diagnostics: 4 x 7 bytes = 28 bytes (offset 135..162)
    Total: 163 bytes
    """
    header = struct.pack(
        '<BfHHHHH',
        spi_ok, die_temp,
        alert_status, alert_mask,
        supply_alert_status, supply_alert_mask,
        live_status,
    )

    if channels is None:
        channels = [_default_channel(i) for i in range(4)]

    ch_buf = b''
    for ch in channels:
        ch_buf += _pack_channel(ch)

    if diagnostics is None:
        diagnostics = [{"source": 0, "raw_code": 0, "value": 0.0}] * 4

    diag_buf = b''
    for d in diagnostics:
        diag_buf += struct.pack('<BH', d["source"], d["raw_code"])
        diag_buf += struct.pack('<f', d["value"])

    return header + ch_buf + diag_buf


def _default_channel(ch_id: int) -> dict:
    return {
        "id": ch_id, "function": 0,
        "adc_raw": 0, "adc_value": 0.0,
        "adc_range": 0, "adc_rate": 0, "adc_mux": 0,
        "dac_code": 0, "dac_value": 0.0,
        "din_state": 0, "din_counter": 0,
        "do_state": 0, "channel_alert": 0,
        "channel_alert_mask": 0, "rtd_excitation_ua": 0,
    }


def _pack_channel(ch: dict) -> bytes:
    """
    Pack one channel into 30 bytes matching the firmware layout:
      id(1) + func(1) + raw(3) + adc_val(4) + adc_rng(1) + adc_rate(1)
      + adc_mux(1) + dac_code(2) + dac_val(4) + din_state(1) + din_counter(4)
      + do_state(1) + ch_alert(2) + ch_alert_mask(2) + rtd_ua(2) = 30 bytes
    """
    buf = struct.pack('<BB', ch["id"], ch["function"])
    buf += ch["adc_raw"].to_bytes(3, 'little')
    buf += struct.pack('<f', ch["adc_value"])
    buf += struct.pack('<BBB', ch["adc_range"], ch["adc_rate"], ch["adc_mux"])
    buf += struct.pack('<H', ch["dac_code"])
    buf += struct.pack('<f', ch["dac_value"])
    buf += struct.pack('<B', ch["din_state"])
    buf += struct.pack('<I', ch["din_counter"])
    buf += struct.pack('<B', ch["do_state"])
    buf += struct.pack('<H', ch["channel_alert"])
    buf += struct.pack('<H', ch["channel_alert_mask"])
    buf += struct.pack('<H', ch["rtd_excitation_ua"])
    return buf


def _build_faults_payload(
    alert_status: int = 0,
    alert_mask: int = 0,
    supply_alert_status: int = 0,
    supply_alert_mask: int = 0,
    channels: Optional[list] = None,
) -> bytes:
    """
    Build a complete _parse_faults binary payload.

    Header: alert_status(2) + alert_mask(2) + supply(2) + supply_mask(2) = 8 bytes
    Channels: 4 x 5 bytes (id(1) + ch_alert(2) + ch_mask(2))
    Total: 28 bytes
    """
    header = struct.pack('<HHHH', alert_status, alert_mask,
                         supply_alert_status, supply_alert_mask)

    if channels is None:
        channels = [{"id": i, "alert": 0, "mask": 0} for i in range(4)]

    ch_buf = b''
    for ch in channels:
        ch_buf += struct.pack('<BHH', ch["id"], ch["alert"], ch["mask"])

    return header + ch_buf


# ===================================================================
# 1. _parse_status — header fields
# ===================================================================

class TestParseStatusHeader:

    def test_spi_ok_true(self):
        payload = _build_status_payload(spi_ok=1)
        result = _parse_status(payload)
        assert result["spi_ok"] is True

    def test_spi_ok_false(self):
        payload = _build_status_payload(spi_ok=0)
        result = _parse_status(payload)
        assert result["spi_ok"] is False

    def test_die_temp(self):
        payload = _build_status_payload(die_temp=42.75)
        result = _parse_status(payload)
        assert result["die_temp_c"] == pytest.approx(42.75, abs=1e-4)

    def test_die_temp_negative(self):
        payload = _build_status_payload(die_temp=-10.5)
        result = _parse_status(payload)
        assert result["die_temp_c"] == pytest.approx(-10.5, abs=1e-4)

    @pytest.mark.parametrize("field,value", [
        ("alert_status", 0x1234),
        ("alert_mask", 0xABCD),
        ("supply_alert_status", 0x00FF),
        ("supply_alert_mask", 0xFF00),
        ("live_status", 0x5A5A),
    ])
    def test_header_u16_fields(self, field, value):
        payload = _build_status_payload(**{field: value})
        result = _parse_status(payload)
        assert result[field] == value


# ===================================================================
# 2. _parse_status — channel fields
# ===================================================================

class TestParseStatusChannels:

    def test_four_channels_present(self):
        payload = _build_status_payload()
        result = _parse_status(payload)
        assert len(result["channels"]) == 4

    def test_channel_ids(self):
        channels = [_default_channel(i) for i in range(4)]
        channels[0]["id"] = 0
        channels[1]["id"] = 1
        channels[2]["id"] = 2
        channels[3]["id"] = 3
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        for i in range(4):
            assert result["channels"][i]["id"] == i

    def test_channel_function_codes(self):
        channels = [_default_channel(i) for i in range(4)]
        funcs = [0, 1, 3, 8]  # HIGH_IMP, VOUT, VIN, DIN_LOGIC
        for i, f in enumerate(funcs):
            channels[i]["function"] = f
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        for i, f in enumerate(funcs):
            assert result["channels"][i]["function"] == f

    def test_adc_raw_24bit(self):
        """ADC raw is a 24-bit little-endian value (3 bytes)."""
        channels = [_default_channel(i) for i in range(4)]
        channels[0]["adc_raw"] = 0x123456
        channels[2]["adc_raw"] = 0xFFFFFF
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        assert result["channels"][0]["adc_raw"] == 0x123456
        assert result["channels"][2]["adc_raw"] == 0xFFFFFF

    def test_adc_value_float(self):
        channels = [_default_channel(i) for i in range(4)]
        channels[1]["adc_value"] = 3.14159
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        assert result["channels"][1]["adc_value"] == pytest.approx(3.14159, rel=1e-5)

    @pytest.mark.parametrize("adc_range", [0, 1, 2, 5, 7])
    def test_adc_range(self, adc_range):
        channels = [_default_channel(0)] + [_default_channel(i) for i in range(1, 4)]
        channels[0]["adc_range"] = adc_range
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        assert result["channels"][0]["adc_range"] == adc_range

    @pytest.mark.parametrize("adc_rate", [0, 1, 4, 9, 13])
    def test_adc_rate(self, adc_rate):
        channels = [_default_channel(i) for i in range(4)]
        channels[0]["adc_rate"] = adc_rate
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        assert result["channels"][0]["adc_rate"] == adc_rate

    @pytest.mark.parametrize("adc_mux", [0, 1, 2, 3, 4])
    def test_adc_mux(self, adc_mux):
        channels = [_default_channel(i) for i in range(4)]
        channels[0]["adc_mux"] = adc_mux
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        assert result["channels"][0]["adc_mux"] == adc_mux

    def test_dac_code(self):
        channels = [_default_channel(i) for i in range(4)]
        channels[3]["dac_code"] = 0xBEEF
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        assert result["channels"][3]["dac_code"] == 0xBEEF

    def test_dac_value_float(self):
        channels = [_default_channel(i) for i in range(4)]
        channels[2]["dac_value"] = 11.999
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        assert result["channels"][2]["dac_value"] == pytest.approx(11.999, rel=1e-5)

    def test_din_state(self):
        channels = [_default_channel(i) for i in range(4)]
        channels[0]["din_state"] = 1
        channels[1]["din_state"] = 0
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        assert result["channels"][0]["din_state"] is True
        assert result["channels"][1]["din_state"] is False

    def test_din_counter(self):
        channels = [_default_channel(i) for i in range(4)]
        channels[1]["din_counter"] = 123456
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        assert result["channels"][1]["din_counter"] == 123456

    def test_do_state(self):
        channels = [_default_channel(i) for i in range(4)]
        channels[3]["do_state"] = 1
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        assert result["channels"][3]["do_state"] is True

    def test_channel_alert(self):
        channels = [_default_channel(i) for i in range(4)]
        channels[0]["channel_alert"] = 0x00FF
        channels[3]["channel_alert"] = 0xFF00
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        assert result["channels"][0]["channel_alert"] == 0x00FF
        assert result["channels"][3]["channel_alert"] == 0xFF00

    def test_channel_alert_mask(self):
        """Verify the channel_alert_mask field (30-byte layout, was 28)."""
        channels = [_default_channel(i) for i in range(4)]
        channels[0]["channel_alert_mask"] = 0x1234
        channels[1]["channel_alert_mask"] = 0xFFFF
        channels[2]["channel_alert_mask"] = 0x0000
        channels[3]["channel_alert_mask"] = 0xABCD
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        assert result["channels"][0]["channel_alert_mask"] == 0x1234
        assert result["channels"][1]["channel_alert_mask"] == 0xFFFF
        assert result["channels"][2]["channel_alert_mask"] == 0x0000
        assert result["channels"][3]["channel_alert_mask"] == 0xABCD

    def test_rtd_excitation_ua(self):
        channels = [_default_channel(i) for i in range(4)]
        channels[0]["rtd_excitation_ua"] = 500
        channels[1]["rtd_excitation_ua"] = 1000
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        assert result["channels"][0]["rtd_excitation_ua"] == 500
        assert result["channels"][1]["rtd_excitation_ua"] == 1000

    def test_all_channels_different_values(self):
        """Full round-trip: each channel has distinct values for every field."""
        channels = []
        for i in range(4):
            channels.append({
                "id": i,
                "function": i + 1,
                "adc_raw": (i + 1) * 0x010101,
                "adc_value": (i + 1) * 1.5,
                "adc_range": i,
                "adc_rate": i * 3,
                "adc_mux": i % 5,
                "dac_code": (i + 1) * 1000,
                "dac_value": (i + 1) * 2.5,
                "din_state": i % 2,
                "din_counter": (i + 1) * 100,
                "do_state": (i + 1) % 2,
                "channel_alert": (i + 1) * 0x0101,
                "channel_alert_mask": (i + 1) * 0x1111,
                "rtd_excitation_ua": (i + 1) * 250,
            })
        payload = _build_status_payload(channels=channels)
        result = _parse_status(payload)
        for i in range(4):
            ch = result["channels"][i]
            assert ch["id"] == i
            assert ch["function"] == i + 1
            assert ch["adc_raw"] == (i + 1) * 0x010101
            assert ch["adc_value"] == pytest.approx((i + 1) * 1.5, rel=1e-5)
            assert ch["adc_range"] == i
            assert ch["adc_rate"] == i * 3
            assert ch["adc_mux"] == i % 5
            assert ch["dac_code"] == (i + 1) * 1000
            assert ch["dac_value"] == pytest.approx((i + 1) * 2.5, rel=1e-5)
            assert ch["din_state"] is bool(i % 2)
            assert ch["din_counter"] == (i + 1) * 100
            assert ch["do_state"] is bool((i + 1) % 2)
            assert ch["channel_alert"] == (i + 1) * 0x0101
            assert ch["channel_alert_mask"] == (i + 1) * 0x1111
            assert ch["rtd_excitation_ua"] == (i + 1) * 250


# ===================================================================
# 3. _parse_status — channel byte layout is exactly 30 bytes
# ===================================================================

class TestParseStatusChannelLayout:

    def test_channel_block_is_30_bytes(self):
        """Ensure each channel occupies exactly 30 bytes in the payload."""
        ch_bytes = _pack_channel(_default_channel(0))
        assert len(ch_bytes) == 30

    def test_total_payload_size(self):
        """Total: 15 (header) + 4*30 (channels) + 4*7 (diag) = 163 bytes."""
        payload = _build_status_payload()
        assert len(payload) == 163


# ===================================================================
# 4. _parse_status — diagnostics
# ===================================================================

class TestParseStatusDiagnostics:

    def test_four_diagnostics_present(self):
        payload = _build_status_payload()
        result = _parse_status(payload)
        assert len(result["diagnostics"]) == 4

    def test_diagnostic_values(self):
        diags = [
            {"source": 1, "raw_code": 0x1234, "value": 3.14},
            {"source": 2, "raw_code": 0x5678, "value": -1.0},
            {"source": 3, "raw_code": 0x0000, "value": 0.0},
            {"source": 4, "raw_code": 0xFFFF, "value": 99.99},
        ]
        payload = _build_status_payload(diagnostics=diags)
        result = _parse_status(payload)
        for i, d in enumerate(diags):
            assert result["diagnostics"][i]["source"] == d["source"]
            assert result["diagnostics"][i]["raw_code"] == d["raw_code"]
            assert result["diagnostics"][i]["value"] == pytest.approx(d["value"], rel=1e-5)

    def test_diagnostics_offset_135(self):
        """Diagnostics start at byte 135 = 15 + 4*30."""
        diags = [
            {"source": 0xAA, "raw_code": 0xBBCC, "value": 1.0},
            {"source": 0, "raw_code": 0, "value": 0.0},
            {"source": 0, "raw_code": 0, "value": 0.0},
            {"source": 0, "raw_code": 0, "value": 0.0},
        ]
        payload = _build_status_payload(diagnostics=diags)
        # Manually verify offset 135
        assert payload[135] == 0xAA
        result = _parse_status(payload)
        assert result["diagnostics"][0]["source"] == 0xAA
        assert result["diagnostics"][0]["raw_code"] == 0xBBCC


# ===================================================================
# 5. _parse_faults
# ===================================================================

class TestParseFaults:

    def test_header_fields(self):
        payload = _build_faults_payload(
            alert_status=0x1111,
            alert_mask=0x2222,
            supply_alert_status=0x3333,
            supply_alert_mask=0x4444,
        )
        result = _parse_faults(payload)
        assert result["alert_status"] == 0x1111
        assert result["alert_mask"] == 0x2222
        assert result["supply_alert_status"] == 0x3333
        assert result["supply_alert_mask"] == 0x4444

    def test_four_channels(self):
        payload = _build_faults_payload()
        result = _parse_faults(payload)
        assert len(result["channels"]) == 4

    def test_channel_faults(self):
        channels = [
            {"id": 0, "alert": 0x00FF, "mask": 0xFF00},
            {"id": 1, "alert": 0x1234, "mask": 0x5678},
            {"id": 2, "alert": 0x0000, "mask": 0xFFFF},
            {"id": 3, "alert": 0xAAAA, "mask": 0x5555},
        ]
        payload = _build_faults_payload(channels=channels)
        result = _parse_faults(payload)
        for i, expected in enumerate(channels):
            ch = result["channels"][i]
            assert ch["id"] == expected["id"]
            assert ch["alert"] == expected["alert"]
            assert ch["mask"] == expected["mask"]

    def test_all_zeros(self):
        payload = _build_faults_payload()
        result = _parse_faults(payload)
        assert result["alert_status"] == 0
        assert result["alert_mask"] == 0
        for ch in result["channels"]:
            assert ch["alert"] == 0
            assert ch["mask"] == 0

    def test_payload_size(self):
        """Total: 8 (header) + 4*5 (channels) = 28 bytes."""
        payload = _build_faults_payload()
        assert len(payload) == 28


# ===================================================================
# 6. hat_la_decode — bit-unpacking
# ===================================================================

class TestHatLaDecode:

    def test_1ch_single_byte(self):
        """1-channel mode: each bit is one sample, 8 samples per byte."""
        raw = bytes([0b10110001])  # bits 0..7: 1,0,0,0,1,1,0,1
        result = BugBuster.hat_la_decode(raw, channels=1)
        assert len(result) == 1
        assert result[0] == [1, 0, 0, 0, 1, 1, 0, 1]

    def test_1ch_multiple_bytes(self):
        raw = bytes([0xFF, 0x00])
        result = BugBuster.hat_la_decode(raw, channels=1)
        assert result[0] == [1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0]

    def test_2ch_single_byte(self):
        """
        2-channel mode: 2 bits per sample, 4 samples per byte.
        Byte 0b11_10_01_00:
          bits[0:1] = 00 -> ch0=0, ch1=0
          bits[2:3] = 01 -> ch0=1, ch1=0
          bits[4:5] = 10 -> ch0=0, ch1=1
          bits[6:7] = 11 -> ch0=1, ch1=1
        """
        raw = bytes([0b11100100])
        result = BugBuster.hat_la_decode(raw, channels=2)
        assert len(result) == 2
        assert result[0] == [0, 1, 0, 1]  # ch0: bit0 of each pair
        assert result[1] == [0, 0, 1, 1]  # ch1: bit1 of each pair

    def test_4ch_single_byte(self):
        """
        4-channel mode: 4 bits per sample, 2 samples per byte.
        Byte 0b1010_0101:
          bits[0:3] = 0101 -> ch0=1, ch1=0, ch2=1, ch3=0
          bits[4:7] = 1010 -> ch0=0, ch1=1, ch2=0, ch3=1
        """
        raw = bytes([0b10100101])
        result = BugBuster.hat_la_decode(raw, channels=4)
        assert len(result) == 4
        assert result[0] == [1, 0]  # ch0
        assert result[1] == [0, 1]  # ch1
        assert result[2] == [1, 0]  # ch2
        assert result[3] == [0, 1]  # ch3

    def test_4ch_all_high(self):
        raw = bytes([0xFF])
        result = BugBuster.hat_la_decode(raw, channels=4)
        assert result == [[1, 1], [1, 1], [1, 1], [1, 1]]

    def test_4ch_all_low(self):
        raw = bytes([0x00])
        result = BugBuster.hat_la_decode(raw, channels=4)
        assert result == [[0, 0], [0, 0], [0, 0], [0, 0]]

    def test_1ch_all_zeros(self):
        raw = bytes([0x00, 0x00])
        result = BugBuster.hat_la_decode(raw, channels=1)
        assert result[0] == [0] * 16

    def test_1ch_all_ones(self):
        raw = bytes([0xFF, 0xFF])
        result = BugBuster.hat_la_decode(raw, channels=1)
        assert result[0] == [1] * 16

    def test_empty_input(self):
        result = BugBuster.hat_la_decode(b'', channels=4)
        assert result == [[], [], [], []]

    @pytest.mark.parametrize("channels,samples_per_byte", [
        (1, 8),
        (2, 4),
        (4, 2),
    ])
    def test_sample_count_per_byte(self, channels, samples_per_byte):
        """Verify that each byte produces the expected number of samples per channel."""
        raw = bytes([0xAA])
        result = BugBuster.hat_la_decode(raw, channels=channels)
        for ch_data in result:
            assert len(ch_data) == samples_per_byte

    def test_2ch_multiple_bytes(self):
        """Multi-byte 2-channel decode consistency check."""
        raw = bytes([0b01010101, 0b10101010])
        result = BugBuster.hat_la_decode(raw, channels=2)
        assert len(result[0]) == 8  # 4 samples/byte * 2 bytes
        assert len(result[1]) == 8
        # First byte: pairs are 01, 01, 01, 01
        #   ch0 bits: 1,1,1,1  ch1 bits: 0,0,0,0
        assert result[0][:4] == [1, 1, 1, 1]
        assert result[1][:4] == [0, 0, 0, 0]
        # Second byte: pairs are 10, 10, 10, 10
        #   ch0 bits: 0,0,0,0  ch1 bits: 1,1,1,1
        assert result[0][4:] == [0, 0, 0, 0]
        assert result[1][4:] == [1, 1, 1, 1]


# ===================================================================
# 7. _parse_status — edge cases (truncated / short payloads)
# ===================================================================

class TestParseStatusEdgeCases:

    def test_empty_payload_raises(self):
        with pytest.raises((struct.error, IndexError)):
            _parse_status(b'')

    def test_header_only_raises(self):
        """15 bytes of header but no channel data should fail."""
        header = struct.pack('<BfHHHHH', 1, 25.0, 0, 0, 0, 0, 0)
        assert len(header) == 15
        with pytest.raises((struct.error, IndexError)):
            _parse_status(header)

    def test_partial_channels_raises(self):
        """Header + 2 channels (75 bytes) instead of 4 -- should fail on channel 3."""
        header = struct.pack('<BfHHHHH', 1, 25.0, 0, 0, 0, 0, 0)
        ch_buf = _pack_channel(_default_channel(0)) * 2
        payload = header + ch_buf
        assert len(payload) == 15 + 60
        with pytest.raises((struct.error, IndexError)):
            _parse_status(payload)

    def test_missing_diagnostics_raises(self):
        """Header + 4 channels (135 bytes) but no diagnostics -- should fail."""
        header = struct.pack('<BfHHHHH', 1, 25.0, 0, 0, 0, 0, 0)
        ch_buf = _pack_channel(_default_channel(0)) * 4
        payload = header + ch_buf
        assert len(payload) == 135
        with pytest.raises((struct.error, IndexError)):
            _parse_status(payload)

    def test_one_extra_byte_still_works(self):
        """Payload with trailing extra bytes should still parse fine."""
        payload = _build_status_payload() + b'\x00'
        result = _parse_status(payload)
        assert result["spi_ok"] is True
        assert len(result["channels"]) == 4


class TestParseFaultsEdgeCases:

    def test_empty_payload_raises(self):
        with pytest.raises((struct.error, IndexError)):
            _parse_faults(b'')

    def test_header_only_raises(self):
        header = struct.pack('<HHHH', 0, 0, 0, 0)
        assert len(header) == 8
        with pytest.raises((struct.error, IndexError)):
            _parse_faults(header)
