#!/usr/bin/env python3
"""
Serial test for DMX Sniffer via COM7.
Reads ESP32 logs, monitors DMX reception status.
"""
import serial
import time
import sys
import re

PORT = "COM7"
BAUD = 115200


def test_serial_log_monitor(duration=15):
    """Monitor serial output for DMX-related log messages."""
    print(f"Подключение к {PORT} @ {BAUD}...")
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    ser.reset_input_buffer()
    print(f"OK. Мониторинг {duration} сек...\n")

    start = time.time()
    frame_counts = []
    rmt_lines = []
    error_lines = []
    all_lines = []

    while time.time() - start < duration:
        if ser.in_waiting > 0:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if line:
                ts = time.time() - start
                all_lines.append((ts, line))

                if "RMT" in line or "rmt" in line:
                    rmt_lines.append((ts, line))
                    print(f"  [{ts:6.2f}s] RMT: {line}")
                elif "error" in line.lower() or "assert" in line.lower() or "panic" in line.lower():
                    error_lines.append((ts, line))
                    print(f"  [{ts:6.2f}s] ERR: {line}")
                elif "frame" in line.lower() or "dmx" in line.lower() or "break" in line.lower():
                    print(f"  [{ts:6.2f}s] DMX: {line}")

                m = re.search(r'frame_count[=: ]+(\d+)', line, re.IGNORECASE)
                if m:
                    frame_counts.append((ts, int(m.group(1))))
        else:
            time.sleep(0.01)

    ser.close()

    print(f"\n{'='*60}")
    print(f"ИТОГО за {duration}с:")
    print(f"  Всего строк: {len(all_lines)}")
    print(f"  RMT строк:   {len(rmt_lines)}")
    print(f"  Ошибок:      {len(error_lines)}")
    if frame_counts:
        fc0 = frame_counts[0][1]
        fc1 = frame_counts[-1][1]
        print(f"  Frame count: {fc0} -> {fc1} (delta={fc1-fc0})")

    print(f"\nВсе строки ({len(all_lines)}):")
    for ts, line in all_lines[:100]:
        print(f"  [{ts:6.2f}] {line}")
    if len(all_lines) > 100:
        print(f"  ... и ещё {len(all_lines)-100} строк")


def test_serial_send_command(cmd=b"\x01"):
    """Send a byte to serial and read response."""
    print(f"Отправка {cmd.hex()} на {PORT}...")
    ser = serial.Serial(PORT, BAUD, timeout=1.0)
    ser.reset_input_buffer()
    ser.write(cmd)
    time.sleep(0.5)
    resp = ser.read(ser.in_waiting)
    ser.close()
    print(f"Ответ ({len(resp)} байт): {resp[:64].hex()}")
    return resp


if __name__ == "__main__":
    duration = int(sys.argv[1]) if len(sys.argv) > 1 else 15
    test_serial_log_monitor(duration)
