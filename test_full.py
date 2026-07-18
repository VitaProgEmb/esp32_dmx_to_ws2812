import socket, time, sys
sys.stdout.reconfigure(encoding='utf-8')

# Test /test (API)
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
try:
    s.connect(('192.168.1.222', 80))
    s.send(b'GET /test HTTP/1.0\r\nHost: 192.168.1.222\r\n\r\n')
    data = s.recv(4096)
    print(f'/test: {len(data)} bytes')
    print(data.decode(errors='replace')[:300])
except Exception as e:
    print(f'/test FAIL: {e}')
s.close()

print('---')

# Test / (main HTML page - 51KB)
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(30)
try:
    s.connect(('192.168.1.222', 80))
    s.send(b'GET / HTTP/1.1\r\nHost: 192.168.1.222\r\nConnection: close\r\n\r\n')
    data = b''
    start = time.time()
    while True:
        try:
            chunk = s.recv(4096)
            if not chunk: break
            data += chunk
        except socket.timeout:
            print('timeout')
            break
    elapsed = time.time() - start
    print(f'/ : {len(data)} bytes in {elapsed:.1f}s')
    if len(data) > 200:
        text = data[:200].decode(errors='replace')
        print(f'First 200 chars: {text}')
except Exception as e:
    print(f'/ FAIL: {e}')
s.close()

print('---')

# Test /api/status
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
try:
    s.connect(('192.168.1.222', 80))
    s.send(b'GET /api/status HTTP/1.0\r\nHost: 192.168.1.222\r\n\r\n')
    data = s.recv(8192)
    print(f'/api/status: {len(data)} bytes')
    print(data.decode(errors='replace')[:500])
except Exception as e:
    print(f'/api/status FAIL: {e}')
s.close()
