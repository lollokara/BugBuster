"""ODR/filter/decimation config must be append-only on the wire and BLE-reachable."""
import re
from pathlib import Path

PROTO = Path("Firmware/DAQ_HAT/ESP32P4/src/stream/usb_proto.h").read_text()
BOARD = Path("Firmware/DAQ_HAT/ESP32P4/src/board/daq_board.c").read_text()
S3LINK = Path("Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.h").read_text()
HAT_H = Path("Firmware/ESP32/src/hat/hat.h").read_text()
API = Path("Firmware/ESP32/src/net/api_core.cpp").read_text()


def _extract_function_body(src: str, name: str) -> str:
    """Return the full braced body of the first function named `name`, found
    by balancing braces from its opening `{` (robust to reformatting, unlike
    a fixed line-range or single-line regex). Same technique as
    test_s3link_dispatch_coverage.py's extractor."""
    m = re.search(re.escape(name) + r"\s*\([^;]*?\)\s*\{", src, re.DOTALL)
    assert m, f"could not locate function {name}() in source"
    start = m.end() - 1  # index of the opening brace
    depth = 0
    for i in range(start, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
    raise AssertionError(f"unbalanced braces while scanning {name}()")


def _extract_named_struct(src: str, type_name: str) -> str:
    """Return the full `typedef struct ... } type_name;` block, found by
    balancing braces backward from the closing `} type_name;` marker. Used to
    parse real field offsets out of usb_status_payload_t rather than
    searching the whole file for bare substrings."""
    marker = "} " + type_name + ";"
    end = src.index(marker)
    depth = 0
    start = None
    i = end
    while i >= 0:
        if src[i] == "}":
            depth += 1
        elif src[i] == "{":
            depth -= 1
            if depth == 0:
                start = i
                break
        i -= 1
    assert start is not None, f"could not find opening brace for {type_name}"
    return src[start:end + len(marker)]


def _field_offsets(struct_body: str) -> dict:
    """Map field name -> byte offset, parsed from each field's trailing
    `// NN` comment (the convention every field in this struct already
    follows). Only meaningful because usb_status_payload_t is
    __attribute__((packed)) — no compiler-inserted padding to account for."""
    return {name: int(off) for name, off in
            re.findall(r"(\w+)\s*;\s*//\s*(\d+)", struct_body)}


def test_status_growth_is_append_only():
    assert re.search(r"#define\s+USB_PROTO_VERSION\s+2u", PROTO), \
        "protocol version must stay 2 — STATUS growth is append-only"

    struct_body = _extract_named_struct(PROTO, "usb_status_payload_t")
    offsets = _field_offsets(struct_body)

    # v5 must still end at 88 (wave_v_drops is its last field, 4 bytes wide).
    assert offsets.get("wave_v_drops") == 84, \
        f"v5's last field wave_v_drops must sit at offset 84, got {offsets.get('wave_v_drops')}"

    # v6 fields must sit at their exact documented offsets — this is precisely
    # what the iOS decoder's field END offsets depend on.
    expected = {"filter": 88, "adc_dec": 89, "stream_decim": 90, "odr_mhz": 92}
    for field, off in expected.items():
        assert field in offsets, f"STATUS v6 field {field} missing"
        assert offsets[field] == off, (
            f"STATUS v6 field {field} must sit at offset {off}, got {offsets[field]} "
            "— v6 fields must be appended after the v5 block, never inserted or reordered")

    # Total struct size (documented in the trailing comment on the closing
    # brace's line) must be 96 — 88 (v5) + 8 (v6: u8+u8+u16+u32).
    end_idx = PROTO.index(struct_body) + len(struct_body)
    line_end = PROTO.index("\n", end_idx)
    tail = PROTO[end_idx:line_end]
    m = re.search(r"total:\s*(\d+)\s*bytes", tail)
    assert m and int(m.group(1)) == 96, \
        f"usb_status_payload_t total size must be documented as 96 bytes, got {tail!r}"


def test_command_byte_does_not_collide():
    bytes_used = re.findall(r"HATP_CMD_\w+\s*=?\s*0x([0-9A-Fa-f]{2})", S3LINK)
    assert len(bytes_used) == len(set(bytes_used)), f"duplicate HATP command byte: {bytes_used}"


def test_acq_config_is_ble_reachable():
    """The phone is on the DAQ hotspot and cannot reach the S3 over HTTP, so
    the route must be dispatched from api_core_handle() itself (shared by
    both HTTP and BLE), not merely mentioned somewhere in the file (a comment
    or an orphaned handler function would also satisfy a bare substring
    check — the same presence-not-reachability shape as the
    HATP_CMD_DAQ_WIFI_STREAM_RECYCLE dispatch-table bug)."""
    dispatch_body = _extract_function_body(API, "api_core_handle")
    assert '"/api/daq/acq_config"' in dispatch_body, (
        "/api/daq/acq_config must be routed inside api_core_handle() itself — "
        "BLE is the only control channel while streaming, and api_core_handle() "
        "is the one dispatcher both HTTP and BLE call through")


def test_device_reports_what_it_actually_applied():
    """STATUS must report the ODR read back from the driver, not the last
    requested value. Anchored on the actual assignment inside
    daq_board_stream_summary() rather than a bare substring: that string
    (adaq7769_output_data_rate) already appeared elsewhere in daq_board.c
    before the v6 fields existed (it drives sample_rate and the FFT axis
    too), so a whole-file substring check would pass whether or not
    st.odr_mhz was ever wired up — it is not evidence of the v6 readback."""
    summary_body = _extract_function_body(BOARD, "daq_board_stream_summary")
    assert re.search(r"st\.odr_mhz\s*=.*adaq7769_output_data_rate", summary_body, re.DOTALL), (
        "usb_status_payload_t.odr_mhz must be assigned from "
        "adaq7769_output_data_rate() inside daq_board_stream_summary()")
