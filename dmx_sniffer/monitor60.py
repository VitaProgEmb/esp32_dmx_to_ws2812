import serial
import time
import sys
import io

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

ser = serial.Serial('COM3', 115200, timeout=1)
ser.dtr = False
ser.rts = False
time.sleep(0.05)
ser.rts = True
time.sleep(0.05)
ser.rts = False
time.sleep(0.1)

print("Monitoring for 60s. Try connecting to ESP32 now!")
start = time.time()
output = b""
while time.time() - start < 60:
    if ser.in_waiting > 0:
        data = ser.read(ser.in_waiting)
        output += data
        text = data.decode('utf-8', errors='replace')
        for line in text.split('\n'):
            line = line.strip()
            if line:
                print(f"[{int(time.time()-start):2d}s] {line}")
    time.sleep(0.05)

ser.close()
with open(r'C:\DmxSnifer\dmx_sniffer\monitor_log.txt', 'wb') as f:
    f.write(output)
print(f"\nTotal: {len(output)} bytes saved to monitor_log.txt")
