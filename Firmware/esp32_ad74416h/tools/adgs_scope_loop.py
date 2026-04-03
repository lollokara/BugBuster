#!/usr/bin/env python3
import argparse
import sys
import time

import serial


def read_response(ser, timeout_s):
    deadline = time.monotonic() + timeout_s
    chunks = []
    while time.monotonic() < deadline:
        data = ser.read(4096)
        if data:
            chunks.append(data)
            deadline = time.monotonic() + 0.05
        else:
            time.sleep(0.01)
    return b"".join(chunks).decode(errors="replace")


def send_command(ser, command, timeout_s):
    ser.write((command + "\n").encode("ascii"))
    ser.flush()
    return read_response(ser, timeout_s)


def main():
    parser = argparse.ArgumentParser(
        description="Continuously trigger ADGS mux writes for oscilloscope probing."
    )
    parser.add_argument("--port", default="/dev/cu.usbmodem1234561")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--period", type=float, default=1.0,
                        help="Delay between write attempts in seconds.")
    parser.add_argument("--dev", type=int, default=0)
    parser.add_argument("--switch", type=int, default=1)
    parser.add_argument("--state", type=int, default=1, choices=(0, 1))
    parser.add_argument("--no-reset", action="store_true",
                        help="Do not send muxreset before each write.")
    args = parser.parse_args()

    mux_cmd = f"mux {args.dev} {args.switch} {args.state}"

    try:
        with serial.Serial(args.port, args.baud, timeout=0.05) as ser:
            time.sleep(0.3)
            ser.reset_input_buffer()
            print(f"Looping '{mux_cmd}' on {args.port} every {args.period:.3f}s")
            if not args.no_reset:
                print("Sending 'muxreset' before each write attempt")
            print("Press Ctrl-C to stop\n")

            seq = 0
            while True:
                seq += 1
                print(f"=== attempt {seq} ===")
                if not args.no_reset:
                    sys.stdout.write(send_command(ser, "muxreset", 0.8))
                sys.stdout.write(send_command(ser, mux_cmd, 0.8))
                sys.stdout.flush()
                time.sleep(args.period)
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
