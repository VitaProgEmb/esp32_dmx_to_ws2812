#!/usr/bin/env python3
"""Detailed byte analysis: send known pattern, show ALL 512 bytes from ESP32."""
import serial, socket, struct, time

ESP32 = "192.168.1.100"
COM_PORT = "COM9"
BAUD = 250000
DEBUG_PORT = 5555
DMX_CHANNELS = 512

def send_dmx(ser, frame):
    ser.send_break(0.00012)
    time.sleep(0.000012)
    ser.write(frame)
    ser.flush()

def query_debug():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect((ESP32, DEBUG_PORT))
    s.sendall(b"\x01")
    data = b''
    while len(data) < 1088:
        chunk = s.recv(1088 - len(data))
        if not chunk:
            break
        data += chunk
    s.close()
    if len(data) < 1088:
        return None
    fc0 = struct.unpack_from("<I", data, 0)[0]
    fc1 = struct.unpack_from("<I", data, 4)[0]
    err0 = struct.unpack_from("<I", data, 24)[0]
    err1 = struct.unpack_from("<I", data, 28)[0]
    dmx0 = list(data[64:576])
    dmx1 = list(data[576:1088])
    return dict(fc0=fc0, fc1=fc1, err0=err0, err1=err1, dmx0=dmx0, dmx1=dmx1)

ser = serial.Serial(COM_PORT, BAUD, timeout=1)
time.sleep(1)

# Test 1: all 0x00 (start code only, rest zeros)
print("=== Test 1: all-0x00 ===")
frame = bytes([0x00] + [0x00] * DMX_CHANNELS)
for i in range(200):
    send_dmx(ser, frame)
time.sleep(0.2)
r = query_debug()
if r:
    print("fc0=%d err0=%d" % (r['fc0'], r['err0']))
    nonzero = sum(1 for b in r['dmx0'] if b != 0x00)
    print("  P0: non-zero count=%d, first 10: %s" % (nonzero, ["%02X" % b for b in r['dmx0'][:10]]))

# Test 2: all 0xFF
print("\n=== Test 2: all-0xFF ===")
frame = bytes([0x00] + [0xFF] * DMX_CHANNELS)
for i in range(200):
    send_dmx(ser, frame)
time.sleep(0.2)
r = query_debug()
if r:
    print("fc0=%d err0=%d" % (r['fc0'], r['err0']))
    xff_count = sum(1 for b in r['dmx0'] if b == 0xFF)
    print("  P0: 0xFF count=%d, first 10: %s" % (xff_count, ["%02X" % b for b in r['dmx0'][:10]]))

# Test 3: gradient with small range (0-10)
print("\n=== Test 3: gradient 0-10 ===")
ch = bytes([(i % 11) for i in range(DMX_CHANNELS)])
frame = bytes([0x00]) + ch
for i in range(200):
    send_dmx(ser, frame)
time.sleep(0.2)
r = query_debug()
if r:
    print("fc0=%d err0=%d" % (r['fc0'], r['err0']))
    expected_first10 = [i % 11 for i in range(10)]
    print("  Expected first 10: %s" % ["%02X" % b for b in expected_first10])
    print("  P0 first 10:       %s" % ["%02X" % b for b in r['dmx0'][:10]])
    print("  P0 first 20:       %s" % ["%02X" % b for b in r['dmx0'][:20]])
    # Check first 10 bytes match expected
    match = sum(1 for i in range(10) if r['dmx0'][i] == expected_first10[i])
    print("  First 10 match: %d/10" % match)
    # Check for shifted match (shifted by 2)
    match2 = sum(1 for i in range(10) if r['dmx0'][i] == (i+2) % 11)
    print("  First 10 match(shift+2): %d/10" % match2)

ser.close()
