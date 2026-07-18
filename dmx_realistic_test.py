"""
DMX Realistic Timing Test
Sends DMX at real-world rate: 44fps = 22.7ms between frames
Both ports simultaneously from different 'controllers'
"""
import urllib.request
import socket
import serial
import time
import random

ESP32_IP = "192.168.1.222"
DMX_CHANNELS = 512
NUM_FIXTURES = 6
CH_PER_FIXTURE = 3
BASE_ADDR = 1
FPS = 44
FRAME_MS = 1000.0 / FPS  # ~22.7ms


def recv_debug():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect((ESP32_IP, 5555))
    s.send(b"\x01")
    data = b""
    while len(data) < 1032:
        chunk = s.recv(1032 - len(data))
        if not chunk:
            break
        data += chunk
    s.close()
    c0 = int.from_bytes(data[0:4], "little")
    c1 = int.from_bytes(data[4:8], "little")
    p0 = list(data[8:520])
    p1 = list(data[520:1032])
    return c0, c1, p0, p1


def send_dmx(ser, channels):
    ser.break_condition = True
    time.sleep(0.0001)
    ser.break_condition = False
    time.sleep(0.000012)
    ser.write(bytes([0x00]) + bytes(channels[:DMX_CHANNELS]))


def reboot():
    req = urllib.request.Request("http://192.168.1.222/api/reboot", method="POST", data=b"{}")
    try:
        urllib.request.urlopen(req, timeout=5)
    except Exception:
        pass
    time.sleep(5)


def set_fixture(ch, f, r, g, b):
    a = (BASE_ADDR - 1) + f * CH_PER_FIXTURE
    if a + 2 < DMX_CHANNELS:
        ch[a] = r
        ch[a + 1] = g
        ch[a + 2] = b


def main():
    print("=== DMX Realistic Timing Test (%d fps, %.1fms frame) ===" % (FPS, FRAME_MS))
    print()

    reboot()
    recv_debug()

    ser0 = serial.Serial("COM7", 250000, timeout=0.1)
    ser1 = serial.Serial("COM6", 250000, timeout=0.1)
    time.sleep(0.5)
    print("[OK] Both ports opened\n")

    NUM_FRAMES = 200
    p0_err_total = 0
    p1_err_total = 0

    # Test 1: Same static color on both ports
    print("[Test 1] Static white both ports, %d frames at %d fps" % (NUM_FRAMES, FPS))
    ch0 = [0] * DMX_CHANNELS
    ch1 = [0] * DMX_CHANNELS
    for f in range(NUM_FIXTURES):
        set_fixture(ch0, f, 200, 200, 200)
        set_fixture(ch1, f, 200, 200, 200)

    p0_err = 0
    p1_err = 0
    for i in range(NUM_FRAMES):
        send_dmx(ser0, ch0)
        send_dmx(ser1, ch1)
        time.sleep(FRAME_MS / 1000.0)

    c0, c1, p0, p1 = recv_debug()
    for f in range(NUM_FIXTURES):
        a = (BASE_ADDR - 1) + f * CH_PER_FIXTURE
        if p0[a:a+3] != [200, 200, 200]:
            p0_err += 1
        if p1[a:a+3] != [200, 200, 200]:
            p1_err += 1
    print("  P0 frames: %d, P1 frames: %d" % (c0, c1))
    print("  P0 errors: %d/%d, P1 errors: %d/%d" % (p0_err, NUM_FIXTURES, p1_err, NUM_FIXTURES))
    p0_err_total += p0_err
    p1_err_total += p1_err

    # Test 2: Different colors per port
    print("\n[Test 2] P0=Red, P1=Green, %d frames at %d fps" % (NUM_FRAMES, FPS))
    ch0 = [0] * DMX_CHANNELS
    ch1 = [0] * DMX_CHANNELS
    for f in range(NUM_FIXTURES):
        set_fixture(ch0, f, 255, 0, 0)
        set_fixture(ch1, f, 0, 255, 0)

    for i in range(NUM_FRAMES):
        send_dmx(ser0, ch0)
        send_dmx(ser1, ch1)
        time.sleep(FRAME_MS / 1000.0)

    c0, c1, p0, p1 = recv_debug()
    p0_err = 0
    p1_err = 0
    for f in range(NUM_FIXTURES):
        a = (BASE_ADDR - 1) + f * CH_PER_FIXTURE
        if p0[a:a+3] != [255, 0, 0]:
            p0_err += 1
        if p1[a:a+3] != [0, 255, 0]:
            p1_err += 1
    print("  P0 frames: %d, P1 frames: %d" % (c0, c1))
    print("  P0 errors: %d/%d, P1 errors: %d/%d" % (p0_err, NUM_FIXTURES, p1_err, NUM_FIXTURES))
    p0_err_total += p0_err
    p1_err_total += p1_err

    # Test 3: Random changing colors
    print("\n[Test 3] Random RGB both ports, %d frames at %d fps" % (NUM_FRAMES, FPS))
    errors_p0 = 0
    errors_p1 = 0
    for i in range(NUM_FRAMES):
        ch0 = [0] * DMX_CHANNELS
        ch1 = [0] * DMX_CHANNELS
        for f in range(NUM_FIXTURES):
            set_fixture(ch0, f, random.randint(0, 255), random.randint(0, 255), random.randint(0, 255))
            set_fixture(ch1, f, random.randint(0, 255), random.randint(0, 255), random.randint(0, 255))
        send_dmx(ser0, ch0)
        send_dmx(ser1, ch1)
        time.sleep(FRAME_MS / 1000.0)

        # Check every 20th frame
        if i % 20 == 19:
            c0, c1, p0, p1 = recv_debug()
            for f in range(NUM_FIXTURES):
                a = (BASE_ADDR - 1) + f * CH_PER_FIXTURE
                if p0[a:a+3] != ch0[a:a+3]:
                    errors_p0 += 1
                if p1[a:a+3] != ch1[a:a+3]:
                    errors_p1 += 1

    print("  P0 check errors: %d, P1 check errors: %d" % (errors_p0, errors_p1))
    p0_err_total += errors_p0
    p1_err_total += errors_p1

    # Test 4: Per-fixture different colors
    print("\n[Test 4] Per-fixture rainbow, %d frames at %d fps" % (NUM_FRAMES, FPS))
    ch0 = [0] * DMX_CHANNELS
    ch1 = [0] * DMX_CHANNELS
    colors = [(255,0,0), (255,127,0), (255,255,0), (0,255,0), (0,0,255), (127,0,255)]
    for f in range(NUM_FIXTURES):
        set_fixture(ch0, f, *colors[f])
        set_fixture(ch1, f, *colors[(f+3) % 6])

    for i in range(NUM_FRAMES):
        send_dmx(ser0, ch0)
        send_dmx(ser1, ch1)
        time.sleep(FRAME_MS / 1000.0)

    c0, c1, p0, p1 = recv_debug()
    p0_err = 0
    p1_err = 0
    for f in range(NUM_FIXTURES):
        a = (BASE_ADDR - 1) + f * CH_PER_FIXTURE
        expected0 = list(colors[f])
        expected1 = list(colors[(f+3) % 6])
        if p0[a:a+3] != expected0:
            p0_err += 1
        if p1[a:a+3] != expected1:
            p1_err += 1
    print("  P0 errors: %d/%d, P1 errors: %d/%d" % (p0_err, NUM_FIXTURES, p1_err, NUM_FIXTURES))
    p0_err_total += p0_err
    p1_err_total += p1_err

    ser0.close()
    ser1.close()

    print("\n" + "=" * 50)
    print("TOTAL P0 errors: %d, P1 errors: %d" % (p0_err_total, p1_err_total))
    if p0_err_total == 0 and p1_err_total == 0:
        print("ALL PASS")
    else:
        print("FAIL")


if __name__ == "__main__":
    main()
