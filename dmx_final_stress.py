"""
DMX Final Stress Test
- 16-bit frame_id in channels 0-1 for clean P0-vs-P1 matching
- 510 channels compared per frame (channels 2-511)
"""
import socket, serial, time, random, sys

ESP32_IP = "192.168.1.222"
ESP32_PORT = 5555
DMX_CH = 512
HDR = 24
RSP = HDR + DMX_CH * 2
TOTAL = 1000


def recv_all(timeout=2.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect((ESP32_IP, ESP32_PORT))
        s.send(b"\x01")
        data = b""
        while len(data) < RSP:
            c = s.recv(RSP - len(data))
            if not c:
                break
            data += c
    except:
        s.close()
        return None, None, None, None, 0, 0, 0, 0
    s.close()
    if len(data) >= RSP:
        fc0 = int.from_bytes(data[0:4], 'little')
        fc1 = int.from_bytes(data[4:8], 'little')
        isr0 = int.from_bytes(data[8:12], 'little')
        isr1 = int.from_bytes(data[12:16], 'little')
        bk0 = int.from_bytes(data[16:20], 'little')
        bk1 = int.from_bytes(data[20:24], 'little')
        p0 = list(data[HDR:HDR + DMX_CH])
        p1 = list(data[HDR + DMX_CH:HDR + DMX_CH * 2])
        return fc0, fc1, p0, p1, isr0, isr1, bk0, bk1
    return None, None, None, None, 0, 0, 0, 0


def send_dmx(ser, channels):
    ser.break_condition = True
    time.sleep(0.0001)
    ser.break_condition = False
    time.sleep(0.000012)
    ser.write(bytes([0x00]) + bytes(channels[:DMX_CH]))


def make_pattern(name, frame_id):
    """frame_id in channels 0-1 (big-endian), pattern in channels 2-511"""
    ch = [0] * DMX_CH
    ch[0] = (frame_id >> 8) & 0xFF
    ch[1] = frame_id & 0xFF
    n = DMX_CH - 2  # 510 data channels
    if name == "random":
        for i in range(n):
            ch[i + 2] = random.randint(0, 255)
    elif name == "gradient":
        for i in range(n):
            ch[i + 2] = int(i * 255 / (n - 1))
    elif name == "solid_0":
        pass  # already 0
    elif name == "solid_128":
        for i in range(n):
            ch[i + 2] = 128
    elif name == "solid_255":
        for i in range(n):
            ch[i + 2] = 255
    elif name == "walking":
        ch[2 + (frame_id % n)] = 255
    elif name == "alternating":
        for i in range(n):
            ch[i + 2] = 0 if i % 2 == 0 else 255
    elif name == "blocks_16":
        for i in range(n):
            ch[i + 2] = ((i // 16) % 2) * 255
    elif name == "blocks_64":
        for i in range(n):
            ch[i + 2] = ((i // 64) % 2) * 255
    elif name == "blocks_128":
        for i in range(n):
            ch[i + 2] = ((i // 128) % 2) * 255
    return ch


def extract_id(buf):
    return (buf[0] << 8) | buf[1]


def compare_data(a, b):
    id_a, id_b = extract_id(a), extract_id(b)
    if id_a != id_b:
        return None
    mism = sum(1 for i in range(2, DMX_CH) if a[i] != b[i])
    return mism, id_a


def main():
    esp_ip = sys.argv[1] if len(sys.argv) > 1 else ESP32_IP
    n = int(sys.argv[2]) if len(sys.argv) > 2 else TOTAL

    print(f"=== DMX STRESS TEST (frame_id matching) ===")
    print(f"Both UARTs on GPIO4 (COM7) via IO matrix")
    print(f"{DMX_CH} channels, {n} frames, frame_id in ch0-1\n")

    recv_all(1.0)

    ser = serial.Serial("COM7", 250000, timeout=0.1)
    time.sleep(0.5)
    print("[OK] COM7 opened\n")

    pnames = ["random", "gradient", "solid_0", "solid_128", "solid_255",
              "walking", "alternating", "blocks_16", "blocks_64", "blocks_128"]

    total_mismatch = 0
    max_mismatch = 0
    no_data = 0
    skipped = 0
    frames_with_mismatch = 0
    frames_matched = 0

    send_dmx(ser, make_pattern("solid_0", 0))
    t_start = time.perf_counter()

    for frame in range(n):
        frame_id = frame + 1
        pname = pnames[frame % len(pnames)]
        ch = make_pattern(pname, frame_id)

        send_dmx(ser, ch)
        time.sleep(0.020)

        fc0, fc1, r0, r1, isr0, isr1, bk0, bk1 = recv_all(0.1)

        if r0 is None:
            no_data += 1
            continue

        result = compare_data(r0, r1)
        if result is None:
            skipped += 1
            continue

        e, id0 = result
        frames_matched += 1
        total_mismatch += e
        max_mismatch = max(max_mismatch, e)

        if e > 0:
            frames_with_mismatch += 1
            if frames_with_mismatch <= 200:
                print(f"  F{frame} id={id0:4d} mismatch={e} "
                      f"fc0={fc0} fc1={fc1} brk0={bk0} brk1={bk1} [{pname}]")

    elapsed = time.perf_counter() - t_start
    ser.close()

    fc0, fc1, _, _, isr0, isr1, bk0, bk1 = recv_all(1.0)

    print()
    print("=" * 55)
    print(f"Total: {n} frames, {no_data} no-data, {skipped} skips (ID mismatch)")
    print(f"Matched: {frames_matched}, {elapsed:.2f}s ({n/elapsed:.1f} fps)")
    print(f"P0: frame={fc0} brk={bk0} isr={isr0}")
    print(f"P1: frame={fc1} brk={bk1} isr={isr1}")
    print(f"Frames with mismatch: {frames_with_mismatch}/{frames_matched}")
    chk = (DMX_CH - 2) * frames_matched
    rate = total_mismatch / chk * 100 if chk else 0
    print(f"Channels checked: {chk}")
    if frames_with_mismatch == 0:
        print(">>> ALL MATCH (P0 == P1) <<<")
    else:
        print(f">>> FAIL: {total_mismatch} mismatches ({rate:.4f}%) <<<")
        print(f"Max mismatch per frame: {max_mismatch}")


if __name__ == "__main__":
    main()
