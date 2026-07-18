"""
DMX Sniffer Debug Tool
Отправляет тестовые паттерны через FT2232 (COM7) и проверяет приём на ESP32.

Использование:
  1. Подключи FT2232: COM7 → DMX вход ESP32 (порт 1)
  2. python dmx_test.py [esp_ip]
"""

import socket
import serial
import time
import sys

DMX_CHANNELS = 512
ESP32_IP = "192.168.1.222"
ESP32_PORT = 5555
FTDI_PORT = "COM7"


def send_dmx_ftdi(channels, port=FTDI_PORT):
    """Отправить DMX кадр через FT2232 COM-порт."""
    try:
        ser = serial.Serial(port, 250000, timeout=0.1)
    except Exception as e:
        print(f"  [ERROR] Не удалось открыть {port}: {e}")
        return False

    try:
        # DMX break: low >= 88 us
        ser.break_condition = True
        time.sleep(0.0001)  # 100 us
        ser.break_condition = False

        # Mark after break: >= 8 us
        time.sleep(0.000012)  # 12 us

        # Start code + channels
        data = bytes([0x00]) + bytes(channels[:DMX_CHANNELS])
        ser.write(data)

        # Wait for transmission to complete
        time.sleep(0.01)  # ~25ms at 250000 baud for 513 bytes
    finally:
        ser.close()

    return True


def recv_dmx_esp32(ip, port):
    """Прочитать s_rx_buf с ESP32 через TCP."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5.0)
        s.connect((ip, port))
        s.send(b"\x01")
        data = s.recv(1024)
        s.close()
        if len(data) == 1024:
            return list(data)
    except Exception as e:
        print(f"[ERROR] Не удалось подключиться к ESP32: {e}")
    return None


def compare(sent, received, label=""):
    """Сравнить отправленные и принятые данные."""
    errors = 0
    first_err = None
    last_err = None
    for i in range(min(len(sent), len(received))):
        if sent[i] != received[i]:
            if first_err is None:
                first_err = i
            last_err = i
            errors += 1
    return errors, first_err, last_err


def test_pattern_sweep():
    return [(i + 1) & 0xFF for i in range(DMX_CHANNELS)]


def test_pattern_single(ch, val):
    data = [0] * DMX_CHANNELS
    if 0 <= ch < DMX_CHANNELS:
        data[ch] = val
    return data


def test_pattern_all(val):
    return [val] * DMX_CHANNELS


def test_pattern_rgb():
    data = []
    for i in range(DMX_CHANNELS):
        data.append([255, 128, 64][i % 3])
    return data


def run_test(name, pattern_func, esp_ip):
    pattern = pattern_func()
    print(f"\n--- Тест: {name} ---")
    print(f"  Отправляю {len(pattern)} каналов через {FTDI_PORT}...")

    ok = send_dmx_ftdi(pattern)
    if not ok:
        print("  [FAIL] Ошибка отправки")
        return False

    time.sleep(0.3)  # ждём приём кадра на ESP32

    print("  Читаю s_rx_buf с ESP32...")
    received = recv_dmx_esp32(esp_ip, ESP32_PORT)
    if received is None:
        print("  [FAIL] Не удалось прочитать данные")
        return False

    errors, first_err, last_err = compare(pattern, received[:512], label="port0")
    if errors == 0:
        print(f"  [OK] Все {DMX_CHANNELS} каналов совпадают")
    else:
        print(f"  [FAIL] {errors} ошибок: first={first_err}, last={last_err}")
        # Show first few mismatches
        count = 0
        for i in range(512):
            if pattern[i] != received[i]:
                print(f"    [{i}] sent={pattern[i]} recv={received[i]}")
                count += 1
                if count >= 5:
                    print(f"    ... и ещё {errors - 5} ошибок")
                    break
    return errors == 0


def main():
    esp_ip = sys.argv[1] if len(sys.argv) > 1 else ESP32_IP
    print(f"ESP32: {esp_ip}:{ESP32_PORT}")
    print(f"FTDI: {FTDI_PORT}")

    print("\nПроверка соединения с ESP32...")
    test_data = recv_dmx_esp32(esp_ip, ESP32_PORT)
    if test_data is None:
        print("[FAIL] ESP32 не отвечает. Проверь IP и Wi-Fi.")
        return
    print("[OK] Соединение установлено")

    passed = 0
    total = 0

    tests = [
        ("Single CH1=255", lambda: test_pattern_single(0, 255)),
        ("Single CH256=255", lambda: test_pattern_single(255, 255)),
        ("All 0", lambda: test_pattern_all(0)),
        ("All 128", lambda: test_pattern_all(128)),
        ("All 255", lambda: test_pattern_all(255)),
        ("Sweep 1-512", test_pattern_sweep),
        ("RGB repeat", test_pattern_rgb),
    ]

    for name, func in tests:
        total += 1
        if run_test(name, func, esp_ip):
            passed += 1

    print(f"\n=== Результат: {passed}/{total} тестов пройдено ===")


if __name__ == "__main__":
    main()
