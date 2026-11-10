#!/usr/bin/env -S uv run --script
# /// script
# dependencies = ["numpy"]
# ///
"""Compare the port's sound with PCSX2's on a few fixed screens.

  audio_compare.py PORT.wav PORT.log PCSX2_OUT POINTS [--search S]

PORT.wav: the port's --wav (48 kHz stereo s16 from frame 0, 800 samples a frame); PORT.log: that run's log with
SC3_TRACE_SCENE=1. PCSX2_OUT: a p2drive.py run with --sound (pcsx2_audio.raw: SDL's disk driver, float32 stereo 48 kHz
from the device's opening) and its journal (every line timed from PCSX2's launch). POINTS: `label from to` in port
frames. A port frame goes to PCSX2's counter by the same scene and clock anchors as the driver, the counter to a time by
the journal; the segment is then found by cross-correlating envelopes within +-S seconds. Printed per segment: the lag
found, the envelope correlation, each level (RMS dBFS), their difference and the cosine of their log spectra.
"""
import argparse
import os
import re
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from p2drive import Anchors, read_scenes  # noqa: E402

RATE = 48000


def load_port(path):
    w = wave.open(path)
    raw = np.frombuffer(w.readframes(w.getnframes()), '<i2').astype(np.float64).reshape(-1, w.getnchannels())
    return raw.mean(axis=1) / 32768.0


def load_pcsx2(path):
    raw = np.fromfile(path, dtype='<f4')
    return raw[:len(raw) // 2 * 2].reshape(-1, 2).mean(axis=1).astype(np.float64)


def envelope(x, hop=480):
    n = len(x) // hop
    return np.sqrt((x[:n * hop].reshape(n, hop) ** 2).mean(axis=1) + 1e-12)


def spectrum(x):
    n = 4096
    frames = [x[i:i + n] * np.hanning(n) for i in range(0, len(x) - n, n // 2)]
    if not frames:
        return np.zeros(n // 2 + 1)
    return np.log10(np.mean([np.abs(np.fft.rfft(f)) ** 2 for f in frames], axis=0) + 1e-12)


def db(x):
    return 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-12)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port_wav')
    ap.add_argument('port_log')
    ap.add_argument('pcsx2_out')
    ap.add_argument('points')
    ap.add_argument('--search', type=float, default=3.0)
    args = ap.parse_args()
    port = load_port(args.port_wav)
    ref = load_pcsx2(os.path.join(args.pcsx2_out, 'pcsx2_audio.raw'))
    anchors = Anchors(read_scenes(args.port_log), 0)
    timeline = []
    for line in open(os.path.join(args.pcsx2_out, 'journal.txt')):
        m = re.match(r't=([0-9.]+) (scene 0x[0-9a-f]+|clock 0x[0-9a-f]+)\S* .*counter (\d+)', line)
        if m:
            anchors.note(m[2], int(m[3]))
        m2 = re.match(r't=([0-9.]+) .*counter (\d+)', line)
        if m2:
            timeline.append((int(m2[2]), float(m2[1])))
    timeline.sort()
    opened = None
    for line in open(os.path.join(args.pcsx2_out, 'pcsx2.log'), errors='replace'):
        m = re.match(r'\[\s*([0-9.]+)\] Creating SDL audio stream', line)
        if m:
            opened = float(m[1])
    counters = np.array([c for c, _ in timeline], dtype=np.float64)
    times = np.array([t for _, t in timeline])
    print('| segment | port frames | lag s | envelope corr | port dBFS | PCSX2 dBFS | diff dB | spectrum cos |')
    print('|---|---|---|---|---|---|---|---|')
    for line in open(args.points):
        f = line.split('#')[0].split()
        if len(f) < 3:
            continue
        label, a, b = f[0], int(f[1]), int(f[2])
        seg = port[a * 800:b * 800]
        key, delta = anchors.anchor(a)
        counter = anchors.due(key, delta)
        if counter is None or opened is None:
            print(f'| {label} | {a}-{b} | not reached on PCSX2 | | | | | |')
            continue
        guess = float(np.interp(counter, counters, times)) - opened
        lo = max(0, int((guess - args.search) * RATE))
        hi = min(len(ref), int((guess + args.search) * RATE) + len(seg))
        hay = ref[lo:hi]
        if len(hay) < len(seg) or len(seg) == 0:
            print(f'| {label} | {a}-{b} | no PCSX2 audio there | | | | | |')
            continue
        e1, e2 = envelope(seg), envelope(hay)
        e1 = (e1 - e1.mean()) / (e1.std() + 1e-12)
        best, lag = -2.0, 0
        for k in range(0, len(e2) - len(e1) + 1):
            w = e2[k:k + len(e1)]
            c = float(np.dot(e1, (w - w.mean()) / (w.std() + 1e-12)) / len(e1))
            if c > best:
                best, lag = c, k
        other = hay[lag * 480:lag * 480 + len(seg)]
        s1, s2 = spectrum(seg), spectrum(other)
        cos = float(np.dot(s1 - s1.mean(), s2 - s2.mean()) / (np.linalg.norm(s1 - s1.mean()) * np.linalg.norm(s2 - s2.mean()) + 1e-12))
        print(f'| {label} | {a}-{b} | {(lo / RATE + lag * 480 / RATE) - guess:+.2f} | {best:.3f} | {db(seg):.1f} | {db(other):.1f} | '
              f'{db(seg) - db(other):+.1f} | {cos:.3f} |')


if __name__ == '__main__':
    main()
