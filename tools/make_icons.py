#!/usr/bin/env python3
"""Generate the PWA icons (stdlib only). Usage: python3 tools/make_icons.py [outdir]"""
import struct, sys, zlib, os

BG = (11, 92, 171)
FG = (255, 255, 255)
FLAME = (255, 150, 40)


def png(path, size, safe):
    # safe = fraction of the canvas used by the glyph (maskable needs the glyph inside ~60 %)
    px = bytearray()
    cx = size / 2
    r = size * safe / 2
    for y in range(size):
        px.append(0)
        for x in range(size):
            dx, dy = (x - cx) / r, (y - cx) / r
            col = BG
            # house-like heater glyph: rounded box with flame
            if abs(dx) < 0.8 and abs(dy) < 0.8 and (abs(dx) > 0.62 or abs(dy) > 0.62):
                col = FG
            # flame: teardrop approximated by a width that shrinks upward
            if -0.45 < dy < 0.45:
                w = 0.32 * (1 - ((dy + 0.45) / 0.9) ** 1.5) if dy < 0.25 else 0.32 * (1 - (dy - 0.25) / 0.2) ** 0.5
                if abs(dx) < w:
                    col = FLAME
            px.extend(col)

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)

    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) \
        + chunk(b'IDAT', zlib.compress(bytes(px), 9)) + chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(data)


out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'pwa', 'icons')
os.makedirs(out, exist_ok=True)
png(os.path.join(out, 'icon-192.png'), 192, 0.92)
png(os.path.join(out, 'icon-512.png'), 512, 0.92)
png(os.path.join(out, 'icon-maskable-512.png'), 512, 0.62)
