import sys, time
sys.path.insert(0, "python")
import bugbuster as bb

port = "/dev/cu.usbmodem1234561"
print(f"Connecting to {port}...")
client = bb.connect_usb(port)

print("Configuring LA (4ch, 1MHz, RLE=True)")
client.hat_la_configure(channels=4, rate_hz=1000000, depth=100000, rle_enabled=True)

print("Starting USB stream via newly added API hat_la_stream_usb_cycle...")
t0 = time.time()
data = client.hat_la_stream_usb_cycle(duration_s=0.5)
t1 = time.time()

print(f"Stream complete in {t1-t0:.2f}s")
if data and len(data) >= 4:
    samples = len(data[0])
    print(f"SUCCESS! Decoded {samples} samples per channel.")
    
    # Just show a few samples to verify logic
    print("CH0 (first 10):", data[0][:10])
else:
    print("FAILED! No data returned.")

