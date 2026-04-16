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
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--no-log-relay", action="store_true")
    args = parser.parse_args()

    log(f"repro_0x11_debug — port={args.port} iterations={args.iterations} log_relay={not args.no_log_relay}")

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
    log(f"\nOpening BBP (stays open for entire run)...")
    import bugbuster as bb
    bbp = bb.connect_usb(args.port)
    log("  BBP connected")
    if not args.no_log_relay:
        bbp.on_la_log(lambda msg: log(f"  [RP2040] {msg.rstrip()}"))
        try:
            bbp.hat_la_log_enable(True)
            log("  LA log relay enabled")
        except Exception as e:
            log(f"  LA log relay FAILED (non-fatal): {e}")
    else:
        log("  LA log relay disabled by user")

    for it in range(args.iterations):
        log("\n" + "="*70)
        log(f"ITERATION {it+1}/{args.iterations}")
        log("="*70)

        for cycle in range(5):
            label = f"IT{it+1}/C{cycle+1}"
            log(f"\n  --- Cycle {cycle+1}/5 ---")
            
            ok = bbp_preflight(bbp, dev, label=f"{label}/preflight")
            if not ok:
                log(f"  {label}: preflight failed — ABORTING")
                sys.exit(1)
            
            got_start = wait_for_pkt_start(dev, bbp, timeout_ms=2000, label=f"{label}/wait") if ep_out_start(dev, bbp, label=label) else False
            
            if got_start:
                n, nb, se = collect_data(dev, duration_s=0.5, label=f"{label}/data")
                log(f"  {label}: collected {n} pkts / {nb}B / {se} seq errors")
            else:
                log(f"  {label}: START TIMEOUT")
                sys.exit(1)

            bbp_stop(bbp, label=f"{label}/stop")

    log("\nFinished all iterations successfully!")

if __name__ == "__main__":
    main()
