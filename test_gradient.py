#!/usr/bin/env python3
"""
Full gradient test: send gradient on COM7, read from ESP32, verify all 512 channels.
"""
import serial
import socket
import struct
import time
import sys

ESP32 = "192.168.1.100"
COM_PORT = "COM7"
BAUD = 250000
DEBUG_PORT = 5555
DMX_CHANNELS = 512


def make_gradient(offset):
    ch = bytearray(DMX_CHANNELS)
    for i in range(DMX_CHANNELS):
        ch[i] = (i + offset) & 0xFF
    return ch


def send_dmx(ser, channels):
    ser.sendBreak(120 / 1000000.0)
    time.sleep(12 / 1000000.0)
    frame = bytearray(513)
    frame[0] = 0x00
    frame[1:1 + len(channels)] = channels
    ser.write(frame)
    ser.flush()


def query_debug():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3.0)
        s.connect((ESP32, DEBUG_PORT))
        s.sendall(b"\x01")
        data = b""
        while len(data) < 1088:
            chunk = s.recv(1088 - len(data))
            if not chunk:
                break
            data += chunk
        s.close()
        if len(data) < 1088:
            return None
        fc0 = struct.unpack_from("<I", data, 0)[0]
        fc1 = struct.unpack_from("<I", data, 4)[0]
        brk0 = struct.unpack_from("<I", data, 16)[0]
        brk1 = struct.unpack_from("<I", data, 20)[0]
        err0 = struct.unpack_from("<I", data, 24)[0]
        err1 = struct.unpack_from("<I", data, 28)[0]
        brk_found = struct.unpack_from("<I", data, 56)[0]
        brk_idx = struct.unpack_from("<I", data, 60)[0]
        dmx0 = list(data[64:576])
        dmx1 = list(data[576:1088])
        return dict(fc0=fc0, fc1=fc1, brk0=brk0, brk1=brk1,
                    err0=err0, err1=err1,
                    brk_found=brk_found, brk_idx=brk_idx,
                    dmx0=dmx0, dmx1=dmx1)
    except Exception as e:
        print(f"  TCP error: {e}")
        return None


def main():
    print("=== FULL GRADIENT TEST: 512 channels, 0x00..0xFF ===\n")

    print("Step 1: Open COM7...")
    ser = serial.Serial(COM_PORT, BAUD, bytesize=8, stopbits=2, parity='N',
                        timeout=0, write_timeout=0)
    print(f"  COM7 opened")

    print("\nStep 2: Send gradient frames (30fps, 3 seconds)...")
    channels = make_gradient(0)
    sent = 0
    t_start = time.time()
    while time.time() - t_start < 3.0:
        send_dmx(ser, channels)
        sent += 1
        elapsed = time.time() - t_start
        if sent % 30 == 0:
            print(f"  Sent {sent} frames ({elapsed:.1f}s)")
        time.sleep(1.0 / 30.0 - (time.time() - t_start) % (1.0 / 30.0))
    ser.close()
    print(f"  Done: {sent} frames sent")

    print("\nStep 3: Read from ESP32...")
    time.sleep(0.5)
    r = query_debug()
    if r is None:
        print("  NO RESPONSE!")
        return

    print(f"  FC0={r['fc0']} FC1={r['fc1']} BRK0={r['brk0']} BRK1={r['brk1']} ERR0={r['err0']} ERR1={r['err1']}")
    print(f"  BREAK found: {r['brk_found']} at index {r['brk_idx']}")

    print("\nStep 4: Verify all 512 channels (Port 0)...")
    errors = []
    for i in range(DMX_CHANNELS):
        expected = (i + 0) & 0xFF
        actual = r['dmx0'][i]
        if actual != expected:
            errors.append((i + 1, expected, actual))

    if errors:
        print(f"  MISMATCHES: {len(errors)} / {DMX_CHANNELS}")
        print(f"  First 30 mismatches:")
        for ch, exp, act in errors[:30]:
            print(f"    ch{ch:3d}: expected=0x{exp:02X} got=0x{act:02X}")
        # Find first 0x00/0x01 issues
        zero_errs = [(c, e, a) for c, e, a in errors if e == 0x00]
        one_errs = [(c, e, a) for c, e, a in errors if e == 0x01]
        if zero_errs:
            print(f"  0x00 mismatches: {len(zero_errs)} — first at ch{zero_errs[0][0]} got=0x{zero_errs[0][2]:02X}")
        if one_errs:
            print(f"  0x01 mismatches: {len(one_errs)} — first at ch{one_errs[0][0]} got=0x{one_errs[0][2]:02X}")
    else:
        print(f"  ALL 512 CHANNELS CORRECT!")

    print(f"\n  Port0 first 32: ", end="")
    for i in range(32):
        print(f"{r['dmx0'][i]:02X}", end=" ")
    print()
    print(f"  Port0 ch128-135: ", end="")
    for i in range(127, 135):
        print(f"{r['dmx0'][i]:02X}", end=" ")
    print()
    print(f"  Port0 ch252-259: ", end="")
    for i in range(251, 259):
        print(f"{r['dmx0'][i]:02X}", end=" ")
    print()
    print(f"  Port0 ch508-512: ", end="")
    for i in range(507, 512):
        print(f"{r['dmx0'][i]:02X}", end=" ")
    print()

    nonzero = sum(1 for v in r['dmx0'] if v != 0)
    print(f"\n  Non-zero channels: P0={nonzero} P1={sum(1 for v in r['dmx1'] if v != 0)}")


if __name__ == "__main__":
    main()
