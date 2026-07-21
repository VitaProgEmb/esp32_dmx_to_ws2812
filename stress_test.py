#!/usr/bin/env python3
"""
15-minute stress test: send gradient on COM7, read from ESP32 every 5s, verify all 512 channels.
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
DURATION_SEC = 15 * 60  # 15 minutes
READ_INTERVAL = 5.0  # read every 5 seconds
FPS = 30


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
        dmx0 = list(data[64:576])
        dmx1 = list(data[576:1088])
        return dict(fc0=fc0, fc1=fc1, brk0=brk0, brk1=brk1,
                    err0=err0, err1=err1, dmx0=dmx0, dmx1=dmx1)
    except Exception as e:
        print(f"  TCP error: {e}")
        return None


def main():
    print(f"=== STRESS TEST: {DURATION_SEC//60} minutes, gradient 512ch, 30fps ===\n")

    print("Open COM7...")
    ser = serial.Serial(COM_PORT, BAUD, bytesize=8, stopbits=2, parity='N',
                        timeout=0, write_timeout=0)
    print("  OK\n")

    total_checks = 0
    total_errors = 0
    total_ch_errors = 0
    prev_fc0 = 0
    prev_fc1 = 0
    prev_err0 = 0
    prev_err1 = 0
    offset = 0
    last_sent_offset = -1  # track actual last frame sent
    frame_sent = 0
    t_start = time.time()

    print("Starting...\n")

    try:
        while True:
            elapsed = time.time() - t_start
            if elapsed >= DURATION_SEC:
                break

            remaining = DURATION_SEC - elapsed
            send_dmx(ser, make_gradient(offset))
            last_sent_offset = offset
            frame_sent += 1
            offset = (offset + 1) & 0xFF

            if frame_sent % 30 == 0:
                time.sleep(1.0 / 30.0)

            # Periodic read + verify
            if frame_sent % (int(READ_INTERVAL * FPS)) == 0:
                time.sleep(0.2)
                r = query_debug()
                if r is None:
                    print(f"  [{elapsed:6.0f}s] NO RESPONSE")
                    continue

                total_checks += 1
                fc_delta0 = r['fc0'] - prev_fc0
                fc_delta1 = r['fc1'] - prev_fc1
                err_delta0 = r['err0'] - prev_err0
                err_delta1 = r['err1'] - prev_err1
                prev_fc0 = r['fc0']
                prev_fc1 = r['fc1']
                prev_err0 = r['err0']
                prev_err1 = r['err1']

                # ESP32 BREAK handler saves the PREVIOUS frame (BREAK = start of next).
                # When we stop sending during sleep, ESP32 has last_sent in rx_active
                # but only rx_done is visible (frame before that). So ESP32 shows last_sent - 1.
                check_offset = (last_sent_offset - 1) & 0xFF

                # Verify all 512 channels on both ports
                ch_errors_p0 = 0
                ch_errors_p1 = 0
                for i in range(DMX_CHANNELS):
                    expected = (i + check_offset) & 0xFF
                    if r['dmx0'][i] != expected:
                        ch_errors_p0 += 1
                    if r['dmx1'][i] != expected:
                        ch_errors_p1 += 1

                total_ch_errors += ch_errors_p0 + ch_errors_p1
                has_err = ch_errors_p0 > 0 or ch_errors_p1 > 0 or err_delta0 > 0 or err_delta1 > 0

                status = "FAIL" if has_err else " OK "
                mins = int(elapsed) // 60
                secs = int(elapsed) % 60
                print(f"  [{mins:2d}:{secs:02d}] {status} FC0={r['fc0']:4d}(+{fc_delta0:3d}) "
                      f"FC1={r['fc1']:4d}(+{fc_delta1:3d}) "
                      f"ERR0={r['err0']}(+{err_delta0}) ERR1={r['err1']}(+{err_delta1}) "
                      f"CH_ERR P0={ch_errors_p0} P1={ch_errors_p1}")

                if has_err:
                    total_errors += 1
                    # Show first few mismatches on each port
                    for port_idx, (dmx, ch_err) in enumerate([(r['dmx0'], ch_errors_p0), (r['dmx1'], ch_errors_p1)]):
                        if ch_err > 0:
                            shown = 0
                            for i in range(DMX_CHANNELS):
                                expected = (i + check_offset) & 0xFF
                                if dmx[i] != expected and shown < 3:
                                    print(f"        P{port_idx} ch{i+1}: exp=0x{expected:02X} got=0x{dmx[i]:02X}")
                                    shown += 1

    except KeyboardInterrupt:
        print("\n  Interrupted!")

    ser.close()
    t_total = time.time() - t_start

    print(f"\n{'='*60}")
    print(f"STRESS TEST RESULTS ({t_total:.0f}s = {t_total/60:.1f}min)")
    print(f"{'='*60}")
    print(f"  Total checks:     {total_checks}")
    print(f"  Failed checks:    {total_errors}")
    print(f"  Total ch errors:  {total_ch_errors}")
    print(f"  Frames sent:      {frame_sent}")
    print(f"  Duration:         {t_total:.1f}s")
    if total_errors == 0:
        print(f"\n  >>> ALL CHECKS PASSED! <<<")
    else:
        print(f"\n  >>> {total_errors} CHECKS FAILED <<<")
    print()


if __name__ == "__main__":
    main()
