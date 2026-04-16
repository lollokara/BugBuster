"""
raw_usb_test.py — Verbose multi-cycle LA streaming diagnostic.

BBP port (ESP32 CDC) and vendor bulk (RP2040 interface 3) are on DIFFERENT
USB devices — no IOKit conflict, both can stay open simultaneously.

Tests the full start/stop lifecycle. BBP connection is held open for the
entire run; vendor bulk is held open throughout (no release/re-claim cycles).

Usage:
    .venv/bin/python Diag/raw_usb_test.py \
        --port /dev/cu.usbmodem1234561 \
        --cycles 3 --duration 1.0
"""
import argparse
import sys
import time

sys.path.insert(0, "python")

import usb.core
import usb.util

# USB identifiers (RP2040 vendor bulk device)
VID = 0x2E8A
PID = 0x000C
LA_INTERFACE = 3
EP_IN  = 0x87   # bulk IN  (device → host)
EP_OUT = 0x06   # bulk OUT (host → device)

# Stream commands
STREAM_CMD_STOP  = 0x00
STREAM_CMD_START = 0x01

# Packet types
PKT_START = 0x01
PKT_DATA  = 0x02
PKT_STOP  = 0x03
PKT_ERROR = 0x04

INFO_START_REJECTED = 0x80

PKT_TYPE_NAMES = {PKT_START: "PKT_START", PKT_DATA: "PKT_DATA",
                  PKT_STOP: "PKT_STOP",  PKT_ERROR: "PKT_ERROR"}


def log(msg: str) -> None:
    ts = time.monotonic()
    print(f"[{ts:9.3f}] {msg}", flush=True)


def decode_packets(buf: bytes) -> list[dict]:
    packets, i = [], 0
    while i + 4 <= len(buf):
        pkt_type, seq, payload_len, info = buf[i], buf[i+1], buf[i+2], buf[i+3]
        end = i + 4 + payload_len
        if end > len(buf):
            break
        packets.append({"type": pkt_type, "seq": seq, "payload_len": payload_len,
                         "info": info, "payload": buf[i+4:end]})
        i = end
    return packets


def describe_packet(p: dict) -> str:
    name = PKT_TYPE_NAMES.get(p["type"], f"PKT_0x{p['type']:02x}")
    extra = ""
    if p["type"] == PKT_ERROR and p["info"] == INFO_START_REJECTED:
        extra = " ← bb_la_start_stream() returned false (state not IDLE or pio_loaded=false)"
    elif p["type"] == PKT_STOP:
        extra = f" [stop_reason=0x{p['info']:02x}]"
    return (f"{name} seq={p['seq']} payload_len={p['payload_len']} "
            f"info=0x{p['info']:02x}{extra}")


def drain_ep_in(dev: usb.core.Device, timeout_ms: int = 200, label: str = "drain") -> int:
    total = 0
    while True:
        try:
            raw = bytes(dev.read(EP_IN, 16384, timeout=timeout_ms))
            if not raw:
                break
            log(f"  [{label}] {len(raw)} bytes: {raw[:16].hex()}{'...' if len(raw)>16 else ''}")
            for p in decode_packets(raw):
                log(f"    {describe_packet(p)}")
            total += len(raw)
        except usb.core.USBTimeoutError:
            break
        except Exception as e:
            log(f"  [{label}] read error: {e}")
            break
    return total


def run_cycle(dev: usb.core.Device, bbp, cycle: int, duration_s: float) -> bool:
    log(f"\n{'='*60}")
    log(f"CYCLE {cycle}")
    log(f"{'='*60}")

    # ── 1. BBP preflight (interface stays claimed — different USB device) ─────
    log("\n[1] BBP preflight (vendor bulk stays open, BBP on separate ESP32 USB)...")
    try:
        status = bbp.hat_la_get_status()
        log(f"  Status before reset: {status}")
    except Exception as e:
        log(f"  hat_la_get_status() FAILED: {e}")

    try:
        bbp.hat_la_usb_reset()
        log("  hat_la_usb_reset() OK")
    except Exception as e:
        log(f"  hat_la_usb_reset() FAILED: {e}")
    time.sleep(0.1)

    configured = False
    for attempt in range(5):
        if attempt > 0:
            time.sleep(0.2)
        try:
            bbp.hat_la_configure(channels=4, rate_hz=1_000_000, depth=100_000)
            log(f"  hat_la_configure() OK (attempt {attempt+1})")
            configured = True
            break
        except Exception as e:
            log(f"  hat_la_configure() attempt {attempt+1} FAILED: {e}")

    if not configured:
        log("  PREFLIGHT FAILED — skipping cycle")
        return False

    try:
        status = bbp.hat_la_get_status()
        log(f"  Status after configure: {status}")
    except Exception as e:
        log(f"  hat_la_get_status() post-configure FAILED: {e}")

    # ── 2. Drain stale EP_IN ──────────────────────────────────────────────────
    log("\n[2] Draining stale EP_IN data...")
    stale = drain_ep_in(dev, timeout_ms=100, label="stale")
    if stale == 0:
        log("  No stale data")

    # ── 3. Send STREAM_START via BBP (HAT_CMD_LA_STREAM_START = 0x37) ──────────
    log(f"\n[3] Sending START via BBP (hat_la_stream_start)...")
    try:
        bbp.hat_la_stream_start()
        log(f"  hat_la_stream_start() OK ✓")
    except Exception as e:
        log(f"  hat_la_stream_start() FAILED: {e}")
        return False

    # ── 4. Wait for PKT_START ─────────────────────────────────────────────────
    log("\n[4] Waiting for PKT_START on EP_IN (timeout=3s)...")
    buf = bytearray()
    got_start = False
    t0 = time.monotonic()
    while time.monotonic() - t0 < 3.0:
        try:
            raw = bytes(dev.read(EP_IN, 16384, timeout=500))
            buf.extend(raw)
            log(f"  EP_IN {len(raw)}B: {raw[:16].hex()}{'...' if len(raw)>16 else ''}")
            for p in decode_packets(bytes(buf)):
                log(f"  → {describe_packet(p)}")
                if p["type"] == PKT_START:
                    got_start = True
                elif p["type"] == PKT_ERROR:
                    log(f"  ✗ START REJECTED — bb_la_start_stream() returned false")
                    log(f"    This means LA state != IDLE or pio_loaded=false after configure")
            if got_start:
                log("  ✓ PKT_START received — stream is live")
                break
            buf.clear()   # parsed, clear for next read
        except usb.core.USBTimeoutError:
            log(f"  EP_IN silent for {time.monotonic()-t0:.2f}s (no data received at all)")
            log(f"  Possible causes:")
            log(f"    1. STREAM_CMD_START byte was not received by firmware (EP_OUT broken)")
            log(f"    2. bb_la_start_stream() returned false but PKT_ERROR also lost")
            log(f"    3. PKT_START queued but tud_vendor_n_write_available() returned 0")
            break
        except Exception as e:
            log(f"  EP_IN read error: {e}")
            break

    if not got_start:
        log(f"  ✗ FAILED to receive PKT_START")
        log(f"  Running BBP stop to clean up device state...")
        try:
            bbp.hat_la_stop()
            log(f"  hat_la_stop() OK (cleanup)")
        except Exception as e:
            log(f"  hat_la_stop() FAILED (cleanup): {e}")
        time.sleep(0.5)
        drain_ep_in(dev, timeout_ms=200, label="post-fail-drain")
        return False

    # ── 5. Collect DATA ───────────────────────────────────────────────────────
    log(f"\n[5] Collecting DATA packets for {duration_s}s...")
    data_pkts, data_bytes, seq_errors = 0, 0, 0
    expected_seq = 0
    early_stop = False
    t0 = time.monotonic()
    last_log = t0

    while time.monotonic() - t0 < duration_s:
        try:
            raw = bytes(dev.read(EP_IN, 16384, timeout=200))
            for p in decode_packets(raw):
                if p["type"] == PKT_DATA:
                    if p["seq"] != expected_seq:
                        log(f"  SEQ MISMATCH: expected {expected_seq}, got {p['seq']}")
                        seq_errors += 1
                    expected_seq = (p["seq"] + 1) & 0xFF
                    data_pkts += 1
                    data_bytes += p["payload_len"]
                elif p["type"] == PKT_STOP:
                    log(f"  Firmware sent early: {describe_packet(p)}")
                    early_stop = True
                elif p["type"] == PKT_ERROR:
                    log(f"  Unexpected during stream: {describe_packet(p)}")
            if early_stop:
                break
            now = time.monotonic()
            if now - last_log >= 0.5:
                elapsed = now - t0
                log(f"  {data_pkts} pkts / {data_bytes}B / {data_bytes/elapsed/1024:.1f}KB/s")
                last_log = now
        except usb.core.USBTimeoutError:
            log(f"  EP_IN timeout during stream (got {data_pkts} pkts so far)")
            break
        except Exception as e:
            log(f"  EP_IN error during stream: {e}")
            break

    elapsed = max(time.monotonic() - t0, 0.001)
    log(f"  Stream done: {data_pkts} DATA pkts, {data_bytes}B, "
        f"{data_bytes/elapsed/1024:.1f}KB/s, {seq_errors} seq errors")

    # ── 6. STOP via BBP (vendor bulk stays open — no re-claim needed) ─────────
    if not early_stop:
        # Pre-stop status — confirm RP2040 state before we issue stop.
        try:
            pre_stop_status = bbp.hat_la_get_status()
            log(f"  Pre-stop status: {pre_stop_status}")
        except Exception as e:
            log(f"  hat_la_get_status() pre-stop FAILED: {e}")

        log("\n[6] Sending STOP via BBP (vendor bulk stays open)...")
        t_stop_start = time.perf_counter()
        stop_ok = False
        try:
            bbp.hat_la_stop()
            t_stop_rsp = time.perf_counter()
            log(f"  hat_la_stop() OK  rtt={1000*(t_stop_rsp-t_stop_start):.1f}ms")
            stop_ok = True
        except Exception as e:
            t_stop_rsp = time.perf_counter()
            log(f"  hat_la_stop() FAILED: {e}  rtt={1000*(t_stop_rsp-t_stop_start):.1f}ms")

        # Sleep so Core 0 processes the rearm triggered by bb_la_usb_soft_reset().
        # PKT_STOP is queued in ctrl buffer by HAT_CMD_LA_STOP handler; rearm runs
        # first (clears TX FIFO), then ctrl buffer drains → PKT_STOP goes to EP_IN.
        log("  Sleeping 0.5s for endpoint rearm + PKT_STOP flush...")
        time.sleep(0.5)

        # ── 7. Wait for PKT_STOP on EP_IN ──────────────────────────────────────
        log("\n[7] Waiting for PKT_STOP on EP_IN (timeout=3s)...")
        got_stop = False
        t0 = time.monotonic()
        while time.monotonic() - t0 < 3.0:
            try:
                raw = bytes(dev.read(EP_IN, 16384, timeout=500))
                log(f"  EP_IN {len(raw)}B: {raw[:16].hex()}{'...' if len(raw)>16 else ''}")
                for p in decode_packets(raw):
                    log(f"  → {describe_packet(p)}")
                    if p["type"] == PKT_STOP:
                        got_stop = True
            except usb.core.USBTimeoutError:
                log(f"  EP_IN timeout waiting for PKT_STOP ({time.monotonic()-t0:.2f}s elapsed)")
                break
            except Exception as e:
                log(f"  EP_IN error: {e}")
                break
            if got_stop:
                break

        if got_stop:
            log("  ✓ PKT_STOP received — clean stop")
        else:
            log("  ✗ PKT_STOP NOT received — device in dirty state")
            log("    Possible: rearm cleared TX FIFO before PKT_STOP was flushed,")
            log("    OR EP_IN transfer stuck after stop")
    else:
        log("\n[6/7] Skipped — firmware already sent PKT_STOP")
        got_stop = True

    try:
        status = bbp.hat_la_get_status()
        log(f"  Post-stop status: {status}")
    except Exception as e:
        log(f"  hat_la_get_status() post-stop FAILED: {e}")

    success = data_pkts > 0 and seq_errors == 0
    log(f"\nCYCLE {cycle} {'PASSED ✓' if success else 'FAILED ✗'}")
    return success


def main():
    parser = argparse.ArgumentParser(description="Verbose LA USB streaming diagnostic")
    parser.add_argument("--port", required=True,
                        help="ESP32 BBP serial port (e.g. /dev/cu.usbmodem1234561)")
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--duration", type=float, default=1.0)
    args = parser.parse_args()

    log(f"raw_usb_test — port={args.port} cycles={args.cycles} duration={args.duration}s")
    log(f"RP2040 vendor bulk: VID={VID:#06x} PID={PID:#06x} interface={LA_INTERFACE}")
    log(f"BBP (ESP32 CDC) and vendor bulk (RP2040) are on separate USB devices — no conflict")

    # ── Find and claim RP2040 vendor bulk ─────────────────────────────────────
    log("\nFinding RP2040 vendor bulk device...")
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        log(f"ERROR: Device {VID:#06x}:{PID:#06x} not found")
        sys.exit(1)
    log(f"  Found: {dev.manufacturer} / {dev.product}")

    try:
        usb.util.claim_interface(dev, LA_INTERFACE)
        log(f"  Claimed interface {LA_INTERFACE}")
    except Exception:
        log("  Re-claiming interface...")
        usb.util.release_interface(dev, LA_INTERFACE)
        time.sleep(0.05)
        usb.util.claim_interface(dev, LA_INTERFACE)

    # ── Open BBP (ESP32 serial) — stays open for all cycles ──────────────────
    log(f"\nOpening BBP connection on {args.port} (stays open for all cycles)...")
    import bugbuster as bb
    try:
        bbp = bb.connect_usb(args.port)
        log("  BBP connected")
        # Relay RP2040 bb_la_log() messages to stdout
        bbp.on_la_log(lambda msg: log(f"  [RP2040] {msg.rstrip()}"))
        try:
            bbp.hat_la_log_enable(True)
            log("  LA log relay enabled")
        except Exception as e_log:
            log(f"  LA log relay enable FAILED (non-fatal): {e_log}")
    except Exception as e:
        log(f"  ERROR: {e}")
        usb.util.release_interface(dev, LA_INTERFACE)
        sys.exit(1)

    passed, failed = 0, 0
    try:
        for cycle in range(1, args.cycles + 1):
            ok = run_cycle(dev, bbp, cycle, args.duration)
            if ok:
                passed += 1
            else:
                failed += 1
            if cycle < args.cycles:
                log("\nPausing 0.3s between cycles...")
                time.sleep(0.3)
    finally:
        log("\nFinal cleanup...")
        try:
            bbp.disconnect()
            log("  BBP disconnected")
        except Exception:
            pass
        try:
            usb.util.release_interface(dev, LA_INTERFACE)
            log(f"  Interface {LA_INTERFACE} released")
        except Exception:
            pass
        usb.util.dispose_resources(dev)
        log("  USB resources disposed")

    log(f"\n{'='*60}")
    log(f"RESULTS: {passed}/{args.cycles} cycles passed, {failed} failed")
    log(f"{'='*60}")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
