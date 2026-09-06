#!/usr/bin/env python3
"""Measure horizontal disparity between left/right eye PPM captures.

For each horizontal band the best integer shift of the right image relative
to the left is found by minimizing the mean absolute difference over the
pillarboxed viewport. Perspective geometry near the bottom of the screen
(the floor, close to the camera) should shift one way and far geometry the
other way; a mono pair reports zero everywhere.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ppm_to_png import read_ppm  # noqa: E402


def luma_rows(width, height, pixels):
    rows = []
    for y in range(height):
        row = pixels[y * width * 3:(y + 1) * width * 3]
        rows.append([(row[i] * 299 + row[i + 1] * 587 + row[i + 2] * 114) // 1000
                     for i in range(0, len(row), 3)])
    return rows


def band_shift(left, right, y0, y1, x0, x1, max_shift):
    best = None
    for shift in range(-max_shift, max_shift + 1):
        total = 0
        count = 0
        for y in range(y0, y1):
            lrow = left[y]
            rrow = right[y]
            for x in range(x0 + max_shift, x1 - max_shift):
                total += abs(lrow[x] - rrow[x + shift])
                count += 1
        score = total / max(count, 1)
        if best is None or score < best[1]:
            best = (shift, score)
    return best


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('left', type=Path)
    parser.add_argument('right', type=Path)
    parser.add_argument('--bands', type=int, default=6)
    parser.add_argument('--max-shift', type=int, default=24)
    parser.add_argument('--viewport', default='40,360',
                        help='x0,x1 of the original viewport in screen pixels')
    args = parser.parse_args()
    lw, lh, lp = read_ppm(args.left)
    rw, rh, rp = read_ppm(args.right)
    if (lw, lh) != (rw, rh):
        raise SystemExit('capture sizes differ')
    left = luma_rows(lw, lh, lp)
    right = luma_rows(rw, rh, rp)
    x0, x1 = (int(v) for v in args.viewport.split(','))
    identical = lp == rp
    print(f'identical={identical}')
    band_height = lh // args.bands
    for band in range(args.bands):
        y0 = band * band_height
        y1 = lh if band == args.bands - 1 else y0 + band_height
        shift, score = band_shift(left, right, y0, y1, x0, x1, args.max_shift)
        print(f'rows {y0:3d}-{y1:3d}: right shifted {shift:+d} px (mad {score:.1f})')


if __name__ == '__main__':
    main()
