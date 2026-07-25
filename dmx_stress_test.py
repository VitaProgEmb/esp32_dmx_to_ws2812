"""
DMX RX Stress Test — проверка приёмника через COM9 TX + UDP readback
====================================================================

Схема:
  COM9 TX (FT2232) ──┬── GPIO15 (RX0)
                     └── GPIO16 (RX1)

Протокол:
  1. Отправляем DMX кадр через COM9 (250kbaud, 8N2)
  2. Читаем обратно через UDP (порт 5124, команда 0x01)
  3. Сравниваем: что отправили == что приняли

Запуск:
  python dmx_stress_test.py [--port COM9] [--ip 192.168.1.100] [--duration 60] [--fps 30]
"""

import serial
import socket
import struct
import time
import argparse
import sys
import random


def build_dmx_frame(channels: list[int]) -> bytes:
    """Собираем DMX кадр: BREAK + MAB + StartCode + 512 каналов"""
    frame = bytearray(513)
    frame[0] = 0x00  # Start Code
    for i, ch in enumerate(channels[:512]):
        frame[i + 1] = ch & 0xFF
    return bytes(frame)


def send_break_uart(ser: serial.Serial, break_us: int = 176):
    """Отправляем BREAK через инверсию TX"""
    ser.break_condition = True
    time.sleep(break_us / 1_000_000)
    ser.break_condition = False
    time.sleep(0.008)  # MAB 8мкс


def send_dmx_frame(ser: serial.Serial, channels: list[int]):
    """Отправляем полный DMX кадр"""
    send_break_uart(ser)
    frame = build_dmx_frame(channels)
    ser.write(frame)
    ser.flush()


def read_udp_blob(ip: str, port: int = 5124) -> tuple[bytes, bytes]:
    """Читаем P0+P1 через UDP"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    sock.sendto(bytes([0x01]), (ip, port))
    data, _ = sock.recvfrom(1024)
    sock.close()
    return data[:512], data[512:]


def generate_test_pattern(frame_num: int, mode: str = "increment") -> list[int]:
    """Генерация тестового паттерна"""
    channels = [0] * 512

    if mode == "increment":
        # Каждый канал = свой уникальный цвет
        for i in range(0, 510, 3):
            fixture = i // 3
            channels[i]     = (fixture * 3) % 256      # R
            channels[i + 1] = (fixture * 7 + 50) % 256 # G
            channels[i + 2] = (fixture * 11 + 100) % 256 # B

    elif mode == "counter":
        # Канал 0 = frame_num % 256 (для трекинга)
        channels[0] = frame_num % 256
        # Канал 1 = FPS counter (для проверки скорости)
        channels[1] = 0  # заполняется позже
        # Остальные = фиксированный паттерн (не зависит от номера кадра)
        for i in range(2, 512):
            channels[i] = (i * 3 + 7) & 0xFF

    elif mode == "random":
        for i in range(512):
            channels[i] = random.randint(0, 255)

    elif mode == "chase":
        # Бегущий пиксель
        pos = frame_num % 170
        for i in range(170):
            idx = i * 3
            if i == pos:
                channels[idx] = 255
                channels[idx + 1] = 0
                channels[idx + 2] = 0
            elif i == (pos - 1) % 170:
                channels[idx] = 128
                channels[idx + 1] = 0
                channels[idx + 2] = 0
            elif i == (pos - 2) % 170:
                channels[idx] = 64
                channels[idx + 1] = 0
                channels[idx + 2] = 0
            else:
                channels[idx] = 0
                channels[idx + 1] = 0
                channels[idx + 2] = 0

    return channels


def run_stress_test(args):
    print(f"=== DMX RX Stress Test ===")
    print(f"Serial: {args.port} @ 250000 baud")
    print(f"UDP: {args.ip}:{args.udp_port}")
    print(f"Duration: {args.duration}s, FPS: {args.fps}")
    print(f"Pattern: {args.pattern}")
    print()

    # Открываем COM9 для TX
    try:
        ser = serial.Serial(
            port=args.port,
            baudrate=250000,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_TWO,
            timeout=0.1,
        )
        print(f"COM9 opened: {ser.name}")
    except serial.SerialException as e:
        print(f"ERROR: Cannot open {args.port}: {e}")
        sys.exit(1)

    time.sleep(0.5)

    # Статистика
    total_frames = 0
    total_errors = 0
    mismatch_channels = 0
    start_time = time.time()
    interval = 1.0 / args.fps

    print(f"Sending DMX at {args.fps} FPS...")
    print("-" * 60)

    try:
        # Предыдущий кадр для сравнения с учётом 1-frame задержки ESP
        prev_channels = None
        while True:
            elapsed = time.time() - start_time
            if elapsed >= args.duration:
                break

            frame_num = total_frames
            channels = generate_test_pattern(frame_num, args.pattern)

            # Отправляем DMX кадр
            send_dmx_frame(ser, channels)

            # Читаем обратно через UDP
            try:
                p0, p1 = read_udp_blob(args.ip, args.udp_port)

                # Сравниваем с ПРЕДЫДУЩИМ кадром (ESP возвращает кадр с задержкой ~1)
                errors = 0
                first_err = -1
                if prev_channels is not None:
                    for i in range(512):
                        if prev_channels[i] != p0[i]:
                            errors += 1
                            if first_err < 0:
                                first_err = i

                if errors > 0:
                    total_errors += errors
                    mismatch_channels += 1
                    if args.verbose:
                        print(f"  Frame {frame_num}: {errors} mismatches, first at ch{first_err} "
                              f"(prev={prev_channels[first_err]}, got={p0[first_err]})")
                else:
                    if frame_num % 100 == 0:
                        print(f"  Frame {frame_num}: OK (ch0-2: {p0[0]:3d},{p0[1]:3d},{p0[2]:3d})")

            except socket.timeout:
                print(f"  Frame {frame_num}: UDP TIMEOUT")
                total_errors += 512
            except Exception as e:
                print(f"  Frame {frame_num}: UDP ERROR: {e}")
                total_errors += 512

            total_frames += 1

            # Запоминаем для сравнения со следующим кадром
            prev_channels = channels[:]

            # Ждём до следующего кадра
            next_time = start_time + (frame_num + 1) * interval
            sleep_time = next_time - time.time()
            if sleep_time > 0:
                time.sleep(sleep_time)

    except KeyboardInterrupt:
        print("\nInterrupted by user")

    finally:
        ser.close()

    # Итоги
    duration = time.time() - start_time
    actual_fps = total_frames / duration if duration > 0 else 0

    print()
    print("=" * 60)
    print(f"RESULTS")
    print(f"  Frames sent:     {total_frames}")
    print(f"  Duration:        {duration:.1f}s")
    print(f"  Actual FPS:      {actual_fps:.1f}")
    print(f"  Errors total:    {total_errors} channels")
    print(f"  Frames w/errors: {mismatch_channels}")
    if total_frames > 0:
        accuracy = (1 - total_errors / (total_frames * 512)) * 100
        print(f"  Accuracy:        {accuracy:.4f}%")
    print("=" * 60)

    if total_errors == 0:
        print("PASS — all frames matched!")
        return 0
    else:
        print("FAIL — mismatches detected")
        return 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="DMX RX Stress Test")
    parser.add_argument("--port", default="COM9", help="Serial port (default: COM9)")
    parser.add_argument("--ip", default="192.168.1.100", help="ESP IP (default: 192.168.1.100)")
    parser.add_argument("--udp-port", type=int, default=5124, help="UDP port (default: 5124)")
    parser.add_argument("--duration", type=int, default=60, help="Test duration in seconds (default: 60)")
    parser.add_argument("--fps", type=int, default=30, help="Target FPS (default: 30)")
    parser.add_argument("--pattern", choices=["increment", "counter", "random", "chase"],
                       default="increment", help="Test pattern (default: increment)")
    parser.add_argument("--verbose", action="store_true", help="Print every error")
    args = parser.parse_args()

    sys.exit(run_stress_test(args))
