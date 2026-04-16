"""
repro_five_cycles.py — Reproduce the test_stream_five_cycles 0x11 cascade.

Mirrors the exact pytest flow from:
  tests/device/test_la_usb_bulk.py::TestLaUsbBulk::test_stream_five_cycles

The test_stream_five_cycles test does 5 cycles of:
  1. _stop_preflight_and_configure():
     a. BBP connect
     b. hat_la_get_status()
     c. hat_la_usb_reset() + sleep(0.1)
     d. hat_la_configure() with up to 5 retries
     e. BBP disconnect  ← releases CDC before touching vendor bulk
     f. la_host.close() + la_host.connect() (fresh claim)
     g. la_host._bbp_port = port  (enable BBP STOP path)
  2. la_host.stream_capture(duration_s=0.5):
     a. start_stream() → hat_la_stream_start() via BBP
     b. wait_for_start() → read PKT_START
     c. collect DATA packets for 0.5s
     d. stop_stream() → _stop_stream_via_bbp()
        - drain local buffer
        - BBP connect → hat_la_stop() → BBP disconnect
        - sleep(0.5)

After the first few cycles, the next hat_la_configure() may fail with 0x11
(UART sync loss on the ESP32 HAT link).

This script runs up to 20 cycles to provoke the failure and logs every BBP
command's RTT, RP2040 status, and USB endpoint state.

Usage:
    .venv/bin/python Diag/repro_five_cycles.py --port /dev/cu.usbmodem1234561

Options:
    --cycles N        Number of cycles to run (default 20)
    --duration S      Stream duration per cycle in seconds (default 0.5)
    --keep-bbp        Keep a single BBP connection open (skip disconnect/reconnect)
    --stop-on-fail    Stop immediately on first failure
"""
import argparse
import sys
import time
import traceback

sys.path.insert(0, "python")

import usb.core
import usb.util

# USB identifiers (RP2040 vendor bulk device)
VID = 0x2E8A
PID = 0x000C
LA_INTERFACE = 3
EP_IN  = 0x87   # bulk IN  (device → host)
EP_OUT = 0x06   # bulk OUT (host → device)

# Stream packet types
PKT_START = 0x01
PKT_DATA  = 0x02
PKT_STOP  = 0x03
PKT_ERROR = 0x04

# Info byte constants
INFO_NONE           = 0x00
INFO_COMPRESSED     = 0x01
INFO_START_REJECTED = 0x80
INFO_STOP_HOST      = 0x01

# Command bytes
STREAM_CMD_STOP  = 0x00
STREAM_CMD_START = 0x01

PKT_NAMES = {PKT_START: "PKT_START", PKT_DATA: "PKT_DATA",
             PKT_STOP: "PKT_STOP",  PKT_ERROR: "PKT_ERROR"}

t0_global = time.monotonic()


def log(msg: str) -> None:
    ts = time.monotonic() - t0_global
    print(f"[{ts:9.3f}] {msg}", flush=True)


def decode_packets(buf: bytes) -> list[dict]:
    packets, i = [], 0
    while i + 4 <= len(buf):
        pkt_type, seq, payload_len, info = buf[i], buf[i+1], buf[i+2], buf[i+3]
        end = i + 4 + payload_len
        if end > len(buf):
            break
        packets.append({"type": pkt_type, "seq": seq, "len": payload_len,
                        "info": info, "payload": buf[i+4:end]})
        i = end
    return packets


def pkt_name(t):
    return PKT_NAMES.get(t, f"PKT_0x{t:02x}")


def fmt_status(st: dict) -> str:
    """Format RP2040 status dict into a compact one-liner."""
    parts = [
        f"state={st.get('state_name', '?')}",
        f"rearm_pend={st.get('usb_rearm_pending', '?')}",
        f"rearm_req={st.get('usb_rearm_request_count', '?')}",
        f"rearm_cmp={st.get('usb_rearm_complete_count', '?')}",
    ]
    if 'usb_mounted' in st:
        parts.append(f"usb_mounted={st['usb_mounted']}")
    if 'stream_stop_reason_name' in st:
        parts.append(f"stop_reason={st['stream_stop_reason_name']}")
    if st.get('stream_overrun_count', 0) > 0:
        parts.append(f"overruns={st['stream_overrun_count']}")
    return ", ".join(parts)


def drain_ep_in(dev, timeout_ms=100, label="drain"):
    """Drain stale EP_IN data. Returns bytes drained."""
    total = 0
    while True:
        try:
            raw = bytes(dev.read(EP_IN, 16384, timeout=timeout_ms))
            if not raw:
                break
            total += len(raw)
            pkts = decode_packets(raw)
            if pkts:
                for p in pkts:
                    log(f"  [{label}] {pkt_name(p['type'])} seq={p['seq']} "
                        f"len={p['len']} info=0x{p['info']:02x}")
            else:
                log(f"  [{label}] {len(raw)}B (unparseable): {raw[:16].hex()}")
        except usb.core.USBTimeoutError:
            break
        except Exception as e:
            log(f"  [{label}] error: {e}")
            break
    return total


def _rle_decompress(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i + 1 < len(data):
        val = data[i]
        count = data[i + 1] + 1
        out.extend(bytes([val]) * count)
        i += 2
    return bytes(out)


# ---------------------------------------------------------------------------
# The core cycle — mirrors _stop_preflight_and_configure + stream_capture
# ---------------------------------------------------------------------------

def run_cycle(
    usb_dev: usb.core.Device,
    port: str,
    cycle: int,
    duration_s: float,
    keep_bbp_handle=None,
) -> tuple[bool, str]:
    """Run one full start/stop cycle. Returns (success, failure_reason)."""
    log(f"\n{'='*70}")
    log(f"CYCLE {cycle}")
    log(f"{'='*70}")

    # ── Phase 1: _stop_preflight_and_configure ─────────────────────────
    log(f"\n  [Phase 1] _stop_preflight_and_configure (mirrors pytest fixture)")

    # 1a. Connect BBP (transient, like the test does)
    bbp = keep_bbp_handle
    if bbp is None:
        import bugbuster as bb
        t = time.perf_counter()
        try:
            bbp = bb.connect_usb(port)
            rtt = 1000 * (time.perf_counter() - t)
            log(f"  BBP connect OK  rtt={rtt:.0f}ms")
        except Exception as e:
            log(f"  BBP connect FAILED: {e}")
            return False, f"bbp_connect_failed: {e}"

    # 1b. hat_la_get_status before reset
    try:
        st = bbp.hat_la_get_status()
        log(f"  Status before reset: {fmt_status(st)}")
    except Exception as e:
        log(f"  hat_la_get_status() FAILED: {e}")

    # 1c. hat_la_usb_reset
    t = time.perf_counter()
    try:
        bbp.hat_la_usb_reset()
        rtt = 1000 * (time.perf_counter() - t)
        log(f"  hat_la_usb_reset() OK  rtt={rtt:.1f}ms")
    except Exception as e:
        rtt = 1000 * (time.perf_counter() - t)
        log(f"  hat_la_usb_reset() FAILED: {e}  rtt={rtt:.1f}ms")
        if keep_bbp_handle is None:
            try: bbp.disconnect()
            except: pass
        return False, f"usb_reset_failed: {e}"
    time.sleep(0.1)

    # 1d. hat_la_configure (up to 5 attempts, exact mirror of test)
    configured = False
    last_err = None
    for attempt in range(5):
        if attempt > 0:
            time.sleep(0.2)
        t = time.perf_counter()
        try:
            bbp.hat_la_configure(channels=4, rate_hz=1_000_000, depth=100_000)
            rtt = 1000 * (time.perf_counter() - t)
            log(f"  hat_la_configure() OK (attempt {attempt+1})  rtt={rtt:.1f}ms")
            configured = True
            break
        except Exception as e:
            rtt = 1000 * (time.perf_counter() - t)
            log(f"  hat_la_configure() attempt {attempt+1} FAILED: {e}  rtt={rtt:.1f}ms")
            last_err = e

    if not configured:
        log(f"  ✗ CONFIG FAILED after 5 attempts — this is the 0x11 cascade!")
        log(f"  ✗ Last error: {last_err}")
        # Diagnostic: try to get status even though configure failed
        try:
            st = bbp.hat_la_get_status()
            log(f"  Post-failure status: {fmt_status(st)}")
        except Exception as e2:
            log(f"  hat_la_get_status() also FAILED (UART fully desync'd): {e2}")
        if keep_bbp_handle is None:
            try: bbp.disconnect()
            except: pass
        return False, f"configure_failed_0x11: {last_err}"

    # 1e. Status after configure
    try:
        st = bbp.hat_la_get_status()
        log(f"  Status after configure: {fmt_status(st)}")
    except Exception as e:
        log(f"  hat_la_get_status() post-configure FAILED: {e}")

    # 1f. Disconnect BBP before touching vendor bulk (mirrors test fixture)
    if keep_bbp_handle is None:
        try:
            bbp.disconnect()
            log(f"  BBP disconnected (before vendor bulk phase)")
        except Exception as e:
            log(f"  BBP disconnect FAILED: {e}")

    # 1g. Release and re-claim vendor bulk (mirrors la_host.close() + connect())
    try:
        usb.util.release_interface(usb_dev, LA_INTERFACE)
        time.sleep(0.05)
        usb.util.claim_interface(usb_dev, LA_INTERFACE)
        log(f"  Vendor bulk interface re-claimed (fresh)")
    except Exception as e:
        log(f"  Interface re-claim FAILED: {e}")

    # Drain stale EP_IN
    stale = drain_ep_in(usb_dev, timeout_ms=50, label="stale")
    if stale:
        log(f"  Drained {stale}B stale EP_IN data")
    else:
        log(f"  No stale EP_IN data")

    # ── Phase 2: stream_capture (mirrors la_host.stream_capture) ───────
    log(f"\n  [Phase 2] stream_capture (duration={duration_s}s)")

    # 2a. start_stream via BBP (hat_la_stream_start)
    #     Test uses _bbp_port which opens a TRANSIENT BBP just for the start
    import bugbuster as bb
    bbp2 = keep_bbp_handle
    if bbp2 is None:
        t = time.perf_counter()
        try:
            bbp2 = bb.connect_usb(port)
            rtt = 1000 * (time.perf_counter() - t)
            log(f"  START: BBP connect OK  rtt={rtt:.0f}ms")
        except Exception as e:
            log(f"  START: BBP connect FAILED: {e}")
            return False, f"start_bbp_connect_failed: {e}"

    t = time.perf_counter()
    try:
        bbp2.hat_la_stream_start()
        rtt = 1000 * (time.perf_counter() - t)
        log(f"  hat_la_stream_start() OK  rtt={rtt:.1f}ms")
    except Exception as e:
        rtt = 1000 * (time.perf_counter() - t)
        log(f"  hat_la_stream_start() FAILED: {e}  rtt={rtt:.1f}ms")
        if keep_bbp_handle is None:
            try: bbp2.disconnect()
            except: pass
        return False, f"stream_start_failed: {e}"

    if keep_bbp_handle is None:
        try: bbp2.disconnect()
        except: pass

    # 2b. Wait for PKT_START
    log(f"  Waiting for PKT_START (timeout=2s)...")
    got_start = False
    stream_buf = bytearray()
    t0 = time.monotonic()
    stale_count = 0
    while time.monotonic() - t0 < 2.0:
        try:
            raw = bytes(usb_dev.read(EP_IN, 16384, timeout=500))
            stream_buf.extend(raw)
            # Parse complete packets from buffer
            while len(stream_buf) >= 4:
                plen = stream_buf[2]
                frame_len = 4 + plen
                if len(stream_buf) < frame_len:
                    break
                ptype = stream_buf[0]
                seq = stream_buf[1]
                info = stream_buf[3]
                if ptype == PKT_START:
                    got_start = True
                    log(f"  ✓ PKT_START received (skipped {stale_count} stale packets)")
                    del stream_buf[:frame_len]
                    break
                elif ptype == PKT_ERROR:
                    log(f"  ✗ PKT_ERROR info=0x{info:02x} — START rejected")
                    del stream_buf[:frame_len]
                    break
                else:
                    stale_count += 1
                    del stream_buf[:frame_len]
            if got_start:
                break
        except usb.core.USBTimeoutError:
            log(f"  EP_IN timeout waiting for PKT_START ({time.monotonic()-t0:.1f}s)")
            break
        except Exception as e:
            log(f"  EP_IN error: {e}")
            break

    if not got_start:
        log(f"  ✗ FAILED to receive PKT_START")
        return False, "no_pkt_start"

    # 2c. Collect DATA packets for duration_s
    data_pkts, data_bytes, seq_errors = 0, 0, 0
    expected_seq = 0
    early_stop = False
    t0 = time.monotonic()
    while time.monotonic() - t0 < duration_s:
        try:
            raw = bytes(usb_dev.read(EP_IN, 16384, timeout=200))
            stream_buf.extend(raw)
            while len(stream_buf) >= 4:
                plen = stream_buf[2]
                frame_len = 4 + plen
                if len(stream_buf) < frame_len:
                    break
                ptype = stream_buf[0]
                seq_val = stream_buf[1]
                info = stream_buf[3]
                payload = bytes(stream_buf[4:frame_len])
                del stream_buf[:frame_len]

                if ptype == PKT_DATA:
                    if seq_val != expected_seq:
                        seq_errors += 1
                    expected_seq = (seq_val + 1) & 0xFF
                    if info & INFO_COMPRESSED:
                        payload = _rle_decompress(payload)
                    data_pkts += 1
                    data_bytes += len(payload)
                elif ptype == PKT_STOP:
                    early_stop = True
                    log(f"  Early PKT_STOP info=0x{info:02x}")
                    break
                elif ptype == PKT_ERROR:
                    log(f"  PKT_ERROR during stream info=0x{info:02x}")
                    early_stop = True
                    break
            if early_stop:
                break
        except usb.core.USBTimeoutError:
            log(f"  EP_IN timeout during collect ({data_pkts} pkts so far)")
            break
        except Exception as e:
            log(f"  EP_IN error: {e}")
            break

    elapsed = max(time.monotonic() - t0, 0.001)
    log(f"  Stream: {data_pkts} pkts / {data_bytes}B / "
        f"{data_bytes/elapsed/1024:.1f}KB/s / {seq_errors} seq errors")

    # 2d. stop_stream → _stop_stream_via_bbp
    if not early_stop:
        # Drain local buffer (mirrors _stop_stream_via_bbp local drain)
        local_drain_pkts = 0
        while len(stream_buf) >= 4:
            plen = stream_buf[2]
            frame_len = 4 + plen
            if len(stream_buf) < frame_len:
                break
            ptype = stream_buf[0]
            del stream_buf[:frame_len]
            if ptype == PKT_DATA:
                local_drain_pkts += 1
            elif ptype == PKT_STOP:
                log(f"  PKT_STOP found in local buffer (firmware self-stopped)")
                early_stop = True
                break
        if local_drain_pkts:
            log(f"  Drained {local_drain_pkts} DATA pkts from local buffer")

    if not early_stop:
        # Send STOP via BBP (transient connection, mirrors _stop_stream_via_bbp)
        bbp3 = keep_bbp_handle
        if bbp3 is None:
            try:
                bbp3 = bb.connect_usb(port)
            except Exception as e:
                log(f"  STOP: BBP connect FAILED: {e}")
                bbp3 = None

        if bbp3 is not None:
            t = time.perf_counter()
            try:
                bbp3.hat_la_stop()
                rtt = 1000 * (time.perf_counter() - t)
                log(f"  hat_la_stop() OK  rtt={rtt:.1f}ms")
            except Exception as e:
                rtt = 1000 * (time.perf_counter() - t)
                log(f"  hat_la_stop() FAILED: {e}  rtt={rtt:.1f}ms")
            if keep_bbp_handle is None:
                try: bbp3.disconnect()
                except: pass

        # Sleep 0.5s — mirrors _stop_stream_via_bbp
        log(f"  Sleeping 0.5s (endpoint rearm window)...")
        time.sleep(0.5)

    # Final status check
    bbp4 = keep_bbp_handle
    if bbp4 is None:
        try:
            bbp4 = bb.connect_usb(port)
        except Exception as e:
            log(f"  Post-stop BBP connect FAILED: {e}")
            bbp4 = None

    if bbp4 is not None:
        try:
            st = bbp4.hat_la_get_status()
            log(f"  Post-stop status: {fmt_status(st)}")
        except Exception as e:
            log(f"  hat_la_get_status() post-stop FAILED: {e}")
        if keep_bbp_handle is None:
            try: bbp4.disconnect()
            except: pass

    success = data_pkts > 0 and seq_errors == 0
    log(f"\nCYCLE {cycle}: {'PASSED ✓' if success else 'FAILED ✗'}")
    if not success:
        reason = []
        if data_pkts == 0:
            reason.append("no_data")
        if seq_errors > 0:
            reason.append(f"seq_errors={seq_errors}")
        return False, ", ".join(reason)
    return True, "pass"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Reproduce test_stream_five_cycles 0x11 cascade"
    )
    parser.add_argument("--port", required=True,
                        help="ESP32 BBP serial port (e.g. /dev/cu.usbmodem1234561)")
    parser.add_argument("--cycles", type=int, default=20,
                        help="Number of cycles to run (default 20)")
    parser.add_argument("--duration", type=float, default=0.5,
                        help="Stream duration per cycle in seconds (default 0.5)")
    parser.add_argument("--keep-bbp", action="store_true",
                        help="Keep a single BBP connection open (skip disconnect/reconnect)")
    parser.add_argument("--stop-on-fail", action="store_true",
                        help="Stop immediately on the first failure")
    args = parser.parse_args()

    log(f"repro_five_cycles — port={args.port} cycles={args.cycles} "
        f"duration={args.duration}s keep_bbp={args.keep_bbp}")
    log(f"Reproducing: test_stream_five_cycles (5-cycle start/stop with "
        f"_stop_preflight_and_configure between each cycle)")
    log(f"Looking for: 0x11 error on hat_la_configure() after multiple cycles")

    # Find RP2040 vendor bulk device
    log("\nFinding RP2040 vendor bulk device...")
    usb_dev = usb.core.find(idVendor=VID, idProduct=PID)
    if usb_dev is None:
        log(f"ERROR: Device {VID:#06x}:{PID:#06x} not found")
        sys.exit(1)
    log(f"  Found: {usb_dev.manufacturer} / {usb_dev.product}")

    try:
        usb.util.claim_interface(usb_dev, LA_INTERFACE)
        log(f"  Claimed interface {LA_INTERFACE}")
    except Exception:
        log("  Re-claiming interface...")
        usb.util.release_interface(usb_dev, LA_INTERFACE)
        time.sleep(0.05)
        usb.util.claim_interface(usb_dev, LA_INTERFACE)
        log(f"  Claimed interface {LA_INTERFACE}")

    # Open persistent BBP if --keep-bbp
    import bugbuster as bb
    persistent_bbp = None
    if args.keep_bbp:
        log(f"\nOpening persistent BBP on {args.port}...")
        try:
            persistent_bbp = bb.connect_usb(args.port)
            persistent_bbp.on_la_log(lambda msg: log(f"  [RP2040] {msg.rstrip()}"))
            try:
                persistent_bbp.hat_la_log_enable(True)
                log("  LA log relay enabled")
            except Exception as e:
                log(f"  LA log relay FAILED (non-fatal): {e}")
            log("  Persistent BBP connected")
        except Exception as e:
            log(f"  Persistent BBP connect FAILED: {e}")
            persistent_bbp = None

    results = []
    try:
        for cycle in range(1, args.cycles + 1):
            ok, reason = run_cycle(
                usb_dev, args.port, cycle, args.duration,
                keep_bbp_handle=persistent_bbp,
            )
            results.append((cycle, ok, reason))

            if not ok and args.stop_on_fail:
                log(f"\n  --stop-on-fail: stopping after cycle {cycle}")
                break

            if cycle < args.cycles:
                # Brief pause between cycles (no extra sleep needed — the
                # _stop_stream_via_bbp already sleeps 0.5s)
                pass

    except KeyboardInterrupt:
        log("\n\nInterrupted by user")
    finally:
        log("\n\nFinal cleanup...")
        if persistent_bbp is not None:
            try:
                persistent_bbp.hat_la_log_enable(False)
            except Exception:
                pass
            try:
                persistent_bbp.disconnect()
                log("  Persistent BBP disconnected")
            except Exception:
                pass
        try:
            usb.util.release_interface(usb_dev, LA_INTERFACE)
            log(f"  Interface {LA_INTERFACE} released")
        except Exception:
            pass
        usb.util.dispose_resources(usb_dev)
        log("  USB resources disposed")

    # Summary
    passed = sum(1 for _, ok, _ in results if ok)
    failed = sum(1 for _, ok, _ in results if not ok)
    total = len(results)

    log(f"\n{'='*70}")
    log(f"RESULTS SUMMARY: {passed}/{total} passed, {failed} failed")
    log(f"{'='*70}")
    for cycle, ok, reason in results:
        status = "PASS ✓" if ok else f"FAIL ✗ ({reason})"
        log(f"  Cycle {cycle:3d}: {status}")

    if failed > 0:
        log(f"\n{'='*70}")
        log("FAILURE ANALYSIS")
        log(f"{'='*70}")
        first_fail = next((c, r) for c, ok, r in results if not ok)
        log(f"  First failure at cycle {first_fail[0]}: {first_fail[1]}")
        if "0x11" in str(first_fail[1]) or "configure_failed" in str(first_fail[1]):
            log(f"  Root cause: 0x11 = UART sync loss between ESP32 and RP2040")
            log(f"  The RP2040 is likely still in STREAMING state when configure")
            log(f"  is called. bb_la_configure() rejects it (state != IDLE/ERROR)")
            log(f"  and sends HAT_RSP_ERROR over BB_UART. The ESP32 misinterprets")
            log(f"  the response, losing UART framing → all subsequent BBP commands")
            log(f"  fail with 0x11 (which is actually CmdId.SET_DAC_CODE being read")
            log(f"  as an error code byte).")
            log(f"")
            log(f"  Likely race: s_streaming_session or la_state not fully cleared")
            log(f"  before Core 1 processes the next hat_la_configure() via BBP.")
        log(f"{'='*70}")

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
