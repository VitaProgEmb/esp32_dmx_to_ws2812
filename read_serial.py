import serial
import time

s = serial.Serial('COM3', 115200, timeout=2)
s.reset_input_buffer()

count = 0
start = time.time()
while count < 200 and (time.time() - start) < 15:
    line = s.readline()
    if line:
        try:
            print(line.decode('utf-8', errors='replace').rstrip())
            count += 1
        except:
            pass

s.close()
