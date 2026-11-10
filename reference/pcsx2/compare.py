#!/usr/bin/env -S uv run --script
# /// script
# dependencies = ["numpy", "pillow"]
# ///
"""Compare the port's window captures with PCSX2's stills of the same moments.

  compare.py PORT_CAPS PCSX2_OUT POINTS [--window LO HI] [--diff DIR] [--md]

POINTS: a file of `label frame [pcsx2 frame]` lines (or `label frame` with the PCSX2 frame given by a sync table, see
sync.py). For each point the PCSX2 still is compared with port frames frame+k, k in LO..HI, and the best k is kept.
Both pictures are cut to the 4:3 picture and scaled to 640x448. Printed per point: best k, PSNR, the share of pixels off
by more than 24 (of 255) in any channel, the mean colour offset (port minus PCSX2, per channel), and the 4 worst tiles
of an 8x8 grid (column, row, mean abs difference). --diff writes a side-by-side and an amplified difference picture.
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

W, H = 640, 448
GRID = 8


def load(path):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    if w * 3 > h * 4:
        cw, ch = h * 4 // 3, h
    else:
        cw, ch = w, w * 3 // 4
    left, top = (w - cw) // 2, (h - ch) // 2
    im = im.crop((left, top, left + cw, top + ch)).resize((W, H), Image.BILINEAR)
    return np.asarray(im, dtype=np.float32)


def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


def measure(port, ref):
    d = port - ref
    ad = np.abs(d)
    tiles = []
    th, tw = H // GRID, W // GRID
    for r in range(GRID):
        for c in range(GRID):
            tiles.append((float(ad[r * th:(r + 1) * th, c * tw:(c + 1) * tw].mean()), c, r))
    tiles.sort(reverse=True)
    return {
        'psnr': psnr(port, ref),
        'visible': float(np.mean(ad.max(axis=2) > 24)) * 100,
        'bias': d.reshape(-1, 3).mean(axis=0),
        'tiles': tiles[:4],
        'mean_port': port.reshape(-1, 3).mean(axis=0),
        'mean_ref': ref.reshape(-1, 3).mean(axis=0),
    }


def read_points(path):
    pts = []
    for line in open(path):
        line = line.split('#')[0].split()
        if len(line) >= 2:
            pts.append((line[0], int(line[1]), int(line[2]) if len(line) > 2 else int(line[1])))
    return pts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port_caps')
    ap.add_argument('pcsx2_out')
    ap.add_argument('points')
    ap.add_argument('--window', nargs=2, type=int, default=(-6, 6))
    ap.add_argument('--diff')
    args = ap.parse_args()
    lo, hi = args.window
    print('| point | port frame | pcsx2 frame | best k | PSNR | visible % | bias R G B | port mean | pcsx2 mean | worst tiles (c,r: mad) |')
    print('|---|---|---|---|---|---|---|---|---|---|')
    for label, frame, ref_frame in read_points(args.points):
        rp = os.path.join(args.pcsx2_out, f'frame_{ref_frame:06d}.png')
        if not os.path.exists(rp):
            print(f'| {label} | {frame} | {ref_frame} | no PCSX2 still | | | | | | |')
            continue
        ref = load(rp)
        best = None
        for k in range(lo, hi + 1):
            pp = os.path.join(args.port_caps, f'frame_{frame + k:06d}.png')
            if os.path.exists(pp):
                port = load(pp)
                m = measure(port, ref)
                if best is None or m['psnr'] > best[1]['psnr']:
                    best = (k, m, port)
        if best is None:
            print(f'| {label} | {frame} | {ref_frame} | no port capture | | | | | | |')
            continue
        k, m, port = best
        tiles = ' '.join(f'{c},{r}:{v:.0f}' for v, c, r in m['tiles'])
        fmt = lambda v: ' '.join(f'{x:+.1f}' for x in v)
        print(f'| {label} | {frame} | {ref_frame} | {k:+d} | {m["psnr"]:.2f} | {m["visible"]:.1f} | {fmt(m["bias"])} | '
              f'{" ".join(f"{x:.0f}" for x in m["mean_port"])} | {" ".join(f"{x:.0f}" for x in m["mean_ref"])} | {tiles} |')
        if args.diff:
            os.makedirs(args.diff, exist_ok=True)
            amp = np.clip(np.abs(port - ref) * 4, 0, 255)
            side = np.concatenate([ref, port, amp], axis=1).astype(np.uint8)
            Image.fromarray(side).save(os.path.join(args.diff, f'{label}.png'))
        sys.stdout.flush()


if __name__ == '__main__':
    main()
