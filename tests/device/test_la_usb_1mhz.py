"""
Focused 1 MHz / 4-channel / 10-second proof for Logic Analyzer.
"""
import time
import pytest
import bugbuster as bb
from tests.mock.la_usb_host import LaUsbHost

@pytest.mark.usb_only
@pytest.mark.requires_hat
def test_stream_1mhz_4ch_10s(request: pytest.FixtureRequest):
    if not request.config.getoption("--hat", default=False):
        pytest.skip("--hat flag required")

    port = request.config.getoption("--device-usb", default=None)
    if port is None:
        pytest.skip("--device-usb <port> required")

    channels = 4
    rate_hz = 1_000_000
    target_s = 10.0
    depth = 11_000_000  # > 10s at 1MHz — stops on duration not depth

    print(f"\n--- Starting 1 MHz / 4-ch / 10s Proof ---")

    # Phase 1: configure via BBP, then disconnect before claiming vendor bulk.
    # On macOS, pyserial (CDC0) and pyusb (vendor interface 3) conflict when
    # both hold the device simultaneously — EP_OUT becomes broken after the
    # first la_host.close() + re-claim cycle.  Disconnecting BBP first avoids
    # macOS USB arbitration issues.
    dev = bb.connect_usb(port)
    print("Preflight: Resetting USB endpoints...")
    dev.hat_la_usb_reset()
    time.sleep(0.1)

    print(f"Configuring LA: {rate_hz/1e6:.1f} MHz, {channels} ch...")
    dev.hat_la_configure(channels=channels, rate_hz=rate_hz, depth=depth)
    dev.disconnect()  # release CDC0 before claiming vendor interface 3

    # Phase 2: stream directly via vendor bulk (no BBP involved).
    la_host = LaUsbHost()
    la_host.connect()
    try:
        print(f"Starting stream for {target_s}s...")
        t_start = time.monotonic()
        result = la_host.stream_capture(duration_s=target_s)
        wall_clock = time.monotonic() - t_start
    finally:
        la_host.close()

    # Calculate decoded duration: 4 channels → 1 nibble/sample → 2 samples/byte
    samples_per_byte = 8 // channels
    decoded_samples = result.bytes_received * samples_per_byte
    decoded_s = decoded_samples / float(rate_hz)

    print(f"Result:             {result.stop_reason}")
    print(f"Wall Clock:         {wall_clock:.3f}s")
    print(f"Decoded Duration:   {decoded_s:.3f}s")
    print(f"Bytes Received:     {result.bytes_received}")
    print(f"Packets Received:   {result.packets_received}")
    print(f"Seq Mismatches:     {result.sequence_mismatches}")
    if result.errors:
        print(f"Errors:             {result.errors}")

    assert not result.errors, f"Stream had errors: {result.errors}"
    assert result.stop_reason == "host_stop", f"Stream ended early: {result.stop_reason}"
    assert result.sequence_mismatches == 0, "Sequence mismatches detected"
    assert abs(decoded_s - target_s) / target_s <= 0.01, \
        f"Duration drift too high: {decoded_s:.3f}s vs {target_s}s"

    print("--- 1 MHz Proof PASSED ---")
