
import time
import sys
import logging
import bugbuster as bb

logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(message)s')
logger = logging.getLogger(__name__)

def stress_test_hat_status(port, iterations=100, interval=0.1):
    logger.info(f"Connecting to {port}...")
    try:
        dev = bb.connect_usb(port)
    except Exception as e:
        logger.error(f"Connection failed: {e}")
        return

    logger.info(f"Connected. Starting stress test: {iterations} iterations, {interval}s interval.")
    
    success = 0
    failures = 0
    
    for i in range(iterations):
        try:
            status = dev.hat_la_get_status()
            success += 1
            if i % 10 == 0:
                logger.info(f"Iteration {i}: state={status.get('state_name')}, captured={status.get('samples_captured')}")
        except Exception as e:
            failures += 1
            logger.error(f"Iteration {i} FAILED: {e}")
        
        time.sleep(interval)
    
    logger.info(f"Test complete. Success: {success}, Failures: {failures}")
    dev.disconnect()

if __name__ == "__main__":
    port = "/dev/cu.usbmodem1234561"
    if len(sys.argv) > 1:
        port = sys.argv[1]
    stress_test_hat_status(port)
