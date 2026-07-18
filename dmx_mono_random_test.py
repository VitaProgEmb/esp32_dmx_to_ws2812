"""
DMX Monochrome Random Test
Отправляет монохромный рандом (R=G=B) и проверяет что ESP32 получает чистые данные.
Выявляет цветные артефакты (жёлтый, голубой, розовый) при монохромном входе.
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
NUM_LEDS = 6  # количество фикстур (приборов)
CH_PER_FIXTURE = 3  # R, G, B на прибор


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
            return list(data[:512])
    except Exception as e:
        if "10054" not in str(e):
            print(f"  [ERROR] recv: {e}")
    return None


def make_mono_random_frame(fixtures, base_addr=1):
    """Создать монохромный кадр: все приборы получают одинаковый случайный цвет."""
    brightness = random.randint(0, 255)
    data = [0] * DMX_CHANNELS
    for f in range(fixtures):
        addr = base_addr + f * CH_PER_FIXTURE
        if addr + 2 < DMX_CHANNELS:
            data[addr] = brightness
            data[addr + 1] = brightness
            data[addr + 2] = brightness
    return data, brightness


def make_mono_random_per_fixture(fixtures, base_addr=1):
    """Создать монохромный кадр: каждый прибор получает свой случайный цвет (но R=G=B)."""
    data = [0] * DMX_CHANNELS
    values = []
    for f in range(fixtures):
        val = random.randint(0, 255)
        values.append(val)
        addr = base_addr + f * CH_PER_FIXTURE
        if addr + 2 < DMX_CHANNELS:
            data[addr] = val
            data[addr + 1] = val
            data[addr + 2] = val
    return data, values


def check_mono(sent, received, frame_num, label=""):
    """Проверить что принятые данные монохромны для каждой фикстуры."""
    errors = []
    for f in range(NUM_LEDS):
        addr = f * CH_PER_FIXTURE
        if addr + 2 >= len(sent) or addr + 2 >= len(received):
            break
        sr, sg, sb = sent[addr], sent[addr + 1], sent[addr + 2]
        rr, rg, rb = received[addr], received[addr + 1], received[addr + 2]

        # Проверка 1: отправленные монохромны
        if sr != sg or sg != sb:
            errors.append(("sent_not_mono", f, addr, sr, sg, sb))

        # Проверка 2: принятые совпадают с отправленными
        if rr != sr or rg != sg or rb != sb:
            errors.append(("mismatch", f, addr, rr, rg, rb, sr, sg, sb))

        # Проверка 3: принятые монохромны
        if rr != rg or rg != rb:
            errors.append(("recv_not_mono", f, addr, rr, rg, rb))

    return errors


def main():
    esp_ip = sys.argv[1] if len(sys.argv) > 1 else ESP32_IP
    num_frames = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    base_addr = int(sys.argv[3]) if len(sys.argv) > 3 else 1

    print(f"=== DMX Monochrome Random Test ===")
    print(f"ESP32: {esp_ip}:{ESP32_PORT}")
    print(f"FTDI: {FTDI_PORT}")
    print(f"Fixtures: {NUM_LEDS}, base_addr: {base_addr}")
    print(f"Frames: {num_frames}")
    print()

    # Проверка связи
    test = recv_dmx()
    if test is None:
        print("[FAIL] ESP32 не отвечает")
        return
    print("[OK] Соединение установлено\n")

    recv_not_mono = 0
    mismatch = 0
    sent_not_mono = 0
    frame_errors = []

    for frame in range(num_frames):
        # Чередуем: общий brightness и per-fixture
        if frame % 2 == 0:
            sent, brightness = make_mono_random_frame(NUM_LEDS, base_addr)
            label = f"all={brightness}"
        else:
            sent, values = make_mono_random_per_fixture(NUM_LEDS, base_addr)
            label = f"per={values}"

        ok = send_dmx(sent)
        if not ok:
            continue

        time.sleep(0.01)

        received = recv_dmx()
        if received is None:
            print(f"  Frame {frame}: [ERROR] no data")
            continue

        violations = check_mono(sent, received, frame, label)
        frame_has_error = False

        for v in violations:
            if v[0] == "recv_not_mono":
                recv_not_mono += 1
                if not frame_has_error:
                    frame_errors.append(frame)
                    frame_has_error = True
                f, addr = v[1], v[2]
                print(f"  Frame {frame} [{label}]: NOT MONO fixture {f} ch[{addr}]: "
                      f"R={v[3]} G={v[4]} B={v[5]}")

            elif v[0] == "mismatch":
                mismatch += 1
                if not frame_has_error:
                    frame_errors.append(frame)
                    frame_has_error = True
                f, addr = v[1], v[2]
                print(f"  Frame {frame} [{label}]: MISMATCH fixture {f} ch[{addr}]: "
                      f"recv=({v[3]},{v[4]},{v[5]}) sent=({v[6]},{v[7]},{v[8]})")

            elif v[0] == "sent_not_mono":
                sent_not_mono += 1

    print(f"\n=== Результат ===")
    print(f"Кадров: {num_frames}")
    print(f"С ошибками: {len(frame_errors)}")
    print(f"Цветных артефактов (recv_not_mono): {recv_not_mono}")
    print(f"Рассинхрон (mismatch): {mismatch}")

    if recv_not_mono > 0:
        print(f"\n>>> ПРОБЛЕМА: ESP32 искажает монохромные данные")
        print(f"    Возможные причины:")
        print(f"    1. channel_order remap в firmware меняет R↔G")
        print(f"    2. Тайминги DMX (данные не успевают прийти)")
        print(f"    3. Буфер не очищается между кадрами")
    elif mismatch > 0:
        print(f"\n>>> ПРОБЛЕМА: данные теряются между FT2232 и ESP32")
    else:
        print(f"\n>>> ВСЁ ОК: монохромные данные передаются чисто")


if __name__ == "__main__":
    main()
