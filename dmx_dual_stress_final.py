"""
DMX Dual-Port Stress Test - Final Version (with header)
COM7 -> port 0 (universe 1)
COM6 -> port 1 (universe 2)
6 fixtures, base_addr=1, spacing=3
Response: 8-byte header (P0_count, P1_count) + 1024 bytes DMX
"""
import socket
import serial
import time
import random
import sys

ESP32_IP = "192.168.1.222"
ESP32_PORT = 5555
DMX_CHANNELS = 512
HEADER_SIZE = 24
RESP_SIZE = HEADER_SIZE + DMX_CHANNELS * 2
NUM_FIXTURES = 6
CH_PER_FIXTURE = 3
BASE_ADDR = 1
NUM_FRAMES = 500


def recv_all():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect((ESP32_IP, ESP32_PORT))
    s.send(b"\x01")
    data = b""
    while len(data) < RESP_SIZE:
        chunk = s.recv(RESP_SIZE - len(data))
        if not chunk:
            break
        data += chunk
    s.close()
    if len(data) >= RESP_SIZE:
        c0 = int.from_bytes(data[0:4], 'little')
        c1 = int.from_bytes(data[4:8], 'little')
        isr0 = int.from_bytes(data[8:12], 'little')
        isr1 = int.from_bytes(data[12:16], 'little')
        brk0 = int.from_bytes(data[16:20], 'little')
        brk1 = int.from_bytes(data[20:24], 'little')
        p0 = list(data[HEADER_SIZE:HEADER_SIZE + DMX_CHANNELS])
        p1 = list(data[HEADER_SIZE + DMX_CHANNELS:HEADER_SIZE + DMX_CHANNELS * 2])
        return c0, c1, p0, p1, isr0, isr1, brk0, brk1
    return None, None, None, None, 0, 0, 0, 0


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
    esp_ip = sys.argv[1] if len(sys.argv) > 1 else ESP32_IP
    num_frames = int(sys.argv[2]) if len(sys.argv) > 2 else NUM_FRAMES

    print(f"=== DMX Dual-Port Stress Test ===")
    print(f"Port 0: COM7 (universe 1)")
    print(f"Port 1: COM6 (universe 2)")
    print(f"Fixtures: {NUM_FIXTURES}, base_addr: {BASE_ADDR}")
    print(f"Frames: {num_frames}")
    print()

    recv_all()

    ser0 = serial.Serial("COM7", 250000, timeout=0.1)
    time.sleep(0.5)
    ser1 = serial.Serial("COM6", 250000, timeout=0.1)
    time.sleep(0.5)
    print("[OK] Both ports opened\n")

    prev_c0, prev_c1, _, _, _, _, _, _ = recv_all()

    p0_err = 0
    p1_err = 0
    no_data = 0
    details = []
    prev_ch0 = make_channels()
    prev_ch1 = make_channels()

    for frame in range(num_frames):
        ch0 = make_channels()
        ch1 = make_channels()

        mode = frame % 5
        if mode == 0:
            v = random.randint(0, 255)
            for f in range(NUM_FIXTURES):
                set_fixture(ch0, f, v, v, v)
                set_fixture(ch1, f, v, v, v)
            label = f"mono={v}"
        elif mode == 1:
            for f in range(NUM_FIXTURES):
                r, g, b = random.randint(0, 255), random.randint(0, 255), random.randint(0, 255)
                set_fixture(ch0, f, r, g, b)
                r2, g2, b2 = random.randint(0, 255), random.randint(0, 255), random.randint(0, 255)
                set_fixture(ch1, f, r2, g2, b2)
            label = "random RGB"
        elif mode == 2:
            for f in range(NUM_FIXTURES):
                v = random.choice([0, 255])
                set_fixture(ch0, f, v, 0, 0)
                set_fixture(ch1, f, 0, v, 0)
            label = "P0=R P1=G"
        elif mode == 3:
            for f in range(NUM_FIXTURES):
                v = random.choice([0, 128, 255])
                set_fixture(ch0, f, v, v, v)
                set_fixture(ch1, f, 255 - v, 255 - v, 255 - v)
            label = "inverted"
        else:
            for f in range(NUM_FIXTURES):
                set_fixture(ch0, f, 0, 0, 0)
                set_fixture(ch1, f, 255, 255, 255)
            label = "P0=off P1=white"

        send_dmx(ser0, ch0)
        time.sleep(0.005)
        send_dmx(ser1, ch1)
        time.sleep(0.02)

        c0, c1, recv0, recv1, isr0, isr1, brk0, brk1 = recv_all()
        if recv0 is None:
            no_data += 1
            prev_ch0 = ch0[:]
            prev_ch1 = ch1[:]
            continue

        errs0 = verify(prev_ch0, recv0)
        errs1 = verify(prev_ch1, recv1)
        p0_err += len(errs0)
        p1_err += len(errs1)

        if errs0 or errs1:
            if len(details) < 20:
                for f, rr, rg, rb, sr, sg, sb in errs0:
                    details.append(f"  F{frame} P0 F{f}: recv=({rr},{rg},{rb}) sent=({sr},{sg},{sb}) [{label}]")
                for f, rr, rg, rb, sr, sg, sb in errs1:
                    details.append(f"  F{frame} P1 F{f}: recv=({rr},{rg},{rb}) sent=({sr},{sg},{sb}) [{label}]")

        prev_ch0 = ch0[:]
        prev_ch1 = ch1[:]

    ser0.close()
    ser1.close()

    for d in details:
        print(d)

    print(f"\n{'=' * 50}")
    print(f"Frames: {num_frames}, no data: {no_data}")
    print(f"Frame counts: P0={c0} P1={c1}")
    print(f"ISR counts:   P0={isr0} P1={isr1}")
    print(f"Break counts: P0={brk0} P1={brk1}")
    print(f"Port 0: {p0_err} mismatches")
    print(f"Port 1: {p1_err} mismatches")
    if p0_err == 0 and p1_err == 0:
        print(f"ALL PASS")
    else:
        print(f"FAIL: {p0_err + p1_err} total mismatches")


if __name__ == "__main__":
    main()
