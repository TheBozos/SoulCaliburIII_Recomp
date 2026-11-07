#!/usr/bin/env python3
"""sc3_build: from the player's own Soulcalibur III disc (USA, SLUS-21216) to a game folder that runs, in one
command. Nothing derived from the game is distributed: the disc's files, the generated code, the packs and the
executable are all made here, on the player's PC. The game is built on BozoEngine (the BozoEngine submodule, at the
commit this code pins; --engine <dir> for another tree).

  python scripts/sc3_build.py --disc "Soulcalibur III (USA).iso" --out D:/SC3
  python scripts/sc3_build.py --disc D:/SC3/disc_files --out D:/SC3          (an extracted disc folder, its .iso beside it)

Steps (each skipped when an earlier run finished it with the same inputs; --redo <step> runs it and the ones after
again; within a step CMake and Ninja rebuild only what changed):
  verify     the disc image or folder: ISO9660 (.iso, .bin/.cue), SYSTEM.CNF's boot file, SLUS_212.16's SHA-1
  extract    the files the build reads into <out>/disc: SLUS_212.16, ROOT.OLK (the packs' input) and the three sound
             modules IRX/CRI_ADXI.IRX, NUSOUND.IRX, SDRDRV.IRX (translated at build time), no outside tool (BozoEngine's tools/native_pack/ps2_disc.py); and the image itself as <game>/disc.iso (the
             game reads the disc from it): a hard link to the .iso on the same drive, else a copy (a .bin / .cue
             converted)
  configure  cmake (Ninja) of SC3 on BozoEngine (cmake/bozo.cmake, SC3_BOZO=ON): SDL3 and Dear ImGui from the kit's
             deps/, FFmpeg (the movies) from the kit's ffmpeg/
  tools      BozoEngine's recompiler and externals writer (bozo-recomp, bozo-externals), built from its sources
  recompile  the externals list (the ELF's library symbols named by recomp/sc3_symbols.txt, SC3's overrides,
             recomp/sc3_stubs.txt) and bozo-recomp -> <out>/build/sc3_gen
  packs      the texture, mesh and 2D packs (scripts/sc3_texture_pack.py, sc3_mesh_pack.py, sc3_2d_pack.py on
             BozoEngine's tools/native_pack) -> <out>/build/packs
  compile    the game (sc3: SDL3, OpenGL 3.3) with its sound modules
  stage      <game>: sc3.exe, SLUS_212.16, sc3_symbols.txt, packs/, disc.iso, "Play SC3.cmd"

The build kit (scripts/make_build_kit.py): a folder with kit_manifest.json, the compiler (ucrt64/), a minimal static
FFmpeg (ffmpeg/), the libraries' sources (deps/: SDL3, Dear ImGui) and an embeddable Python. Used when it sits in
<SoulCaliburIII_Recomp>/kit or next to the SoulCaliburIII_Recomp folder (<parent>/kit), or with --kit <dir> (--no-kit: not used). With it
nothing else is needed (no MSYS2, git, network or installed Python), and the game is the player's build: statically
linked (no DLL), FFmpeg from the kit.
  kit/python/python.exe -I scripts/sc3_build.py --disc "Soulcalibur III (USA).iso" --out D:/SC3

Without a kit: --toolchain <dir> (a folder whose bin/ has g++, cmake, ninja: an MSYS2 UCRT64 prefix such as
C:/msys64/ucrt64), else the PATH, else C:/msys64/ucrt64; FFmpeg through pkg-config; SDL3 and ImGui downloaded by CMake
(pinned URLs and SHA-256) unless --fetch-cache <dir> holds sdl3-src and imgui-src. --toolchain and --fetch-cache also
override the kit's.

--game <dir> stages the game somewhere else than <out>/SC3 (the launcher: <release>/game). --check only checks the disc
(no work folder): OK <message>, or ERROR verify <message> and exit code 1.
Every successful run writes <game>/built_from.ini: the release stamp (<SoulCaliburIII_Recomp>/release.ini, from
scripts/make_release.py) of the code the game was built from; the launcher offers Rebuild when the source's differs.

Output for a launcher, one line each on stdout (everything else goes to <out>/logs/<step>.log):
  PROGRESS <step> <i>/<n> <message>     a step starts
  PERCENT <step> <0-100>                within a long step (extract, packs, compile)
  SKIP <step> <i>/<n>                   finished by an earlier run
  DONE <step> <i>/<n> <seconds>
  ERROR <step> <message>                then exit code 1
  RESULT <game folder> <launcher>       all done

Standard library only (Python 3.8+): runs on a bundled Windows embeddable Python.
"""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def configure_gcc_paths(env, compiler_bin):
    """Keep MinGW's linker paths on the ASCII alias, even if GCC resolves its executable's junction.

    GCC_EXEC_PREFIX also selects startup objects; LIBRARY_PATH selects the native static libraries.
    See https://gcc.gnu.org/onlinedocs/gcc/Environment-Variables.html .
    """
    prefix = os.path.dirname(os.path.abspath(compiler_bin))
    gcc = os.path.join(prefix, 'lib', 'gcc')
    versions = sorted(p for p in glob.glob(os.path.join(gcc, '*', '*')) if os.path.isdir(p))
    if os.name != 'nt' or not versions:
        return
    env['GCC_EXEC_PREFIX'] = gcc.replace('\\', '/') + '/'
    libraries = versions + [os.path.join(prefix, 'x86_64-w64-mingw32', 'lib'), os.path.join(prefix, 'lib')]
    env['LIBRARY_PATH'] = os.pathsep.join(p.replace('\\', '/') for p in libraries if os.path.isdir(p))
ENGINE = os.path.join(ROOT, 'BozoEngine')  # --engine replaces it
ps2_disc = None  # BozoEngine's tools/native_pack/ps2_disc.py, imported by load_engine()

ELF_NAME = 'SLUS_212.16'
ELF_SHA1 = '9bbb67a70137052931bc245855eedeee6bbe5b49'
OTHER_RELEASES = {
    'SLUS_212.16': 'the USA release',
}
# The disc's files the build reads besides the ELF: the packs' input and the sound modules (cmake/sc3_audio.cmake).
DISC_FILES = ('ROOT.OLK', 'IRX/CRI_ADXI.IRX', 'IRX/NUSOUND.IRX', 'IRX/SDRDRV.IRX')
STEPS = ['verify', 'extract', 'configure', 'tools', 'recompile', 'packs', 'compile', 'stage']
# FetchContent sources (BozoEngine's runtime/CMakeLists.txt): <name>-src in the kit's deps/.
FETCH_NAMES = ['sdl3', 'imgui']
GAME_EXE = 'sc3'
EXE = '.exe' if os.name == 'nt' else ''
KIT_MANIFEST = 'kit_manifest.json'
RECOMP_INPUTS = ('sc3_stubs.txt', 'sc3_symbols.txt', 'sc3_entries.txt', 'sc3_patches.txt')
PACK_SCRIPTS = ('sc3_texture_pack.py', 'sc3_mesh_pack.py', 'sc3_2d_pack.py')
PACK_FILES = ('sc3_textures.pack', 'sc3_textures.keys', 'sc3_meshes.pack', 'sc3_meshes.leaves', 'sc3_2d.pack',
              'sc3_2d.leaves', 'sc3_2d_textures.pack')
PLAY_CMD = 'Play SC3.cmd'
PLAY_SH = 'play_sc3.sh'


class BuildError(Exception):
    pass


def emit(*parts):
    print(' '.join(str(p) for p in parts), flush=True)


def load_engine(path=None):
    """BozoEngine's tree (--engine, else the submodule) and its disc reader."""
    global ENGINE, ps2_disc
    if path:
        ENGINE = os.path.abspath(path)
    tools = os.path.join(ENGINE, 'tools', 'native_pack')
    if not os.path.isfile(os.path.join(tools, 'ps2_disc.py')):
        raise SystemExit('ERROR verify BozoEngine is missing (%s): the release is incomplete, or in a git checkout run '
                         '"git submodule update --init BozoEngine"' % ENGINE)
    sys.path.insert(0, tools)
    import ps2_disc as module  # noqa: E402
    ps2_disc = module


def sha1_file(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def fingerprint(*items):
    return hashlib.sha1(json.dumps(items, sort_keys=True, default=str).encode()).hexdigest()[:16]


def file_stamp(path):
    try:
        st = os.stat(path)
        return [os.path.normcase(os.path.abspath(path)), st.st_size, int(st.st_mtime)]
    except OSError:
        return [path, None]


def tree_stamp(folder, exts=None):
    """Sizes and mtimes of a source tree (cheap; changes when the source is updated)."""
    out = []
    for dirpath, dirnames, filenames in os.walk(folder):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith('.') and d not in ('build', '__pycache__'))
        for name in sorted(filenames):
            if exts and not name.lower().endswith(exts):
                continue
            p = os.path.join(dirpath, name)
            st = os.stat(p)
            out.append((os.path.relpath(p, folder), st.st_size, int(st.st_mtime)))
    return fingerprint(out)


SOURCE_EXTS = ('.cpp', '.h', '.hpp', '.inl', '.c', '.txt', '.cmake', '.rc', '.ico', '.manifest', '.py')


# ---- PE imports (the DLLs the game needs, found in the toolchain's bin) ----

# windres (the game's icon, version and UTF-8 manifest resources) runs its preprocessor through popen, that is
# cmd /c: by default as "<its own folder>\gcc" -E ... "<the .rc>". With a space in the toolchain's path (C:\Users\Jean
# Dupont\..., D:\Games\SC3 Recomp\kit) and a second quoted argument, cmd strips the outer quotes and runs
# "D:\Games\SC3" ("preprocessing failed"). The bare name, found on the PATH sc3_build puts the toolchain first on, has no
# quotes for cmd to strip.
RC_FLAGS = '--preprocessor=gcc --preprocessor-arg=-E --preprocessor-arg=-xc --preprocessor-arg=-DRC_INVOKED'


def cmake_rc_flags():
    return ['-DCMAKE_RC_FLAGS=' + RC_FLAGS] if os.name == 'nt' else []


def pe_imports(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'MZ':
        return []
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        return []
    nsec, opt_size = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    dd = opt + (112 if magic == 0x20b else 96)
    sections = []
    for i in range(nsec):
        s = pe + 24 + opt_size + 40 * i
        vsize, va, rsize, roff = struct.unpack_from('<IIII', data, s + 8)
        sections.append((va, max(vsize, rsize), roff))

    def off(rva):
        for va, size, roff in sections:
            if va <= rva < va + size:
                return rva - va + roff
        return None

    def cstr(o):
        return data[o:data.index(b'\0', o)].decode('latin-1')

    names = []
    for index, stride, name_at in ((1, 20, 12), (13, 32, 4)):  # import table, delay-load table
        rva = struct.unpack_from('<I', data, dd + 8 * index)[0]
        o = off(rva) if rva else None
        while o is not None and o + stride <= len(data):
            name_rva = struct.unpack_from('<I', data, o + name_at)[0]
            if not name_rva:
                break
            no = off(name_rva)
            if no is not None:
                names.append(cstr(no))
            o += stride
    return names


def runtime_dlls(exe, search_dirs):
    """The DLLs exe loads (recursively) that one of search_dirs has: what must sit next to it."""
    found, queue = {}, [exe]
    while queue:
        for name in pe_imports(queue.pop()):
            key = name.lower()
            if key in found:
                continue
            for d in search_dirs:
                p = os.path.join(d, name)
                if os.path.isfile(p):
                    found[key] = p
                    queue.append(p)
                    break
            else:
                found[key] = None  # a Windows DLL
    return sorted(p for p in found.values() if p)


# ---- the build ----

class Build:
    def __init__(self, args):
        self.args = args
        self.out = os.path.abspath(args.out)
        self.game = os.path.abspath(args.game) if args.game else os.path.join(self.out, 'SC3')
        self.disc_dir = os.path.join(self.out, 'disc')
        self.cmake_build = os.path.join(self.out, 'build')
        self.logs = os.path.join(self.out, 'logs')
        self.state_path = os.path.join(self.out, 'sc3_build_state.json')
        os.makedirs(self.logs, exist_ok=True)
        try:
            with open(self.state_path) as f:
                self.state = json.load(f)
        except (OSError, ValueError):
            self.state = {}
        if self.state.get('engine') != 'bozo':
            # A work folder of the fork's build (generated/, its CMake cache): its step records mean nothing here.
            self.state = {'engine': 'bozo'}
        self.toolchain_bin = None
        self.env = dict(os.environ)
        self.step = None
        self.kit = find_kit(args)
        self.ffmpeg_root = None
        if self.kit:
            # The kit's parts unless the command line names others.
            args.toolchain = args.toolchain or os.path.join(self.kit, 'ucrt64')
            args.fetch_cache = args.fetch_cache or os.path.join(self.kit, 'deps')
            self.ffmpeg_root = os.path.join(self.kit, 'ffmpeg')
        if args.debug_info is None:  # a player's build (the kit): no debug information
            args.debug_info = 'no' if self.kit else 'yes'

    def save_state(self):
        tmp = self.state_path + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(self.state, f, indent=1)
        os.replace(tmp, self.state_path)

    # -- helpers

    def log_path(self):
        return os.path.join(self.logs, self.step + '.log')

    def run(self, cmd, cwd=None, env=None, percent=None, append=False):
        """Runs cmd with its output in the step's log; percent(line) -> 0-100 or None for PERCENT lines."""
        with open(self.log_path(), 'a' if append else 'w', encoding='utf-8', errors='replace') as log:
            log.write('$ %s\n' % subprocess.list2cmdline(cmd))
            log.flush()
            try:
                p = subprocess.Popen(cmd, cwd=cwd, env=env or self.env, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, bufsize=1, universal_newlines=True,
                                     encoding='utf-8', errors='replace')
            except OSError as e:
                raise BuildError('cannot start %s: %s' % (cmd[0], e.strerror))
            last = -1
            for line in p.stdout:
                log.write(line)
                if percent:
                    v = percent(line)
                    if v is not None and v != last:
                        last = v
                        emit('PERCENT', self.step, v)
            code = p.wait()
        if code != 0:
            with open(self.log_path(), encoding='utf-8', errors='replace') as f:
                tail = f.readlines()[-25:]
            sys.stderr.write(''.join(tail))
            raise BuildError('%s failed (exit %d); the full output is in %s'
                             % (os.path.basename(cmd[0]), code, self.log_path()))

    def cmake_build_targets(self, *targets, percent=None):
        self.run([self.tool('cmake'), '--build', self.cmake_build, '--target'] + list(targets) +
                 ['-j', str(self.args.jobs)], percent=percent)

    # -- inputs

    def find_toolchain(self):
        if self.toolchain_bin:
            return self.toolchain_bin
        cands = []
        if self.args.toolchain:
            t = os.path.abspath(self.args.toolchain)
            cands.append(t if os.path.isfile(os.path.join(t, 'g++' + EXE)) else os.path.join(t, 'bin'))
        else:
            gxx = shutil.which('g++')
            if gxx:
                cands.append(os.path.dirname(gxx))
            if os.name == 'nt':
                cands.append('C:/msys64/ucrt64/bin')
        for c in cands:
            missing = [t for t in ('g++', 'gcc', 'cmake', 'ninja') if not os.path.isfile(os.path.join(c, t + EXE)) and
                       (t in ('g++', 'gcc') or not shutil.which(t))]
            if not missing:
                self.toolchain_bin = os.path.normpath(c)
                break
        else:
            raise BuildError('no compiler toolchain found (looked in %s): pass --toolchain <dir> with g++, gcc, cmake '
                             'and ninja in its bin folder (an MSYS2 UCRT64 prefix: C:/msys64/ucrt64)'
                             % (', '.join(cands) or 'PATH'))
        self.env['PATH'] = self.toolchain_bin + os.pathsep + self.env.get('PATH', '')
        configure_gcc_paths(self.env, self.toolchain_bin)
        return self.toolchain_bin

    def tool(self, name):
        path = os.path.join(self.find_toolchain(), name + EXE)
        return path if os.path.isfile(path) else shutil.which(name, path=self.env['PATH']) or path

    def toolchain_key(self):
        self.find_toolchain()
        return fingerprint(file_stamp(self.tool('gcc')), self.env.get('GCC_EXEC_PREFIX', ''),
                           self.env.get('LIBRARY_PATH', ''))

    def disc_input(self):
        src = os.path.abspath(self.args.disc)
        if not os.path.exists(src):
            raise BuildError('%s does not exist' % src)
        return src

    def elf(self):
        return os.path.join(self.disc_dir, ELF_NAME)

    # -- steps

    def key(self, step):
        """What a step's result depends on (its inputs and the earlier steps' keys)."""
        a = self.args
        prev = lambda s: self.state.get(s, {}).get('key')  # noqa: E731
        if step == 'verify':
            return fingerprint(file_stamp(self.disc_input()))
        if step == 'extract':
            return fingerprint(prev('verify'), DISC_FILES, self.game)
        if step == 'configure':
            return fingerprint(self.find_toolchain(), a.build_type, a.debug_info, a.fetch_cache, self.ffmpeg_root,
                               self.toolchain_key(), cmake_rc_flags(), ROOT, ENGINE, sys.executable, prev('extract'),
                               [file_stamp(os.path.join(ROOT, p)) for p in ('CMakeLists.txt', 'cmake/bozo.cmake', 'cmake/sc3_audio.cmake')])
        if step == 'tools':
            return fingerprint(prev('configure'), tree_stamp(os.path.join(ENGINE, 'recompiler'), SOURCE_EXTS),
                               tree_stamp(os.path.join(ENGINE, 'tools', 'externals'), SOURCE_EXTS))
        if step == 'recompile':
            return fingerprint(prev('tools'), tree_stamp(os.path.join(ROOT, 'bozo', 'port'), ('.cpp',)),
                               [file_stamp(os.path.join(ROOT, 'recomp', n)) for n in RECOMP_INPUTS],
                               [file_stamp(os.path.join(ROOT, 'scripts', n)) for n in
                                ('sc3_overrides_list.py', 'sc3_externals_all_transfers.py')])
        if step == 'packs':
            return fingerprint(prev('configure'),
                               [file_stamp(os.path.join(ROOT, 'scripts', t)) for t in PACK_SCRIPTS],
                               tree_stamp(os.path.join(ENGINE, 'tools', 'native_pack'), ('.py',)))
        if step == 'compile':
            return fingerprint(prev('recompile'), prev('packs'),
                               tree_stamp(os.path.join(ROOT, 'bozo'), SOURCE_EXTS),
                               file_stamp(os.path.join(ROOT, 'scripts', 'sc3_irx_images.py')),
                               tree_stamp(os.path.join(ENGINE, 'runtime'), SOURCE_EXTS))
        if step == 'stage':
            return fingerprint(prev('compile'), self.game)
        raise AssertionError(step)

    def step_verify(self):
        src = self.disc_input()
        if os.path.isdir(src):
            elf = os.path.join(src, ELF_NAME)
            if not os.path.isfile(elf):
                others = [n for n in os.listdir(src) if re.match(r'S[CL][A-Z]S_\d{3}\.\d{2}$', n, re.I)]
                raise BuildError(self.wrong_disc(others[0] if others else None, src))
            digest = sha1_file(elf)
        else:
            try:
                disc = ps2_disc.Disc(src)
            except ps2_disc.DiscError as e:
                raise BuildError('%s: %s' % (os.path.basename(src), e))
            try:
                boot = disc.boot_elf()
                if not boot or boot.upper() != ELF_NAME:
                    raise BuildError(self.wrong_disc(boot, src))
                digest = hashlib.sha1(disc.read(ELF_NAME)).hexdigest()
            finally:
                disc.close()
        if digest != ELF_SHA1:
            raise BuildError('%s on this disc is not the one SoulCaliburIII_Recomp supports (SHA-1 %s, expected %s): a '
                             'different printing or a modified or damaged image. Make a fresh 1:1 copy of the USA disc '
                             '(SLUS-21216).'
                             % (ELF_NAME, digest, ELF_SHA1))
        emit('INFO', 'verify', 'USA disc (SLUS-21216), %s SHA-1 ok' % ELF_NAME)

    @staticmethod
    def wrong_disc(boot, src):
        if boot and boot.upper() in OTHER_RELEASES and boot.upper() != ELF_NAME:
            what = OTHER_RELEASES[boot.upper()]
            return ('%s is %s of Soulcalibur III; SoulCaliburIII_Recomp only supports the USA disc '
                    '(SLUS-21216).' % (os.path.basename(src), what))
        if boot:
            return ('%s boots %s: not Soulcalibur III (USA, SLUS-21216), the only disc SoulCaliburIII_Recomp supports.'
                    % (os.path.basename(src), boot))
        return ('%s: no %s found: not the Soulcalibur III USA disc (SLUS-21216), or not a PS2 disc image.'
                % (os.path.basename(src), ELF_NAME))

    def step_extract(self):
        src = self.disc_input()
        image = src
        if os.path.isdir(src):
            image = self.find_folder_image(src)
            if not image:
                raise BuildError('the game reads the disc image itself: no .iso of the USA disc in %s or the folders '
                                 'above it. Pass the .iso (--disc <image>) instead.' % src)
        os.makedirs(self.game, exist_ok=True)
        marker = os.path.join(self.disc_dir, '.extracted')
        if os.path.exists(marker):
            os.remove(marker)
        self.extract_files(src)
        if sha1_file(self.elf()) != ELF_SHA1:
            raise BuildError("the extracted %s differs from the disc's (disk error?)" % ELF_NAME)
        # The packs' inputs are ready (cmake/bozo.cmake's SC3_DISC_DIR).
        with open(marker, 'w') as f:
            f.write('extracted by scripts/sc3_build.py\n')
        self.place_image(image)

    def find_folder_image(self, folder):
        """An extracted folder: the .iso it came from, in it or up to three folders above (as the runtime looks)."""
        d = os.path.abspath(folder)
        for _ in range(4):
            for name in sorted(os.listdir(d)):
                if not name.lower().endswith('.iso'):
                    continue
                try:
                    disc = ps2_disc.Disc(os.path.join(d, name))
                except ps2_disc.DiscError:
                    continue
                try:
                    if (disc.boot_elf() or '').upper() == ELF_NAME and \
                            hashlib.sha1(disc.read(ELF_NAME)).hexdigest() == ELF_SHA1:
                        return os.path.join(d, name)
                finally:
                    disc.close()
            parent = os.path.dirname(d)
            if parent == d:
                break
            d = parent
        return None

    def place_image(self, src):
        """The game reads the disc from a 2048-byte-sector image of it, <game>/disc.iso (sc3.exe --iso; it also finds
        the one .iso beside the ELF). A hard link to the player's .iso when it is on the same drive, else a
        copy; a .bin / .cue is converted."""
        dest = os.path.join(self.game, 'disc.iso')
        disc = ps2_disc.Disc(src)
        try:
            img = disc.image
            total = img.size // img.sector_size
            if os.path.exists(dest) and os.path.getsize(dest) == total * 2048:
                return
            if os.path.exists(dest):
                os.remove(dest)
            if img.sector_size == 2048:
                try:
                    os.link(src, dest)
                    return
                except OSError:
                    pass
            free = shutil.disk_usage(self.game).free
            if free < total * 2048 + (2 << 30):
                raise BuildError('not enough free space on %s for the disc image: %.1f GB free, about %.1f GB needed'
                                 % (self.game, free / 2**30, (total * 2048 + (2 << 30)) / 2**30))
            tmp = dest + '.part'
            with open(tmp, 'wb') as f:
                lba = 0
                while lba < total:
                    n = min(4096, total - lba)
                    f.write(img.read_sectors(lba, n))
                    lba += n
                    emit('PERCENT', 'extract', lba * 100 // total)
            os.replace(tmp, dest)
        finally:
            disc.close()

    @staticmethod
    def wanted(rel):
        rel = rel.replace('\\', '/')
        return rel.upper() == ELF_NAME or rel.upper() in DISC_FILES

    def extract_files(self, src):
        """Only what the build reads: the ELF, ROOT.OLK and the sound modules (resumable: finished files stay)."""
        os.makedirs(self.disc_dir, exist_ok=True)
        if os.path.isdir(src):
            files = []
            for dirpath, _dirs, names in os.walk(src):
                for n in names:
                    rel = os.path.relpath(os.path.join(dirpath, n), src)
                    if self.wanted(rel):
                        files.append(rel)
            total = sum(os.path.getsize(os.path.join(src, f)) for f in files) or 1
            done, last = 0, -1
            for rel in files:
                s, d = os.path.join(src, rel), os.path.join(self.disc_dir, rel)
                os.makedirs(os.path.dirname(d), exist_ok=True)
                size = os.path.getsize(s)
                if not (os.path.exists(d) and os.path.getsize(d) == size):
                    if os.path.exists(d):
                        os.remove(d)
                    try:
                        os.link(s, d)  # same volume: no copy
                    except OSError:
                        shutil.copy2(s, d)
                done += size
                pct = done * 100 // total
                if pct != last:
                    last = pct
                    emit('PERCENT', 'extract', pct)
            return
        disc = ps2_disc.Disc(src)
        try:
            files = [e for e in disc.entries() if not e.is_dir and self.wanted(e.path)]
            total = sum(e.size for e in files) or 1
            free = shutil.disk_usage(self.out).free
            if free < total + (3 << 30):
                raise BuildError('not enough free space on %s: %.1f GB free, about %.1f GB needed (the disc\'s files '
                                 'and the build)' % (self.out, free / 2**30, (total + (3 << 30)) / 2**30))
            done, last = 0, -1
            for e in files:
                dest = os.path.join(self.disc_dir, e.path)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                if not (os.path.exists(dest) and os.path.getsize(dest) == e.size):
                    tmp = dest + '.part'
                    with open(tmp, 'wb') as f:
                        for chunk in disc.image.read_extent(e.lba, e.size):
                            f.write(chunk)
                    os.replace(tmp, dest)
                    if e.mtime is not None:
                        os.utime(dest, (e.mtime, e.mtime))
                done += e.size
                pct = done * 100 // total
                if pct != last:
                    last = pct
                    emit('PERCENT', 'extract', pct)
        finally:
            disc.close()
        emit('INFO', 'extract', '%d files, %.0f MB' % (len(files), total / 1e6))

    def fetch_args(self):
        if not self.args.fetch_cache:
            return []  # CMake downloads the pinned archives (SHA-256 checked)
        cache = os.path.abspath(self.args.fetch_cache)
        out = ['-DFETCHCONTENT_FULLY_DISCONNECTED=ON'] if self.kit else []  # never the network
        for n in FETCH_NAMES:
            d = os.path.join(cache, n + '-src')
            if os.path.isdir(d):
                out.append('-DFETCHCONTENT_SOURCE_DIR_%s=%s' % (n.upper(), d.replace('\\', '/')))
            elif self.kit:
                raise BuildError('the build kit has no deps/%s-src: the kit is incomplete or of an older release' % n)
        return out

    def step_configure(self):
        # A build folder configured from another path (the release folder moved or renamed, or reached once through
        # the non-ASCII aliases and once not), or a fork build's: CMake refuses its cache; start that folder afresh.
        cache = os.path.join(self.cmake_build, 'CMakeCache.txt')
        stamp = os.path.join(self.cmake_build, '.sc3-toolchain-key')
        toolchain_key = self.toolchain_key()
        try:
            with open(stamp, encoding='utf-8') as f:
                previous_toolchain = f.read().strip()
        except OSError:
            previous_toolchain = ''
        if os.path.isfile(cache):
            with open(cache, encoding='utf-8', errors='replace') as f:
                text = f.read()
            m = re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', text, re.M)
            moved = m and os.path.normcase(os.path.normpath(m.group(1))) != os.path.normcase(os.path.normpath(ROOT))
            if moved or previous_toolchain != toolchain_key or not re.search(r'^BOZO_ENGINE_DIR:PATH=', text, re.M):
                emit('INFO', 'configure', 'the build folder was configured %s: configured again'
                     % ('from %s' % m.group(1) if moved else 'with another toolchain configuration'))
                os.remove(cache)
                shutil.rmtree(os.path.join(self.cmake_build, 'CMakeFiles'), ignore_errors=True)
        fwd = lambda p: p.replace('\\', '/')  # noqa: E731
        cmd = [self.tool('cmake'), '-S', ROOT, '-B', self.cmake_build, '-G', 'Ninja',
               '-DCMAKE_BUILD_TYPE=' + self.args.build_type,
               '-DBOZO_ENGINE_DIR=' + fwd(ENGINE),
               '-DSC3_BOZO=ON', '-DSC3_WINDOW=ON', '-DSC3_NATIVE_SOUND=ON',
               '-DSC3_ELF=' + fwd(self.elf()), '-DSC3_DISC_DIR=' + fwd(self.disc_dir),
               '-DSC3_IRX_DIR=' + fwd(os.path.join(self.disc_dir, 'IRX')), '-DSC3_ISO=',
               '-DBOZO_BUILD_TOOLS=OFF', '-DBUILD_TESTING=OFF', '-DSC3_GUI_SUBSYSTEM=ON',
               '-DPython3_EXECUTABLE=' + fwd(sys.executable),
               '-DCMAKE_C_COMPILER=' + self.tool('gcc'), '-DCMAKE_CXX_COMPILER=' + self.tool('g++'),
               '-DCMAKE_MAKE_PROGRAM=' + self.tool('ninja')] + cmake_rc_flags()
        if self.args.debug_info == 'no' and self.args.build_type != 'Release':
            # The configuration's own flags without -g: the same code (GCC's -g never changes it).
            flags = {'RelWithDebInfo': '-O2 -DNDEBUG', 'Debug': '-O0'}[self.args.build_type]
            cmd += ['-DCMAKE_%s_FLAGS_%s=%s' % (lang, self.args.build_type.upper(), flags) for lang in ('C', 'CXX')]
        if self.ffmpeg_root:
            cmd.append('-DBOZO_FFMPEG_ROOT=' + fwd(self.ffmpeg_root))
        else:
            pkg = os.path.join(self.find_toolchain(), 'pkg-config' + EXE)
            if os.path.isfile(pkg):
                cmd.append('-DPKG_CONFIG_EXECUTABLE=' + pkg)
        self.run(cmd + self.fetch_args())
        with open(stamp, 'w', encoding='utf-8') as f:
            f.write(toolchain_key + '\n')

    def step_tools(self):
        self.cmake_build_targets('bozo-recomp', 'bozo-externals', percent=ninja_percent)

    def step_recompile(self):
        self.cmake_build_targets('sc3_recompile', percent=ninja_percent)

    def step_packs(self):
        self.cmake_build_targets('sc3_packs', percent=ninja_percent)

    def step_compile(self):
        self.cmake_build_targets(GAME_EXE, percent=ninja_percent)

    def exe_path(self):
        return os.path.join(self.cmake_build, GAME_EXE + EXE)

    def step_stage(self):
        exe = self.exe_path()
        if not os.path.isfile(exe):
            raise BuildError('no game at %s' % exe)
        os.makedirs(self.game, exist_ok=True)
        staged = [exe]
        dest = os.path.join(self.game, os.path.basename(exe))
        strip = os.path.join(self.find_toolchain(), 'strip' + EXE)
        if os.path.exists(dest):
            os.remove(dest)
        if self.args.debug_info == 'no' and os.path.isfile(strip):
            self.run([strip, '--strip-debug', '-o', dest, exe])
            shutil.copystat(exe, dest)
        else:
            shutil.copy2(exe, dest)
        if os.name == 'nt':
            for dll in runtime_dlls(exe, [os.path.dirname(exe), self.find_toolchain()]):
                shutil.copy2(dll, os.path.join(self.game, os.path.basename(dll)))
                staged.append(dll)
        shutil.copy2(self.elf(), os.path.join(self.game, ELF_NAME))
        shutil.copy2(self.symbol_map(), os.path.join(self.game, 'sc3_symbols.txt'))
        packs = os.path.join(self.game, 'packs')
        os.makedirs(packs, exist_ok=True)
        for name in PACK_FILES:
            src, dest_pack = os.path.join(self.cmake_build, 'packs', name), os.path.join(packs, name)
            if os.path.exists(dest_pack):
                os.remove(dest_pack)
            try:
                os.link(src, dest_pack)
            except OSError:
                shutil.copy2(src, dest_pack)
        name = os.path.basename(exe)
        if os.name == 'nt':
            lines = ['@echo off', 'rem Made by scripts/sc3_build.py: Soulcalibur III, native PC port (BozoEngine).',
                     'cd /d "%~dp0"',
                     'start "" "%%~dp0%s" --elf "%%~dp0%s" --iso "%%~dp0disc.iso" %%*' % (name, ELF_NAME), '']
            with open(os.path.join(self.game, PLAY_CMD), 'w', newline='\r\n') as f:
                f.write('\n'.join(lines))
        else:
            sh = os.path.join(self.game, PLAY_SH)
            with open(sh, 'w', newline='\n') as f:
                f.write('#!/bin/sh\ncd "$(dirname "$0")"\nexec ./%s --elf "$PWD/%s" --iso "$PWD/disc.iso" "$@"\n'
                        % (name, ELF_NAME))
            os.chmod(sh, 0o755)
        with open(os.path.join(self.game, 'sc3_build.json'), 'w') as f:
            json.dump({'built': time.strftime('%Y-%m-%d %H:%M:%S'), 'engine': 'BozoEngine', 'elf_sha1': ELF_SHA1,
                       'build_type': self.args.build_type, 'debug_info': self.args.debug_info,
                       'toolchain': self.find_toolchain(), 'kit': self.kit, 'exe': name,
                       'files': [os.path.basename(p) for p in staged]}, f, indent=1)
        emit('INFO', 'stage', '%s, %.1f MB, %d DLLs staged' % (name, os.path.getsize(dest) / 1e6, len(staged) - 1))

    def symbol_map(self):
        return os.path.join(ROOT, 'recomp', 'sc3_symbols.txt')

    def write_built_from(self):
        """<game>/built_from.ini: the release stamp (<SoulCaliburIII_Recomp>/release.ini, written by scripts/make_release.py) of the
        code this game folder was built from, after every successful run, even one where every step was skipped (a
        release that changed nothing the build reads). The launcher compares it with the source's to offer Rebuild
        after an update. Without a stamp (a git checkout) the commit is git's when git is there, else empty."""
        stamp = os.path.join(ROOT, 'release.ini')
        if os.path.isfile(stamp):
            with open(stamp, encoding='utf-8') as f:
                text = f.read()
        else:
            commit = ''
            try:
                commit = subprocess.run(['git', '-C', ROOT, 'rev-parse', 'HEAD'], stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, universal_newlines=True).stdout.strip()
            except OSError:
                pass
            text = 'version = dev\ncommit = %s\n' % commit
        with open(os.path.join(self.game, 'built_from.ini'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('# The SoulCaliburIII_Recomp code this game folder was built from (scripts/sc3_build.py writes this file).\n')
            f.write('\n'.join(l for l in text.splitlines() if l and not l.startswith('#')) + '\n')

    # -- driver

    def main(self):
        redo_from = STEPS.index(self.args.redo) if self.args.redo else len(STEPS)
        n = len(STEPS)
        dirty = False
        total_start = time.time()
        for i, step in enumerate(STEPS, 1):
            self.step = step
            try:
                key = self.key(step)
                st = self.state.get(step, {})
                if not dirty and i - 1 < redo_from and st.get('done') and st.get('key') == key:
                    emit('SKIP', step, '%d/%d' % (i, n))
                    if step == self.args.stop_after:
                        emit('INFO', 'stopped', 'after ' + step)
                        return 0
                    continue
                dirty = True  # every later step runs again (Ninja only redoes what changed)
                emit('PROGRESS', step, '%d/%d' % (i, n), STEP_TITLES[step])
                t = time.time()
                self.state[step] = {'done': False}
                self.save_state()
                getattr(self, 'step_' + step)()
                secs = round(time.time() - t, 1)
                self.state[step] = {'done': True, 'key': self.key(step), 'seconds': secs}
                self.save_state()
                emit('DONE', step, '%d/%d' % (i, n), secs)
                if step == self.args.stop_after:
                    emit('INFO', 'stopped', 'after ' + step)
                    return 0
            except BuildError as e:
                emit('ERROR', step, e)
                return 1
            except KeyboardInterrupt:
                emit('ERROR', step, 'interrupted (run again to resume)')
                return 1
            except Exception as e:  # an unexpected failure: still one clear line, details in the log
                with open(os.path.join(self.logs, step + '.log'), 'a') as log:
                    import traceback
                    traceback.print_exc(file=log)
                emit('ERROR', step, '%s: %s (details in %s)' % (type(e).__name__, e, self.log_path()))
                return 1
        self.write_built_from()
        launcher = os.path.join(self.game, PLAY_CMD if os.name == 'nt' else PLAY_SH)
        emit('INFO', 'total', '%.1f s' % (time.time() - total_start))
        emit('RESULT', self.game, launcher)
        return 0


def find_kit(args):
    """The build kit: --kit, else <SoulCaliburIII_Recomp>/kit or <SoulCaliburIII_Recomp's parent>/kit when it has kit_manifest.json."""
    if args.no_kit:
        return None
    if args.kit:
        kit = os.path.abspath(args.kit)
        if not os.path.isfile(os.path.join(kit, KIT_MANIFEST)):
            raise SystemExit('ERROR kit %s has no %s' % (kit, KIT_MANIFEST))
        return kit
    for kit in (os.path.join(ROOT, 'kit'), os.path.join(os.path.dirname(ROOT), 'kit')):
        if os.path.isfile(os.path.join(kit, KIT_MANIFEST)):
            return kit
    return None


STEP_TITLES = {
    'verify': 'Checking the disc',
    'extract': 'Reading the disc\'s files',
    'configure': 'Configuring the build',
    'tools': 'Preparing the recompiler',
    'recompile': 'Recompiling the game\'s code',
    'packs': 'Converting textures and models',
    'compile': 'Compiling (this takes a while)',
    'stage': 'Assembling the game folder',
}

NINJA_RE = re.compile(r'^\[(\d+)/(\d+)\]')


def ninja_percent(line):
    m = NINJA_RE.match(line)
    if m and int(m.group(2)):
        return int(m.group(1)) * 100 // int(m.group(2))
    return None


def check_disc(path):
    """--check: the verify step alone, for a launcher checking the image the player picked."""
    b = Build.__new__(Build)
    b.args = argparse.Namespace(disc=path)
    try:
        b.step_verify()
    except BuildError as e:
        emit('ERROR', 'verify', e)
        return 1
    except Exception as e:  # noqa: BLE001 (an unreadable file: still one line)
        emit('ERROR', 'verify', '%s: %s' % (os.path.basename(path), e))
        return 1
    emit('OK', 'USA disc (SLUS-21216)')
    return 0


def main():
    ap = argparse.ArgumentParser(description='Builds Soulcalibur III (USA) as a native PC game from your own disc.')
    ap.add_argument('--disc', required=True, help='the disc image (.iso, .bin, .cue) or an extracted disc folder')
    ap.add_argument('--out', default='sc3_build', help='the work folder; the game ends up in <out>/SC3')
    ap.add_argument('--game', help='the game folder to stage (default: <out>/SC3)')
    ap.add_argument('--check', action='store_true', help='only check the disc (no work folder): OK <message> or '
                    'ERROR verify <message>')
    ap.add_argument('--engine', help='BozoEngine\'s source tree (default: the BozoEngine submodule)')
    ap.add_argument('--kit', help='the build kit folder (default: <SoulCaliburIII_Recomp>/kit or <its parent>/kit if present)')
    ap.add_argument('--no-kit', action='store_true', help='do not use a build kit even if one is found')
    ap.add_argument('--toolchain', help='compiler folder (with bin/g++, cmake, ninja), default: the kit, PATH, '
                    'C:/msys64/ucrt64')
    ap.add_argument('--fetch-cache', help='folder with the FetchContent sources (sdl3-src, imgui-src) instead of the '
                    'download')
    ap.add_argument('--build-type', default='Release', choices=['Release', 'RelWithDebInfo', 'Debug'])
    ap.add_argument('--debug-info', choices=['yes', 'no'], help='debug information in the game (default: no with '
                    'the build kit, a player build; yes without it)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--redo', choices=STEPS, help='run this step and the ones after it again')
    ap.add_argument('--stop-after', choices=STEPS, help='stop once this step is done')
    args = ap.parse_args()
    load_engine(args.engine)
    if args.check:
        sys.exit(check_disc(args.disc))
    os.makedirs(os.path.abspath(args.out), exist_ok=True)
    if not os.environ.get(ALIAS_ENV):
        remove_stale_sources()
    if needs_ascii_aliases(args):
        sys.exit(run_through_aliases(args))
    sys.exit(Build(args).main())


def remove_stale_sources():
    """A release's source folder lists its files (release_files.txt, scripts/make_release.py). A newer release unzipped
    over an older one leaves the files it no longer has; CMake globs some folders (runtime/*.cpp, BozoEngine's
    sources), so they are removed here: regular files under the source folder only, never through a link, never
    Python's caches; then the folders left empty."""
    listing = os.path.join(ROOT, 'release_files.txt')
    if not os.path.isfile(listing):
        return
    with open(listing, encoding='utf-8') as f:
        keep = set(l.strip() for l in f if l.strip())
    removed = 0
    tops = sorted(set(rel.split('/', 1)[0] for rel in keep if '/' in rel))
    linked = lambda p: os.path.islink(p) or (hasattr(os.path, 'isjunction') and os.path.isjunction(p))
    for top in tops:
        if top in ('', '.', '..') or os.path.isabs(top) or '\\' in top or ':' in top:
            raise BuildError('invalid source manifest directory: %r' % top)
        base = os.path.abspath(os.path.join(ROOT, top))
        if os.path.normcase(os.path.commonpath([os.path.abspath(ROOT), base])) != os.path.normcase(os.path.abspath(ROOT)):
            raise BuildError('source cleanup escaped its root: %s' % base)
        if linked(base):
            continue
        visited = []
        # Prune links before descending. A bottom-up walk cannot prevent traversal of a Windows junction.
        for dirpath, dirs, names in os.walk(base, topdown=True, followlinks=False):
            visited.append(dirpath)
            dirs[:] = [d for d in dirs if d != '__pycache__' and not linked(os.path.join(dirpath, d))]
            for n in names:
                p = os.path.join(dirpath, n)
                rel = os.path.relpath(p, ROOT).replace('\\', '/')
                if rel not in keep and not linked(p) and os.path.isfile(p):
                    os.remove(p)
                    removed += 1
        for dirpath in reversed(visited):
            if not os.listdir(dirpath):
                os.rmdir(dirpath)
    if removed:
        emit('INFO', 'update', '%d files of an earlier release removed from %s' % (removed, ROOT))


# ---- folders whose path is not ASCII (Windows) ----
#
# MSYS2's binutils open files in the ANSI code page: ld cannot open an object or library whose path, read from the
# response file CMake writes in UTF-8, has a non-ASCII character (C:\Users\Jérôme\..., D:\Jeux\Été\...): "cannot find
# ...obj". GCC itself copes; ld does not, and it is not ours to rebuild. So when the code, the work folder, the game
# folder or the kit has such a path, sc3_build runs itself again through directory junctions with plain names
# (%ProgramData%\SoulCaliburIII_Recomp\paths\<hash>-<name>, the same each time so CMake's cache stays valid) and removes the
# junctions afterwards (only the links: os.rmdir on a junction never touches its target). A junction needs no
# administrator rights; the disc image is read by Python only and keeps its path.

ALIAS_ENV = 'SC3_BUILD_ALIASED'


def is_ascii(path):
    try:
        path.encode('ascii')
        return True
    except UnicodeEncodeError:
        return False


def needs_ascii_aliases(args):
    if os.name != 'nt' or os.environ.get(ALIAS_ENV):
        return False
    kit = find_kit(args)
    paths = [ROOT, ENGINE, os.path.abspath(args.out), os.path.abspath(args.game) if args.game else '', kit or '',
             os.path.abspath(args.toolchain) if args.toolchain else '']
    return not all(is_ascii(p) for p in paths)


def alias_folder():
    base = os.path.join(os.environ.get('ProgramData') or r'C:\ProgramData', 'SoulCaliburIII_Recomp', 'paths')
    if is_ascii(base):
        return base
    return os.path.join(os.path.splitdrive(os.path.abspath(ROOT))[0] + os.sep, 'SoulCaliburIII_Recomp_paths')


def make_alias(target, name):
    import _winapi
    target = os.path.abspath(target)
    os.makedirs(target, exist_ok=True)
    folder = alias_folder()
    os.makedirs(folder, exist_ok=True)
    link = os.path.join(folder, hashlib.sha1(os.path.normcase(target).encode('utf-8')).hexdigest()[:10] + '-' + name)
    if os.path.lexists(link):
        remove_alias(link)
    _winapi.CreateJunction(target, link)
    return link


def remove_alias(link):
    """Removes a junction this script made: the link only."""
    if os.path.lexists(link) and (os.path.islink(link) or getattr(os.path, 'isjunction', lambda _p: True)(link)):
        os.rmdir(link)


def run_through_aliases(args):
    kit = find_kit(args)
    links = []
    try:
        root = make_alias(ROOT, 'source')
        links.append(root)
        argv = [sys.executable, '-I', '-X', 'utf8', os.path.join(root, 'scripts', 'sc3_build.py')]
        out = make_alias(args.out, 'build')
        links.append(out)
        game = None
        if args.game:
            game = make_alias(args.game, 'game')
            links.append(game)
        rest = sys.argv[1:]
        # The same command line, the folders replaced by their aliases.
        replace = {'--out': out, '--game': game}
        if args.toolchain:
            t = make_alias(args.toolchain, 'toolchain')
            links.append(t)
            replace['--toolchain'] = t
        if kit:
            k = make_alias(kit, 'kit')
            links.append(k)
            replace['--kit'] = k
        if args.engine:
            e = make_alias(ENGINE, 'engine')
            links.append(e)
            replace['--engine'] = e
        new = []
        i = 0
        while i < len(rest):
            a = rest[i]
            key = a.split('=', 1)[0]
            if key in replace and replace[key]:
                new += [key, replace[key]]
                i += 1 if '=' in a else 2
                continue
            new.append(a)
            i += 1
        if kit and '--kit' not in [n.split('=', 1)[0] for n in new]:
            new += ['--kit', replace['--kit']]
        emit('INFO', 'paths', 'non-ASCII folders: building through %s' % alias_folder())
        env = dict(os.environ)
        env[ALIAS_ENV] = '1'
        sys.stdout.flush()
        # The links disappear at exit. Protocol paths shown to the launcher/CLI must point at the real folders.
        display = [(root, ROOT), (out, os.path.abspath(args.out))]
        if game:
            display.append((game, os.path.abspath(args.game)))
        if kit:
            display.append((replace['--kit'], kit))
        proc = subprocess.Popen(argv + new, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding='utf-8', errors='replace')
        for line in proc.stdout:
            for alias, real in display:
                line = line.replace(alias, real).replace(alias.replace('\\', '/'), real.replace('\\', '/'))
            print(line, end='', flush=True)
        return proc.wait()
    except OSError as e:
        emit('ERROR', 'verify', 'could not make plain-named links to the folders (%s): move the game\'s folder to a '
             'path with plain letters, such as C:\\Games\\SoulCaliburIII_Recomp' % e)
        return 1
    finally:
        for link in links:
            try:
                remove_alias(link)
            except OSError:
                pass


if __name__ == '__main__':
    main()
