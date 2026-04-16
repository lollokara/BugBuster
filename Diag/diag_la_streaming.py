#!/usr/bin/env python3
"""
LA Streaming Diagnostic — determines if RP2040 survives a streaming cycle.

Uses BBP (UART) for STOP — mirrors the production path used in tests.
EP_OUT is NOT used for STOP (known-broken: TinyUSB DCD drops OUT completions
during active IN streaming). EP_OUT is still used for START (safe: no IN
contention at that point).

Usage (from project root, after power-cycling the RP2040):
    .venv/bin/python Diag/diag_la_streaming.py --port /dev/cu.usbmodem1234561

Exit codes:
    0 = RP2040 alive after streaming
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
    PKT_START,
    PKT_DATA,
    PKT_STOP,
    PKT_ERROR,
    LA_INTERFACE,
)
import usb.util as usb_util

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


def stop_via_bbp(host: LaUsbHost, port: str) -> bool:
    """
    Stop streaming via BBP/UART — the production path.

    1. Release vendor bulk interface (IOKit conflict: cannot hold vendor bulk
       and open CDC serial simultaneously on macOS).
    2. Send hat_la_stop() via BBP.
    3. Sleep 0.5s so Core 0 processes the endpoint rearm.

    Returns True on success.
    """
    # Step 1: release vendor bulk so we can open CDC serial
    if host._dev is not None and host._claimed:
        try:
            usb_util.release_interface(host._dev, LA_INTERFACE)
        except Exception as e:
            print(f"  [STOP] release_interface warning: {e}")
        host._claimed = False

    # Step 2: BBP stop
    print("  Sending hat_la_stop() via BBP...")
    try:
        dev = bb.connect_usb(port)
        try:
            dev.hat_la_stop()
            print("  hat_la_stop() OK")
        finally:
            dev.disconnect()
    except Exception as e:
        print(f"  hat_la_stop() FAILED: {e}")
        return False

    # Step 3: wait for Core 0 endpoint rearm
    print("  Waiting 0.5s for endpoint rearm...")
    time.sleep(0.5)
    return True


def do_streaming_cycle(port: str, duration_s: float = 0.2) -> dict:
    """
    Single START → read data → BBP-STOP cycle.

    START is sent via vendor bulk EP_OUT (safe: no IN contention at start time).
    STOP is sent via BBP/UART (robust: EP_OUT STOP is lost during active IN streaming
    due to RP2040 TinyUSB DCD silicon limitation — see Diag/FINDINGS.md).
    """
    result = {
        "start_ok": False,
        "data_packets": 0,
        "stop_ok": False,
        "pkt_stop_received": False,
        "errors": [],
    }
    host = LaUsbHost()
    try:
        host.connect()
        print("  Sending STREAM_CMD_START via EP_OUT...")
        host.send_command(STREAM_CMD_START)

        # Wait for PKT_START
        try:
            start_pkt = host.wait_for_start(timeout_ms=2000, max_packets=512)
            result["start_ok"] = True
            print(f"  Got: {fmt_pkt(start_pkt)}")
        except Exception as e:
            result["errors"].append(f"No START: {e}")
            print(f"  START FAILED: {e}")
            try:
                raw = host.read_raw(timeout_ms=500)
                raw_hex = raw.hex()[:MAX_RAW_DISPLAY]
                print(f"  Raw EP_IN dump ({len(raw)}B): {raw_hex}")
            except Exception:
                print("  Raw EP_IN dump: <nothing — endpoint silent>")
            host.close()
            return result

        # Read data for duration_s
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
                    print(f"  Early PKT_STOP: {fmt_pkt(pkt)}")
                    result["stop_ok"] = True
                    result["pkt_stop_received"] = True
                    host.close()
                    return result
                elif pkt.pkt_type == PKT_ERROR:
                    result["errors"].append(f"PKT_ERROR info={pkt.info:#x}")
                    print(f"  Got: {fmt_pkt(pkt)}")
            except Exception as e:
                result["errors"].append(f"Read error during data: {e}")
                break

        print(f"  Read {result['data_packets']} DATA packets in {duration_s}s")

        # --- STOP via BBP (not EP_OUT) ---
        # This is the production path: release interface, BBP stop, 0.5s rearm sleep.
        # After release, we can no longer read EP_IN for PKT_STOP — that's expected.
        # The 0.5s sleep gives Core 0 time to emit and rearm before we reconnect.
        result["stop_ok"] = stop_via_bbp(host, port)

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
    parser.add_argument("--post-wait", type=float, default=1.5,
                        help="Wait time after streaming before post-check (default: 1.5)")
    parser.add_argument("--cycles", type=int, default=1,
                        help="Number of streaming cycles to run (default: 1)")
    parser.add_argument("--rate", type=int, default=1_000_000,
                        help="Sample rate Hz (default: 1000000)")
    args = parser.parse_args()

    print("=" * 60)
    print("LA STREAMING DIAGNOSTIC (BBP-stop path)")
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
    if not configure_la(args.port, rate_hz=args.rate):
        print("\n*** Could not configure LA. Power-cycle and re-run.")
        return 2

    # Phase 3: Streaming cycles
    all_ok = True
    for cycle in range(args.cycles):
        cycle_label = f"cycle {cycle + 1}/{args.cycles}" if args.cycles > 1 else "single cycle"
        print(f"\n--- Phase 3: Streaming ({cycle_label}, {args.duration}s) ---")
        stream_result = do_streaming_cycle(port=args.port, duration_s=args.duration)
        print(f"  Summary: start={stream_result['start_ok']}, "
              f"data={stream_result['data_packets']}, "
              f"stop_bbp={stream_result['stop_ok']}, "
              f"pkt_stop={stream_result['pkt_stop_received']}, "
              f"errors={stream_result['errors']}")
        if not stream_result["start_ok"] or not stream_result["stop_ok"]:
            all_ok = False

        if cycle < args.cycles - 1:
            # Re-configure between cycles
            print(f"\n  Re-configuring for cycle {cycle + 2}...")
            if not configure_la(args.port, rate_hz=args.rate):
                print("  *** Re-configure failed — stopping cycles")
                all_ok = False
                break

    # Phase 4: Post-streaming health check
    print(f"\n--- Phase 4: Wait {args.post_wait}s, then post-check ---")
    time.sleep(args.post_wait)
    post_status = None
    for attempt in range(3):
        post_status = check_bbp_alive(args.port, f"POST attempt {attempt + 1}")
        if post_status is not None:
            break
        time.sleep(1.0)

    # Verdict
    print("\n" + "=" * 60)
    if post_status is not None:
        if all_ok:
            print("VERDICT: RP2040 ALIVE and all cycles completed OK")
            print("  -> Streaming path is healthy")
        else:
            print("VERDICT: RP2040 ALIVE but streaming had errors")
            print("  -> Check errors above — likely EP_IN stuck or stream not starting")
        return 0
    else:
        print("VERDICT: RP2040 DEAD after streaming")
        print("  -> RP2040 crashes or hangs during deferred stop / endpoint rearm")
        return 1


if __name__ == "__main__":
    sys.exit(main())
