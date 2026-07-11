#!/usr/bin/env python3
"""Create a BIOS-installed GooberOS test disk from build/install payloads."""
import os
import struct
import subprocess
import sys

PART_START = 2048
CORE_FIRST = 1
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INSTALL = os.path.join(REPO, "build", "install")


def encode_chs(lba):
    if lba >= 0xFBFC00:
        return bytes([0xFF, 0xFF, 0xFF])
    cyl = lba // (255 * 63)
    head = (lba // 63) % 255
    sec = (lba % 63) + 1
    if cyl >= 1024:
        return bytes([0xFF, 0xFF, 0xFF])
    return bytes([head, ((cyl >> 2) & 0xC0) | (sec & 0x3F), cyl & 0xFF])


def patch_core_sector0(sector0, core_first_lba, total_sectors):
    data = bytearray(sector0[:512])
    list_off = 512 - 12
    term_off = list_off - 12
    data[term_off : term_off + 12] = b"\x00" * 12
    struct.pack_into("<Q", data, list_off, core_first_lba + 1)
    struct.pack_into("<H", data, list_off + 8, total_sectors - 1)
    struct.pack_into("<H", data, list_off + 10, 0x0820)
    return bytes(data)


def write_mbr(boot_img, core_first_lba, part_start, part_sectors):
    mbr = bytearray(512)
    mbr[:440] = boot_img[:440]
    struct.pack_into("<I", mbr, 0x5C, core_first_lba)
    struct.pack_into("<I", mbr, 0x60, 0)
    mbr[0x64] = 0xFF
    pe = 446
    mbr[pe] = 0x80
    mbr[pe + 4] = 0x0C
    mbr[pe + 1 : pe + 4] = encode_chs(part_start)
    end = part_start + part_sectors - 1
    mbr[pe + 5 : pe + 8] = encode_chs(end)
    struct.pack_into("<I", mbr, pe + 8, part_start)
    struct.pack_into("<I", mbr, pe + 12, part_sectors)
    mbr[510] = 0x55
    mbr[511] = 0xAA
    return bytes(mbr)


def blast_partition(out_path, part_start, part_img_path):
    with open(out_path, "r+b") as f, open(part_img_path, "rb") as pf:
        f.seek(part_start * 512)
        f.write(pf.read())


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build", "GooberOS-installed.img")
    boot = open(os.path.join(INSTALL, "boot.img"), "rb").read()
    core = open(os.path.join(INSTALL, "core.img"), "rb").read()
    fat_template = os.path.join(INSTALL, "fat-partition.img")

    total_sectors = 256 * 1024 * 1024 // 512
    part_sectors = total_sectors - PART_START

    with open(out, "wb") as f:
        f.seek(total_sectors * 512 - 1)
        f.write(b"\x00")

    mbr = write_mbr(boot, CORE_FIRST, PART_START, part_sectors)
    with open(out, "r+b") as f:
        f.write(mbr)
        core_secs = (len(core) + 511) // 512
        s0 = patch_core_sector0(core, CORE_FIRST, core_secs)
        f.seek(CORE_FIRST * 512)
        f.write(s0)
        off = 512
        lba = CORE_FIRST + 1
        while off < len(core):
            chunk = core[off : off + 512].ljust(512, b"\x00")
            f.seek(lba * 512)
            f.write(chunk)
            off += 512
            lba += 1

    if os.path.isfile(fat_template):
        blast_partition(out, PART_START, fat_template)
    else:
        # Fallback for older payloads without template
        part_img = out + ".part"
        kernel = open(os.path.join(INSTALL, "kernel_payload.bin"), "rb").read()
        grub_cfg = open(os.path.join(INSTALL, "grub.cfg"), "rb").read()
        with open(part_img, "wb") as pf:
            pf.seek(part_sectors * 512 - 1)
            pf.write(b"\x00")
        subprocess.run(
            ["mkfs.vfat", "-F", "32", "-n", "GOOBEROS", "-S", "512", "-s", "8", "-R", "32", part_img],
            check=True,
        )
        subprocess.run(["mmd", "-i", part_img, "::boot", "::boot/grub"], check=False)
        import tempfile
        with tempfile.NamedTemporaryFile(delete=False) as kf:
            kf.write(kernel)
            kpath = kf.name
        with tempfile.NamedTemporaryFile(delete=False) as gf:
            gf.write(grub_cfg)
            gpath = gf.name
        subprocess.run(["mcopy", "-i", part_img, kpath, "::boot/kernel.bin"], check=True)
        subprocess.run(["mcopy", "-i", part_img, gpath, "::boot/grub/grub.cfg"], check=True)
        os.unlink(kpath)
        os.unlink(gpath)
        blast_partition(out, PART_START, part_img)
        os.unlink(part_img)

    print(f"[+] Wrote {out} ({total_sectors} sectors)")


if __name__ == "__main__":
    main()
