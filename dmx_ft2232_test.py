"""
Test: COM7 stays open but does NOT send. Only COM6 sends.
Check if just having COM7 open blocks port 1 reception.
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


def main():
    print("=== FT2232 Dual Channel Test ===\n")

    # Step 1: COM7 open, COM6 closed -> send on COM6 only
    print("[Step 1] COM7 open (idle), COM6 closed")
    ser0 = serial.Serial("COM7", 250000, timeout=0.1, dsrdtr=False, rtscts=False)
    ser0.dtr = False
    ser0.rts = False
    print("  COM7 opened (not sending)")

    time.sleep(1)  # Wait for ESP32 to settle after potential DTR reset

    # Now send on COM6 (will open it)
    ser1 = serial.Serial("COM6", 250000, timeout=0.1, dsrdtr=False, rtscts=False)
    ser1.dtr = False
    ser1.rts = False
    print("  COM6 opened")

    time.sleep(0.5)  # Wait for ESP32 to settle

    ch = [0] * DMX_CHANNELS
    for f in range(NUM_FIXTURES):
        set_fixture(ch, f, 255, 0, 0)

    print("\n  Sending RED on COM6 (COM7 idle)...")
    for i in range(5):
        send_dmx(ser1, ch)
        time.sleep(0.05)
        _, recv1 = recv_all()
        if recv1:
            r_ch = [recv1[j] for j in range(0, 18, 3)]
            print(f"    Attempt {i}: Port1 R = {r_ch}")

    ser1.close()

    # Step 2: Now also send on COM7 (both active)
    print("\n[Step 2] COM7 also sending + COM6 sending")
    ch0 = [0] * DMX_CHANNELS
    for f in range(NUM_FIXTURES):
        set_fixture(ch0, f, 0, 255, 0)  # GREEN on port 0

    ch1 = [0] * DMX_CHANNELS
    for f in range(NUM_FIXTURES):
        set_fixture(ch1, f, 0, 0, 255)  # BLUE on port 1

    for i in range(10):
        send_dmx(ser0, ch0)
        time.sleep(0.02)
        send_dmx(ser1, ch1)
        time.sleep(0.05)
        recv0, recv1 = recv_all()
        if recv0 and recv1:
            p0_r = [recv0[j] for j in range(0, 18, 3)]
            p1_r = [recv1[j] for j in range(0, 18, 3)]
            print(f"  Attempt {i}: P0 G={p0_r} P1 B={p1_r}")

    ser0.close()
    try:
        ser1.close()
    except:
        pass


if __name__ == "__main__":
    main()
