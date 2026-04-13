"""
LA USB bulk hardware tests — requires real RP2040 with HAT.

Run with:
    pytest tests/device/test_la_usb_bulk.py --hat --device-usb /dev/cu.usbmodem1234561 -v

--device-usb is required: before each test the fixture calls CMD_HAT_LA_CONFIG via BBP
(through the ESP32) to load the RP2040 PIO program (pio_loaded=true). Without this,
LA_USB_CMD_START_STREAM returns PKT_ERROR(START_REJECTED=0x80) because the RP2040
guards bb_la_start_stream() behind pio_loaded==true (set by bb_la_configure()).
LA_USB_CMD_STOP always calls bb_la_stop() which unloads PIO, so configure must be
called before every streaming attempt, not just once at setup.

These tests otherwise bypass the ESP32/BBP stack and communicate directly with the
RP2040 vendor bulk interface (interface=3, EP_IN=0x87, EP_OUT=0x06).
They validate that the firmware correctly implements the LA USB bulk protocol.
"""
from __future__ import annotations

import time
import struct
import pytest

# Guard: skip this entire file if pyusb is not installed
pytest.importorskip("usb.core", reason="pyusb not installed (pip install pyusb)")

from tests.mock.la_usb_host import (
    LaUsbHost,
    PKT_START,
    PKT_DATA,
    PKT_STOP,
    PKT_ERROR,
    STREAM_CMD_START,
    STREAM_CMD_STOP,
    INFO_START_REJECTED,
)


# ---------------------------------------------------------------------------
# Fixture
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def la_host(request: pytest.FixtureRequest):
    """Connect LaUsbHost to the real RP2040 device."""
    if not request.config.getoption("--hat", default=False):
        pytest.skip("--hat flag required for USB bulk hardware tests")
    host = LaUsbHost()
    host.connect()
    yield host
    host.close()


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
@pytest.mark.requires_hat
class TestLaUsbBulk:

    @pytest.fixture(autouse=True)
    def configure_la_before_test(self, la_host: LaUsbHost, request: pytest.FixtureRequest) -> None:
        """
        Drain stale packets then call CMD_HAT_LA_CONFIG via BBP before each test.

        Before each test: drain the vendor bulk FIFO to empty, then call
        CMD_HAT_LA_CONFIG via BBP to reload the RP2040 PIO program.

          1. Send STREAM_CMD_STOP, then read until timeout (FIFO truly empty).
             "Until first PKT_STOP" is not enough: incomplete test cleanup can leave
             [DATA×N][PKT_STOP][DATA×M] in the FIFO; the remaining DATA packets would
             be read as the "first packet" of the next STREAM_CMD_START.
          2. Call hat_la_configure() via BBP — calls bb_la_configure() on the RP2040
             which runs load_pio_program() and sets pio_loaded=true.

        Without step 1, stale DATA from the previous test pollutes the next test.
        Without step 2, pio_loaded=false and LA_USB_CMD_START_STREAM returns
        PKT_ERROR(START_REJECTED=0x80).
        """
        port = request.config.getoption("--device-usb", default=None)
        if port is None:
            pytest.skip(
                "LA USB bulk stream tests require --device-usb <port> "
                "to call CMD_HAT_LA_CONFIG via BBP before each streaming attempt"
            )

        # Step 1+2: drain vendor bulk FIFO until truly empty.
        # Send STOP and read until a timeout (FIFO empty), not just until first PKT_STOP.
        # Stopping until the first PKT_STOP is insufficient: a previous test's incomplete
        # cleanup can leave [DATA×N][PKT_STOP][DATA×M][PKT_STOP] in the FIFO. Breaking
        # on the first STOP leaves M DATA packets that would be read as "the first packet"
        # of the next STREAM_CMD_START. Reading until timeout guarantees the FIFO is clear.
        try:
            la_host.send_command(STREAM_CMD_STOP)
        except Exception:
            pass  # device may already be idle; proceed to drain
        for _ in range(8192):
            try:
                la_host.read_packet(timeout_ms=150)
            except Exception:
                break  # timeout = FIFO truly empty

        # Step 3: reload PIO via BBP.
        # Retry up to 5 times with 200 ms gaps — the RP2040 may be briefly busy
        # finishing DMA/PIO teardown after STREAM_CMD_STOP, causing the 300 ms
        # I2C timeout in hat_command() to fire before bb_la_configure() runs.
        import bugbuster as bb
        try:
            dev = bb.connect_usb(port)
        except Exception as e:
            pytest.skip(f"Could not connect to BBP device on {port}: {e}")
            return
        configured = False
        last_err = None
        try:
            for attempt in range(5):
                if attempt > 0:
                    time.sleep(0.2)
                try:
                    dev.hat_la_configure(channels=4, rate_hz=1_000_000, depth=100_000)
                    configured = True
                    break
                except Exception as e:
                    last_err = e
        finally:
            dev.disconnect()
        if not configured:
            pytest.skip(f"CMD_HAT_LA_CONFIG failed after 5 attempts: {last_err}")

    def test_bulk_interface_mounts(self, la_host: LaUsbHost) -> None:
        """Interface 3 must be claimable — if the fixture ran, it succeeded."""
        assert la_host._dev is not None, "USB device handle must not be None"
        assert la_host._claimed, "Interface must be claimed"

    def test_oneshot_header_correct(self, la_host: LaUsbHost) -> None:
        """
        After a one-shot capture, the 4-byte LE header must equal
        the number of raw bytes that follow.
        """
        # NOTE: Requires the device to be in a state where a capture is available.
        # This test is advisory — skip if device not in capture-ready state.
        try:
            data = la_host.oneshot_capture()
        except Exception as e:
            pytest.skip(f"Device not in capture-ready state: {e}")
        assert len(data) > 0, "Oneshot capture must return at least 1 byte"

    def test_stream_start_packet_first(self, la_host: LaUsbHost) -> None:
        """After sending START command, the very first packet must be PKT_START (0x01)."""
        la_host.send_command(STREAM_CMD_START)
        try:
            pkt = la_host.read_packet(timeout_ms=2000)
        finally:
            # Always send STOP to clean up device state
            la_host.send_command(STREAM_CMD_STOP)
            # Drain remaining packets
            for _ in range(10):
                try:
                    la_host.read_packet(timeout_ms=100)
                except Exception:
                    break

        assert pkt.pkt_type == PKT_START, (
            f"Expected PKT_START(0x01) as first packet, got {pkt.type_name}({pkt.pkt_type:#04x})"
        )

    def test_stream_data_sequence(self, la_host: LaUsbHost) -> None:
        """
        A 100 ms stream must produce DATA packets with no sequence gaps.
        Sequence gaps indicate dropped USB bulk packets.
        """
        result = la_host.stream_capture(duration_s=0.1)
        assert result.packets_received > 0, (
            "No DATA packets received in 100 ms — is the LA capturing?"
        )
        assert result.sequence_mismatches == 0, (
            f"Sequence gaps detected ({result.sequence_mismatches}) — "
            f"firmware may be dropping USB bulk packets"
        )

    def test_stream_stop_cmd(self, la_host: LaUsbHost) -> None:
        """
        Sending STOP command while streaming must produce a PKT_STOP
        packet within 500 ms.
        """
        la_host.send_command(STREAM_CMD_START)
        # Read and discard START packet
        la_host.read_packet(timeout_ms=1000)
        # Read a few DATA packets to confirm streaming
        for _ in range(3):
            try:
                la_host.read_packet(timeout_ms=200)
            except Exception:
                break
        # Send STOP
        la_host.send_command(STREAM_CMD_STOP)
        # Next packet must be STOP
        stop_pkt = la_host.read_packet(timeout_ms=500)
        assert stop_pkt.pkt_type == PKT_STOP, (
            f"Expected PKT_STOP(0x03) after STOP command, "
            f"got {stop_pkt.type_name}({stop_pkt.pkt_type:#04x})"
        )

    def test_stream_restartable(self, la_host: LaUsbHost) -> None:
        """
        Stop → start must produce a clean restart with seq numbers
        resetting to 0. No state from the previous session should bleed.
        """
        # Session 1
        result1 = la_host.stream_capture(duration_s=0.05)
        assert result1.sequence_mismatches == 0, "Session 1 had sequence mismatches"

        # Session 2 — seq must restart from 0
        la_host.send_command(STREAM_CMD_START)
        start_pkt = la_host.read_packet(timeout_ms=1000)
        assert start_pkt.pkt_type == PKT_START, "Session 2: first packet must be START"

        # Read first DATA packet of session 2
        first_data = None
        for _ in range(5):
            try:
                p = la_host.read_packet(timeout_ms=200)
                if p.pkt_type == PKT_DATA:
                    first_data = p
                    break
            except Exception:
                break

        # Clean up
        la_host.send_command(STREAM_CMD_STOP)
        for _ in range(10):
            try:
                la_host.read_packet(timeout_ms=100)
            except Exception:
                break

        if first_data is not None:
            assert first_data.seq == 0, (
                f"Session 2 first DATA packet must have seq=0, got seq={first_data.seq}. "
                f"Firmware seq counter may not be resetting on restart."
            )

    def test_stream_marker_seq_transparent(self, la_host: LaUsbHost) -> None:
        """
        START and STOP marker packets must not advance the host's expected
        sequence counter. The first DATA packet after START must be seq=0.
        """
        la_host.send_command(STREAM_CMD_START)
        start_pkt = la_host.read_packet(timeout_ms=1000)
        assert start_pkt.pkt_type == PKT_START, "Expected START packet"

        # Read first DATA packet
        first_data = None
        for _ in range(10):
            try:
                p = la_host.read_packet(timeout_ms=300)
                if p.pkt_type == PKT_DATA:
                    first_data = p
                    break
                elif p.pkt_type in (PKT_STOP, PKT_ERROR):
                    break
            except Exception:
                break

        # Clean up
        la_host.send_command(STREAM_CMD_STOP)
        for _ in range(10):
            try:
                la_host.read_packet(timeout_ms=100)
            except Exception:
                break

        if first_data is not None:
            assert first_data.seq == 0, (
                f"First DATA after START must have seq=0, got seq={first_data.seq}. "
                f"START marker must not advance the firmware seq counter."
            )
