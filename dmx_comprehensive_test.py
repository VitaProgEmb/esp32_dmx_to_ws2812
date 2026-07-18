"""
DMX Comprehensive Test
Комплексная проверка ESP32 DMX приёмника:
  1. White Strobe — все фикстуры одинаковый белый
  2. Per-Fixture White — каждая фикстура свой белый
  3. Random RGB — случайные цвета
  4. Known Pattern — эталонные паттерны
  5. Rapid Fire — быстрая серия кадров
  6. Zero Fill — все нули (чёрный)
  7. Max Values — все 255 (максимум)
  8. Alternating — чередование 0/255
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
DMX_PORT = 0   # какой порт слушаем (0 или 1)
PORT_OFFSET = DMX_PORT * DMX_CHANNELS  # смещение в ответе debug server


def open_serial():
    try:
        ser = serial.Serial(FTDI_PORT, 250000, timeout=0.1)
        return ser
    except Exception as e:
        print(f"  [ERROR] {FTDI_PORT}: {e}")
        return None


def send_dmx(ser, channels):
    try:
        ser.break_condition = True
        time.sleep(0.0001)
        ser.break_condition = False
        time.sleep(0.000012)
        data = bytes([0x00]) + bytes(channels[:DMX_CHANNELS])
        ser.write(data)
        return True
    except Exception as e:
        print(f"  [ERROR] send: {e}")
        return False


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


def get_fix_addrs(base_addr=1):
    addrs = []
    for f in range(NUM_FIXTURES):
        a = (base_addr - 1) + f * CH_PER_FIXTURE
        addrs.append(a)
    return addrs


def verify(sent, received, test_name, frame, base_addr=1):
    addrs = get_fix_addrs(base_addr)
    errors = []
    for f in range(NUM_FIXTURES):
        a = addrs[f]
        if a + 2 >= len(sent) or a + 2 >= len(received):
            break
        sr, sg, sb = sent[a], sent[a + 1], sent[a + 2]
        rr, rg, rb = received[a], received[a + 1], received[a + 2]
        if rr != sr or rg != sg or rb != sb:
            errors.append((f, a, rr, rg, rb, sr, sg, sb))
    return errors


def make_channels():
    return [0] * DMX_CHANNELS


def set_fixture(ch, f, r, g, b, base_addr=1):
    a = (base_addr - 1) + f * CH_PER_FIXTURE
    if a + 2 < DMX_CHANNELS:
        ch[a] = r
        ch[a + 1] = g
        ch[a + 2] = b


def get_fixture(ch, f, base_addr=1):
    a = (base_addr - 1) + f * CH_PER_FIXTURE
    if a + 2 < len(ch):
        return ch[a], ch[a + 1], ch[a + 2]
    return 0, 0, 0


def run_test(ser, name, gen_func, num_frames=50, base_addr=1, delay=0.01):
    print(f"  [{name}] ", end="", flush=True)
    errors_total = 0
    for i in range(num_frames):
        sent = gen_func(i)
        if not send_dmx(ser, sent):
            continue
        time.sleep(delay)
        received = recv_dmx()
        if received is None:
            errors_total += NUM_FIXTURES
            continue
        errs = verify(sent, received, name, i, base_addr)
        errors_total += len(errs)
        if errs and errors_total <= 10:
            for f, a, rr, rg, rb, sr, sg, sb in errs:
                print(f"\n    F{f} ch[{a}]: recv=({rr},{rg},{rb}) sent=({sr},{sg},{sb})", end="")
    if errors_total == 0:
        print(f"OK ({num_frames} frames)")
    else:
        print(f"\n    FAIL: {errors_total} errors in {num_frames} frames")
    return errors_total


def main():
    esp_ip = sys.argv[1] if len(sys.argv) > 1 else ESP32_IP
    base_addr = int(sys.argv[2]) if len(sys.argv) > 2 else BASE_ADDR

    print(f"=== DMX Comprehensive Test ===")
    print(f"ESP32: {esp_ip}:{ESP32_PORT}")
    print(f"FTDI: {FTDI_PORT}")
    print(f"Fixtures: {NUM_FIXTURES}, base_addr: {base_addr}")
    print()

    ser = open_serial()
    if ser is None:
        return

    test = recv_dmx()
    if test is None:
        print("[FAIL] ESP32 no response")
        ser.close()
        return
    print("[OK] Connected\n")

    total_errors = 0

    # --- 1. White Strobe ---
    def gen_white_strobe(i):
        ch = make_channels()
        b = random.randint(0, 255)
        for f in range(NUM_FIXTURES):
            set_fixture(ch, f, b, b, b, base_addr)
        return ch

    total_errors += run_test(ser, "White Strobe", gen_white_strobe, 50, base_addr)

    # --- 2. Per-Fixture White ---
    def gen_per_fixture_white(i):
        ch = make_channels()
        for f in range(NUM_FIXTURES):
            b = random.randint(0, 255)
            set_fixture(ch, f, b, b, b, base_addr)
        return ch

    total_errors += run_test(ser, "Per-Fixture White", gen_per_fixture_white, 50, base_addr)

    # --- 3. Random RGB ---
    def gen_random_rgb(i):
        ch = make_channels()
        for f in range(NUM_FIXTURES):
            r = random.randint(0, 255)
            g = random.randint(0, 255)
            b = random.randint(0, 255)
            set_fixture(ch, f, r, g, b, base_addr)
        return ch

    total_errors += run_test(ser, "Random RGB", gen_random_rgb, 50, base_addr)

    # --- 4. Known Pattern: each fixture = unique solid color ---
    palette = [
        (255, 0, 0), (0, 255, 0), (0, 0, 255),
        (255, 255, 0), (0, 255, 255), (255, 0, 255),
    ]
    def gen_known_pattern(i):
        ch = make_channels()
        idx = i % len(palette)
        r, g, b = palette[(idx) % len(palette)]
        for f in range(NUM_FIXTURES):
            cr, cg, cb = palette[(idx + f) % len(palette)]
            set_fixture(ch, f, cr, cg, cb, base_addr)
        return ch

    total_errors += run_test(ser, "Known Pattern", gen_known_pattern, 50, base_addr)

    # --- 5. Rapid Fire (минимальная задержка) ---
    def gen_rapid(i):
        ch = make_channels()
        b = (i * 17) & 0xFF
        for f in range(NUM_FIXTURES):
            set_fixture(ch, f, b, b, b, base_addr)
        return ch

    total_errors += run_test(ser, "Rapid Fire", gen_rapid, 100, base_addr, delay=0.003)

    # --- 6. Zero Fill ---
    def gen_zero(i):
        return make_channels()

    total_errors += run_test(ser, "Zero Fill", gen_zero, 30, base_addr)

    # --- 7. Max Values ---
    def gen_max(i):
        ch = make_channels()
        for f in range(NUM_FIXTURES):
            set_fixture(ch, f, 255, 255, 255, base_addr)
        return ch

    total_errors += run_test(ser, "Max Values", gen_max, 30, base_addr)

    # --- 8. Alternating ---
    def gen_alternating(i):
        ch = make_channels()
        val = 255 if (i % 2 == 0) else 0
        for f in range(NUM_FIXTURES):
            set_fixture(ch, f, val, val, val, base_addr)
        return ch

    total_errors += run_test(ser, "Alternating", gen_alternating, 50, base_addr)

    ser.close()

    print(f"\n{'=' * 40}")
    if total_errors == 0:
        print(f"ALL PASS (360 frames, 0 errors)")
    else:
        print(f"FAIL: {total_errors} total errors")


if __name__ == "__main__":
    main()
