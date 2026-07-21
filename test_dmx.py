#!/usr/bin/env python3
"""
Тест RMT RX DMX-приёмника ESP32.
Подключается к ESP32 по TCP:5555 (debug server) и/или HTTP:80.
Выводит DMX-данные, диагностику, проверяет стабильность приёма.
"""

import socket
import struct
import time
import sys
import argparse

DEFAULT_HOST = "192.168.4.1"  # AP mode: Fountain_Config
DEFAULT_HTTP_HOST = "192.168.4.1"
DEBUG_PORT = 5555
HTTP_PORT = 80


def query_debug(host: str, timeout: float = 2.0) -> dict | None:
    """Отправить запрос debug-серверу, получить 1088 байт (64 header + 1024 DMX)."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect((host, DEBUG_PORT))
        s.sendall(b"\x01")
        data = b""
        while len(data) < 1088:
            chunk = s.recv(1088 - len(data))
            if not chunk:
                break
            data += chunk
        s.close()
        if len(data) < 64:
            return None

        fc0 = struct.unpack_from("<I", data, 0)[0]
        fc1 = struct.unpack_from("<I", data, 4)[0]
        isr0 = struct.unpack_from("<I", data, 8)[0]
        isr1 = struct.unpack_from("<I", data, 12)[0]
        brk0 = struct.unpack_from("<I", data, 16)[0]
        brk1 = struct.unpack_from("<I", data, 20)[0]
        err0 = struct.unpack_from("<I", data, 24)[0]
        err1 = struct.unpack_from("<I", data, 28)[0]
        sym0 = struct.unpack_from("<I", data, 32)[0]
        sym1 = struct.unpack_from("<I", data, 36)[0]
        nsym = struct.unpack_from("<I", data, 40)[0]
        ilst = struct.unpack_from("<I", data, 44)[0]
        symNm1 = struct.unpack_from("<I", data, 48)[0]
        symN = struct.unpack_from("<I", data, 52)[0]
        brk_found = struct.unpack_from("<I", data, 56)[0]
        brk_idx = struct.unpack_from("<I", data, 60)[0]
        dmx0 = list(data[64:576])
        dmx1 = list(data[576:1088])

        return dict(
            frame_count0=fc0, frame_count1=fc1,
            isr_count0=isr0, isr_count1=isr1,
            break_count0=brk0, break_count1=brk1,
            err_count0=err0, err_count1=err1,
            sym0_raw=sym0, sym1_raw=sym1,
            symNm1_raw=symNm1, symN_raw=symN,
            num_symbols=nsym, is_last=ilst,
            break_found=brk_found, break_idx=brk_idx,
            dmx0=dmx0, dmx1=dmx1,
        )
    except Exception as e:
        print(f"  Ошибка TCP: {e}")
        return None


def query_http_status(host: str) -> str | None:
    """GET /api/status — JSON."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect((host, HTTP_PORT))
        s.sendall(b"GET /api/status HTTP/1.0\r\nHost: " + host.encode() + b"\r\n\r\n")
        data = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        s.close()
        text = data.decode("utf-8", errors="replace")
        body_start = text.find("\r\n\r\n")
        if body_start >= 0:
            return text[body_start + 4:]
        return text
    except Exception as e:
        print(f"  Ошибка HTTP: {e}")
        return None


def query_http_channels(host: str) -> bytes | None:
    """GET /api/channels — 1024 байт raw DMX."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect((host, HTTP_PORT))
        s.sendall(b"GET /api/channels HTTP/1.0\r\nHost: " + host.encode() + b"\r\n\r\n")
        data = b""
        while True:
            chunk = s.recv(8192)
            if not chunk:
                break
            data += chunk
        s.close()
        idx = data.find(b"\r\n\r\n")
        if idx >= 0:
            return data[idx + 4:]
        return data
    except Exception as e:
        print(f"  Ошибка HTTP: {e}")
        return None


def format_dmx_line(channels: list, start: int = 1, count: int = 20) -> str:
    """Форматировать каналы DMX для вывода: [1] 0xFF [2] 0x00 ..."""
    parts = []
    for i in range(start - 1, min(start - 1 + count, len(channels))):
        parts.append(f"[{i+1}]0x{channels[i]:02X}")
    return " ".join(parts)


def test_single_read(host: str):
    """Одиночное чтение — базовая проверка связи."""
    print("=" * 60)
    print("ТЕСТ 1: Одиночное чтение")
    print("=" * 60)

    print(f"\nTCP debug ({host}:{DEBUG_PORT})...")
    r = query_debug(host)
    if r:
        print(f"  Frame count P0={r['frame_count0']}  P1={r['frame_count1']}")
        print(f"  ISR count   P0={r['isr_count0']}  P1={r['isr_count1']}")
        print(f"  Break count P0={r['break_count0']}  P1={r['break_count1']}")
        print(f"  Err count   P0={r['err_count0']}  P1={r['err_count1']}")
        # Decode RMT symbols from last callback (port 0)
        s0 = r['sym0_raw']
        s1 = r['sym1_raw']
        sNm1 = r['symNm1_raw']
        sN = r['symN_raw']
        lvl0 = (s0 >> 15) & 1
        dur0 = s0 & 0x7FFF
        lvl1 = (s1 >> 15) & 1
        dur1 = s1 & 0x7FFF
        lvlNm1 = (sNm1 >> 15) & 1
        durNm1 = sNm1 & 0x7FFF
        lvlN = (sN >> 15) & 1
        durN = sN & 0x7FFF
        ilast = "idle" if r['is_last'] else "buf_full"
        print(f"  Last callback: num_sym={r['num_symbols']} type={ilast}")
        print(f"  sym0: lvl={lvl0} dur={dur0}  sym1: lvl={lvl1} dur={dur1}")
        print(f"  symN-1: lvl={lvlNm1} dur={durNm1}  symN: lvl={lvlN} dur={durN}")
        # BREAK search results
        brk_found = r['break_found']
        brk_idx = r['break_idx']
        if brk_found:
            print(f"  *** BREAK FOUND in buffer at item index {brk_idx} ***")
        else:
            print(f"  No BREAK in buffer (scanned {brk_idx} items)")
        nonzero_p0 = sum(1 for v in r['dmx0'] if v != 0)
        nonzero_p1 = sum(1 for v in r['dmx1'] if v != 0)
        print(f"  Non-zero channels: P0={nonzero_p0}  P1={nonzero_p1}")
        if nonzero_p0 > 0:
            print(f"  Port0 DMX (first 20): {format_dmx_line(r['dmx0'], 1, 20)}")
        else:
            print("  Port0: all zeros (нет сигнала или нет данных)")
        if nonzero_p1 > 0:
            print(f"  Port1 DMX (first 20): {format_dmx_line(r['dmx1'], 1, 20)}")
    else:
        print("  НЕТ ОТВЕТА")

    print(f"\nHTTP status ({host}:{HTTP_PORT})...")
    status = query_http_status(host)
    if status:
        print(f"  {status[:300]}")
    else:
        print("  НЕТ ОТВЕТА")

    print(f"\nHTTP channels ({host}:{HTTP_PORT})...")
    raw = query_http_channels(host)
    if raw and len(raw) >= 512:
        print(f"  Received {len(raw)} bytes")
        dmx0 = list(raw[:512])
        nonzero = sum(1 for v in dmx0 if v != 0)
        print(f"  Port0 non-zero: {nonzero}")
        if nonzero > 0:
            print(f"  Port0 DMX: {format_dmx_line(dmx0, 1, 20)}")
    else:
        print(f"  НЕТ ОТВЕТА (got {len(raw) if raw else 0} bytes)")

    print()


def test_repeated_read(host: str, count: int = 20, interval: float = 0.1):
    """Многократное чтение — проверка стабильности и frame_count."""
    print("=" * 60)
    print(f"ТЕСТ 2: Многократное чтение ({count} раз, интервал {interval*1000:.0f}мс)")
    print("=" * 60)

    prev_fc0 = None
    prev_isr0 = None
    frame_updates = 0
    errors = 0

    for i in range(count):
        r = query_debug(host, timeout=1.0)
        if r is None:
            errors += 1
            print(f"  [{i+1:3d}] ERROR")
            time.sleep(interval)
            continue

        fc0 = r['frame_count0']
        brk0 = r['break_count0']
        isr0 = r['isr_count0']
        nonzero0 = sum(1 for v in r['dmx0'] if v != 0)

        marker = ""
        if prev_fc0 is not None and fc0 != prev_fc0:
            marker = " <-- NEW FRAME"
            frame_updates += 1
        prev_fc0 = fc0
        prev_isr0 = isr0

        ch_preview = format_dmx_line(r['dmx0'], 1, 8)
        print(f"  [{i+1:3d}] FC={fc0:6d} BRK={brk0:6d} ISR={isr0:8d} ERR={r['err_count0']:5d} NZ={nonzero0:3d}  {ch_preview}{marker}")

        time.sleep(interval)

    print(f"\n  Frame updates: {frame_updates}/{count}")
    print(f"  Errors: {errors}/{count}")
    print()


def test_delta_analysis(host: str, count: int = 50, interval: float = 0.05):
    """Анализ deltas — стабильность frame_count, наличие данных."""
    print("=" * 60)
    print(f"ТЕСТ 3: Анализ delta ({count} чтений)")
    print("=" * 60)

    results = []
    for i in range(count):
        r = query_debug(host, timeout=1.0)
        if r:
            results.append(r)
        time.sleep(interval)

    if not results:
        print("  Нет данных!")
        return

    fc0_vals = [r['frame_count0'] for r in results]
    isr0_vals = [r['isr_count0'] for r in results]
    brk0_vals = [r['break_count0'] for r in results]
    nz0_vals = [sum(1 for v in r['dmx0'] if v != 0) for r in results]

    fc0_delta = fc0_vals[-1] - fc0_vals[0]
    isr0_delta = isr0_vals[-1] - isr0_vals[0]
    brk0_delta = brk0_vals[-1] - brk0_vals[0]

    print(f"  Frame count:  start={fc0_vals[0]}  end={fc0_vals[-1]}  delta={fc0_delta}")
    print(f"  Break count:  start={brk0_vals[0]}  end={brk0_vals[-1]}  delta={brk0_delta}")
    print(f"  ISR count:    start={isr0_vals[0]}  end={isr0_vals[-1]}  delta={isr0_delta}")
    print(f"  Non-zero ch:  min={min(nz0_vals)}  max={max(nz0_vals)}  avg={sum(nz0_vals)/len(nz0_vals):.1f}")

    if fc0_delta == 0:
        print("  ВНИМАНИЕ: frame_count не растёт — нет DMX сигнала или декодер не работает!")
    else:
        print(f"  Скорость: ~{fc0_delta / (count * interval):.1f} fps")

    # Check for data consistency across frames
    if len(results) >= 2:
        same = sum(1 for i in range(1, len(results)) if results[i]['dmx0'] == results[i-1]['dmx0'])
        print(f"  Стабильность данных: {same}/{len(results)-1} одинаковых подряд")

    print()


def test_channel_patterns(host: str):
    """Проверка данных по каналам — если передатчик шлёт паттерн."""
    print("=" * 60)
    print("ТЕСТ 4: Анализ каналов")
    print("=" * 60)

    r = query_debug(host)
    if r is None:
        print("  Нет данных!")
        return

    dmx0 = r['dmx0']
    nonzero = [(i+1, v) for i, v in enumerate(dmx0) if v != 0]

    if not nonzero:
        print("  Port0: нет активных каналов (все нули)")
        print("  Убедитесь что передатчик подключён к GPIO15")
    else:
        print(f"  Port0: {len(nonzero)} активных каналов")
        print(f"  Первые 50 каналов:")
        for row in range(5):
            line = []
            for col in range(10):
                ch = row * 10 + col + 1
                if ch <= 512:
                    val = dmx0[ch - 1]
                    line.append(f"{ch:3d}=0x{val:02X}")
            print(f"    {'  '.join(line)}")

        # Find max channel
        max_ch = max(c for c, v in nonzero)
        print(f"  Максимальный активный канал: {max_ch}")

    print()


def test_persistence(host: str, duration: float = 10.0, interval: float = 0.5):
    """Длительный тест — мониторинг frame_count и ошибок."""
    print("=" * 60)
    print(f"ТЕСТ 5: Непрерывный мониторинг ({duration:.0f} сек)")
    print("=" * 60)

    start = time.time()
    prev_fc0 = None
    frames = 0
    errors = 0
    max_gap_ms = 0
    last_frame_time = None

    while time.time() - start < duration:
        t = time.time()
        r = query_debug(host, timeout=1.0)

        if r is None:
            errors += 1
            print(f"  [{t - start:5.1f}s] ERROR")
            time.sleep(interval)
            continue

        fc0 = r['frame_count0']
        nonzero0 = sum(1 for v in r['dmx0'] if v != 0)

        if prev_fc0 is not None and fc0 != prev_fc0:
            frames += 1
            if last_frame_time is not None:
                gap_ms = (t - last_frame_time) * 1000
                if gap_ms > max_gap_ms:
                    max_gap_ms = gap_ms
            last_frame_time = t

        prev_fc0 = fc0
        print(f"  [{t - start:5.1f}s] FC={fc0:6d} NZ={nonzero0:3d}")
        time.sleep(interval)

    elapsed = time.time() - start
    fps = frames / elapsed if elapsed > 0 else 0
    print(f"\n  Время: {elapsed:.1f}с  Кадров: {frames}  FPS: {fps:.1f}")
    print(f"  Ошибок: {errors}  Макс. пауза между кадрами: {max_gap_ms:.0f}мс")
    print()


def test_http_full(host: str):
    """Полный HTTP тест."""
    print("=" * 60)
    print("ТЕСТ 6: HTTP API полный")
    print("=" * 60)

    print("\n/api/status:")
    s = query_http_status(host)
    if s:
        # Parse key fields manually
        for line in s.strip().split("\n"):
            print(f"  {line}")
    else:
        print("  НЕТ ОТВЕТА")

    print("\n/api/channels (первые 32 канала порта 0):")
    raw = query_http_channels(host)
    if raw and len(raw) >= 512:
        dmx0 = list(raw[:512])
        for i in range(0, 32, 8):
            chunk = [f"{dmx0[j]:02X}" for j in range(i, min(i+8, 512))]
            labels = [f"{j+1:3d}" for j in range(i, min(i+8, 512))]
            print(f"    кан: {' '.join(labels)}")
            print(f"    знач: {' '.join(chunk)}")
    else:
        print("  НЕТ ОТВЕТА")

    print()


def main():
    parser = argparse.ArgumentParser(description="Тест DMX RMT RX")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"IP ESP32 (default: {DEFAULT_HOST})")
    parser.add_argument("--test", choices=["1", "2", "3", "4", "5", "6", "all"], default="all",
                        help="Номер теста (1-6) или 'all'")
    parser.add_argument("--count", type=int, default=50, help="Количество итераций для тестов 2/3")
    parser.add_argument("--duration", type=float, default=10.0, help="Длительность теста 5 (сек)")
    args = parser.parse_args()

    print(f"DMX RMT RX Test  |  Target: {args.host}")
    print(f"{'='*60}\n")

    tests = {
        "1": lambda: test_single_read(args.host),
        "2": lambda: test_repeated_read(args.host, count=args.count),
        "3": lambda: test_delta_analysis(args.host, count=args.count),
        "4": lambda: test_channel_patterns(args.host),
        "5": lambda: test_persistence(args.host, duration=args.duration),
        "6": lambda: test_http_full(args.host),
    }

    if args.test == "all":
        for key in ["1", "2", "3", "4", "5", "6"]:
            tests[key]()
    else:
        tests[args.test]()


if __name__ == "__main__":
    main()
