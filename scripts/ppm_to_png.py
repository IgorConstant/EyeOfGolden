#!/usr/bin/env python3
"""Convert binary PPM (P6) captures written by the 3DS probes to PNG.

Dependency-free so it runs on any host Python. Usage:
    scripts/ppm_to_png.py capture.ppm [more.ppm ...]  -> capture.png ...
    scripts/ppm_to_png.py --scale 2 capture.ppm       -> nearest-neighbor 2x
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path


def read_ppm(path):
    data = path.read_bytes()
    tokens = []
    position = 0
    while len(tokens) < 4:
        while data[position:position + 1].isspace():
            position += 1
        if data[position:position + 1] == b'#':
            while data[position:position + 1] not in (b'\n', b''):
                position += 1
            continue
        start = position
        while not data[position:position + 1].isspace():
            position += 1
        tokens.append(data[start:position])
    position += 1
    if tokens[0] != b'P6' or int(tokens[3]) != 255:
        raise ValueError(f'{path}: unsupported PPM header')
    width, height = int(tokens[1]), int(tokens[2])
    pixels = data[position:position + width * height * 3]
    if len(pixels) != width * height * 3:
        raise ValueError(f'{path}: truncated pixel data')
    return width, height, pixels


def write_png(path, width, height, pixels, scale=1):
    def chunk(kind, payload):
        return (struct.pack('>I', len(payload)) + kind + payload
                + struct.pack('>I', zlib.crc32(kind + payload) & 0xffffffff))
    rows = []
    stride = width * 3
    for y in range(height):
        row = pixels[y * stride:(y + 1) * stride]
        if scale != 1:
            row = b''.join(row[x * 3:x * 3 + 3] * scale for x in range(width))
        rows.extend([b'\0' + row] * scale)
    raw = b''.join(rows)
    header = struct.pack('>IIBBBBB', width * scale, height * scale, 8, 2, 0, 0, 0)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header)
                     + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', nargs='+', type=Path)
    parser.add_argument('--scale', type=int, default=1)
    parser.add_argument('--out-dir', type=Path)
    args = parser.parse_args()
    for source in args.inputs:
        width, height, pixels = read_ppm(source)
        target = (args.out_dir or source.parent) / (source.stem + '.png')
        target.parent.mkdir(parents=True, exist_ok=True)
        write_png(target, width, height, pixels, args.scale)
        print(target)


if __name__ == '__main__':
    sys.exit(main())
