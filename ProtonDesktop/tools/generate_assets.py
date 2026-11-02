#!/usr/bin/env python3
"""Original geometric product tiles. No third-party artwork or fonts."""
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]

def chunk(name, data):
    return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))

def rounded(x, y, x0, y0, x1, y1, radius):
    if not (x0 <= x <= x1 and y0 <= y <= y1):
        return False
    cx = min(max(x, x0 + radius), x1 - radius)
    cy = min(max(y, y0 + radius), y1 - radius)
    return (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2

def tile(path, width, height):
    rows = bytearray()
    scale = min(width, height) / 100
    offset = (width - min(width, height)) / 2
    for py in range(height):
        rows.append(0)
        for px in range(width):
            x, y = (px - offset) / scale, py / scale
            rgb = (11, 36, 74, 255)
            if rounded(x, y, 7, 7, 93, 93, 22):
                rgb = (36, 105, 213, 255)
            stem = 30 <= x <= 42 and 25 <= y <= 77
            bowl = rounded(x, y, 35, 25, 74, 59, 15)
            hole = rounded(x, y, 42, 35, 62, 49, 5)
            if stem or (bowl and not hole):
                rgb = (242, 248, 255, 255)
            rows.extend(rgb)
    data = b'\x89PNG\r\n\x1a\n'
    data += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
    data += chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b'')
    path.write_bytes(data)

if __name__ == '__main__':
    assets = ROOT / 'Assets'
    assets.mkdir(exist_ok=True)
    for name, size in {'Logo44.png': (44, 44), 'Logo150.png': (150, 150),
                       'StoreLogo.png': (50, 50), 'WideLogo.png': (310, 150),
                       'SplashScreen.png': (620, 300)}.items():
        tile(assets / name, *size)
