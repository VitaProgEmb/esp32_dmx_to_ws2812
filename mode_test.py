import urllib.request
import json
import time

# Send TESTER mode
data = json.dumps({"mode": "tester"}).encode()
req = urllib.request.Request("http://192.168.1.182/api/mode", data=data, headers={"Content-Type": "application/json"})
try:
    urllib.request.urlopen(req, timeout=5)
    print("TESTER mode sent")
except Exception as e:
    print(f"Error: {e}")
    exit(1)

time.sleep(2)

# Check settings
try:
    resp = urllib.request.urlopen("http://192.168.1.182/api/settings", timeout=5)
    print(resp.read().decode())
except Exception as e:
    print(f"Error: {e}")

time.sleep(1)

# Switch back to sniffer
data2 = json.dumps({"mode": "sniffer"}).encode()
req2 = urllib.request.Request("http://192.168.1.182/api/mode", data=data2, headers={"Content-Type": "application/json"})
try:
    urllib.request.urlopen(req2, timeout=5)
    print("SNIFFER mode sent")
except Exception as e:
    print(f"Error: {e}")
