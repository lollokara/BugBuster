"""
repro_pytest_ordering.py — Reproduce the test_la_usb_bulk → test_stream_1mhz_4ch_10s
failure sequence with granular logging.

Mirrors the exact pytest flow:
  Phase A: test_stream_start_packet_arrives  (EP_OUT START, BBP STOP)
  Phase B: test_stream_marker_seq_transparent (EP_OUT START, BBP STOP)
  Phase C: test_rle_compressed_payload_is_even_length (EP_OUT START, BBP STOP)
  Phase D: test_stream_1mhz_4ch_10s (BBP usb_reset+configure, EP_OUT START, 10s stream)

BBP connection kept open throughout for RP2040 log relay.
Each phase runs the exact same preflight as the pytest fixture.

Usage:
    .venv/bin/python Diag/repro_pytest_ordering.py \
        --port /dev/cu.usbmodem1234561
"""
import argparse
import sys
import time

sys.path.insert(0, "python")

import usb.core
import usb.util

VID = 0x2E8A
PID = 0x000C
LA_INTERFACE = 3
EP_IN  = 0x87
EP_OUT = 0x06

PKT_START = 0x01
PKT_DATA  = 0x02
PKT_STOP  = 0x03
PKT_ERROR = 0x04

STREAM_CMD_STOP  = 0x00
STREAM_CMD_START = 0x01

t0_global = time.monotonic()


def log(msg: str) -> None:
    ts = time.monotonic() - t0_global
    print(f"[{ts:8.3f}] {msg}", flush=True)


def decode_packets(buf: bytes) -> list:
    pkts, i = [], 0
    while i + 4 <= len(buf):
        ptype, seq, plen, info = buf[i], buf[i+1], buf[i+2], buf[i+3]
        end = i + 4 + plen
        if end > len(buf):
            break
        pkts.append({"type": ptype, "seq": seq, "len": plen, "info": info,
                     "payload": buf[i+4:end]})
        i = end
    return pkts


def pkt_name(t):
    return {PKT_START:"PKT_START", PKT_DATA:"PKT_DATA",
            PKT_STOP:"PKT_STOP", PKT_ERROR:"PKT_ERROR"}.get(t, f"PKT_0x{t:02x}")


def drain_ep_in(dev, timeout_ms=100, label="drain"):
    """Drain stale EP_IN data. Returns bytes drained."""
    total = 0
    while True:
        try:
            raw = bytes(dev.read(EP_IN, 16384, timeout=timeout_ms))
            if not raw:
                break
            log(f"  [{label}] {len(raw)}B: {raw[:16].hex()}")
            for p in decode_packets(raw):
                log(f"    {pkt_name(p['type'])} seq={p['seq']} info=0x{p['info']:02x}")
            total += len(raw)
        except usb.core.USBTimeoutError:
            break
        except Exception as e:
            log(f"  [{label}] error: {e}")
            break
    return total


def bbp_preflight(bbp, dev, *, channels=4, rate_hz=1_000_000, depth=100_000, label="preflight"):
    """Mirror the pytest configure_la_before_test fixture exactly."""
    log(f"\n  [{label}] --- preflight start ---")

    # Status before reset
    try:
        st = bbp.hat_la_get_status()
        log(f"  [{label}] status before reset: state={st.get('state_name')} "
            f"rearm_req={st.get('usb_rearm_request_count')} "
            f"rearm_cmp={st.get('usb_rearm_complete_count')} "
            f"rearm_pend={st.get('usb_rearm_pending')}")
    except Exception as e:
        log(f"  [{label}] hat_la_get_status() failed: {e}")

    # hat_la_usb_reset
    t = time.perf_counter()
    try:
        bbp.hat_la_usb_reset()
        log(f"  [{label}] hat_la_usb_reset() OK  rtt={1000*(time.perf_counter()-t):.1f}ms")
    except Exception as e:
        log(f"  [{label}] hat_la_usb_reset() FAILED: {e}  rtt={1000*(time.perf_counter()-t):.1f}ms")
        return False
    time.sleep(0.1)

    # Small pause to let any log frames arrive and be consumed by relay
    time.sleep(0.05)
    log(f"  [{label}] (50ms log-drain pause after usb_reset)")

    # hat_la_configure (up to 5 attempts, mirroring fixture)
    configured = False
    for attempt in range(5):
        if attempt > 0:
            time.sleep(0.2)
        t = time.perf_counter()
        try:
            bbp.hat_la_configure(channels=channels, rate_hz=rate_hz, depth=depth)
            log(f"  [{label}] hat_la_configure() OK (attempt {attempt+1})  rtt={1000*(time.perf_counter()-t):.1f}ms")
            configured = True
            break
        except Exception as e:
            log(f"  [{label}] hat_la_configure() attempt {attempt+1} FAILED: {e}  rtt={1000*(time.perf_counter()-t):.1f}ms")

    if not configured:
        return False

    # Status after configure
    try:
        st = bbp.hat_la_get_status()
        log(f"  [{label}] status after configure: state={st.get('state_name')} "
            f"rearm_req={st.get('usb_rearm_request_count')} "
            f"rearm_cmp={st.get('usb_rearm_complete_count')} "
            f"rearm_pend={st.get('usb_rearm_pending')}")
    except Exception as e:
        log(f"  [{label}] hat_la_get_status() post-configure failed: {e}")

    # Drain stale EP_IN (the fresh connect in the fixture clears this,
    # but we're keeping the interface open, so drain manually)
    stale = drain_ep_in(dev, timeout_ms=100, label=f"{label}/stale")
    if stale == 0:
        log(f"  [{label}] no stale EP_IN data")

    log(f"  [{label}] --- preflight done ---")
    return True


def bbp_stop(bbp, label="stop"):
    """Mirror _stop_stream_via_bbp: call hat_la_stop via the open BBP connection."""
    log(f"\n  [{label}] sending STOP via BBP...")
    t = time.perf_counter()
    try:
        bbp.hat_la_stop()
        rtt = 1000 * (time.perf_counter() - t)
        log(f"  [{label}] hat_la_stop() OK  rtt={rtt:.1f}ms")
    except Exception as e:
        rtt = 1000 * (time.perf_counter() - t)
        log(f"  [{label}] hat_la_stop() FAILED: {e}  rtt={rtt:.1f}ms")

    # Mirror the 0.5s sleep in _stop_stream_via_bbp
    log(f"  [{label}] sleeping 0.5s (endpoint rearm window)...")
    time.sleep(0.5)
    log(f"  [{label}] sleep done")


def ep_out_start(dev, bbp, label="start"):
    """Send STREAM_CMD_START directly via EP_OUT (mirrors send_command()).
    Also polls RP2040 status immediately after write to see if it was processed."""
    log(f"  [{label}] sending STREAM_CMD_START via EP_OUT...")
    t = time.perf_counter()
    try:
        dev.write(EP_OUT, bytes([STREAM_CMD_START]), timeout=2000)
        rtt = 1000*(time.perf_counter()-t)
        log(f"  [{label}] EP_OUT write OK  rtt={rtt:.1f}ms  (bytes on the bus)")
    except Exception as e:
        log(f"  [{label}] EP_OUT write FAILED: {e}  rtt={1000*(time.perf_counter()-t):.1f}ms")
        return False

    # Poll status immediately: did RP2040 transition out of idle?
    time.sleep(0.02)
    try:
        st = bbp.hat_la_get_status()
        log(f"  [{label}] status 20ms after EP_OUT write: state={st.get('state_name')} "
            f"streaming={st.get('state_name')=='streaming'} "
            f"rearm_pend={st.get('usb_rearm_pending')} "
            f"rearm_req={st.get('usb_rearm_request_count')} "
            f"rearm_cmp={st.get('usb_rearm_complete_count')}")
    except Exception as e:
        log(f"  [{label}] status poll after write FAILED: {e}")

    return True


def wait_for_pkt_start(dev, bbp, timeout_ms=2000, label="wait_start"):
    """Wait for PKT_START on EP_IN, consuming and logging all packets seen."""
    log(f"  [{label}] waiting for PKT_START (timeout={timeout_ms}ms)...")
    buf = bytearray()
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout_ms / 1000:
        try:
            raw = bytes(dev.read(EP_IN, 16384, timeout=500))
            buf.extend(raw)
            log(f"  [{label}] EP_IN {len(raw)}B: {raw[:16].hex()}")
            for p in decode_packets(bytes(buf)):
                log(f"  [{label}]   {pkt_name(p['type'])} seq={p['seq']} info=0x{p['info']:02x}")
                if p["type"] == PKT_START:
                    log(f"  [{label}] ✓ PKT_START received")
                    return True
                elif p["type"] == PKT_ERROR:
                    log(f"  [{label}] ✗ PKT_ERROR (start rejected)")
                    return False
            buf.clear()
        except usb.core.USBTimeoutError:
            elapsed = time.monotonic() - t0
            log(f"  [{label}] EP_IN silent ({elapsed:.2f}s elapsed)")
            break
        except Exception as e:
            log(f"  [{label}] EP_IN error: {e}")
            break
    log(f"  [{label}] ✗ FAILED to receive PKT_START — querying RP2040 state via BBP...")
    try:
        st = bbp.hat_la_get_status()
        log(f"  [{label}] post-timeout RP2040 state: state={st.get('state_name')} "
            f"rearm_pend={st.get('usb_rearm_pending')} "
            f"rearm_req={st.get('usb_rearm_request_count')} "
            f"rearm_cmp={st.get('usb_rearm_complete_count')} "
            f"usb_mounted={st.get('usb_mounted')}")
        log(f"  [{label}] DIAGNOSIS: RP2040 state={st.get('state_name')!r} — "
            + ("EP_OUT command received but bb_la_start_stream() rejected"
               if st.get('state_name') == 'idle'
               else "RP2040 may not have received EP_OUT byte at all"))
    except Exception as e:
        log(f"  [{label}] BBP status query failed: {e}")
    return False


def collect_data(dev, duration_s=0.5, label="collect"):
    """Collect DATA packets for duration_s seconds. Returns (count, bytes, seq_errors)."""
    pkts, nbytes, seq_errors, expected = 0, 0, 0, 0
    t0 = time.monotonic()
    while time.monotonic() - t0 < duration_s:
        try:
            raw = bytes(dev.read(EP_IN, 16384, timeout=200))
            for p in decode_packets(raw):
                if p["type"] == PKT_DATA:
                    if p["seq"] != expected:
                        log(f"  [{label}] SEQ MISMATCH: expected {expected} got {p['seq']}")
                        seq_errors += 1
                    expected = (p["seq"] + 1) & 0xFF
                    pkts += 1
                    nbytes += p["len"]
                elif p["type"] == PKT_STOP:
                    log(f"  [{label}] early PKT_STOP info=0x{p['info']:02x}")
                    return pkts, nbytes, seq_errors
        except usb.core.USBTimeoutError:
            log(f"  [{label}] EP_IN timeout during collect ({pkts} pkts so far)")
            break
        except Exception as e:
            log(f"  [{label}] error: {e}")
            break
    return pkts, nbytes, seq_errors


# ---------------------------------------------------------------------------
# Main reproduction sequence
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    args = parser.parse_args()

    log(f"repro_pytest_ordering — port={args.port}")
    log("Reproducing: test_stream_start_packet_arrives → test_stream_marker_seq_transparent")
    log("          → test_rle_compressed_payload_is_even_length → test_stream_1mhz_4ch_10s")

    # Find RP2040 vendor bulk device
    log("\nFinding RP2040 vendor bulk device...")
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        log(f"ERROR: device {VID:#06x}:{PID:#06x} not found")
        sys.exit(1)
    log(f"  Found: {dev.manufacturer} / {dev.product}")
    try:
        usb.util.claim_interface(dev, LA_INTERFACE)
    except Exception:
        usb.util.release_interface(dev, LA_INTERFACE)
        time.sleep(0.05)
        usb.util.claim_interface(dev, LA_INTERFACE)
    log(f"  Interface {LA_INTERFACE} claimed")

    # Open BBP once and keep it open for log relay throughout
    log("\nOpening BBP (stays open for entire run)...")
    import bugbuster as bb
    bbp = bb.connect_usb(args.port)
    log("  BBP connected")
    bbp.on_la_log(lambda msg: log(f"  [RP2040] {msg.rstrip()}"))
    try:
        bbp.hat_la_log_enable(True)
        log("  LA log relay enabled")
    except Exception as e:
        log(f"  LA log relay FAILED (non-fatal): {e}")

    results = {}

    try:
        # =====================================================================
        # PHASE A: test_stream_start_packet_arrives
        # Uses EP_OUT for START, BBP for STOP (via _bbp_port set in fixture)
        # =====================================================================
        log("\n" + "="*70)
        log("PHASE A: test_stream_start_packet_arrives")
        log("="*70)

        ok = bbp_preflight(bbp, dev, label="A/preflight")
        if not ok:
            log("  PHASE A: preflight failed — aborting")
            results["A"] = "preflight_failed"
        else:
            got_start = wait_for_pkt_start(dev, bbp, timeout_ms=2000, label="A/wait") if ep_out_start(dev, bbp, label="A") else False

            if got_start:
                # Read 10 packets (mirrors the test body)
                n, nb, se = collect_data(dev, duration_s=0.3, label="A/data")
                log(f"  PHASE A: collected {n} pkts / {nb}B / {se} seq errors")

            # Always stop (mirrors the finally block)
            bbp_stop(bbp, label="A/stop")
            results["A"] = "pass" if got_start else "start_timeout"
            log(f"\nPHASE A: {'PASSED ✓' if got_start else 'FAILED ✗ (PKT_START timeout)'}")

        # =====================================================================
        # PHASE B: test_stream_marker_seq_transparent
        # =====================================================================
        log("\n" + "="*70)
        log("PHASE B: test_stream_marker_seq_transparent")
        log("="*70)

        ok = bbp_preflight(bbp, dev, label="B/preflight")
        if not ok:
            log("  PHASE B: preflight failed — aborting")
            results["B"] = "preflight_failed"
        else:
            got_start = wait_for_pkt_start(dev, bbp, timeout_ms=2000, label="B/wait") if ep_out_start(dev, bbp, label="B") else False

            if got_start:
                n, nb, se = collect_data(dev, duration_s=0.3, label="B/data")
                log(f"  PHASE B: collected {n} pkts / {nb}B / {se} seq errors")

            bbp_stop(bbp, label="B/stop")
            results["B"] = "pass" if got_start else "start_timeout"
            log(f"\nPHASE B: {'PASSED ✓' if got_start else 'FAILED ✗ (PKT_START timeout)'}")

        # =====================================================================
        # PHASE C: test_rle_compressed_payload_is_even_length
        # (_collect_data_packets: EP_OUT START, 0.5s data, BBP STOP)
        # =====================================================================
        log("\n" + "="*70)
        log("PHASE C: test_rle_compressed_payload_is_even_length (_collect_data_packets)")
        log("="*70)

        ok = bbp_preflight(bbp, dev, label="C/preflight")
        if not ok:
            log("  PHASE C: preflight failed — aborting")
            results["C"] = "preflight_failed"
        else:
            got_start = wait_for_pkt_start(dev, bbp, timeout_ms=2000, label="C/wait") if ep_out_start(dev, bbp, label="C") else False

            if got_start:
                n, nb, se = collect_data(dev, duration_s=0.5, label="C/data")
                log(f"  PHASE C: collected {n} pkts / {nb}B / {se} seq errors")

            bbp_stop(bbp, label="C/stop")
            results["C"] = "pass" if got_start else "start_timeout"
            log(f"\nPHASE C: {'PASSED ✓' if got_start else 'FAILED ✗ (PKT_START timeout)'}")

        # =====================================================================
        # PHASE D: test_stream_1mhz_4ch_10s
        # BBP usb_reset + configure, then EP_OUT START (no bbp_port on la_host)
        # =====================================================================
        log("\n" + "="*70)
        log("PHASE D: test_stream_1mhz_4ch_10s")
        log("="*70)

        log("\n  [D] Phase 1: configure via BBP (mirrors test exactly)...")
        t = time.perf_counter()
        try:
            bbp.hat_la_usb_reset()
            log(f"  [D] hat_la_usb_reset() OK  rtt={1000*(time.perf_counter()-t):.1f}ms")
        except Exception as e:
            log(f"  [D] hat_la_usb_reset() FAILED: {e}  rtt={1000*(time.perf_counter()-t):.1f}ms")
            results["D"] = "usb_reset_failed"
        else:
            time.sleep(0.1)

            # Extra log-drain pause to see any late log frames
            time.sleep(0.05)
            log("  [D] (50ms log-drain pause after usb_reset)")

            t = time.perf_counter()
            try:
                bbp.hat_la_configure(channels=4, rate_hz=1_000_000, depth=11_000_000)
                log(f"  [D] hat_la_configure() OK  rtt={1000*(time.perf_counter()-t):.1f}ms")
            except Exception as e:
                log(f"  [D] hat_la_configure() FAILED: {e}  rtt={1000*(time.perf_counter()-t):.1f}ms")
                results["D"] = "configure_failed"
            else:
                try:
                    st = bbp.hat_la_get_status()
                    log(f"  [D] status after configure: state={st.get('state_name')} "
                        f"rearm_req={st.get('usb_rearm_request_count')} "
                        f"rearm_cmp={st.get('usb_rearm_complete_count')}")
                except Exception as e:
                    log(f"  [D] hat_la_get_status() failed: {e}")

                # test_stream_1mhz_4ch_10s does NOT disconnect BBP before START —
                # it disconnects the BBP dev object, but here we keep it for logging.
                # We DO replicate: vendor bulk stays claimed (no release/re-claim).
                stale = drain_ep_in(dev, timeout_ms=100, label="D/stale")
                if stale == 0:
                    log("  [D] no stale EP_IN data")

                log("\n  [D] Phase 2: stream via EP_OUT START (no BBP port, mirrors test)...")
                got_start = wait_for_pkt_start(dev, bbp, timeout_ms=2000, label="D/wait") if ep_out_start(dev, bbp, label="D") else False

                if got_start:
                    log("  [D] streaming 3s (abbreviated from 10s for repro speed)...")
                    n, nb, se = collect_data(dev, duration_s=3.0, label="D/data")
                    elapsed = 3.0
                    log(f"  [D] collected {n} pkts / {nb}B / {nb/elapsed/1024:.1f}KB/s / {se} seq errors")
                    # Stop via EP_OUT (mirrors the test which has no bbp_port on la_host)
                    log("  [D] sending STREAM_CMD_STOP via EP_OUT...")
                    try:
                        dev.write(EP_OUT, bytes([STREAM_CMD_STOP]), timeout=2000)
                        log("  [D] EP_OUT STOP sent")
                    except Exception as e:
                        log(f"  [D] EP_OUT STOP failed: {e}")
                    # Drain until PKT_STOP or timeout
                    t0 = time.monotonic()
                    got_stop = False
                    while time.monotonic() - t0 < 2.0:
                        try:
                            raw = bytes(dev.read(EP_IN, 16384, timeout=500))
                            for p in decode_packets(raw):
                                log(f"  [D]   {pkt_name(p['type'])} seq={p['seq']}")
                                if p["type"] == PKT_STOP:
                                    got_stop = True
                            if got_stop:
                                break
                        except usb.core.USBTimeoutError:
                            break
                    log(f"  [D] stop drain done, got_stop={got_stop}")

                results["D"] = "pass" if got_start else "start_timeout"
                log(f"\nPHASE D: {'PASSED ✓' if got_start else 'FAILED ✗ (PKT_START timeout after EP_OUT START)'}")

    finally:
        log("\nCleanup...")
        try:
            bbp.hat_la_log_enable(False)
        except Exception:
            pass
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

    log("\n" + "="*70)
    log("RESULTS SUMMARY")
    log("="*70)
    for phase, result in results.items():
        log(f"  Phase {phase}: {result}")
    log("="*70)


if __name__ == "__main__":
    main()
