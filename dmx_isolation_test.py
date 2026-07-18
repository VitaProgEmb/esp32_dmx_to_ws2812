"""
DMX Isolation Test - Tests with P0 port closed to eliminate FT2232 interference
Also tests with both ports open but only P1 sending
"""
import socket
import serial
import time
import random
import sys

ESP32_IP = "192.168.1.222"
ESP32_PORT = 5555
DMX_CHANNELS = 512
NUM_FIXTURES = 6
CH_PER_FIXTURE = 3
BASE_ADDR = 1


def recv_all():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect((ESP32_IP, ESP32_PORT))
    s.send(b"\x01")
    data = b""
    while len(data) < 1032:
        chunk = s.recv(1032 - len(data))
        if not chunk:
            break
        data += chunk
    s.close()
    if len(data) >= 1032:
        c0 = int.from_bytes(data[0:4], 'little')
        c1 = int.from_bytes(data[4:8], 'little')
        p0 = list(data[8:520])
        p1 = list(data[520:1032])
        return c0, c1, p0, p1
    return None, None, None, None


def send_dmx(ser, channels):
    ser.break_condition = True
    time.sleep(0.0001)
    ser.break_condition = False
    time.sleep(0.000012)
    ser.write(bytes([0x00]) + bytes(channels[:DMX_CHANNELS]))


def make_channels():
    return [0] * DMX_CHANNELS


def set_fixture(ch, f, r, g, b):
    a = (BASE_ADDR - 1) + f * CH_PER_FIXTURE
    if a + 2 < DMX_CHANNELS:
        ch[a] = r
        ch[a + 1] = g
        ch[a + 2] = b


def verify(sent, received):
    errors = []
    for f in range(NUM_FIXTURES):
        a = (BASE_ADDR - 1) + f * CH_PER_FIXTURE
        if a + 2 >= len(sent) or a + 2 >= len(received):
            break
        sr, sg, sb = sent[a], sent[a + 1], sent[a + 2]
        rr, rg, rb = received[a], received[a + 1], received[a + 2]
        if rr != sr or rg != sg or rb != sb:
            errors.append((f, rr, rg, rb, sr, sg, sb))
    return errors


def main():
    num_frames = int(sys.argv[1]) if len(sys.argv) > 1 else 200

    print(f"=== DMX Isolation Test ===")
    print(f"Frames: {num_frames}")
    print()

    recv_all()

    prev_c0, prev_c1, _, _ = recv_all()
    print(f"[BOOT] P0_count={prev_c0} P1_count={prev_c1}")

    ser0 = serial.Serial("COM7", 250000, timeout=0.1)
    time.sleep(0.3)
    ser1 = serial.Serial("COM6", 250000, timeout=0.1)
    time.sleep(0.3)
    print("[OK] Both ports opened\n")

    p1_err = 0
    no_data = 0
    details = []
    c0, c1 = prev_c0, prev_c1

    for frame in range(num_frames):
        ch1 = make_channels()
        r_val = random.randint(0, 255)
        g_val = random.randint(0, 255)
        b_val = random.randint(0, 255)
        for f in range(NUM_FIXTURES):
            set_fixture(ch1, f, r_val, g_val, b_val)

        send_dmx(ser0, make_channels())
        time.sleep(0.005)
        send_dmx(ser1, ch1)
        time.sleep(0.02)

        c0_new, c1_new, recv0, recv1 = recv_all()
        if recv1 is None:
            no_data += 1
            continue

        errs1 = verify(ch1, recv1)
        p1_err += len(errs1)

        if errs1:
            if len(details) < 30:
                for f, rr, rg, rb, sr, sg, sb in errs1:
                    details.append(f"  F{frame} P1 F{f}: recv=({rr},{rg},{rb}) sent=({sr},{sg},{sb})")

        delta_p0 = c0_new - c0 if c0_new >= c0 else 0
        delta_p1 = c1_new - c1 if c1_new >= c1 else 0
        c0, c1 = c0_new, c1_new

        if frame % 20 == 0 and frame > 0:
            print(f"  ... frame {frame}/{num_frames} P1_err={p1_err} P0_delta={delta_p0} P1_delta={delta_p1}")

    ser0.close()
    ser1.close()

    for d in details:
        print(d)

    print(f"\n{'=' * 50}")
    print(f"Frames: {num_frames}, no data: {no_data}")
    print(f"Final counts: P0={c0} P1={c1}")
    print(f"P0 received since test start: {c0 - prev_c0}")
    print(f"P1 received since test start: {c1 - prev_c1}")
    print(f"Port 1: {p1_err} mismatches")
    if p1_err == 0:
        print(f"ALL PASS")
    else:
        print(f"FAIL: {p1_err} mismatches")


if __name__ == "__main__":
    main()
