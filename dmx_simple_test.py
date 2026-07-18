"""
Minimal test: COM6 only, default DTR/RTS
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
    print("=== COM6 Only Test (Default DTR/RTS) ===\n")

    # No other serial port open
    ser = serial.Serial("COM6", 250000, timeout=0.1)
    print("[OK] COM6 opened (DTR={}, RTS={})".format(ser.dtr, ser.rts))

    time.sleep(1)  # Wait for ESP32 to settle after possible DTR reset

    ch = [0] * DMX_CHANNELS
    for f in range(NUM_FIXTURES):
        set_fixture(ch, f, 100, 200, 50)

    for attempt in range(5):
        send_dmx(ser, ch)
        time.sleep(0.05)
        recv0, recv1 = recv_all()
        if recv1:
            print(f"  Attempt {attempt}: port1[0:18] = {recv1[0:18]}")
            expected = [100,200,50] * 6
            if recv1[0:18] == expected:
                print("  PASS!")
                break
        else:
            print(f"  Attempt {attempt}: No data")

    # Now also open COM7
    print("\n--- Opening COM7 now ---")
    ser2 = serial.Serial("COM7", 250000, timeout=0.1)
    print("[OK] COM7 opened (DTR={}, RTS={})".format(ser2.dtr, ser2.rts))
    time.sleep(2)  # Wait for ESP32 boot

    for attempt in range(5):
        send_dmx(ser, ch)
        time.sleep(0.05)
        recv0, recv1 = recv_all()
        if recv1:
            r_val = recv1[0]
            g_val = recv1[1]
            b_val = recv1[2]
            print(f"  Attempt {attempt}: F0 = ({r_val},{g_val},{b_val}) expected=(100,200,50)")
        else:
            print(f"  Attempt {attempt}: No data")

    # Now send on COM7 too
    print("\n--- Both ports sending ---")
    ch2 = [0] * DMX_CHANNELS
    for f in range(NUM_FIXTURES):
        set_fixture(ch2, f, 0, 0, 255)

    for attempt in range(5):
        send_dmx(ser2, ch2)
        time.sleep(0.01)
        send_dmx(ser, ch)
        time.sleep(0.05)
        recv0, recv1 = recv_all()
        if recv0 and recv1:
            p0_f0 = recv0[0:3]
            p1_f0 = recv1[0:3]
            print(f"  Attempt {attempt}: P0 F0={p0_f0} P1 F0={p1_f0}")

    ser.close()
    ser2.close()


if __name__ == "__main__":
    main()
