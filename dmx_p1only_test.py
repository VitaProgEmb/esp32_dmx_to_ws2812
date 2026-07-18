"""
DMX P1-only test with P0 port completely closed
Tests if UART0 noise/ISR interferes with UART1
"""
import socket
import serial
import time
import random

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
    num_frames = 300
    print(f"=== P1 Only Test (P0 port CLOSED) ===")
    print(f"Frames: {num_frames}")
    print()

    prev_c0, prev_c1, _, _ = recv_all()
    print(f"[BOOT] P0_count={prev_c0} P1_count={prev_c1}")

    ser1 = serial.Serial("COM6", 250000, timeout=0.1)
    time.sleep(0.3)
    print("[OK] P1 opened (COM6), P0 NOT opened\n")

    p1_err = 0
    details = []
    c0, c1 = prev_c0, prev_c1

    for frame in range(num_frames):
        ch1 = [0] * DMX_CHANNELS
        r_val = random.randint(0, 255)
        g_val = random.randint(0, 255)
        b_val = random.randint(0, 255)
        for f in range(NUM_FIXTURES):
            set_fixture(ch1, f, r_val, g_val, b_val)

        send_dmx(ser1, ch1)
        time.sleep(0.04)

        c0_new, c1_new, recv0, recv1 = recv_all()
        if recv1 is None:
            continue

        errs1 = verify(ch1, recv1)
        p1_err += len(errs1)

        if errs1 and len(details) < 20:
            for f, rr, rg, rb, sr, sg, sb in errs1:
                details.append(f"  F{frame} P1 F{f}: recv=({rr},{rg},{rb}) sent=({sr},{sg},{sb})")

        delta_p0 = c0_new - c0 if c0_new >= c0 else 0
        delta_p1 = c1_new - c1 if c1_new >= c1 else 0
        c0, c1 = c0_new, c1_new

        if frame % 25 == 0:
            print(f"  frame {frame}: P1_err={p1_err} P0_delta={delta_p0} P1_delta={delta_p1}")

    ser1.close()

    for d in details:
        print(d)

    print(f"\n{'=' * 50}")
    print(f"Frames: {num_frames}")
    print(f"P0 received: {c0 - prev_c0}")
    print(f"P1 received: {c1 - prev_c1}")
    print(f"Port 1: {p1_err} mismatches")
    if p1_err == 0:
        print("ALL PASS")
    else:
        print(f"FAIL: {p1_err} mismatches")


if __name__ == "__main__":
    main()
