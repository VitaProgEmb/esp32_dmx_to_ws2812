"""
DMX Single-Port Test — COM6 only (port 1)
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
    while len(data) < 1024:
        chunk = s.recv(1024 - len(data))
        if not chunk:
            break
        data += chunk
    try:
        s.close()
    except:
        pass
    if len(data) >= 1024:
        return list(data[0:512]), list(data[512:1024])
    return None, None


def send_dmx(ser, channels):
    ser.break_condition = True
    time.sleep(0.0001)
    ser.break_condition = False
    time.sleep(0.000012)
    ser.write(bytes([0x00]) + bytes(channels[:DMX_CHANNELS]))
    time.sleep(0.01)


def set_fixture(ch, f, r, g, b):
    a = (BASE_ADDR - 1) + f * CH_PER_FIXTURE
    if a + 2 < DMX_CHANNELS:
        ch[a] = r
        ch[a + 1] = g
        ch[a + 2] = b


def main():
    print("=== COM6 only -> Port 1 ===\n")

    ser = serial.Serial("COM6", 250000, timeout=0.1)
    print("[OK] COM6 opened")

    # Test 1: send known pattern on COM6 only, read port 1
    print("\n[Test 1] Send RGB on COM6, read port 1:")
    ch = [0] * DMX_CHANNELS
    set_fixture(ch, 0, 255, 0, 0)
    set_fixture(ch, 1, 0, 255, 0)
    set_fixture(ch, 2, 0, 0, 255)
    set_fixture(ch, 3, 255, 255, 0)
    set_fixture(ch, 4, 0, 255, 255)
    set_fixture(ch, 5, 255, 0, 255)

    for attempt in range(5):
        send_dmx(ser, ch)
        time.sleep(0.02)
        recv0, recv1 = recv_all()
        if recv1:
            print(f"  Attempt {attempt}: port1[0:18] = {recv1[0:18]}")
            expected = [255,0,0, 0,255,0, 0,0,255, 255,255,0, 0,255,255, 255,0,255]
            if recv1[0:18] == expected:
                print("  PASS!")
                break
            else:
                print(f"  Expected: {expected}")

    # Test 2: send white mono on COM6
    print("\n[Test 2] Send white on COM6, read port 1:")
    for attempt in range(5):
        ch2 = [0] * DMX_CHANNELS
        b = 128
        for f in range(6):
            set_fixture(ch2, f, b, b, b)
        send_dmx(ser, ch2)
        time.sleep(0.02)
        recv0, recv1 = recv_all()
        if recv1:
            vals = [recv1[i] for i in range(0, 18)]
            print(f"  Attempt {attempt}: port1[0:18] = {vals}")

    ser.close()


if __name__ == "__main__":
    main()
