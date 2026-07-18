import socket, time, sys
sys.stdout.reconfigure(encoding='utf-8')

for i in range(10):
    time.sleep(2)
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3)
    try:
        s.connect(('192.168.1.222', 80))
        s.send(b'GET /api/status HTTP/1.0\r\nHost: 192.168.1.222\r\n\r\n')
        data = s.recv(4096)
        print(f'[{i}] OK: {len(data)} bytes', flush=True)
    except Exception as e:
        print(f'[{i}] FAIL: {e}', flush=True)
    finally:
        s.close()
