"""
Test: Does opening COM7 reset the ESP32?
Step 1: Only COM6 open, send+verify port 1 works
Step 2: Open COM7, wait for ESP32 boot, then test port 1 again
"""
import socket
import serial
import time

ESP32_IP = "192.168.1.222"
ESP32_PORT = 5555
DMX_CHANNELS = 512
NUM_FIXTURES = 6
CH_PER_FIXTURE = 3
BASE_ADDR = 1


def recv_all():
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
    except:
        pass
    if len(data) >= 1024:
        return list(data[0:512]), list(data[512:1024])
    return None, None


def send_dmx(ser, channels):
    ser.break_condition = True
    time.sleep(0.0001)
    ser.break_condition = False
    time.sleep(0.000012)
    ser.write(bytes([0x00]) + bytes(channels[:DMX_CHANNELS]))
    time.sleep(0.01)


def set_fixture(ch, f, r, g, b):
    a = (BASE_ADDR - 1) + f * CH_PER_FIXTURE
    if a + 2 < DMX_CHANNELS:
        ch[a] = r
        ch[a + 1] = g
        ch[a + 2] = b


def check_port1(label, ser):
    ch = [0] * DMX_CHANNELS
    for f in range(NUM_FIXTURES):
        set_fixture(ch, f, 200, 0, 0)
    send_dmx(ser, ch)
    time.sleep(0.05)
    _, recv1 = recv_all()
    if recv1:
        ok = all(recv1[i] == 200 for i in range(0, 18, 3))
        r_ch = [recv1[i] for i in range(0, 18, 3)]
        print(f"  [{label}] Port1 R = {r_ch} -> {'OK' if ok else 'FAIL'}")
        return ok
    print(f"  [{label}] No data!")
    return False


def main():
    print("=== DTR Reset Detection Test ===\n")

    # Step 1: COM6 only - verify port 1 works
    print("[Step 1] COM6 only")
    ser1 = serial.Serial("COM6", 250000, timeout=0.1, dsrdtr=False, rtscts=False)
    ser1.dtr = False
    ser1.rts = False
    time.sleep(0.5)
    check_port1("COM6 only", ser1)

    # Step 2: Now open COM7 (may reset ESP32)
    print("\n[Step 2] Opening COM7 (may reset ESP32)...")
    ser0 = serial.Serial("COM7", 250000, timeout=0.1, dsrdtr=False, rtscts=False)
    ser0.dtr = False
    ser0.rts = False
    print("  COM7 opened. Waiting 3s for ESP32 boot...")
    time.sleep(3)

    # Step 3: Test port 1 with COM7 open (idle)
    check_port1("COM7 open idle", ser1)

    # Step 4: Test port 0 with COM7 sending
    print("\n[Step 4] Sending on COM7...")
    ch0 = [0] * DMX_CHANNELS
    for f in range(NUM_FIXTURES):
        set_fixture(ch0, f, 0, 200, 0)
    send_dmx(ser0, ch0)
    time.sleep(0.05)
    recv0, _ = recv_all()
    if recv0:
        g_ch = [recv0[i] for i in range(1, 19, 3)]
        print(f"  Port0 G = {g_ch}")

    # Step 5: Both sending simultaneously
    print("\n[Step 5] Both ports sending...")
    for i in range(5):
        send_dmx(ser0, ch0)
        time.sleep(0.01)
        send_dmx(ser1, ch)
        time.sleep(0.05)
        recv0, recv1 = recv_all()
        if recv0 and recv1:
            p0_ok = all(recv0[i] == 0 for i in range(0, 18, 3)) and all(recv0[i] == 200 for i in range(1, 19, 3))
            p1_ok = all(recv1[i] == 200 for i in range(0, 18, 3))
            print(f"  Attempt {i}: P0={'OK' if p0_ok else 'FAIL'} P1={'OK' if p1_ok else 'FAIL'}")

    ser0.close()
    ser1.close()


if __name__ == "__main__":
    main()
