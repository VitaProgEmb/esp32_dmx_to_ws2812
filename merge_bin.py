"""
Скрипт для слияния bootloader + partition-table + ota_data + app в один .bin
И OTA-образа (dmx_sniffer.bin) для веб-обновления.
Использование: python merge_bin.py
"""
import os, sys, struct, zlib, shutil

BUILD_DIR = "dmx_sniffer/build"
OTA_DATA_ADDR = 0xD000
OTA_DATA_SIZE = 0x2000  # 8KB

def make_ota_initial():
    """Создаёт бинарник ota_data: seq=1 (грузиться из ota_0), state=NEW."""
    ota_seq = struct.pack("<I", 1)
    seq_label = b'\xff\xff\xff\xff'
    ota_state = b'\x00\x00\x00\x00'
    header = ota_seq + seq_label + ota_state
    crc = struct.pack("<I", zlib.crc32(header, 0))
    data = header + crc
    data += b'\xff' * (OTA_DATA_SIZE - len(data))
    return data


def main():
    out = "merged_firmware.bin"

    # Генерируем ota_data_initial.bin
    ota_data = make_ota_initial()
    ota_path = os.path.join(BUILD_DIR, "ota_data_initial.bin")
    with open(ota_path, 'wb') as f:
        f.write(ota_data)
    print(f"  ota_data_initial.bin  0x{OTA_DATA_ADDR:05x}  {OTA_DATA_SIZE:>8d} bytes  (seq=1)")

    # Все куски для merged_firmware.bin
    entries = [
        (0x1000, os.path.join(BUILD_DIR, "bootloader/bootloader.bin")),
        (0x8000, os.path.join(BUILD_DIR, "partition_table/partition-table.bin")),
        (OTA_DATA_ADDR, ota_path),
        (0x10000, os.path.join(BUILD_DIR, "dmx_sniffer.bin")),
    ]

    total_size = max(addr + os.path.getsize(path) for addr, path in entries)
    data = bytearray(b'\xff' * total_size)

    for addr, path in entries:
        with open(path, 'rb') as f:
            chunk = f.read()
        data[addr:addr + len(chunk)] = chunk
        print(f"  {os.path.basename(path):35s} 0x{addr:05x}  {len(chunk):>8d} bytes")

    with open(out, 'wb') as f:
        f.write(data)
    print(f"\nMerged {total_size} bytes -> {out}")

    # Копируем app-образ в корень для OTA
    app_src = os.path.join(BUILD_DIR, "dmx_sniffer.bin")
    app_dst = os.path.join(os.path.dirname(os.path.abspath(out)), "dmx_sniffer.bin")
    shutil.copy2(app_src, app_dst)
    print(f"  dmx_sniffer.bin (OTA)          {os.path.getsize(app_dst):>8d} bytes -> {app_dst}")
    print()


if __name__ == "__main__":
    main()
