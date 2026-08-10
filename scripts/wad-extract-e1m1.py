#!/usr/bin/env python3
"""Extract a tiny E1M1 occupancy grid from shareware DOOM1.WAD for DoomRay.gc."""
from __future__ import annotations

import argparse
import struct
import sys
from collections import deque
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_WAD = REPO / "dosemu" / "fixtures" / "doom" / "DOOM1.WAD"
DEFAULT_OUT = REPO / "gooberc" / "examples" / "data" / "E1M1.map.gcinc"


def read_lumps(data: bytes):
    ident, numlumps, info = struct.unpack_from("<4sII", data)
    if ident not in (b"IWAD", b"PWAD"):
        raise SystemExit("not a Doom WAD")
    lumps = []
    for i in range(numlumps):
        off = info + i * 16
        filepos, size, name = struct.unpack_from("<II8s", data, off)
        lumps.append((name.split(b"\0")[0].decode("ascii", "replace"), filepos, size))
    return lumps


def find_map(lumps, marker: str):
    idx = next((i for i, l in enumerate(lumps) if l[0] == marker), None)
    if idx is None:
        raise SystemExit(f"map {marker} not found")
    by = {}
    for i in range(idx, min(idx + 12, len(lumps))):
        by[lumps[i][0]] = lumps[i]
    return by


def bresenham_cells(x0, y0, x1, y1):
    dx = abs(x1 - x0)
    sx = 1 if x0 < x1 else -1
    dy = -abs(y1 - y0)
    sy = 1 if y0 < y1 else -1
    err = dx + dy
    x, y = x0, y0
    while True:
        yield x, y
        if x == x1 and y == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x += sx
        if e2 <= dx:
            err += dx
            y += sy


def extract_grid(data: bytes, marker: str, width: int, height: int):
    lumps = read_lumps(data)
    by = find_map(lumps, marker)
    vp, vs = by["VERTEXES"][1], by["VERTEXES"][2]
    verts = [struct.unpack_from("<hh", data, vp + i * 4) for i in range(vs // 4)]
    lp, ls = by["LINEDEFS"][1], by["LINEDEFS"][2]
    minx = min(v[0] for v in verts) - 64
    maxx = max(v[0] for v in verts) + 64
    miny = min(v[1] for v in verts) - 64
    maxy = max(v[1] for v in verts) + 64

    def to_cell(x, y):
        cx = int((x - minx) / (maxx - minx) * (width - 1))
        cy = int((y - miny) / (maxy - miny) * (height - 1))
        return cx, cy

    wall = [[False] * width for _ in range(height)]

    for i in range(ls // 14):
        v1, v2, flags, _typ, _tag, _s1, s2 = struct.unpack_from(
            "<hhhhhhh", data, lp + i * 14
        )
        if not ((flags & 1) or s2 == -1):
            continue
        x0, y0 = verts[v1]
        x1, y1 = verts[v2]
        c0 = to_cell(x0, y0)
        c1 = to_cell(x1, y1)
        for cx, cy in bresenham_cells(c0[0], c0[1], c1[0], c1[1]):
            if 0 <= cx < width and 0 <= cy < height:
                wall[cy][cx] = True

    tp, ts = by["THINGS"][1], by["THINGS"][2]
    px = py = 0
    pang = 90
    for i in range(ts // 10):
        x, y, angle, typ, _flags = struct.unpack_from("<hhhhh", data, tp + i * 10)
        if typ == 1:
            px, py = to_cell(x, y)
            pang = angle
            break

    # Ensure spawn is walkable.
    wall[py][px] = False

    seen = set()
    q = deque([(px, py)])
    seen.add((px, py))
    while q:
        x, y = q.popleft()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < width and 0 <= ny < height and (nx, ny) not in seen:
                if not wall[ny][nx]:
                    seen.add((nx, ny))
                    q.append((nx, ny))

    # Crop to open area + 1-cell wall border (keeps asset tiny / playable).
    xs = [x for x, _y in seen]
    ys = [y for _x, y in seen]
    x0 = max(0, min(xs) - 1)
    x1 = min(width - 1, max(xs) + 1)
    y0 = max(0, min(ys) - 1)
    y1 = min(height - 1, max(ys) + 1)
    cw = x1 - x0 + 1
    ch = y1 - y0 + 1
    rows = []
    for y in range(y0, y1 + 1):
        chars = []
        for x in range(x0, x1 + 1):
            if (x, y) not in seen:
                chars.append("#")
            else:
                chars.append(".")
        rows.append("".join(chars))

    # Doom angle: 0 east, 90 north. Raycaster uses degrees CCW from east.
    return {
        "w": cw,
        "h": ch,
        "px": px - x0,
        "py": py - y0,
        "pang": pang % 360,
        "rows": rows,
        "marker": marker,
    }


def emit_gcinc(info: dict) -> str:
    # GooberC is line-oriented — build the row list with push, not a multiline [].
    lines = [
        f"# Auto-generated from DOOM1.WAD {info['marker']} — do not edit.",
        f"var MAP_W = {info['w']}",
        f"var MAP_H = {info['h']}",
        f"var MAP_PX = {info['px']}",
        f"var MAP_PY = {info['py']}",
        f"var MAP_PA = {info['pang']}",
        "var MAP_ROWS = []",
    ]
    for row in info["rows"]:
        lines.append(f'push MAP_ROWS "{row}"')
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--wad", type=Path, default=DEFAULT_WAD)
    ap.add_argument("--map", default="E1M1")
    ap.add_argument("--width", type=int, default=40)
    ap.add_argument("--height", type=int, default=28)
    ap.add_argument("-o", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()
    if not args.wad.is_file():
        print(f"missing WAD: {args.wad}", file=sys.stderr)
        return 1
    info = extract_grid(args.wad.read_bytes(), args.map, args.width, args.height)
    args.o.parent.mkdir(parents=True, exist_ok=True)
    text = emit_gcinc(info)
    args.o.write_text(text)
    print(f"[+] {args.o} ({info['w']}x{info['h']}, start {info['px']},{info['py']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
