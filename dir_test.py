import serial
import time
import urllib.request
import json

s = serial.Serial('COM3', 115200, timeout=0.5)
s.dtr = False
s.rts = False
s.reset_input_buffer()

# Switch to tester
data = json.dumps({"mode": "tester"}).encode()
req = urllib.request.Request("http://192.168.1.182/api/mode", data=data, headers={"Content-Type": "application/json"})
try:
    urllib.request.urlopen(req, timeout=5)
    print(">> TESTER sent")
except Exception as e:
    print(f"Error: {e}")

time.sleep(3)

# Read all serial output
buf = s.read(8192).decode('utf-8', 'replace')
print("--- Serial output (TESTER mode) ---")
for line in buf.split('\n'):
    line = line.strip()
    if line:
        print(line)

# Switch to sniffer
data2 = json.dumps({"mode": "sniffer"}).encode()
req2 = urllib.request.Request("http://192.168.1.182/api/mode", data=data2, headers={"Content-Type": "application/json"})
try:
    urllib.request.urlopen(req2, timeout=5)
    print("\n>> SNIFFER sent")
except Exception as e:
    print(f"Error: {e}")

time.sleep(3)
buf2 = s.read(8192).decode('utf-8', 'replace')
print("--- Serial output (SNIFFER mode) ---")
for line in buf2.split('\n'):
    line = line.strip()
    if line:
        print(line)

s.close()
