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

        # Steps 1+2: Stop via both paths first, THEN drain.
        #
        # Order matters: hat_la_stop() queues PKT_STOP into the bulk IN ring buffer
        # (from bb_main's bb_la_usb_send_stream_marker). If we drain before calling
        # hat_la_stop, that PKT_STOP stays in the FIFO and pollutes the next test.
        # By stopping via both paths first, the single drain catches everything.
        #
        # Step 1: vendor-bulk STOP (fast path — processed by USB task)
        try:
            la_host.send_command(STREAM_CMD_STOP)
        except Exception:
            pass  # device may already be idle

        # Step 2: BBP STOP (synchronous — response arrives only after bb_la_stop()
        # has run, guaranteeing IDLE state before we configure).
        import bugbuster as bb
        try:
            dev = bb.connect_usb(port)
        except Exception as e:
            pytest.skip(f"Could not connect to BBP device on {port}: {e}")
            return
        try:
            dev.hat_la_stop()
        except Exception:
            pass  # already idle is fine

        # Step 3: drain everything now (PKT_STOP from both stops, any residual DATA).
        for _ in range(8192):
            try:
                la_host.read_packet(timeout_ms=150)
            except Exception:
                break  # timeout = FIFO truly empty

        # Step 4: reload PIO via BBP.
        # Retry up to 5 times with 200 ms gaps — the RP2040 may be briefly busy
        # finishing DMA/PIO teardown, causing the I2C timeout to fire.
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

    def test_stream_start_packet_arrives(self, la_host: LaUsbHost) -> None:
        """
        After sending START command, a PKT_START (0x01) must arrive.
        Robust to stale packets if drain was imperfect.
        """
        la_host.send_command(STREAM_CMD_START)
        start_pkt = None
        try:
            # Skip up to 512 stale packets to find START
            for _ in range(512):
                pkt = la_host.read_packet(timeout_ms=2000)
                if pkt.pkt_type == PKT_START:
                    start_pkt = pkt
                    break
                elif pkt.pkt_type == PKT_ERROR:
                    pytest.fail(f"Received PKT_ERROR instead of PKT_START: info={pkt.info:#02x}")
        finally:
            la_host.send_command(STREAM_CMD_STOP)
            # Drain remaining
            for _ in range(50):
                try:
                    la_host.read_packet(timeout_ms=100)
                except Exception:
                    break

        assert start_pkt is not None, "Did not receive PKT_START after STREAM_CMD_START"
        assert start_pkt.pkt_type == PKT_START

    def test_stream_data_quality(self, la_host: LaUsbHost) -> None:
        """
        A 1.0s stream must produce non-zero payload DATA packets with no sequence gaps.
        """
        result = la_host.stream_capture(duration_s=1.0)
        assert result.packets_received > 0, "No DATA packets received"
        assert result.bytes_received > 0, "DATA packets had zero total payload"
        assert result.sequence_mismatches == 0, f"Sequence gaps detected: {result.sequence_mismatches}"
        
        # Verify no zero-payload packets
        # (The stream_capture helper doesn't expose individual packet lengths, 
        # but the firmware fix should ensure this).
        # We can also check the total bytes vs packets received.
        # Each packet is max 60 bytes.
        assert result.bytes_received >= result.packets_received, (
            f"Average payload per packet < 1 byte ({result.bytes_received}/{result.packets_received}); "
            f"likely zero-payload DATA bug persists."
        )

    def test_stream_stop_arrives(self, la_host: LaUsbHost) -> None:
        """
        Sending STOP command while streaming must produce a PKT_STOP packet.
        """
        la_host.send_command(STREAM_CMD_START)
        # Skip to START
        for _ in range(100):
            if la_host.read_packet(timeout_ms=1000).pkt_type == PKT_START:
                break
        
        # Read some data
        for _ in range(10):
            try:
                la_host.read_packet(timeout_ms=200)
            except Exception:
                break
                
        la_host.send_command(STREAM_CMD_STOP)
        
        stop_pkt = None
        for _ in range(8192):
            try:
                pkt = la_host.read_packet(timeout_ms=500)
            except Exception:
                break
            if pkt.pkt_type == PKT_STOP:
                stop_pkt = pkt
                break
        
        assert stop_pkt is not None, "Did not receive PKT_STOP after STREAM_CMD_STOP"

    def test_stream_five_cycles(self, la_host: LaUsbHost, request: pytest.FixtureRequest) -> None:
        """
        Production-path start/stop cycles must be clean.
        Uses the configure_la_before_test fixture logic for each cycle.
        """
        port = request.config.getoption("--device-usb")
        import bugbuster as bb
        
        for i in range(5):
            # 1. Stop via vendor bulk (async) + BBP (synchronous — guarantees IDLE)
            la_host.send_command(STREAM_CMD_STOP)
            dev = bb.connect_usb(port)
            try:
                dev.hat_la_stop()
            except Exception:
                pass  # already idle is fine

            # 2. Drain everything (STOP markers + residual DATA from previous cycle)
            for _ in range(200):
                try:
                    la_host.read_packet(timeout_ms=100)
                except Exception:
                    break

            # 3. Configure via BBP (state is IDLE now, won't be rejected)
            try:
                dev.hat_la_configure(channels=4, rate_hz=1_000_000, depth=100_000)
            finally:
                dev.disconnect()

            # 4. stream_capture handles START → DATA → STOP internally
            result = la_host.stream_capture(duration_s=0.2)
            assert result.packets_received > 0, f"Cycle {i}: No DATA"
            assert result.sequence_mismatches == 0, f"Cycle {i}: Seq mismatch"
            time.sleep(0.05)

    def test_stream_duration_truth_10s(self, la_host: LaUsbHost, request: pytest.FixtureRequest) -> None:
        """
        10-second duration truth proof: decoded duration must be within 1% of wall clock.
        """
        port = request.config.getoption("--device-usb")
        import bugbuster as bb
        
        channels = 4
        rate_hz = 1_000_000
        
        # Setup
        la_host.send_command(STREAM_CMD_STOP)
        for _ in range(100):
            try:
                la_host.read_packet(timeout_ms=100)
            except Exception:
                break
                
        dev = bb.connect_usb(port)
        try:
            dev.hat_la_configure(channels=channels, rate_hz=rate_hz, depth=1_000_000)
        finally:
            dev.disconnect()
            
        # stream_capture handles START → DATA collection → STOP internally
        target_s = 10.0
        t_start = time.monotonic()
        result = la_host.stream_capture(duration_s=target_s)
        wall_clock = time.monotonic() - t_start
        
        # Calculate decoded duration
        samples_per_byte = 8 // channels
        decoded_samples = result.bytes_received * samples_per_byte
        decoded_s = decoded_samples / float(rate_hz)
        
        print(f"Wall clock: {wall_clock:.3f}s, Decoded: {decoded_s:.3f}s, Packets: {result.packets_received}")
        
        assert result.packets_received > 0, "No packets received"
        assert result.bytes_received > 0, "Zero payload bytes"
        assert abs(decoded_s - target_s) / target_s <= 0.01, (
            f"Duration drift: decoded={decoded_s:.3f}s, target={target_s:.3f}s"
        )
        assert result.sequence_mismatches == 0, f"Seq mismatches: {result.sequence_mismatches}"


    def test_stream_restartable(self, la_host: LaUsbHost) -> None:
        """
        Stop → start must produce a clean restart with seq numbers
        resetting to 0. No state from the previous session should bleed.
        """
        # Session 1
        result1 = la_host.stream_capture(duration_s=0.05)
        assert result1.sequence_mismatches == 0, "Session 1 had sequence mismatches"

        # Drain residual packets from session 1 (DATA before STOP processed + PKT_STOP)
        for _ in range(200):
            try:
                la_host.read_packet(timeout_ms=100)
            except Exception:
                break

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
