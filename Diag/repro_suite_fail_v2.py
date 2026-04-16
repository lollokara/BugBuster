#!/usr/bin/env python3
import sys
import time
import argparse
import usb.core
import usb.util
import logging

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
    LA_INTERFACE,
)

# Setup logging to see what bugbuster client is doing
logging.basicConfig(level=logging.DEBUG, format='%(asctime)s [%(levelname)s] %(message)s')
logger = logging.getLogger("repro")

def repro(port, cycles=5):
    logger.info(f"Starting reproduction on {port} for {cycles} cycles...")
    
    # Stay connected to BBP to avoid re-open overhead and potential issues
    logger.info(f"[BBP] Connecting to {port}...")
    dev = bb.connect_usb(port)
    
    try:
        for i in range(cycles):
            logger.info(f"\n{'='*20} Cycle {i+1} {'='*20}")
            
            # Phase 1: BBP Prep
            logger.info("[BBP] hat_la_usb_reset()...")
            dev.hat_la_usb_reset()
            time.sleep(0.1)
            
            logger.info("[BBP] hat_la_configure()...")
            dev.hat_la_configure(channels=4, rate_hz=1_000_000, depth=100_000)
            
            status = dev.hat_la_get_status()
            logger.info(f"[BBP] Status: {status}")

            # Phase 2: USB Start
            host = LaUsbHost()
            try:
                logger.info("[USB] Connecting...")
                host.connect()
                
                logger.info("[USB] Waiting 200ms before START...")
                time.sleep(0.2)

                logger.info("[USB] Sending START...")
                host.send_command(STREAM_CMD_START)
                
                logger.info("[USB] Waiting for START pkt...")
                start_pkt = host.wait_for_start(timeout_ms=2000)
                logger.info(f"[USB] Got START: seq={start_pkt.seq}")
                
                logger.info("[USB] Reading some DATA...")
                for _ in range(5):
                    pkt = host.read_packet(timeout_ms=500)
                    if pkt.pkt_type != PKT_DATA:
                        logger.warning(f"[USB] Unexpected pkt type {pkt.pkt_type}")
                
                # Phase 3: STOP
                # On macOS we MUST release before calling BBP if they were on same device,
                # but user says they are different. However, hat_la_stop() triggers 
                # bb_la_usb_soft_reset() which affects the Bulk interface.
                
                logger.info("[STOP] hat_la_stop() via BBP...")
                dev.hat_la_stop()
                logger.info("[STOP] hat_la_stop() OK")
                
                logger.info("[STOP] Waiting 0.5s for rearm...")
                time.sleep(0.5)
                
                # Try to read PKT_STOP from USB before closing
                try:
                    logger.info("[USB] Draining for PKT_STOP...")
                    # We need to use read_packet since interface is still claimed
                    pkt = host.read_packet(timeout_ms=200)
                    if pkt.pkt_type == PKT_STOP:
                        logger.info(f"[USB] Got PKT_STOP: info={pkt.info:#02x}")
                    else:
                        logger.info(f"[USB] Got other pkt: type={pkt.pkt_type}")
                except Exception as e:
                    logger.info(f"[USB] No PKT_STOP received (expected if rearm cleared FIFO): {e}")
                
            except Exception as e:
                logger.error(f"[USB] FAILED: {e}")
                raise
            finally:
                host.close()
                logger.info("[USB] Closed.")

        logger.info("\nSUCCESS: All cycles completed.")
        
    except Exception as e:
        logger.error(f"REPRO FAILED: {e}")
        # Check status one last time if possible
        try:
            logger.info("[BBP] Final health check...")
            status = dev.hat_la_get_status()
            logger.info(f"[BBP] Final Status: {status}")
        except Exception as e2:
            logger.error(f"[BBP] Final health check FAILED: {e2}")
    finally:
        dev.disconnect()
        logger.info("[BBP] Disconnected.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--cycles", type=int, default=5)
    args = parser.parse_args()
    repro(args.port, args.cycles)
