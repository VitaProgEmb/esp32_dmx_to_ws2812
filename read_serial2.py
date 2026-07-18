import serial
import time

s = serial.Serial('COM3', 115200, timeout=0.5)
s.reset_input_buffer()

time.sleep(2)
data = s.read(4096)
text = data.decode('utf-8', 'replace')
for line in text.split('\n'):
    if 'DIR' in line or 'mode' in line.lower() or 'GPIO27' in line or 'DMX' in line:
        print(line.strip())

s.close()
