#!/usr/bin/env python3
"""
DMX512 transmitter via COM7 (FT2232).
Sends DMX frames with BREAK + MAB + 512 channels.
"""
import serial
import struct
import time
import sys

PORT = "COM7"
BAUD = 250000
BREAK_US = 120
MAB_US = 12


def send_dmx_frame(ser, channels):
    """Send one DMX512 frame: BREAK + StartCode + 512 channels."""
    ser.sendBreak(BREAK_US / 1000000.0)
    time.sleep(MAB_US / 1000000.0)

    frame = bytearray(513)
    frame[0] = 0x00  # DMX start code
    for i in range(min(len(channels), 512)):
        frame[i + 1] = channels[i] & 0xFF
    ser.write(frame)
    ser.flush()


def main():
    ch_start = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    ch_count = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    ch_val = int(sys.argv[3]) if len(sys.argv) > 3 else 255
    fps = float(sys.argv[4]) if len(sys.argv) > 4 else 20.0

    print(f"COM7 DMX TX: ch[{ch_start}..{ch_start+ch_count-1}]={ch_val}, {fps:.0f} fps")

    ser = serial.Serial(
        PORT, BAUD,
        bytesize=serial.EIGHTBITS,
        stopbits=serial.STOPBITS_TWO,
        parity=serial.PARITY_NONE,
        timeout=0,
        write_timeout=0,
    )

    channels = [0] * 512
    for i in range(ch_count):
        idx = ch_start - 1 + i
        if 0 <= idx < 512:
            channels[idx] = ch_val

    interval = 1.0 / fps
    count = 0
    try:
        while True:
            t0 = time.time()
            send_dmx_frame(ser, channels)
            count += 1
            elapsed = time.time() - t0
            if count % 100 == 0:
                print(f"  Sent {count} frames, last={elapsed*1000:.1f}ms")
            sleep_time = interval - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)
    except KeyboardInterrupt:
        print(f"\nStopped. {count} frames sent.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
