#!/usr/bin/env python3
"""
LA Streaming Diagnostic — determines if RP2040 survives a streaming cycle.

Usage (from project root, after power-cycling the RP2040):
    .venv/bin/python Diag/diag_la_streaming.py --port /dev/cu.usbmodem1234561

Exit codes:
    0 = RP2040 alive after streaming (protocol-level issue)
    1 = RP2040 dead after streaming (crash/hang — firmware issue)
    2 = RP2040 dead before streaming (already stuck — power-cycle first)
"""
from __future__ import annotations

import argparse
import sys
import time

# Add project paths
sys.path.insert(0, "python")
sys.path.insert(0, "tests")

import bugbuster as bb
from tests.mock.la_usb_host import (
    LaUsbHost,
    STREAM_CMD_START,
    STREAM_CMD_STOP,
    PKT_START,
    PKT_DATA,
    PKT_STOP,
    PKT_ERROR,
)

# Max chars for raw hex dumps
MAX_RAW_DISPLAY = 200

PKT_TYPE_NAMES = {PKT_START: "START", PKT_DATA: "DATA", PKT_STOP: "STOP", PKT_ERROR: "ERROR"}


def fmt_pkt(pkt) -> str:
    """Format a StreamPacket for display with clamped raw payload."""
    name = PKT_TYPE_NAMES.get(pkt.pkt_type, f"UNK({pkt.pkt_type:#04x})")
    raw_hex = pkt.payload.hex()
    if len(raw_hex) > MAX_RAW_DISPLAY:
        raw_hex = raw_hex[:MAX_RAW_DISPLAY] + f"...({len(pkt.payload)}B total)"
    return f"{name} seq={pkt.seq} len={pkt.payload_len} info={pkt.info:#04x} raw={raw_hex}"


def check_bbp_alive(port: str, label: str) -> dict | None:
    """Try to get LA status via BBP. Returns status dict or None on failure."""
    import logging
    # Temporarily capture BBP transport warnings to show stale frames
    captured_warnings: list[str] = []
    _handler = logging.Handler()
    _handler.emit = lambda record: captured_warnings.append(record.getMessage()) \
        if len(captured_warnings) < 10 else None
    _handler.setLevel(logging.WARNING)
    bbp_logger = logging.getLogger("bugbuster.transport.usb")
    bbp_logger.addHandler(_handler)

    try:
        dev = bb.connect_usb(port)
        status = dev.hat_la_get_status()
        dev.disconnect()
        print(f"  [{label}] Status: state={status.get('state')}, "
              f"usb_connected={status.get('usb_connected')}, "
              f"usb_mounted={status.get('usb_mounted')}, "
              f"rearm_pending={status.get('usb_rearm_pending')}, "
              f"stop_reason={status.get('stream_stop_reason')}, "
              f"overrun_count={status.get('stream_overrun_count')}")
        if captured_warnings:
            print(f"  [{label}] BBP warnings during connect:")
            for w in captured_warnings[:5]:
                print(f"    {w[:120]}")
        return status
    except Exception as e:
        print(f"  [{label}] FAILED: {e}")
        if captured_warnings:
            print(f"  [{label}] BBP warnings (stale frame evidence):")
            for w in captured_warnings[:8]:
                print(f"    {w[:120]}")
        try:
            dev.disconnect()
        except Exception:
            pass
        return None
    finally:
        bbp_logger.removeHandler(_handler)


def configure_la(port: str, channels: int = 4, rate_hz: int = 1_000_000,
                 depth: int = 100_000) -> bool:
    """USB reset + configure via BBP. Returns True on success."""
    try:
        dev = bb.connect_usb(port)
        print("  Sending hat_la_usb_reset()...")
        dev.hat_la_usb_reset()
        time.sleep(0.1)
        print("  Sending hat_la_configure()...")
        dev.hat_la_configure(channels=channels, rate_hz=rate_hz, depth=depth)
        status = dev.hat_la_get_status()
        print(f"  Post-configure status: state={status.get('state')}")
        dev.disconnect()
        return True
    except Exception as e:
        print(f"  Configure FAILED: {e}")
        try:
            dev.disconnect()
        except Exception:
            pass
        return False


def do_streaming_cycle(duration_s: float = 0.2) -> dict:
    """Do a single START → read data → STOP cycle via vendor bulk."""
    result = {
        "start_ok": False,
        "data_packets": 0,
        "stop_ok": False,
        "errors": [],
    }
    host = LaUsbHost()
    try:
        host.connect()
        print("  Sending STREAM_CMD_START...")
        host.send_command(STREAM_CMD_START)

        try:
            start_pkt = host.wait_for_start(timeout_ms=2000, max_packets=512)
            result["start_ok"] = True
            print(f"  Got: {fmt_pkt(start_pkt)}")
        except Exception as e:
            result["errors"].append(f"No START: {e}")
            print(f"  START FAILED: {e}")
            # Try a raw read to see what (if anything) is on EP_IN
            try:
                raw = host.read_raw(timeout_ms=500)
                raw_hex = raw.hex()[:MAX_RAW_DISPLAY]
                print(f"  Raw EP_IN dump ({len(raw)}B): {raw_hex}")
            except Exception:
                print("  Raw EP_IN dump: <nothing — endpoint silent>")
            host.close()
            return result

        # Read data for duration_s, show first few packets in detail
        t0 = time.monotonic()
        shown = 0
        while time.monotonic() - t0 < duration_s:
            try:
                pkt = host.read_packet(timeout_ms=200)
                if pkt.pkt_type == PKT_DATA:
                    result["data_packets"] += 1
                    if shown < 3:
                        print(f"  Got: {fmt_pkt(pkt)}")
                        shown += 1
                    elif shown == 3:
                        print("  ... (further DATA packets suppressed)")
                        shown += 1
                elif pkt.pkt_type == PKT_STOP:
                    print(f"  Early: {fmt_pkt(pkt)}")
                    result["stop_ok"] = True
                    host.close()
                    return result
                elif pkt.pkt_type == PKT_ERROR:
                    result["errors"].append(f"PKT_ERROR info={pkt.info:#x}")
                    print(f"  Got: {fmt_pkt(pkt)}")
            except Exception as e:
                result["errors"].append(f"Read error: {e}")
                break

        print(f"  Read {result['data_packets']} DATA packets in {duration_s}s")

        # Send STOP
        print("  Sending STREAM_CMD_STOP...")
        host.send_command(STREAM_CMD_STOP)
        time.sleep(0.5)  # let deferred stop process

        # Drain until PKT_STOP or timeout
        deadline = time.monotonic() + 2.0
        drain_shown = 0
        while time.monotonic() < deadline:
            try:
                pkt = host.read_packet(timeout_ms=200)
                if pkt.pkt_type == PKT_DATA:
                    result["data_packets"] += 1
                elif pkt.pkt_type == PKT_STOP:
                    print(f"  Got: {fmt_pkt(pkt)}")
                    result["stop_ok"] = True
                    break
                elif pkt.pkt_type == PKT_ERROR:
                    print(f"  Got: {fmt_pkt(pkt)}")
                    result["errors"].append(f"PKT_ERROR after STOP: info={pkt.info:#x}")
                    break
                else:
                    if drain_shown < 3:
                        print(f"  Drain unexpected: {fmt_pkt(pkt)}")
                        drain_shown += 1
            except Exception:
                print("  Drain timeout (no PKT_STOP received)")
                break

        host.close()
    except Exception as e:
        result["errors"].append(f"Streaming exception: {e}")
        print(f"  Streaming exception: {e}")
        try:
            host.close()
        except Exception:
            pass

    return result


def main():
    parser = argparse.ArgumentParser(description="LA Streaming Diagnostic")
    parser.add_argument("--port", default="/dev/cu.usbmodem1234561",
                        help="ESP32 BBP serial port")
    parser.add_argument("--duration", type=float, default=0.2,
                        help="Streaming duration in seconds (default: 0.2)")
    parser.add_argument("--post-wait", type=float, default=2.0,
                        help="Wait time after streaming before post-check (default: 2.0)")
    args = parser.parse_args()

    print("=" * 60)
    print("LA STREAMING DIAGNOSTIC")
    print("=" * 60)

    # Phase 1: Pre-streaming health check
    print("\n--- Phase 1: Pre-streaming health check ---")
    pre_status = check_bbp_alive(args.port, "PRE")
    if pre_status is None:
        print("\n*** RP2040 is ALREADY DEAD (not responding to BBP commands).")
        print("*** Power-cycle the RP2040 (unplug/replug USB) and re-run.")
        return 2

    # Phase 2: Configure LA
    print("\n--- Phase 2: Configure LA ---")
    if not configure_la(args.port):
        print("\n*** Could not configure LA. Power-cycle and re-run.")
        return 2

    # Phase 3: Streaming cycle
    print(f"\n--- Phase 3: Streaming cycle ({args.duration}s) ---")
    stream_result = do_streaming_cycle(duration_s=args.duration)
    print(f"  Summary: start={stream_result['start_ok']}, "
          f"data={stream_result['data_packets']}, "
          f"stop={stream_result['stop_ok']}, "
          f"errors={stream_result['errors']}")

    # Phase 4: Wait for deferred stop to complete
    print(f"\n--- Phase 4: Wait {args.post_wait}s for deferred stop ---")
    time.sleep(args.post_wait)

    # Phase 5: Post-streaming health check
    print("\n--- Phase 5: Post-streaming health check ---")
    for attempt in range(3):
        post_status = check_bbp_alive(args.port, f"POST attempt {attempt + 1}")
        if post_status is not None:
            break
        time.sleep(1.0)

    # Verdict
    print("\n" + "=" * 60)
    if post_status is not None:
        print("VERDICT: RP2040 ALIVE after streaming")
        print("  -> Issue is protocol-level (stale BBP responses, seq collision)")
        print("  -> Fix: Python-side flush + seq randomization (Step 2 of plan)")
        return 0
    else:
        print("VERDICT: RP2040 DEAD after streaming")
        print("  -> RP2040 crashes or hangs during deferred stop / endpoint rearm")
        print("  -> Fix: Guard rearm path in send_pending() (Step 1a of plan)")
        return 1


if __name__ == "__main__":
    sys.exit(main())
