"""
DMX White Strobe Test
Отправляет рандомные белые (R=G=B=X) стробы на порт 1,
читает буфер ESP32 через TCP socket и проверяет на цветные артефакты.
"""

import socket
import serial
import time
import sys
import random

FTDI_PORT = "COM7"
ESP32_IP = "192.168.1.222"
ESP32_PORT = 5555
DMX_CHANNELS = 512
NUM_FIXTURES = 6
CH_PER_FIXTURE = 3
BASE_ADDR = 1
DMX_PORT = 1
PORT_OFFSET = DMX_PORT * DMX_CHANNELS


def send_dmx(channels):
    try:
        ser = serial.Serial(FTDI_PORT, 250000, timeout=0.1)
    except Exception as e:
        print(f"  [ERROR] {FTDI_PORT}: {e}")
        return False
    try:
        ser.break_condition = True
        time.sleep(0.0001)
        ser.break_condition = False
        time.sleep(0.000012)
        data = bytes([0x00]) + bytes(channels[:DMX_CHANNELS])
        ser.write(data)
        time.sleep(0.01)
    finally:
        ser.close()
    return True


def recv_dmx():
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
            return list(data[PORT_OFFSET:PORT_OFFSET + DMX_CHANNELS])
    except Exception as e:
        if "10054" not in str(e):
            print(f"  [ERROR] recv: {e}")
    return None


def make_white_frame(brightness, base_addr=1):
    data = [0] * DMX_CHANNELS
    for f in range(NUM_FIXTURES):
        addr = (base_addr - 1) + f * CH_PER_FIXTURE
        if addr + 2 < DMX_CHANNELS:
            data[addr] = brightness
            data[addr + 1] = brightness
            data[addr + 2] = brightness
    return data


def main():
    esp_ip = sys.argv[1] if len(sys.argv) > 1 else ESP32_IP
    num_frames = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    base_addr = int(sys.argv[3]) if len(sys.argv) > 3 else BASE_ADDR

    print(f"=== DMX White Strobe Test ===")
    print(f"ESP32: {esp_ip}:{ESP32_PORT}")
    print(f"FTDI: {FTDI_PORT}")
    print(f"Fixtures: {NUM_FIXTURES}, base_addr: {base_addr}")
    print(f"Frames: {num_frames}")
    print()

    test = recv_dmx()
    if test is None:
        print("[FAIL] ESP32 no response")
        return
    print("[OK] Connected\n")

    not_mono = 0
    mismatch = 0
    no_data = 0
    frame_errors = []

    for frame in range(num_frames):
        brightness = random.randint(0, 255)
        sent = make_white_frame(brightness)

        ok = send_dmx(sent)
        if not ok:
            continue

        time.sleep(0.01)

        received = recv_dmx()
        if received is None:
            no_data += 1
            print(f"  Frame {frame}: NO DATA")
            continue

        frame_has_error = False
        for f in range(NUM_FIXTURES):
            addr = f * CH_PER_FIXTURE
            if addr + 2 >= len(sent) or addr + 2 >= len(received):
                break

            sr, sg, sb = sent[addr], sent[addr + 1], sent[addr + 2]
            rr, rg, rb = received[addr], received[addr + 1], received[addr + 2]

            if rr != sr or rg != sg or rb != sb:
                mismatch += 1
                if not frame_has_error:
                    frame_errors.append(frame)
                    frame_has_error = True
                print(f"  Frame {frame} [b={brightness}]: MISMATCH fixture {f} ch[{addr}]: "
                      f"recv=({rr},{rg},{rb}) sent=({sr},{sg},{sb})")

            if rr != rg or rg != rb:
                not_mono += 1
                if not frame_has_error:
                    frame_errors.append(frame)
                    frame_has_error = True
                print(f"  Frame {frame} [b={brightness}]: NOT MONO fixture {f} ch[{addr}]: "
                      f"R={rr} G={rg} B={rb}")

    print(f"\n=== Result ===")
    print(f"Frames: {num_frames}")
    print(f"No data: {no_data}")
    print(f"With errors: {len(frame_errors)}")
    print(f"Mismatch: {mismatch}")
    print(f"Not mono: {not_mono}")

    if not_mono > 0 or mismatch > 0:
        print(f"\n>>> FAIL: data corruption detected")
    else:
        print(f"\n>>> PASS: all white frames received correctly")


if __name__ == "__main__":
    main()
