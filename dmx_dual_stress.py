"""
DMX Dual-Port Stress Test
COM7 → port 0 (universe 1), COM6 → port 1 (universe 2)
6 fixtures, base_addr=1, spacing=3
Отправляет на оба порта одновременно и проверяет через debug server.
"""

import socket
import serial
import time
import sys
import random

ESP32_IP = "192.168.1.222"
ESP32_PORT = 5555
DMX_CHANNELS = 512
NUM_FIXTURES = 6
CH_PER_FIXTURE = 3
BASE_ADDR = 1
PORT0_SER = "COM7"
PORT1_SER = "COM6"
NUM_FRAMES = 200


def recv_all():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5.0)
        s.connect((ESP32_IP, ESP32_PORT))
        s.send(b"\x01")
        data = b""
        while len(data) < 1024:
            chunk = s.recv(1024 - len(data))
            if not chunk:
                break
            data += chunk
        try:
            s.close()
        except Exception:
            pass
        if len(data) >= 1024:
            return list(data[0:512]), list(data[512:1024])
    except Exception as e:
        if "10054" not in str(e):
            print(f"  [ERROR] recv: {e}")
    return None, None


def send_dmx(ser, channels):
    try:
        ser.break_condition = True
        time.sleep(0.0001)
        ser.break_condition = False
        time.sleep(0.000012)
        data = bytes([0x00]) + bytes(channels[:DMX_CHANNELS])
        ser.write(data)
        return True
    except Exception:
        return False


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
            errors.append((f, a, rr, rg, rb, sr, sg, sb))
    return errors


def main():
    esp_ip = sys.argv[1] if len(sys.argv) > 1 else ESP32_IP
    num_frames = int(sys.argv[2]) if len(sys.argv) > 2 else NUM_FRAMES

    print(f"=== DMX Dual-Port Stress Test ===")
    print(f"Port 0: {PORT0_SER} (universe 1)")
    print(f"Port 1: {PORT1_SER} (universe 2)")
    print(f"Fixtures: {NUM_FIXTURES}, base_addr: {BASE_ADDR}")
    print(f"Frames: {num_frames}")
    print()

    recv_all()
    ser0 = serial.Serial(PORT0_SER, 250000, timeout=0.1)
    ser1 = serial.Serial(PORT1_SER, 250000, timeout=0.1)
    print("[OK] Both ports opened\n")

    p0_err = 0
    p1_err = 0
    p0_mono_err = 0
    p1_mono_err = 0
    no_data = 0
    details = []

    for frame in range(num_frames):
        ch0 = make_channels()
        ch1 = make_channels()

        if frame % 4 == 0:
            b0 = random.randint(0, 255)
            b1 = random.randint(0, 255)
            for f in range(NUM_FIXTURES):
                set_fixture(ch0, f, b0, b0, b0)
                set_fixture(ch1, f, b1, b1, b1)
            label = f"P0={b0} P1={b1}"
        elif frame % 4 == 1:
            for f in range(NUM_FIXTURES):
                v = random.randint(0, 255)
                set_fixture(ch0, f, v, v, v)
                v2 = random.randint(0, 255)
                set_fixture(ch1, f, v2, v2, v2)
            label = "per-fixture mono"
        elif frame % 4 == 2:
            for f in range(NUM_FIXTURES):
                set_fixture(ch0, f, random.randint(0, 255), random.randint(0, 255), random.randint(0, 255))
                set_fixture(ch1, f, random.randint(0, 255), random.randint(0, 255), random.randint(0, 255))
            label = "random RGB"
        else:
            for f in range(NUM_FIXTURES):
                v = random.choice([0, 128, 255])
                set_fixture(ch0, f, v, v, v)
                v2 = random.choice([0, 128, 255])
                set_fixture(ch1, f, v2, v2, v2)
            label = "step"

        send_dmx(ser0, ch0)
        time.sleep(0.005)
        send_dmx(ser1, ch1)
        time.sleep(0.02)

        recv0, recv1 = recv_all()
        if recv0 is None:
            no_data += 1
            continue

        errs0 = verify(ch0, recv0)
        errs1 = verify(ch1, recv1)
        p0_err += len(errs0)
        p1_err += len(errs1)

        if errs0:
            for f, a, rr, rg, rb, sr, sg, sb in errs0:
                if rr != rg or rg != rb:
                    p0_mono_err += 1
                details.append(f"  F{frame} P0 F{f}: recv=({rr},{rg},{rb}) sent=({sr},{sg},{sb}) [{label}]")

        if errs1:
            for f, a, rr, rg, rb, sr, sg, sb in errs1:
                if rr != rg or rg != rb:
                    p1_mono_err += 1
                details.append(f"  F{frame} P1 F{f}: recv=({rr},{rg},{rb}) sent=({sr},{sg},{sb}) [{label}]")

    ser0.close()
    ser1.close()

    max_print = 30
    for d in details[:max_print]:
        print(d)
    if len(details) > max_print:
        print(f"  ... and {len(details) - max_print} more")

    print(f"\n{'=' * 50}")
    print(f"Frames: {num_frames}, no data: {no_data}")
    print(f"Port 0: {p0_err} mismatches, {p0_mono_err} mono violations")
    print(f"Port 1: {p1_err} mismatches, {p1_mono_err} mono violations")
    if p0_err == 0 and p1_err == 0:
        print(f"ALL PASS")
    else:
        total = p0_err + p1_err
        print(f"FAIL: {total} total mismatches")


if __name__ == "__main__":
    main()
