#!/usr/bin/env python3
"""Host-side GooberC compiler (mirrors in-OS gooberc). Emits bytecode .gob."""
import argparse
import struct
import sys

GOB_MAGIC = 0x00424F47
GOB_VERSION = 1
GOB_ARCH_X86_64 = 2
GOB_KIND_CONSOLE = 1
GOB_KIND_GUI = 2
GOB_KIND_AUTO = 3
GOB_KIND_GFX3D = 4
GOB_FLAG_BYTECODE = 1

GBC_EXIT = 1
GBC_WRITE = 2
GBC_GUI_CREATE = 4
GBC_GUI_TEXT = 5
GBC_GUI_WAIT = 6
GBC_SLEEP_MS = 8
GBC_GFX3D_CLEAR = 9


def compile_gc(src: str) -> bytes:
    code = bytearray()
    rodata = bytearray()
    kind = GOB_KIND_CONSOLE

    def add_str(s: str) -> int:
        off = len(rodata)
        rodata.extend(s.encode("utf-8") + b"\0")
        return off

    for raw in src.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("use "):
            if "gfx3d" in line:
                kind = GOB_KIND_GFX3D
            elif "auto" in line:
                kind = GOB_KIND_AUTO
            elif "gui" in line:
                kind = GOB_KIND_GUI
        elif line.startswith("app "):
            if "gfx3d" in line:
                kind = GOB_KIND_GFX3D
            elif "auto" in line:
                kind = GOB_KIND_AUTO
            elif "gui" in line:
                kind = GOB_KIND_GUI
            else:
                kind = GOB_KIND_CONSOLE
        elif line.startswith("print "):
            q = line.split('"', 2)
            if len(q) >= 3:
                s = q[1]
                off = add_str(s)
                code.append(GBC_WRITE)
                code += struct.pack("<II", off, len(s))
        elif line.startswith("window "):
            parts = line.split('"')
            if len(parts) >= 3:
                title = parts[1]
                rest = parts[2].strip()
                w, h = 420, 240
                if "x" in rest.lower():
                    a, b = rest.lower().split("x", 1)
                    try:
                        w, h = int(a.strip()), int(b.strip().split()[0])
                    except ValueError:
                        pass
                off = add_str(title)
                code.append(GBC_GUI_CREATE)
                code += struct.pack("<IHH", off, w, h)
                kind = GOB_KIND_GUI
        elif line.startswith("text "):
            q = line.split('"', 2)
            if len(q) >= 3:
                s = q[1]
                off = add_str(s)
                code.append(GBC_GUI_TEXT)
                code += struct.pack("<IHHI", 0, 0, 0, off)
        elif line.startswith("wait"):
            code.append(GBC_GUI_WAIT)
            code += struct.pack("<I", 0)
        elif line.startswith("sleep"):
            parts = line.split()
            ms = 0
            if len(parts) >= 2:
                try:
                    ms = int(parts[1])
                except ValueError:
                    ms = 0
            code.append(GBC_SLEEP_MS)
            code += struct.pack("<I", ms)
            if kind == GOB_KIND_CONSOLE:
                kind = GOB_KIND_AUTO
        elif line.startswith("clear"):
            parts = line.split()
            rgba = 0
            if len(parts) >= 2:
                tok = parts[1]
                if tok.lower().startswith("0x"):
                    tok = tok[2:]
                try:
                    rgba = int(tok, 16)
                except ValueError:
                    rgba = 0
            code.append(GBC_GFX3D_CLEAR)
            code += struct.pack("<I", rgba)
            kind = GOB_KIND_GFX3D
        elif line.startswith("exit"):
            code.append(GBC_EXIT)
            code += struct.pack("<I", 0)

    if GBC_EXIT not in code:
        code.append(GBC_EXIT)
        code += struct.pack("<I", 0)

    hdr = struct.pack(
        "<IHBBIIII",
        GOB_MAGIC,
        GOB_VERSION,
        GOB_ARCH_X86_64,
        kind,
        GOB_FLAG_BYTECODE,
        0,
        len(code),
        len(rodata),
    )
    # header has reserved u32 at end — match C gob_header_t
    hdr = struct.pack(
        "<IHBBIIIII",
        GOB_MAGIC,
        GOB_VERSION,
        GOB_ARCH_X86_64,
        kind,
        GOB_FLAG_BYTECODE,
        0,
        len(code),
        len(rodata),
        0,
    )
    return hdr + bytes(code) + bytes(rodata)


def main():
    ap = argparse.ArgumentParser(description="GooberC host compiler")
    ap.add_argument("src")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()
    src = open(args.src, "r", encoding="utf-8").read()
    blob = compile_gc(src)
    open(args.output, "wb").write(blob)
    print(f"[+] wrote {args.output} ({len(blob)} bytes)")


if __name__ == "__main__":
    main()
