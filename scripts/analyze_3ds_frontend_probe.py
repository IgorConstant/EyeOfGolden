#!/usr/bin/env python3
"""Summarize a scripted frontend (menu) probe result.

Frame rows carry exact ARM11 tick counters. A frame misses the 60 Hz budget
when work * 60 > clock_hz; a start interval above 1.5 display periods means
the emulated loop skipped a VBlank. Capture frames are excluded from the
peak because the SD write is diagnostic-only.
"""
import argparse
import json
import math
from pathlib import Path


def summarize(path, warmup):
    values = {}
    frames = []
    menus = []
    captures = []
    steps = []
    for line in path.read_text().splitlines():
        key, separator, value = line.partition('=')
        if key == 'frontend_frame':
            row = [int(item) for item in value.split(',')]
            if len(row) not in (6, 10, 11):
                raise ValueError(f'{path}: invalid frontend frame row')
            frames.append(row)
        elif key == 'menu_summary':
            menus.append(value.split(','))
        elif key == 'capture':
            captures.append(value.split(','))
        elif key == 'step_done':
            steps.append([int(item) for item in value.split(',')])
        elif separator:
            values[key] = value
    hz = int(values['clock_hz'])
    if len(frames) != int(values['frontend_frame_count']):
        raise ValueError(f'{path}: missing frame rows')
    if [row[0] for row in frames] != list(range(1, len(frames) + 1)):
        raise ValueError(f'{path}: nonsequential frame rows')
    capture_frames = {int(row[1]) + 2 for row in captures}
    warm = [row for row in frames[warmup:] if row[0] not in capture_frames]

    def ms(ticks):
        return round(ticks * 1000 / hz, 3)

    def stats(rows, column):
        ticks = sorted(row[column] for row in rows)
        if not ticks:
            return None
        return {
            'mean_ms': ms(sum(ticks) / len(ticks)),
            'p95_ms': ms(ticks[math.ceil(len(ticks) * .95) - 1]),
            'peak_ms': ms(ticks[-1]),
        }

    per_menu = {}
    for row in warm:
        per_menu.setdefault(row[1], []).append(row)
    menu_names = {int(entry[0]): entry[1] for entry in menus}
    report = {
        'status': values.get('status'),
        'failure': values.get('failure'),
        'frames': len(frames),
        'warm_frames': len(warm),
        'steps': values.get('steps'),
        'final_menu': values.get('final_menu'),
        'stage_requested': values.get('stage_requested'),
        'captures': values.get('captures'),
        'work': stats(warm, 2),
        'frame_begin_wait': stats(warm, 3),
        'vblank_wait': stats(warm, 4),
        'work_budget_misses': sum(1 for row in warm if row[2] * 60 > hz),
        'multi_vblank_intervals': sum(
            1 for row in warm if row[5] * 40 > hz),
        'menus': {
            f'{menu:02d}-{menu_names.get(menu, "unknown")}': {
                'frames': len(rows),
                'work': stats(rows, 2),
                'work_budget_misses': sum(1 for r in rows if r[2] * 60 > hz),
                'multi_vblank_intervals': sum(1 for r in rows if r[5] * 40 > hz),
                'phase_mean_ms': [
                    ms(sum(r[6 + p] for r in rows) / len(rows))
                    for p in range(len(rows[0]) - 6)
                ] if len(rows[0]) > 6 else None,
            }
            for menu, rows in sorted(per_menu.items())
        },
        'worst_frames': [
            {'frame': row[0], 'menu': row[1], 'work_ms': ms(row[2])}
            for row in sorted(warm, key=lambda r: -r[2])[:5]
        ],
    }
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('result', type=Path)
    parser.add_argument('--warmup', type=int, default=0,
                        help='frontend frames excluded from statistics')
    args = parser.parse_args()
    print(json.dumps(summarize(args.result, args.warmup), indent=2))


if __name__ == '__main__':
    main()
