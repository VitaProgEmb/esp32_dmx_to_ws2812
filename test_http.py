import socket, time, sys
sys.stdout.reconfigure(encoding='utf-8')

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(30)
s.connect(('192.168.1.222', 80))
s.send(b'GET / HTTP/1.0\r\nHost: 192.168.1.222\r\n\r\n')
data = b''
start = time.time()
while True:
    try:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
        print(f't={time.time()-start:.1f}s chunk={len(chunk)} total={len(data)}')
    except socket.timeout:
        print('timeout')
        break
s.close()
print(f'Total: {len(data)} bytes')
if len(data) > 100:
    hdr_end = data.find(b'\r\n\r\n')
    print(f'Body: {len(data) - hdr_end - 4} bytes')
