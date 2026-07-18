import serial, time, sys, socket, threading
sys.stdout.reconfigure(encoding='utf-8')

serial_output = []

def read_serial():
    s = serial.Serial('COM3', 115200, timeout=1)
    start = time.time()
    while time.time() - start < 30:
        line = s.readline().decode('utf-8', errors='replace').strip()
        if line:
            serial_output.append(f'[{time.time()-start:.1f}s] {line}')
    s.close()

def make_request():
    time.sleep(4)
    print('--- Making HTTP request ---', flush=True)
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(('192.168.1.222', 80))
        s.send(b'GET / HTTP/1.0\r\nHost: 192.168.1.222\r\n\r\n')
        data = s.recv(4096)
        print(f'--- Got {len(data)} bytes ---', flush=True)
        s.close()
    except Exception as e:
        print(f'--- Request failed: {e} ---', flush=True)

t1 = threading.Thread(target=read_serial)
t2 = threading.Thread(target=make_request)
t1.start()
time.sleep(1)
t2.start()
t1.join()
t2.join()

print('\n=== SERIAL LOG (filtered) ===')
for line in serial_output:
    if 'WEB_SRV' in line or 'index' in line.lower() or 'panic' in line.lower() or 'crash' in line.lower() or 'exception' in line.lower() or 'Guru' in line:
        print(line)
print('\n=== LAST 10 LINES ===')
for line in serial_output[-10:]:
    print(line)
