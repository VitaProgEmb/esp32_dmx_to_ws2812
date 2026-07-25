"""
UDP OTA — отправка прошивки на ESP32 через UDP
"""

import socket
import struct
import time
import argparse
import sys
import os

UDP_PORT = 5124
CHUNK_SIZE = 1024


def ota_begin(sock, ip, total_size):
    cmd = struct.pack(">BI", 0x02, total_size)
    sock.sendto(cmd, (ip, UDP_PORT))
    try:
        resp, _ = sock.recvfrom(1024)
        return resp[0] == 0xAA
    except socket.timeout:
        return False


def ota_chunk(sock, ip, seq, data):
    cmd = struct.pack(">BH", 0x03, seq) + data
    for attempt in range(3):
        sock.sendto(cmd, (ip, UDP_PORT))
        try:
            resp, _ = sock.recvfrom(1024)
            if len(resp) >= 3 and resp[0] == 0x06:
                ack_seq = struct.unpack(">H", resp[1:3])[0]
                if ack_seq == seq:
                    return True
        except socket.timeout:
            pass
    return False


def ota_end(sock, ip):
    sock.sendto(bytes([0x04]), (ip, UDP_PORT))
    try:
        resp, _ = sock.recvfrom(1024)
        return resp[0] == 0xAA
    except socket.timeout:
        return False


def run_ota(args):
    if not os.path.exists(args.bin):
        print(f"ERROR: {args.bin} not found")
        return 1

    file_size = os.path.getsize(args.bin)
    print(f"=== UDP OTA ===")
    print(f"Target:   {args.ip}:{UDP_PORT}")
    print(f"Firmware: {args.bin} ({file_size:,} bytes)")
    print()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3.0)

    # OTA begin
    print("OTA begin...")
    if not ota_begin(sock, args.ip, file_size):
        print("ERROR: no ACK on begin")
        sock.close()
        return 1
    print("OK")

    # Send chunks
    with open(args.bin, "rb") as f:
        seq = 0
        sent = 0
        start = time.time()
        retries = 0

        while True:
            chunk = f.read(CHUNK_SIZE)
            if not chunk:
                break
            seq += 1

            if not ota_chunk(sock, args.ip, seq, chunk):
                retries += 1
                print(f"  Chunk {seq}: FAILED after 3 retries")
                if retries > 10:
                    print("ABORT: too many failures")
                    sock.close()
                    return 1
                continue

            sent += len(chunk)
            if seq % 100 == 0 or sent >= file_size:
                pct = sent * 100 // file_size
                elapsed = time.time() - start
                speed = sent / 1024 / elapsed if elapsed > 0 else 0
                print(f"  [{pct:3d}%] {sent:,}/{file_size:,}  {speed:.0f} KB/s")

    # OTA end
    print("OTA end...")
    ota_end(sock, args.ip)
    sock.close()

    elapsed = time.time() - start
    print(f"Done: {elapsed:.1f}s, {file_size/1024/elapsed:.0f} KB/s, {retries} retries")
    print("Device rebooting...")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--ip", default="192.168.1.100")
    parser.add_argument("--bin", default="build/dmx_sniffer.bin")
    args = parser.parse_args()
    sys.exit(run_ota(args))
