"""
DMX 30fps Sine Wave Test
Отправляет монохромную синусоиду 30fps и проверяет, что ESP32 получает корректно.
Выявляет глюки: цветные пятна в монохромном эффекте.
"""

import socket
import serial
import time
import sys
import math

FTDI_PORT = "COM7"
ESP32_IP = "192.168.1.222"
ESP32_PORT = 5555
DMX_CHANNELS = 512
FPS = 30
FRAME_MS = 1000.0 / FPS
WAVE_PERIOD = 50  # кадров на полный цикл волны


def make_mono_sine_wave(frame_num, num_leds=100):
    """Создать монохромный кадр: один RGB-триplet для каждого LED, яркость = sin."""
    led_count = min(num_leds, 170)
    step = (2.0 * math.pi) / WAVE_PERIOD
    brightness = int(127.5 + 127.5 * math.sin(step * frame_num))
    data = [0] * DMX_CHANNELS
    for i in range(led_count):
        base = i * 3
        if base + 2 < DMX_CHANNELS:
            data[base] = brightness
            data[base + 1] = brightness
            data[base + 2] = brightness
    return data, brightness


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
        if len(data) == 1024:
            return list(data[:512])
    except Exception as e:
        # WinError 10054 is expected (server shuts down socket)
        if "10054" not in str(e):
            print(f"  [ERROR] recv: {e}")
    return None


def check_mono_violations(sent, received, frame_num):
    """Проверить что received монохромный и совпадает с sent."""
    errors = []
    limit = min(len(sent), len(received), 510)
    for i in range(0, limit, 3):
        s_r, s_g, s_b = sent[i], sent[i+1], sent[i+2]
        r_r, r_g, r_b = received[i], received[i+1], received[i+2]

        # Проверка 1: отправленные данные должны быть монохромными
        if s_r != s_g or s_g != s_b:
            errors.append(("sent_not_mono", i, s_r, s_g, s_b))

        # Проверка 2: принятые данные должны совпадать с отправленными
        if r_r != s_r or r_g != s_g or r_b != s_b:
            errors.append(("mismatch", i, r_r, r_g, r_b, s_r, s_g, s_b))

        # Проверка 3: принятые должны быть монохромными
        if r_r != r_g or r_g != r_b:
            errors.append(("recv_not_mono", i, r_r, r_g, r_b))
    return errors


def main():
    esp_ip = sys.argv[1] if len(sys.argv) > 1 else ESP32_IP
    num_frames = int(sys.argv[2]) if len(sys.argv) > 2 else 90  # 3 секунды при 30fps

    print(f"=== DMX 30fps Mono Sine Wave Test ===")
    print(f"ESP32: {esp_ip}:{ESP32_PORT}")
    print(f"FTDI: {FTDI_PORT}")
    print(f"Frames: {num_frames} ({num_frames/FPS:.1f}s at {FPS}fps)")
    print()

    # Проверка связи
    test = recv_dmx()
    if test is None:
        print("[FAIL] ESP32 не отвечает")
        return
    print("[OK] Соединение установлено\n")

    total_errors = 0
    recv_not_mono_count = 0
    mismatch_count = 0
    frame_errors = []

    for frame in range(num_frames):
        sent, brightness = make_mono_sine_wave(frame)
        ok = send_dmx(sent)
        if not ok:
            continue

        time.sleep(0.005)  # 5ms — ждём обработки

        received = recv_dmx()
        if received is None:
            print(f"  Frame {frame}: [ERROR] no data")
            continue

        violations = check_mono_violations(sent, received, frame)
        frame_has_error = False

        for v in violations:
            if v[0] == "recv_not_mono":
                recv_not_mono_count += 1
                if not frame_has_error:
                    frame_errors.append(frame)
                    frame_has_error = True
                idx = v[1]
                print(f"  Frame {frame}: NOT MONO at ch[{idx}]: "
                      f"R={v[2]} G={v[3]} B={v[4]} (sent: {brightness},{brightness},{brightness})")

            elif v[0] == "mismatch":
                mismatch_count += 1
                if not frame_has_error:
                    frame_errors.append(frame)
                    frame_has_error = True
                idx = v[1]
                print(f"  Frame {frame}: MISMATCH at ch[{idx}]: "
                      f"recv=({v[2]},{v[3]},{v[4]}) sent=({v[5]},{v[6]},{v[7]})")

        if frame_has_error:
            total_errors += 1

        # Контроль FPS
        time.sleep(max(0, (FRAME_MS - 10) / 1000.0))

    print(f"\n=== Результат ===")
    print(f"Кадров отправлено: {num_frames}")
    print(f"Кадров с ошибками: {total_errors}")
    print(f"Цветных пятен (recv_not_mono): {recv_not_mono_count}")
    print(f"Рассинхрон (mismatch): {mismatch_count}")

    if recv_not_mono_count > 0:
        print(f"\n>>> ПРОБЛЕМА В ПРИЁМНИКЕ: ESP32 искажает монохромные данные")
    elif mismatch_count > 0:
        print(f"\n>>> ПРОБЛЕМА В ПЕРЕДАЧЕ: данные теряются между FT2232 и ESP32")
    else:
        print(f"\n>>> ВСЁ ОК: монохромная синусоида передаётся корректно")


if __name__ == "__main__":
    main()
