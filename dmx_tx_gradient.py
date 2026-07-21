#!/usr/bin/env python3
"""
DMX512 gradient transmitter — all 512 channels, 0x00..0xFF cycling.
Tests edge values 0x00, 0x01, 0x7F, 0x80, 0xFF.
"""
import serial
import time
import sys

PORT = "COM7"
BAUD = 250000
BREAK_US = 120
MAB_US = 12


def make_gradient(offset):
    """512-channel gradient: ch[i] = (i + offset) mod 256"""
    channels = bytearray(512)
    for i in range(512):
        channels[i] = (i + offset) & 0xFF
    return channels


def send_dmx_frame(ser, channels):
    """BREAK + StartCode + 512 channels."""
    ser.sendBreak(BREAK_US / 1000000.0)
    time.sleep(MAB_US / 1000000.0)
    frame = bytearray(513)
    frame[0] = 0x00
    frame[1:] = channels
    ser.write(frame)
    ser.flush()


def main():
    fps = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    mode = sys.argv[2] if len(sys.argv) > 2 else "gradient"

    print(f"DMX TX: mode={mode}, {fps:.0f} fps")
    print(f"  Testing: 0x00, 0x01, 0x7F, 0x80, 0xFF edge values")

    ser = serial.Serial(PORT, BAUD, bytesize=8, stopbits=2, parity='N', timeout=0)

    interval = 1.0 / fps
    count = 0
    offset = 0
    try:
        while True:
            t0 = time.time()

            if mode == "gradient":
                channels = make_gradient(offset)
            elif mode == "all00":
                channels = bytearray(512)
            elif mode == "allFF":
                channels = bytearray([0xFF] * 512)
            elif mode == "all01":
                channels = bytearray([0x01] * 512)
            elif mode == "alternating":
                channels = bytearray(512)
                for i in range(512):
                    channels[i] = 0x55 if (i % 2 == 0) else 0xAA
            elif mode == "edge":
                channels = bytearray(512)
                for i in range(512):
                    channels[i] = [0x00, 0x01, 0x7F, 0x80, 0xFF][i % 5]
            else:
                channels = make_gradient(0)

            send_dmx_frame(ser, channels)
            count += 1
            offset += 1

            if count % 100 == 0:
                elapsed = time.time() - t0
                print(f"  Sent {count} frames, interval={elapsed*1000:.1f}ms")

            sleep_time = interval - (time.time() - t0)
            if sleep_time > 0:
                time.sleep(sleep_time)

    except KeyboardInterrupt:
        print(f"\nStopped. {count} frames sent.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
