"""BBP-TXQ (found on hardware 2026-10-01): usb_cdc_cli_write() queued a frame
once and ignored the count accepted. The TinyUSB CDC TX FIFO is 512 bytes, so
any BBP frame larger than the free FIFO space lost its tail (host: CRC
mismatch, then a timeout). Once TR-2 let 1030-byte frames through, a full
SCRIPT_LOGS reply hit it every time. The write must loop until every byte is
queued, with a bounded wait so a host that stops reading cannot hang the task."""

from tests.firmware_host.fwhost import extract_function

USB_CDC = "Firmware/ESP32/src/net/usb_cdc.cpp"


def test_cdc0_write_queues_the_whole_buffer():
    body = extract_function(USB_CDC, r"^uint32_t usb_cdc_cli_write\(")
    assert "while" in body or "for (" in body, "single write_queue call"
    assert "written < len" in body or "remaining" in body
    assert "deadline" in body or "tries" in body or "attempt" in body, "unbounded wait"
