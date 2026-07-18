import socket, sys
sys.stdout.reconfigure(encoding='utf-8')

# Test POST /api/save
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
try:
    s.connect(('192.168.1.222', 80))
    body = '{}'
    req = f'POST /api/save HTTP/1.0\r\nHost: 192.168.1.222\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n{body}'
    s.send(req.encode())
    data = s.recv(4096)
    print(f'/api/save: {len(data)} bytes')
    print(data.decode(errors='replace'))
except Exception as e:
    print(f'/api/save FAIL: {e}')
s.close()

print('---')

# Test POST /api/save again to verify no crash
import time
time.sleep(1)
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
try:
    s.connect(('192.168.1.222', 80))
    body = '{}'
    req = f'POST /api/save HTTP/1.0\r\nHost: 192.168.1.222\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n{body}'
    s.send(req.encode())
    data = s.recv(4096)
    print(f'/api/save (2nd): {len(data)} bytes')
    print(data.decode(errors='replace'))
except Exception as e:
    print(f'/api/save (2nd) FAIL: {e}')
s.close()

print('---')

# Verify settings still load correctly
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
try:
    s.connect(('192.168.1.222', 80))
    s.send(b'GET /api/settings HTTP/1.0\r\nHost: 192.168.1.222\r\n\r\n')
    data = s.recv(8192)
    print(f'/api/settings: {len(data)} bytes')
    # find JSON body
    idx = data.find(b'\r\n\r\n')
    if idx >= 0:
        body = data[idx+4:].decode(errors='replace')
        print(body[:500])
except Exception as e:
    print(f'/api/settings FAIL: {e}')
s.close()
