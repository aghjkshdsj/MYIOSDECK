#!/usr/bin/env python3
"""Render the MYIOSDECK app icon (1024x1024 opaque PNG) with no dependencies."""
import math, struct, sys, zlib

N = 1024
BG0, BG1 = (14, 20, 27), (22, 38, 56)
ACCENT = (26, 159, 255)
BODY = (32, 42, 54)

def sd_round_rect(x, y, cx, cy, hw, hh, r):
    dx, dy = abs(x - cx) - (hw - r), abs(y - cy) - (hh - r)
    ox, oy = max(dx, 0.0), max(dy, 0.0)
    return math.hypot(ox, oy) + min(max(dx, dy), 0.0) - r

def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

def cov(d):  # anti-aliased coverage from a signed distance
    return max(0.0, min(1.0, 0.5 - d))

def pixel(x, y):
    t = (x + y) / (2 * N)
    c = mix(BG0, BG1, t)
    # glow behind the device
    g = math.exp(-((x - 512) ** 2 + (y - 540) ** 2) / (2 * 300.0 ** 2))
    c = mix(c, ACCENT, 0.18 * g)
    # handheld body
    d = sd_round_rect(x, y, 512, 540, 430, 200, 120)
    c = mix(c, BODY, cov(d))
    # accent outline
    c = mix(c, ACCENT, cov(abs(d) - 9))
    # screen
    s = sd_round_rect(x, y, 512, 540, 200, 130, 22)
    c = mix(c, (8, 12, 17), cov(s))
    # play triangle on the screen
    px, py = x - 500, y - 540
    tri = max(-px - 55, abs(py) * 1.15 + px * 0.6 - 60)
    c = mix(c, ACCENT, cov(tri))
    # thumbsticks
    for sx in (232, 792):
        dd = math.hypot(x - sx, y - 500) - 52
        c = mix(c, (12, 16, 22), cov(dd))
        c = mix(c, ACCENT, cov(abs(dd) - 5))
    # d-pad dots / face buttons
    for bx, by in ((222, 630), (802, 610), (842, 650), (762, 650), (802, 690)):
        c = mix(c, ACCENT if bx > 500 else (60, 72, 86), cov(math.hypot(x - bx, y - by) - 15))
    return c

def main(out):
    rows = []
    for y in range(N):
        row = bytearray(b"\x00")
        for x in range(N):
            row.extend(pixel(x + 0.5, y + 0.5))
        rows.append(bytes(row))
    raw = zlib.compress(b"".join(rows), 9)
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 2, 0, 0, 0)) + chunk(b"IDAT", raw) + chunk(b"IEND", b"")
    open(out, "wb").write(png)

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "App/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png")
