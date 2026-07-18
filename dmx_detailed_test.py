import urllib.request, socket, serial, time

# Reboot
req = urllib.request.Request('http://192.168.1.222/api/reboot', method='POST', data=b'{}')
try: urllib.request.urlopen(req, timeout=5)
except: pass
time.sleep(5)

def recv_debug():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect(('192.168.1.222', 5555))
    s.send(b'\x01')
    data = b''
    while len(data) < 1032:
        chunk = s.recv(1032 - len(data))
        if not chunk: break
        data += chunk
    s.close()
    c0 = int.from_bytes(data[0:4], 'little')
    c1 = int.from_bytes(data[4:8], 'little')
    p0 = list(data[8:520])
    p1 = list(data[520:1032])
    return c0, c1, p0, p1

c0, c1, p0, p1 = recv_debug()
print("After reboot: P0=%d P1=%d" % (c0, c1))

# Test: COM6 only
ser = serial.Serial('COM6', 250000, timeout=0.1)
time.sleep(0.5)
ch = [0]*512
for f in range(6):
    a = f * 3
    ch[a] = 100 + f*20
    ch[a+1] = 200 - f*10
    ch[a+2] = 50 + f*30

for i in range(10):
    ser.break_condition = True; time.sleep(0.0001)
    ser.break_condition = False; time.sleep(0.000012)
    ser.write(bytes([0x00]) + bytes(ch))
    time.sleep(0.03)

c0, c1, p0, p1 = recv_debug()
print("\nCOM6 only: P0=%d P1=%d" % (c0, c1))
for f in range(6):
    a = f*3
    expected = [100+f*20, 200-f*10, 50+f*30]
    got = p1[a:a+3]
    status = "OK" if got == expected else "FAIL"
    print("  F%d: sent=%s recv=%s %s" % (f, expected, got, status))

# Now both ports with 50ms gap
ser2 = serial.Serial('COM7', 250000, timeout=0.1)
time.sleep(0.5)
ch0 = [0]*512
for f in range(6):
    a = f*3
    ch0[a] = 10; ch0[a+1] = 20; ch0[a+2] = 30

print("\nBoth ports, 50ms gap COM7->COM6:")
c0b, c1b, _, _ = recv_debug()
for i in range(20):
    ser2.break_condition = True; time.sleep(0.0001)
    ser2.break_condition = False; time.sleep(0.000012)
    ser2.write(bytes([0x00]) + bytes(ch0))
    time.sleep(0.05)
    ser.break_condition = True; time.sleep(0.0001)
    ser.break_condition = False; time.sleep(0.000012)
    ser.write(bytes([0x00]) + bytes(ch))
    time.sleep(0.05)

c0, c1, p0, p1 = recv_debug()
print("P0=%d(+%d) P1=%d(+%d)" % (c0, c0-c0b, c1, c1-c1b))
for f in range(6):
    a = f*3
    expected = [100+f*20, 200-f*10, 50+f*30]
    got = p1[a:a+3]
    status = "OK" if got == expected else "FAIL"
    print("  F%d: sent=%s recv=%s %s" % (f, expected, got, status))

# P0 check
for f in range(6):
    a = f*3
    expected = [10, 20, 30]
    got = p0[a:a+3]
    status = "OK" if got == expected else "FAIL"
    print("  P0 F%d: sent=%s recv=%s %s" % (f, expected, got, status))

# Now both ports with 5ms gap (stress)
print("\nBoth ports, 5ms gap (stress):")
c0b, c1b, _, _ = recv_debug()
for i in range(50):
    ser2.break_condition = True; time.sleep(0.0001)
    ser2.break_condition = False; time.sleep(0.000012)
    ser2.write(bytes([0x00]) + bytes(ch0))
    time.sleep(0.005)
    ser.break_condition = True; time.sleep(0.0001)
    ser.break_condition = False; time.sleep(0.000012)
    ser.write(bytes([0x00]) + bytes(ch))
    time.sleep(0.025)

c0, c1, p0, p1 = recv_debug()
print("P0=%d(+%d) P1=%d(+%d)" % (c0, c0-c0b, c1, c1-c1b))
for f in range(6):
    a = f*3
    expected = [100+f*20, 200-f*10, 50+f*30]
    got = p1[a:a+3]
    status = "OK" if got == expected else "FAIL"
    print("  F%d: sent=%s recv=%s %s" % (f, expected, got, status))

ser.close(); ser2.close()
