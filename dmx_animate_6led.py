"""
DMX Animation Test для 6 LED через COM5 -> MAX485 -> ESP32

Протокол:
  - 250000 бод, 8N2
  - Break >= 88 мкс (break_condition=True)
  - Start code = 0x00
  - 6 приборов × 3 канала (RGB) = каналы 1-18

Использование:
  python dmx_animate_6led.py [режим]

Режимы: rainbow, breathing, chase, fade, static, off
"""

import serial
import time
import math
import struct
import socket
import sys

# ============================================================
# НАСТРОЙКИ
# ============================================================

DMX_PORT = "COM5"
DMX_BAUD = 250000
NUM_FIXTURES = 6
CH_PER_FIXTURE = 3  # RGB
BASE_ADDR = 1       # DMX адрес первого прибора
NUM_CHANNELS = NUM_FIXTURES * CH_PER_FIXTURE  # 18

# Debug TCP (ESP32)
ESP32_IP = "192.168.4.1"   # AP mode IP
ESP32_PORT = 5555
DEBUG_ENABLED = True

FPS = 30
FRAME_INTERVAL = 1.0 / FPS


# ============================================================
# DMX ОТПРАВКА
# ============================================================

def send_dmx(ser, channels):
    """Отправить DMX кадр с break_condition"""
    frame = bytes(channels)
    ser.break_condition = True
    time.sleep(0.0001)  # 100 мкс break
    ser.break_condition = False
    time.sleep(0.00001)  # 10 мкс MAB
    ser.write(frame)
    ser.flush()


def channels_from_rgb_list(rgb_list):
    """rgb_list = [(r,g,b), (r,g,b), ...] -> bytes[18] с BASE_ADDR=1"""
    buf = [0] * NUM_CHANNELS
    for i, (r, g, b) in enumerate(rgb_list):
        if i >= NUM_FIXTURES:
            break
        idx = i * CH_PER_FIXTURE
        buf[idx]     = max(0, min(255, r))
        buf[idx + 1] = max(0, min(255, g))
        buf[idx + 2] = max(0, min(255, b))
    return buf


# ============================================================
# DEBUG TCP
# ============================================================

def read_debug(sock):
    """Прочитать отладочные данные: cnt0(4) + cnt1(4) + p0[512] + p1[512] = 1032 байт"""
    try:
        sock.settimeout(0.05)
        data = sock.recv(1032)
        if len(data) == 1032:
            cnt0 = struct.unpack_from('<I', data, 0)[0]
            cnt1 = struct.unpack_from('<I', data, 4)[0]
            return cnt0, cnt1
    except:
        pass
    return None, None


def connect_debug():
    """Подключиться к debug TCP серверу ESP32"""
    if not DEBUG_ENABLED:
        return None
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(2)
        sock.connect((ESP32_IP, ESP32_PORT))
        sock.send(b'\x01')  # команда: отправить данные
        return sock
    except Exception as e:
        print(f"[WARN] Debug TCP не доступен: {e}")
        return None


# ============================================================
# АНИМАЦИИ
# ============================================================

def hsv_to_rgb(h, s, v):
    """HSV (h=0..360, s=0..1, v=0..1) -> (r,g,b) 0..255"""
    h = h % 360
    c = v * s
    x = c * (1 - abs((h / 60) % 2 - 1))
    m = v - c
    if h < 60:    r, g, b = c, x, 0
    elif h < 120: r, g, b = x, c, 0
    elif h < 180: r, g, b = 0, c, x
    elif h < 240: r, g, b = 0, x, c
    elif h < 300: r, g, b = x, 0, c
    else:         r, g, b = c, 0, x
    return (int((r + m) * 255), int((g + m) * 255), int((b + m) * 255))


def anim_rainbow(t):
    """Плавная радуга по 6 LED"""
    colors = []
    for i in range(NUM_FIXTURES):
        hue = (t * 120 + i * 60) % 360  # 120 град/сек, 60 град сдвиг
        colors.append(hsv_to_rgb(hue, 1.0, 1.0))
    return colors


def anim_breathing(t):
    """Дыхание: плавное увеличение/уменьшение яркости"""
    # Период 2 сек
    brightness = (math.sin(t * math.pi) + 1) / 2  # 0..1
    val = int(brightness * 255)
    return [(val, val, val)] * NUM_FIXTURES


def anim_chase(t):
    """Бегущий огонь: один яркий LED движется"""
    colors = [(0, 0, 0)] * NUM_FIXTURES
    pos = (t * 2.0) % NUM_FIXTURES  # 2 LED/сек
    idx = int(pos)
    frac = pos - idx
    for i in range(NUM_FIXTURES):
        dist = abs(i - pos)
        dist = min(dist, NUM_FIXTURES - dist)
        brightness = max(0, 1.0 - dist * 0.4)
        colors[i] = (int(brightness * 255), int(brightness * 80), 0)
    return colors


def anim_fade(t):
    """Плавная смена цветов: красный -> зелёный -> синий -> красный"""
    colors = []
    for i in range(NUM_FIXTURES):
        hue = (t * 60 + i * 30) % 360
        colors.append(hsv_to_rgb(hue, 1.0, 0.8))
    return colors


def anim_static(t):
    """Статические проверочные цвета:
       LED0=красный, LED1=зелёный, LED2=синий,
       LED3=жёлтый, LED4=голубой, LED5=белый"""
    test_colors = [
        (255, 0, 0),
        (0, 255, 0),
        (0, 0, 255),
        (255, 255, 0),
        (0, 255, 255),
        (255, 255, 255),
    ]
    return test_colors[:NUM_FIXTURES]


def anim_off(t):
    """Все LED выключены"""
    return [(0, 0, 0)] * NUM_FIXTURES


ANIMATIONS = {
    "rainbow":  anim_rainbow,
    "breathing": anim_breathing,
    "chase":    anim_chase,
    "fade":     anim_fade,
    "static":   anim_static,
    "off":      anim_off,
}


# ============================================================
# ГЛАВНЫЙ ЦИКЛ
# ============================================================

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "rainbow"

    if mode not in ANIMATIONS:
        print(f"Неизвестный режим: {mode}")
        print(f"Доступные: {', '.join(ANIMATIONS.keys())}")
        sys.exit(1)

    anim_func = ANIMATIONS[mode]

    print(f"=== DMX Animation Test ===")
    print(f"Режим:     {mode}")
    print(f"COM порт:  {DMX_PORT}")
    print(f"Приборов:  {NUM_FIXTURES}")
    print(f"Каналов:   {NUM_CHANNELS} (адреса {BASE_ADDR}-{BASE_ADDR + NUM_CHANNELS - 1})")
    print(f"FPS:       {FPS}")
    print(f"Нажмите Ctrl+C для остановки")
    print()

    ser = serial.Serial(DMX_PORT, DMX_BAUD, timeout=0.1)
    time.sleep(0.1)

    sock = connect_debug()

    frame_count = 0
    start_time = time.time()
    last_debug_send = 0

    try:
        while True:
            t = time.time() - start_time
            colors = anim_func(t)
            channels = channels_from_rgb_list(colors)
            send_dmx(ser, channels)
            frame_count += 1

            # Периодически отправляем команду debug и читаем ответ
            if sock and (time.time() - last_debug_send) > 0.5:
                try:
                    sock.send(b'\x01')
                    cnt0, cnt1 = read_debug(sock)
                    if cnt0 is not None:
                        sys.stdout.write(f"\r  Frames TX: {frame_count}  |  RX0: {cnt0}  RX1: {cnt1}  ")
                        sys.stdout.flush()
                except:
                    pass
                last_debug_send = time.time()

            # Поддержка FPS
            elapsed = time.time() - (start_time + t)
            if elapsed < FRAME_INTERVAL:
                time.sleep(FRAME_INTERVAL - elapsed)

    except KeyboardInterrupt:
        print(f"\n\nОстановлено. Отправлено кадров: {frame_count}")

    finally:
        # Выключить все LED перед выходом
        channels = channels_from_rgb_list([(0, 0, 0)] * NUM_FIXTURES)
        send_dmx(ser, channels)
        time.sleep(0.05)
        ser.close()
        if sock:
            sock.close()
        print("LED выключены, порт закрыт.")


if __name__ == "__main__":
    main()
