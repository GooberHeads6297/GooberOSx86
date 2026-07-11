#!/usr/bin/env python3
"""Create a UEFI-bootable GPT+ESP GooberOS disk from build64/install payloads.

Mirrors install/gpt_embed.c + fat32 template blast so VirtualBox/OVMF and
real hardware can be tested without running the in-OS installer.
"""
import os
import struct
import subprocess
import sys
import zlib

PART_START = 2048
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INSTALL = os.path.join(REPO, "build64", "install")
if not os.path.isdir(INSTALL):
    INSTALL = os.path.join(REPO, "build", "install")

# EFI System Partition type GUID (GPT mixed-endian bytes)
ESP_TYPE = bytes([
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
])
ESP_UUID = b"GOOBEROS" + b"ESP0001\x00"
DISK_UUID = b"GOOBERDI" + b"SK00001\x00"


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def write_protective_mbr(f, disk_sectors: int) -> None:
    mbr = bytearray(512)
    mbr[446:450] = bytes([0x00, 0x00, 0x02, 0x00])
    mbr[450] = 0xEE
    mbr[451:454] = bytes([0xFF, 0xFF, 0xFF])
    struct.pack_into("<I", mbr, 454, 1)
    size = 0xFFFFFFFF if disk_sectors > 0x100000000 else max(disk_sectors - 1, 0)
    struct.pack_into("<I", mbr, 458, size)
    mbr[510], mbr[511] = 0x55, 0xAA
    f.seek(0)
    f.write(mbr)


def make_entry(first: int, last: int) -> bytes:
    e = bytearray(128)
    e[0:16] = ESP_TYPE
    e[16:32] = ESP_UUID
    struct.pack_into("<Q", e, 32, first)
    struct.pack_into("<Q", e, 40, last)
    struct.pack_into("<Q", e, 48, 1)  # Required Partition
    name = "GooberOS"
    for i, ch in enumerate(name):
        e[56 + i * 2] = ord(ch)
    return bytes(e)


def write_gpt_header(f, current, alternate, first_usable, last_usable, entries_lba, entries_crc):
    hdr = bytearray(512)
    hdr[0:8] = b"EFI PART"
    struct.pack_into("<I", hdr, 8, 0x00010000)
    struct.pack_into("<I", hdr, 12, 92)
    struct.pack_into("<I", hdr, 16, 0)
    struct.pack_into("<Q", hdr, 24, current)
    struct.pack_into("<Q", hdr, 32, alternate)
    struct.pack_into("<Q", hdr, 40, first_usable)
    struct.pack_into("<Q", hdr, 48, last_usable)
    hdr[56:72] = DISK_UUID
    struct.pack_into("<Q", hdr, 72, entries_lba)
    struct.pack_into("<I", hdr, 80, 128)
    struct.pack_into("<I", hdr, 84, 128)
    struct.pack_into("<I", hdr, 88, entries_crc)
    struct.pack_into("<I", hdr, 16, crc32(bytes(hdr[:92])))
    f.seek(current * 512)
    f.write(hdr)


def patch_hidden_sectors(part_img: bytes, hidden: int) -> bytes:
    data = bytearray(part_img)
    if len(data) >= 512:
        struct.pack_into("<I", data, 28, hidden)
        # Keep FAT32 backup boot sector in sync (BPB BackupBootSector).
        backup = struct.unpack_from("<H", data, 50)[0]
        if 0 < backup < 32 and len(data) >= (backup + 1) * 512:
            off = backup * 512
            data[off : off + 512] = data[0:512]
    return bytes(data)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build64", "GooberOS-uefi-installed.img")
    fat_path = os.path.join(INSTALL, "fat-partition.img")
    if not os.path.isfile(fat_path):
        print(f"[!] missing {fat_path}; build x64 first", file=sys.stderr)
        sys.exit(1)

    fat = open(fat_path, "rb").read()
    # Disk must fit the FAT template (36 MiB) plus GPT heads/tails.
    disk_bytes = max(96 * 1024 * 1024, ((len(fat) + 2048 * 512) + 2 * 1024 * 1024))
    disk_bytes = (disk_bytes + 1024 * 1024 - 1) // (1024 * 1024) * (1024 * 1024)
    disk_sectors = disk_bytes // 512
    alternate = disk_sectors - 1
    backup_entries = alternate - 32
    last_usable = backup_entries - 1
    part_first = PART_START
    part_last = last_usable

    with open(out, "wb") as f:
        f.seek(disk_bytes - 1)
        f.write(b"\x00")

    entries = make_entry(part_first, part_last) + (b"\x00" * (128 * 127))
    entries_crc = crc32(entries)

    with open(out, "r+b") as f:
        write_protective_mbr(f, disk_sectors)
        f.seek(2 * 512)
        f.write(entries)
        write_gpt_header(f, 1, alternate, 34, last_usable, 2, entries_crc)
        f.seek(backup_entries * 512)
        f.write(entries)
        write_gpt_header(f, alternate, 1, 34, last_usable, backup_entries, entries_crc)

        fat = patch_hidden_sectors(fat, PART_START)
        f.seek(PART_START * 512)
        f.write(fat)

    # Host-side sanity checks
    subprocess.run(["sgdisk", "-v", out], check=False)
    print(f"[+] Wrote UEFI GPT disk {out} ({disk_sectors} sectors)")
    print(f"    ESP LBA {part_first}..{part_last}")
    print(f"    Test: qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -drive file={out},format=raw")


if __name__ == "__main__":
    main()
