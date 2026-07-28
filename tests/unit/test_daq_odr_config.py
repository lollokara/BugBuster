"""ODR/filter/decimation config must be append-only on the wire and BLE-reachable."""
import re
from pathlib import Path

PROTO = Path("Firmware/DAQ_HAT/ESP32P4/src/stream/usb_proto.h").read_text()
BOARD = Path("Firmware/DAQ_HAT/ESP32P4/src/board/daq_board.c").read_text()
S3LINK = Path("Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.h").read_text()
HAT_H = Path("Firmware/ESP32/src/hat/hat.h").read_text()
API = Path("Firmware/ESP32/src/net/api_core.cpp").read_text()


def test_status_growth_is_append_only():
    assert re.search(r"#define\s+USB_PROTO_VERSION\s+2u", PROTO), \
        "protocol version must stay 2 — STATUS growth is append-only"
    for field in ("filter", "adc_dec", "stream_decim", "odr_mhz"):
        assert field in PROTO, f"STATUS v6 field {field} missing"
    # v5 fields must keep their offsets — new fields go AFTER them.
    assert PROTO.index("wave_v_drops") < PROTO.index("odr_mhz"), \
        "v6 fields must be appended after the v5 block, never inserted"


def test_command_byte_does_not_collide():
    import re
    bytes_used = re.findall(r"HATP_CMD_\w+\s*=?\s*0x([0-9A-Fa-f]{2})", S3LINK)
    assert len(bytes_used) == len(set(bytes_used)), f"duplicate HATP command byte: {bytes_used}"


def test_acq_config_is_ble_reachable():
    """The phone is on the DAQ hotspot and cannot reach the S3 over HTTP."""
    assert "acq_config" in API, (
        "acq_config must be registered in the shared api_core dispatcher, not "
        "only as an HTTP route — BLE is the only control channel while streaming")


def test_device_reports_what_it_actually_applied():
    assert "adaq7769_output_data_rate" in BOARD, (
        "STATUS must report the ODR read back from the driver, not the requested value")
