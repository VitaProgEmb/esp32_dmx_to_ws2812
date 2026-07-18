import serial
import time

s = serial.Serial('COM3', 115200, timeout=0.5)
s.dtr = False
s.rts = True
time.sleep(0.1)
s.rts = False
time.sleep(0.1)
s.reset_input_buffer()

time.sleep(5)
data = s.read(8192).decode('utf-8', 'replace')
for line in data.split('\n'):
    line = line.strip()
    if line and ('IP' in line or 'DIR' in line or 'WIFI' in line or 'boot' in line.lower() or 'error' in line.lower() or 'main' in line.lower()):
        print(line)
s.close()
