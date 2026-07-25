"""
DMX RX Stress Test — проверка приёмника через COM9 TX + UDP readback
====================================================================

Схема:
  COM9 TX (FT2232) ──┬── GPIO15 (RX0)
                     └── GPIO16 (RX1)

Режимы:
  blob       — полное чтение (1024 байта, --duration секунд)
  checksum   — быстрое чтение (8 байт, --duration секунд)
  ck-test    — checksum ring buffer (900 кадров, анализ после)

Запуск:
  python dmx_stress_test.py --pattern counter --checksum
  python dmx_stress_test.py --ck-test --frames 900
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


def read_udp_blob(ip: str, port: int = 5124, sock: socket.socket = None) -> tuple[bytes, bytes]:
    """Читаем P0+P1 через UDP"""
    close = sock is None
    if sock is None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(0.05)
    # Drain stale responses
    while True:
        try:
            sock.recvfrom(1024)
        except socket.timeout:
            break
    sock.sendto(bytes([0x01]), (ip, port))
    data, _ = sock.recvfrom(1024)
    if close:
        sock.close()
    return data[:512], data[512:]


def read_udp_checksum(ip: str, port: int = 5124, sock: socket.socket = None) -> tuple[int, int, int, int]:
    """Читаем checksum через UDP: (p0_xor, p0_sum, p1_xor, p1_sum)"""
    close = sock is None
    if sock is None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(0.05)
    # Drain stale responses
    while True:
        try:
            sock.recvfrom(64)
        except socket.timeout:
            break
    sock.sendto(bytes([0x05]), (ip, port))
    data, _ = sock.recvfrom(64)
    if close:
        sock.close()
    p0_xor = (data[0] << 8) | data[1]
    p0_sum = (data[2] << 8) | data[3]
    p1_xor = (data[4] << 8) | data[5]
    p1_sum = (data[6] << 8) | data[7]
    return p0_xor, p0_sum, p1_xor, p1_sum


def compute_checksum(channels: list[int]) -> tuple[int, int]:
    """Вычислить checksum: (xor, sum)"""
    xor_val = 0
    sum_val = 0
    for ch in channels:
        xor_val ^= ch
        sum_val += ch
    return xor_val & 0xFFFF, sum_val & 0xFFFF


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


def run_checksum_test(args):
    """Тест checksums без WiFi off: отправляем DMX, читаем checksums через UDP"""
    total_frames = args.frames
    fps = args.fps

    print(f"=== DMX RX Checksum Test (WiFi ON) ===")
    print(f"Serial: {args.port} @ 250000 baud")
    print(f"UDP: {args.ip}:{args.udp_port}")
    print(f"Frames: {total_frames}, FPS: {fps}")
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

    # Ждём ESP
    print("Waiting for ESP...")
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.settimeout(2.0)
    for attempt in range(30):
        try:
            probe.sendto(bytes([0x05]), (args.ip, args.udp_port))
            probe.recvfrom(64)
            print(f"  ESP online after {attempt * 0.5:.1f}s")
            break
        except (socket.timeout, OSError):
            pass
        time.sleep(0.5)
    else:
        print("  ERROR: ESP not reachable!")
        probe.close()
        return 1
    probe.close()

    # Отправляем DMX кадры + считаем checksums на PC
    print(f"Sending {total_frames} DMX frames...")
    interval = 1.0 / fps
    start_time = time.time()
    pc_checksums = []

    for frame_num in range(total_frames):
        channels = generate_test_pattern(frame_num, args.pattern)
        send_dmx_frame(ser, channels)

        xor_val = 0
        sum_val = 0
        for ch in channels:
            xor_val ^= ch
            sum_val += ch
        pc_checksums.append((xor_val & 0xFFFF, sum_val & 0xFFFF))

        if frame_num % 100 == 0:
            elapsed = time.time() - start_time
            print(f"  Frame {frame_num}/{total_frames} ({elapsed:.1f}s)")

        next_time = start_time + (frame_num + 1) * interval
        sleep_time = next_time - time.time()
        if sleep_time > 0:
            time.sleep(sleep_time)

    ser.close()
    send_time = time.time() - start_time
    print(f"  Done: {total_frames} frames in {send_time:.1f}s ({total_frames/send_time:.1f} FPS)")

    # Даём ESP время обработать последние кадры
    time.sleep(0.5)

    # Читаем checksum report (0x06)
    print("Reading checksum report from ESP...")
    data_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    data_sock.settimeout(5.0)
    data_sock.sendto(bytes([0x06]), (args.ip, args.udp_port))
    resp, _ = data_sock.recvfrom(16384)
    data_sock.close()

    p0_count = struct.unpack(">I", resp[0:4])[0]
    p1_count = struct.unpack(">I", resp[4:8])[0]
    print(f"  ESP reports: P0={p0_count} frames, P1={p1_count} frames")

    offset = 8
    esp_ck_p0 = []
    esp_len_p0 = []
    for i in range(p0_count):
        x = (resp[offset] << 8) | resp[offset + 1]
        s = (resp[offset + 2] << 8) | resp[offset + 3]
        fl = (resp[offset + 4] << 8) | resp[offset + 5]
        esp_ck_p0.append((x, s))
        esp_len_p0.append(fl)
        offset += 6

    esp_ck_p1 = []
    esp_len_p1 = []
    for i in range(p1_count):
        x = (resp[offset] << 8) | resp[offset + 1]
        s = (resp[offset + 2] << 8) | resp[offset + 3]
        fl = (resp[offset + 4] << 8) | resp[offset + 5]
        esp_ck_p1.append((x, s))
        esp_len_p1.append(fl)
        offset += 6

    # Анализ: каждый ESP checksum ищем среди всех PC checksums
    print("Analyzing...")
    print("-" * 60)

    pc_ck_set = set(pc_checksums)

    valid_p0 = sum(1 for ck in esp_ck_p0 if ck in pc_ck_set)
    valid_p1 = sum(1 for ck in esp_ck_p1 if ck in pc_ck_set)

    # Статистика длин кадров
    len_dist_p0 = {}
    for fl in esp_len_p0:
        len_dist_p0[fl] = len_dist_p0.get(fl, 0) + 1

    # Только кадры с полной длиной (513 = start code + 512 channels)
    full_frames_p0 = [(ck, fl) for ck, fl in zip(esp_ck_p0, esp_len_p0) if fl == 513]
    full_frames_p1 = [(ck, fl) for ck, fl in zip(esp_ck_p1, esp_len_p1) if fl == 513]
    valid_full_p0 = sum(1 for ck, fl in full_frames_p0 if ck in pc_ck_set)
    valid_full_p1 = sum(1 for ck, fl in full_frames_p1 if ck in pc_ck_set)

    # Debug
    print(f"  PC first 3:  {pc_checksums[0]}, {pc_checksums[1]}, {pc_checksums[2]}")
    print(f"  PC last 3:   {pc_checksums[-3]}, {pc_checksums[-2]}, {pc_checksums[-1]}")
    if p0_count > 0:
        print(f"  ESP P0 first: {esp_ck_p0[0]}, last: {esp_ck_p0[-1]}")
    if p1_count > 0:
        print(f"  ESP P1 first: {esp_ck_p1[0]}, last: {esp_ck_p1[-1]}")

    print()
    print(f"  Frame length distribution (P0):")
    for fl in sorted(len_dist_p0.keys()):
        print(f"    len={fl}: {len_dist_p0[fl]} frames")

    # Поиск best offset ( brute-force )
    best_p0_err = p0_count
    best_p0_off = 0
    for off in range(max(0, len(pc_checksums) - p0_count - 50), min(len(pc_checksums), p0_count + 50)):
        n = min(p0_count, len(pc_checksums) - off)
        if n <= 0:
            continue
        errs = sum(1 for i in range(n) if pc_checksums[off + i] != esp_ck_p0[i])
        if errs < best_p0_err:
            best_p0_err = errs
            best_p0_off = off
            if errs == 0:
                break

    print()
    print("=" * 60)
    print(f"RESULTS (WiFi-ON checksum test)")
    print(f"  Frames sent:       {total_frames}")
    print(f"  P0 frames in buf:  {p0_count} ({total_frames - p0_count} not stored)")
    print(f"  P1 frames in buf:  {p1_count} ({total_frames - p1_count} not stored)")
    print()
    print(f"  ALL frames (any length):")
    print(f"  P0 matched:        {valid_p0}/{p0_count} ({valid_p0/p0_count*100:.1f}%)" if p0_count > 0 else "  P0: no data")
    print(f"  P1 matched:        {valid_p1}/{p1_count} ({valid_p1/p1_count*100:.1f}%)" if p1_count > 0 else "  P1: no data")
    print()
    print(f"  Full frames only (len=513):")
    print(f"  P0 full frames:    {len(full_frames_p0)}/{p0_count}")
    print(f"  P0 full matched:   {valid_full_p0}/{len(full_frames_p0)} ({valid_full_p0/len(full_frames_p0)*100:.1f}%)" if full_frames_p0 else "  P0: no full frames")
    print(f"  P1 full frames:    {len(full_frames_p1)}/{p1_count}")
    print(f"  P1 full matched:   {valid_full_p1}/{len(full_frames_p1)} ({valid_full_p1/len(full_frames_p1)*100:.1f}%)" if full_frames_p1 else "  P1: no full frames")
    print()
    print(f"  Best contiguous offset (P0):")
    print(f"  Offset:            {best_p0_off} frames")
    print(f"  Errors at offset:  {best_p0_err}/{min(p0_count, len(pc_checksums) - best_p0_off)}" if p0_count > 0 else "  N/A")
    n_compare = min(p0_count, len(pc_checksums) - best_p0_off)
    if n_compare > 0:
        print(f"  Accuracy:          {(1 - best_p0_err / n_compare) * 100:.4f}%")
    print("=" * 60)

    if p0_count > 0 and valid_p0 == p0_count and valid_p1 == p1_count:
        print("PASS — all received frames have valid checksums!")
        return 0
    else:
        print("RESULT — see analysis above")
        return 1


def run_stress_test(args):
    print(f"=== DMX RX Stress Test ===")
    print(f"Serial: {args.port} @ 250000 baud")
    print(f"UDP: {args.ip}:{args.udp_port}")
    print(f"Duration: {args.duration}s, FPS: {args.fps}")
    print(f"Pattern: {args.pattern}")
    print(f"Mode: {'checksum' if args.checksum else 'blob'}")
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
    timeout_count = 0
    start_time = time.time()
    interval = 1.0 / args.fps

    print(f"Sending DMX at {args.fps} FPS...")
    print("-" * 60)

    # Persistent UDP socket — не пересоздаём каждый кадр
    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.settimeout(0.05)

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
                if args.checksum:
                    # Режим checksum — быстрый, 8 байт
                    got_p0_xor, got_p0_sum, got_p1_xor, got_p1_sum = read_udp_checksum(args.ip, args.udp_port, udp_sock)
                    if prev_channels is not None:
                        exp_xor, exp_sum = compute_checksum(prev_channels)
                        errors = 0
                        if got_p0_xor != exp_xor or got_p0_sum != exp_sum:
                            errors = 1
                        if errors > 0:
                            total_errors += errors
                            mismatch_channels += 1
                            if args.verbose:
                                print(f"  Frame {frame_num}: CKSUM mismatch "
                                      f"(exp_xor={exp_xor:#06x} exp_sum={exp_sum:#06x} "
                                      f"got_xor={got_p0_xor:#06x} got_sum={got_p0_sum:#06x})")
                        else:
                            if frame_num % 100 == 0:
                                print(f"  Frame {frame_num}: OK (xor={got_p0_xor:#06x} sum={got_p0_sum:#06x})")
                    else:
                        if frame_num % 100 == 0:
                            print(f"  Frame {frame_num}: OK (first frame, no prev)")
                else:
                    # Режим blob — полный кадр, 1024 байта
                    p0, p1 = read_udp_blob(args.ip, args.udp_port, udp_sock)

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
                timeout_count += 1
            except Exception as e:
                timeout_count += 1

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
        udp_sock.close()

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
    print(f"  UDP timeouts:   {timeout_count}")
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
    parser.add_argument("--checksum", action="store_true",
                       help="Use fast checksum mode (8 bytes instead of 1024)")
    parser.add_argument("--ck-test", action="store_true",
                       help="Checksum test: send DMX, read checksum ring buffer after")
    parser.add_argument("--frames", type=int, default=900,
                       help="Number of frames for ck-test (default: 900)")
    args = parser.parse_args()

    if args.ck_test:
        sys.exit(run_checksum_test(args))
    else:
        sys.exit(run_stress_test(args))
