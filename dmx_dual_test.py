"""
DMX Comprehensive Dual-Port Test
Тестирует оба DMX порта одновременно:
  COM6 (Port A) → pin 16 = UART3 = port 1 (universe 2)
  COM7 (Port B) → pin 4  = UART2 = port 0 (universe 1)
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

# COM7 (Port B) → UART2 → port 0 (universe 1)
PORT0_SER = "COM7"
# COM6 (Port A) → UART3 → port 1 (universe 2)
PORT1_SER = "COM6"


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
            port0 = list(data[0:512])
            port1 = list(data[512:1024])
            return port0, port1
    except Exception as e:
        if "10054" not in str(e):
            print(f"  [ERROR] recv: {e}")
    return None, None


def send_dmx_port(ser, channels):
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


def make_channels():
    return [0] * DMX_CHANNELS


def set_fixture(ch, f, r, g, b, base_addr=1):
    a = (base_addr - 1) + f * CH_PER_FIXTURE
    if a + 2 < DMX_CHANNELS:
        ch[a] = r
        ch[a + 1] = g
        ch[a + 2] = b


def verify(sent, received, base_addr=1):
    errors = []
    for f in range(NUM_FIXTURES):
        a = (base_addr - 1) + f * CH_PER_FIXTURE
        if a + 2 >= len(sent) or a + 2 >= len(received):
            break
        sr, sg, sb = sent[a], sent[a + 1], sent[a + 2]
        rr, rg, rb = received[a], received[a + 1], received[a + 2]
        if rr != sr or rg != sg or rb != sb:
            errors.append((f, a, rr, rg, rb, sr, sg, sb))
    return errors


def get_fix_addrs(base_addr=1):
    return [(base_addr - 1) + f * CH_PER_FIXTURE for f in range(NUM_FIXTURES)]


def run_dual_test(ser0, ser1, name, gen_func, num_frames=50, base_addr=1, delay=0.015):
    print(f"  [{name}] ", end="", flush=True)
    errors_p0 = 0
    errors_p1 = 0
    for i in range(num_frames):
        ch0, ch1 = gen_func(i)
        send_dmx_port(ser0, ch0)
        if ser1:
            send_dmx_port(ser1, ch1)
        time.sleep(delay)
        recv0, recv1 = recv_all()
        if recv0 is None:
            errors_p0 += NUM_FIXTURES
            continue
        errs0 = verify(ch0, recv0, base_addr)
        errors_p0 += len(errs0)
        if errs0 and errors_p0 <= 12:
            for f, a, rr, rg, rb, sr, sg, sb in errs0:
                print(f"\n    P0 F{f} ch[{a}]: recv=({rr},{rg},{rb}) sent=({sr},{sg},{sb})", end="")
        if ser1 and recv1:
            errs1 = verify(ch1, recv1, base_addr)
            errors_p1 += len(errs1)
            if errs1 and errors_p1 <= 12:
                for f, a, rr, rg, rb, sr, sg, sb in errs1:
                    print(f"\n    P1 F{f} ch[{a}]: recv=({rr},{rg},{rb}) sent=({sr},{sg},{sb})", end="")

    p0_status = "OK" if errors_p0 == 0 else f"FAIL({errors_p0})"
    p1_status = "OK" if errors_p1 == 0 else f"FAIL({errors_p1})"
    print(f" P0={p0_status} P1={p1_status} ({num_frames} frames)")
    return errors_p0, errors_p1


def main():
    esp_ip = sys.argv[1] if len(sys.argv) > 1 else ESP32_IP
    base_addr = int(sys.argv[2]) if len(sys.argv) > 2 else BASE_ADDR

    print(f"=== DMX Comprehensive Dual-Port Test ===")
    print(f"ESP32: {esp_ip}:{ESP32_PORT}")
    print(f"Port 0: {PORT0_SER} (UART2, RX=4)")
    print(f"Port 1: {PORT1_SER} (UART3, RX=16)")
    print(f"Fixtures: {NUM_FIXTURES}, base_addr: {base_addr}")
    print()

    test = recv_all()
    if test[0] is None:
        print("[FAIL] ESP32 no response")
        return
    print("[OK] Connected\n")

    ser0 = None
    ser1 = None
    try:
        ser0 = serial.Serial(PORT0_SER, 250000, timeout=0.1)
        print(f"[OK] {PORT0_SER} opened")
    except Exception as e:
        print(f"[WARN] {PORT0_SER}: {e}")

    try:
        ser1 = serial.Serial(PORT1_SER, 250000, timeout=0.1)
        print(f"[OK] {PORT1_SER} opened")
    except Exception as e:
        print(f"[WARN] {PORT1_SER}: {e}")

    if not ser0 and not ser1:
        print("[FAIL] No serial ports")
        return

    total_p0 = 0
    total_p1 = 0

    # --- 1. White Strobe ---
    def gen_white_strobe(i):
        b = random.randint(0, 255)
        c0 = make_channels()
        c1 = make_channels()
        for f in range(NUM_FIXTURES):
            set_fixture(c0, f, b, b, b, base_addr)
            set_fixture(c1, f, b, b, b, base_addr)
        return c0, c1

    e0, e1 = run_dual_test(ser0, ser1, "White Strobe", gen_white_strobe, 50, base_addr)
    total_p0 += e0; total_p1 += e1

    # --- 2. Per-Fixture White ---
    def gen_per_white(i):
        c0 = make_channels()
        c1 = make_channels()
        for f in range(NUM_FIXTURES):
            b = random.randint(0, 255)
            set_fixture(c0, f, b, b, b, base_addr)
            set_fixture(c1, f, b, b, b, base_addr)
        return c0, c1

    e0, e1 = run_dual_test(ser0, ser1, "Per-Fixture White", gen_per_white, 50, base_addr)
    total_p0 += e0; total_p1 += e1

    # --- 3. Random RGB ---
    def gen_random_rgb(i):
        c0 = make_channels()
        c1 = make_channels()
        for f in range(NUM_FIXTURES):
            r0, g0, b0 = random.randint(0, 255), random.randint(0, 255), random.randint(0, 255)
            r1, g1, b1 = random.randint(0, 255), random.randint(0, 255), random.randint(0, 255)
            set_fixture(c0, f, r0, g0, b0, base_addr)
            set_fixture(c1, f, r1, g1, b1, base_addr)
        return c0, c1

    e0, e1 = run_dual_test(ser0, ser1, "Random RGB", gen_random_rgb, 50, base_addr)
    total_p0 += e0; total_p1 += e1

    # --- 4. Known Pattern ---
    palette = [
        (255, 0, 0), (0, 255, 0), (0, 0, 255),
        (255, 255, 0), (0, 255, 255), (255, 0, 255),
    ]

    def gen_known(i):
        c0 = make_channels()
        c1 = make_channels()
        for f in range(NUM_FIXTURES):
            r, g, b = palette[(i + f) % len(palette)]
            set_fixture(c0, f, r, g, b, base_addr)
            set_fixture(c1, f, r, g, b, base_addr)
        return c0, c1

    e0, e1 = run_dual_test(ser0, ser1, "Known Pattern", gen_known, 50, base_addr)
    total_p0 += e0; total_p1 += e1

    # --- 5. Rapid Fire ---
    def gen_rapid(i):
        b = (i * 17) & 0xFF
        c0 = make_channels()
        c1 = make_channels()
        for f in range(NUM_FIXTURES):
            set_fixture(c0, f, b, b, b, base_addr)
            set_fixture(c1, f, b, b, b, base_addr)
        return c0, c1

    e0, e1 = run_dual_test(ser0, ser1, "Rapid Fire", gen_rapid, 100, base_addr, delay=0.005)
    total_p0 += e0; total_p1 += e1

    # --- 6. Zero Fill ---
    def gen_zero(i):
        return make_channels(), make_channels()

    e0, e1 = run_dual_test(ser0, ser1, "Zero Fill", gen_zero, 30, base_addr)
    total_p0 += e0; total_p1 += e1

    # --- 7. Max Values ---
    def gen_max(i):
        c0 = make_channels()
        c1 = make_channels()
        for f in range(NUM_FIXTURES):
            set_fixture(c0, f, 255, 255, 255, base_addr)
            set_fixture(c1, f, 255, 255, 255, base_addr)
        return c0, c1

    e0, e1 = run_dual_test(ser0, ser1, "Max Values", gen_max, 30, base_addr)
    total_p0 += e0; total_p1 += e1

    # --- 8. Alternating ---
    def gen_alternating(i):
        val = 255 if (i % 2 == 0) else 0
        c0 = make_channels()
        c1 = make_channels()
        for f in range(NUM_FIXTURES):
            set_fixture(c0, f, val, val, val, base_addr)
            set_fixture(c1, f, val, val, val, base_addr)
        return c0, c1

    e0, e1 = run_dual_test(ser0, ser1, "Alternating", gen_alternating, 50, base_addr)
    total_p0 += e0; total_p1 += e1

    if ser0: ser0.close()
    if ser1: ser1.close()

    print(f"\n{'=' * 50}")
    print(f"Port 0 ({PORT0_SER}): {total_p0} errors")
    print(f"Port 1 ({PORT1_SER}): {total_p1} errors")
    if total_p0 == 0 and total_p1 == 0:
        print(f"ALL PASS (720 frames, 0 errors)")
    else:
        print(f"FAIL")


if __name__ == "__main__":
    main()
