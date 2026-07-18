import urllib.request
import json

data = json.dumps({"mode": "tester"}).encode()
req = urllib.request.Request("http://192.168.1.182/api/mode", data=data, headers={"Content-Type": "application/json"})
try:
    resp = urllib.request.urlopen(req, timeout=5)
    print(resp.read().decode())
except Exception as e:
    print(f"Error: {e}")
