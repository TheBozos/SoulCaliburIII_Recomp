#!/usr/bin/env python3
"""Drive PCSX2 (a private datapath) through SC3 with the port's pad scripts, as an external oracle.

DATAPATH is PCSX2's -datapath: the directory holding PCSX2/ (inis, bios, memcards, snaps).

  p2drive.py setup DATAPATH                  private settings: PINE slot, software GS, screenshots, audio capture
  p2drive.py run DATAPATH OUT [options]      boot (or load a state), play a pad script, take stills and sound

The clock is the game's own frame counter (word 0x46E608), read through PINE; on the port it equals the frame ends
the pad scripts and captures count. PCSX2 loads slower (IOP modules, the drive), so the same counter is not the same
moment: with --scenes (the log of a port run of the same script with SC3_TRACE_SCENE=1) every event is anchored on
the scene the game is in (word 0x4EE008, written by 0x1975E8) and fires as many frames after PCSX2 enters that scene,
for the same time, as it did on the port. Loads inside a scene take longer on PCSX2: --clocks adds anchors at the start
of game counters (SC3_TRACE_CLOCKS on the port), e.g. 0x4ED7F0 and 0x4B6BE4, which start counting once the fight's
stage is loaded and once the fight begins. Stills are named by the port frame they stand for (frame_NNNNNN.png).
The screenshot key goes one frame early (PCSX2 holds a screenshot asked while paused until it resumes, so it is not
paused); the journal keeps the counters, compare.py's best-offset window absorbs the rest.
Run it under a benchlock lease (`benchlock take ...`, then this); the window goes to workspace 9 unfocused.
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import Pine  # noqa: E402

FRAME_COUNTER = 0x46E608
SCENE = 0x4EE008
SLOT = int(os.environ.get("P2_SLOT", "28161"))
ISO = os.path.expanduser('~/Games/PS2/SoulcaliburIII/iso/Soulcalibur III (USA).iso')

KEYS = {'UP': 'Up', 'DOWN': 'Down', 'LEFT': 'Left', 'RIGHT': 'Right', 'TRIANGLE': 'i', 'CIRCLE': 'l', 'CROSS': 'k',
        'SQUARE': 'j', 'SELECT': 'BackSpace', 'START': 'Return', 'L1': 'q', 'L2': '1', 'R1': 'e', 'R2': '3', 'L3': '2',
        'R3': '4'}
PAD_BINDINGS = {'Up': 'Up', 'Down': 'Down', 'Left': 'Left', 'Right': 'Right', 'Triangle': 'I', 'Circle': 'L',
                'Cross': 'K', 'Square': 'J', 'Select': 'Backspace', 'Start': 'Return', 'L1': 'Q', 'L2': '1', 'R1': 'E',
                'R2': '3', 'L3': '2', 'R3': '4'}
HOTKEYS = {'Screenshot': 'F8', 'TogglePause': 'Space', 'FrameAdvance': 'F10', 'ToggleVideoCapture': 'F11',
           'GSDumpSingleFrame': 'Shift & Keyboard/F8'}

SETTINGS = {
    'UI': {'StartPaused': 'false', 'PauseOnFocusLoss': 'false', 'ConfirmShutdown': 'false'},
    'EmuCore': {'EnablePINE': 'true', 'PINESlot': str(SLOT), 'SaveStateOnShutdown': 'false', 'EnableFastBoot': 'true',
                'EnableCheats': 'false', 'EnableWideScreenPatches': 'false', 'EnableNoInterlacingPatches': 'false',
                'HostFs': 'false'},
    'EmuCore/GS': {'Renderer': '13', 'upscale_multiplier': '1', 'ScreenshotSize': '2', 'ScreenshotFormat': '0',
                   'OsdShowFPS': 'false', 'OsdShowVPS': 'false', 'OsdShowIndicators': 'false',
                   'OsdShowVideoCapture': 'false', 'OsdShowInputRec': 'false', 'OsdShowSpeed': 'false',
                   'EnableVideoCapture': 'false', 'EnableAudioCapture': 'true', 'CaptureContainer': 'wav',
                   'AudioCaptureCodec': 'pcm_s16le', 'EnableAudioCaptureParameters': 'false',
                   'OrganizeScreenshotsByGame': 'false', 'OrganizeVideoCaptureByGame': 'false',
                   'deinterlace_mode': '0', 'VsyncEnable': 'false'},
    'Hotkeys': {k: f'Keyboard/{v}' for k, v in HOTKEYS.items()},
    'Pad1': {k: f'Keyboard/{v}' for k, v in PAD_BINDINGS.items()} | {'Type': 'DualShock2'},
}


def set_ini(path, settings):
    lines = open(path).read().split('\n') if os.path.exists(path) else []
    for section, values in settings.items():
        head = f'[{section}]'
        if head not in lines:
            lines += ['', head]
        start = lines.index(head) + 1
        end = next((i for i in range(start, len(lines)) if lines[i].startswith('[')), len(lines))
        for key, value in values.items():
            hit = next((i for i in range(start, end) if re.match(rf'{re.escape(key)}\s*=', lines[i])), None)
            if hit is None:
                lines.insert(end, f'{key} = {value}')
                end += 1
            else:
                lines[hit] = f'{key} = {value}'
    open(path, 'w').write('\n'.join(lines))


def hypr(*args):
    env = dict(os.environ)
    if 'HYPRLAND_INSTANCE_SIGNATURE' not in env:
        env['HYPRLAND_INSTANCE_SIGNATURE'] = sorted(os.listdir(f'/run/user/{os.getuid()}/hypr'))[0]
    return subprocess.run(['hyprctl', *args], capture_output=True, text=True, env=env).stdout


def read_script(text):
    events = []
    for entry in re.split(r'[,\r\n]+', text.strip()):
        if entry:
            m = re.fullmatch(r'(\d+):([A-Z0-9]+)/(\d+)', entry.strip())
            if not m:
                raise SystemExit(f'malformed pad script entry {entry!r}')
            events.append((int(m[1]), m[2], int(m[3])))
    return events


class Session:
    def __init__(self, datapath, out, state=None, iso=ISO, fresh_card=True, sound=False):
        self.datapath, self.out = datapath, out
        if fresh_card:
            for card in ('Mcd001.ps2', 'Mcd002.ps2'):
                path = os.path.join(datapath, 'PCSX2', 'memcards', card)
                if os.path.exists(path):
                    os.remove(path)
        os.makedirs(out, exist_ok=True)
        self.snaps = os.path.join(datapath, 'PCSX2', 'snaps')
        self.videos = os.path.join(datapath, 'PCSX2', 'videos')
        for d in (self.snaps, self.videos):
            os.makedirs(d, exist_ok=True)
        env = dict(os.environ)
        try:
            for line in subprocess.run(['benchlock', 'env'], capture_output=True, text=True).stdout.splitlines():
                m = re.match(r'export (\w+)=(.*)', line)
                if m:
                    env[m[1]] = m[2].strip('\'"')
        except FileNotFoundError:
            pass
        set_ini(os.path.join(datapath, 'PCSX2', 'inis', 'PCSX2.ini'), {'SPU2/Output': {'Backend': 'SDL' if sound else 'Cubeb'}})
        if sound:
            raw = os.path.join(out, 'pcsx2_audio.raw')
            env.update({'SDL_AUDIO_DRIVER': 'disk', 'SDL_AUDIODRIVER': 'disk', 'SDL_AUDIO_DISK_OUTPUT_FILE': raw,
                        'SDL_DISKAUDIOFILE': raw})
        cmd = ['pcsx2-qt', '-datapath', datapath, '-batch', '-nofullscreen', '-logfile', os.path.join(out, 'pcsx2.log')]
        if state:
            cmd += ['-statefile', state]
        else:
            cmd += ['-fastboot']
        cmd += ['--', iso]
        self.log = open(os.path.join(out, 'pcsx2.stdout'), 'w')
        self.started = time.monotonic()
        self.proc = subprocess.Popen(cmd, stdout=self.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                                     start_new_session=True)
        self.address = None
        for _ in range(300):
            self._park()
            if self.address:
                break
            time.sleep(0.1)
        try:
            self.pine = Pine(SLOT, timeout=60)
        except OSError:
            self.stop()
            raise
        self.counter_seen = 0
        self.held = {}

    def _park(self):
        clients = json.loads(hypr('clients', '-j') or '[]')
        for c in clients:
            if c['pid'] == self.proc.pid:
                if c['workspace']['name'] != '9':
                    hypr('dispatch', f'hl.dsp.window.move({{ workspace = "9", follow = false, window = "address:{c["address"]}" }})')
                if c['title'] and not c['title'].startswith('PCSX2'):
                    self.address = c['address']
                elif not self.address:
                    self.address = c['address']

    def key(self, name, down, mods=''):
        hypr('dispatch', f'hl.dsp.send_key_state({{mods="{mods}", key="{name}", state="{"down" if down else "up"}", '
                         f'window="address:{self.address}"}})')

    def tap(self, name, hold=0.08):
        self.key(name, True)
        time.sleep(hold)
        self.key(name, False)

    def counter(self):
        try:
            return self.pine.read32(FRAME_COUNTER)
        except RuntimeError:
            return 0

    def stop(self):
        if hasattr(self, 'pine'):
            self.pine.close()
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(20)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.log.close()


def read_scenes(path):
    """The port's scene changes and clock starts (SC3_TRACE_SCENE=1, SC3_TRACE_CLOCKS): [(frame, 'scene 0x8'), ...]."""
    out = []
    for line in open(path, errors='replace'):
        m = re.search(r'\[sc3:scene\] frame (\d+) (scene 0x[0-9a-f]+|clock 0x[0-9a-f]+)', line)
        if m:
            out.append((int(m[1]), m[2]))
    return out


class Anchors:
    """Port frame -> (scene, its occurrence, frames into it): an event is due that many frames after PCSX2 enters the
    same scene for the same time. The scene is the last one entered strictly before the event's frame (a press that
    changes the scene is logged at the frame it is pressed). Without a scene list, the counter alone (plus an offset)."""

    def __init__(self, scenes, offset):
        self.scenes, self.offset = scenes, offset
        self.seen = {}
        self.entered = {}

    def anchor(self, frame):
        if not self.scenes:
            return None, frame + self.offset
        best, counts = None, {}
        for f, scene in self.scenes:
            counts[scene] = counts.get(scene, 0) + 1
            if f < frame:
                best = ((scene, counts[scene]), frame - f)
        return best if best else (None, frame + self.offset)

    def note(self, scene, counter):
        n = self.seen.get(scene, 0) + 1
        self.seen[scene] = n
        self.entered[(scene, n)] = counter

    def due(self, key, delta):
        if key is None:
            return delta
        start = self.entered.get(key)
        return None if start is None else start + delta


def run(args):
    events = read_script(open(args.script).read()) if args.script else []
    anchors = Anchors(read_scenes(args.scenes) if args.scenes else [], args.offset)
    todo = []
    for f, button, hold in events:
        todo.append([f, 1, KEYS[button]])
        todo.append([f + hold, 0, KEYS[button]])
    for f in sorted(int(x) for x in args.stills.split(',')) if args.stills else []:
        todo.append([f - 1, 2, f])
    for f in (int(x) for x in args.gsdump.split(',')) if args.gsdump else []:
        todo.append([f - 1, 4, f])
    for a, b in args.audio or []:
        todo.append([a, 3, 'start'])
        todo.append([b, 3, 'stop'])
    if args.until:
        todo.append([args.until, 5, None])
    for t in todo:
        t[0:1] = [t[0], *anchors.anchor(t[0])]
    todo.sort(key=lambda t: (t[0], t[3]))
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    s = Session(args.datapath, args.out, args.state, fresh_card=not args.keep_card, sound=args.sound)
    log = open(os.path.join(args.out, 'journal.txt'), 'w')

    class Journal:
        @staticmethod
        def write(text):
            log.write(f't={time.monotonic() - s.started:.3f} {text}')

        @staticmethod
        def flush():
            log.flush()

        @staticmethod
        def close():
            log.close()

    journal = Journal()
    shots, dumps = [], []
    before = set(os.listdir(s.snaps))
    try:
        if s.address is None:
            raise SystemExit('PCSX2 window not found')
        scene, done = None, False
        clocks = [int(x, 16) for x in args.clocks.split(',')] if args.clocks else []
        clock_values = [0] * len(clocks)
        watches = args.watch.split(',') if args.watch else []
        watch_values = [None] * len(watches)
        deadline = time.monotonic() + args.timeout
        while not done and time.monotonic() < deadline:
            c = s.counter()
            try:
                now = s.pine.read32(SCENE)
            except RuntimeError:
                now = None
            if now is not None and now != scene and c > 0:
                scene = now
                anchors.note(f'scene 0x{scene:x}', c)
                journal.write(f'scene 0x{scene:x} counter {c}\n')
            for i, a in enumerate(clocks):
                try:
                    v = s.pine.read32(a)
                except RuntimeError:
                    continue
                if v and (not clock_values[i] or v < clock_values[i]):
                    anchors.note(f'clock 0x{a:x}', c)
                    journal.write(f'clock 0x{a:x} starts counter {c}\n')
                clock_values[i] = v
            for i, w in enumerate(watches):
                try:
                    base, _, field = w.partition('>')
                    a = int(base, 16)
                    if field:
                        a = s.pine.read32(a) + int(field, 16)
                    v = s.pine.read32(a)
                except RuntimeError:
                    continue
                if v != watch_values[i]:
                    watch_values[i] = v
                    journal.write(f'watch {w} = 0x{v:08x} counter {c}\n')
            while todo:
                port, key, delta, kind, what = todo[0]
                at = anchors.due(key, delta)
                if at is None or at > c:
                    break
                todo.pop(0)
                if kind in (0, 1):
                    s.key(what, kind == 1)
                elif kind == 2:
                    s.tap('F8', 0.02)
                    shots.append(what)
                elif kind == 4:
                    s.key('Shift_L', True)
                    s.key('F8', True, 'SHIFT')
                    time.sleep(0.1)
                    s.key('F8', False, 'SHIFT')
                    s.key('Shift_L', False)
                    dumps.append(what)
                elif kind == 3:
                    pass
                elif kind == 5:
                    done = True
                journal.write(f'{"up down still audio gsdump until".split()[kind]} {what} port {port} due {at} counter {c} '
                              f'counter_after {s.counter()}\n')
                journal.flush()
            if not todo:
                done = True
            time.sleep(0.002)
        time.sleep(2)
        if args.save_state is not None:
            s.pine.save_state(args.save_state)
            time.sleep(3)
    finally:
        journal.close()
        s.stop()
    new = sorted((f for f in set(os.listdir(s.snaps)) - before), key=lambda f: os.path.getmtime(os.path.join(s.snaps, f)))
    gs = [f for f in new if '.gs' in f]
    stems = {f.split('.gs', 1)[0] for f in gs}
    pngs = [f for f in new if f.endswith('.png') and f[:-4] not in stems]
    for f, port in zip(pngs, shots):
        shutil.move(os.path.join(s.snaps, f), os.path.join(args.out, f'frame_{port:06d}.png'))
    for f, port in zip(gs, dumps):
        shutil.move(os.path.join(s.snaps, f), os.path.join(args.out, f'gsdump_{port:06d}.gs' + f.split('.gs', 1)[1]))
    if len(pngs) != len(shots) or len(gs) != len(dumps):
        print(f'warning: {len(pngs)} screenshots for {len(shots)} stills, {len(gs)} dumps for {len(dumps)}', file=sys.stderr)
    for f in sorted(os.listdir(s.videos)):
        if f.endswith('.wav'):
            shutil.move(os.path.join(s.videos, f), os.path.join(args.out, f))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('setup')
    p.add_argument('datapath')
    p = sub.add_parser('run')
    p.add_argument('datapath')
    p.add_argument('out')
    p.add_argument('--script', help='pad script, port syntax frame:BUTTON/hold,...')
    p.add_argument('--stills', help='port frame numbers to shoot')
    p.add_argument('--gsdump', help='port frame numbers to GS-dump (one frame each)')
    p.add_argument('--scenes', help='log of a port run with SC3_TRACE_SCENE=1: events anchored on scene changes')
    p.add_argument('--clocks', help='hex words the port traced with SC3_TRACE_CLOCKS: anchors at their starts too')
    p.add_argument('--watch', help='hex words to log on change; A>F reads the word at *(A) + F')
    p.add_argument('--offset', type=int, default=0, help='without --scenes: counter = port frame + offset')
    p.add_argument('--timeout', type=float, default=1800, help='seconds')
    p.add_argument('--audio', nargs=2, type=int, action='append', metavar=('FROM', 'TO'), help='marks in the journal')
    p.add_argument('--until', type=int, help='run until this port frame')
    p.add_argument('--state', help='savestate to start from')
    p.add_argument('--sound', action='store_true', help='PCSX2 through SDL\'s disk audio driver: OUT/pcsx2_audio.raw, the whole run')
    p.add_argument('--keep-card', action='store_true', help='keep the memory card (default: a new formatted one, as the port)')
    p.add_argument('--save-state', type=int, help='save to this slot at the end')
    args = ap.parse_args()
    if args.cmd == 'setup':
        set_ini(os.path.join(args.datapath, 'PCSX2', 'inis', 'PCSX2.ini'), SETTINGS)
    else:
        run(args)


if __name__ == '__main__':
    main()
