#!/usr/bin/env python3
"""make_release: the player's download, from a commit of this repository: SoulCaliburIII_Recomp-<version>.zip (the game
built on BozoEngine; its folder SoulCaliburIII_Recomp) with the launcher, the code and the build kit, its SHA-256, the
licences and the corresponding sources of what the kit redistributes.

  python scripts/make_release.py --version 0.1.0 --kit D:/sc3_kit/kit --out D:/sc3_release [--with-sources]
  python scripts/make_release.py --out D:/sc3_release --publish --dry-run       (the version from the commit's tag)

What it does, from <commit> (default HEAD; only committed files count, the working tree is never read):
  export     git archive of the commit and of the BozoEngine submodule at the commit it records (without its
             reference/ records, which the build never reads) -> source\\ (no .git, no build/, nothing generated);
             source\\release.ini stamps the version and both commits. The tree is audited: no ELF, IRX, disc image,
             pack, generated code or disc file name may be in it.
  kit        the build kit (--kit <dir>: an existing one, hard-linked into the release; else assembled by
             scripts/make_build_kit.py, which needs MSYS2 and the network): SC3's kit (SDL3 and ImGui sources, no
             prebuilt recompilers, FFmpeg with what the movies need). The pins of FFmpeg, Python, the library archives and the MSYS2
             packages are checked against the exported make_build_kit.py.
  launcher   launcher/ configured and built with the kit's toolchain alone (PATH = the kit and Windows), offline (the
             kit's SDL3 and ImGui, BozoEngine's PNG writer; the Exo 2 font from --font-dir or downloaded and SHA-256
             checked), with LAUNCHER_VERSION / LAUNCHER_COMMIT: the footer, the update check, the exe's version
             information.
  licences   LICENSES\\: each component's licence files (SoulCaliburIII_Recomp's own code: MIT, the repository's LICENSE;
             BozoEngine: MIT; the kit's components, the libraries and the launcher's font) and SOURCES.txt: every MSYS2
             package the kit holds files of, with its version and its source package on repo.msys2.org, FFmpeg's
             tarball, the libraries' repositories and commits.
  changelog  CHANGELOG.md's section for the version (## [x.y.z], Keep a Changelog), from the commit: the release
             refuses to be made without one (an empty one, or only "Unreleased", too) unless
             --allow-missing-changelog (test builds). CHANGELOG.md is shipped at the zip's top (the launcher's
             "What's new" reads it) and the section is the release notes' "What's new".
  zip        SoulCaliburIII_Recomp-<version>.zip (deflate, entries sorted, every timestamp the commit's), <zip>.sha256,
             RELEASE_NOTES.md (the changelog section, the hashes, the source offer: the GitHub release's body). The
             zip's names are audited again.

  python scripts/make_release.py --check-changelog --version 0.1.0     the section, printed (exit 1 when missing)
  python scripts/make_release.py --verify-zip <out>/SoulCaliburIII_Recomp-0.1.0.zip    names and contents audited once more
  sources    --with-sources: the MSYS2 source packages (repo.msys2.org/mingw/sources only, cached in --downloads, their
             SHA-256 recorded) and FFmpeg's tarball in SoulCaliburIII_Recomp-<version>-sources.zip (stored: they are compressed
             already), to attach to the same release: the GPL / LGPL corresponding source shipped alongside.
  publish    --publish: a DRAFT GitHub release (gh release create --draft) with the zips, their .sha256 and the notes;
             --dry-run prints the command only. Nothing is ever pushed; the owner reviews and publishes the draft.

Standard library only (Python 3.8+; the kit's embeddable Python runs it too). Needs git; gh only for --publish.
"""
import argparse
import hashlib
import io
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
REPO = 'TheBozos/SoulCaliburIII_Recomp'
ENGINE_REPO = 'TheBozos/BozoEngine'
ENGINE_DIR = 'BozoEngine'  # the submodule
# What BozoEngine's export leaves out (its reference records: hashes of recorded runs, never read by the build).
ENGINE_SKIP = ('reference',)
NAME = 'SoulCaliburIII_Recomp'
# The zips are <NAME>-<version>(.zip, -sources.zip), the folder in them TOP.
TOP = NAME
VERSION_RE = re.compile(r'^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$')
MSYS2_SOURCES = 'https://repo.msys2.org/mingw/sources/'
# The launcher's font (launcher/CMakeLists.txt pins the same commit and hash).
EXO2_COMMIT = '1796e34455b893092713d206e7f23e3e07699f00'
EXO2_SHA256 = '205a448676a2586f9c57c25f3d5c58ca8db7e6cf5edf7506783a010c6fe2bfb5'

# Nothing derived from the game may be in a release: file names and contents that would be.
GAME_EXTENSIONS = ('.elf', '.irx', '.iso', '.bin', '.cue', '.img', '.mdf', '.nrg', '.chd', '.pack', '.pss', '.vag',
                   '.str', '.vb', '.dat', '.cnf', '.mc2', '.ps2')
DISC_NAME_RE = re.compile(r'(^|/)(SCUS|SLUS|SCES|SLES)[_-][0-9]{3}\.?[0-9]{2}(\.[0-9]+)?$', re.I)
# The folders the build makes (generated code, the disc's files, the game): never in the code or at the top. The
# kit's third-party sources have folders of their own called "generated" (rabbitizer's tables): kit\ is not checked.
GAME_DIR_RE = re.compile(r'(^|/)(generated|generated_[a-z]+|game_us|game_eu|game|disc|build)/', re.I)


def game_name(rel):
    """True when a release path is named like the game's files or the build's output folders."""
    return bool(DISC_NAME_RE.search(rel) or (not rel.startswith('kit/') and GAME_DIR_RE.search(rel)))


class ReleaseError(Exception):
    pass


def log(*a):
    print(*a, flush=True)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def find_git():
    for c in (shutil.which('git'), r'C:\Program Files\Git\cmd\git.exe'):
        if c and os.path.isfile(c):
            return c
    raise ReleaseError('git is needed')


def git(*args, cwd=ROOT, binary=False):
    p = subprocess.run([find_git(), '-C', cwd] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode != 0:
        raise ReleaseError('git %s: %s' % (' '.join(args), p.stderr.decode('utf-8', 'replace').strip()))
    return p.stdout if binary else p.stdout.decode('utf-8').strip()


def rmtree(path):
    """Removes a tree this script made; a junction or symlink is unlinked, never followed."""
    if not os.path.lexists(path):
        return
    if os.path.islink(path) or (hasattr(os.path, 'isjunction') and os.path.isjunction(path)):
        os.unlink(path)
        return

    def onerror(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=onerror)
    else:
        shutil.rmtree(path, onerror=onerror)


def dir_size(path):
    total = 0
    for dirpath, _dirs, names in os.walk(path):
        for n in names:
            total += os.path.getsize(os.path.join(dirpath, n))
    return total


def clean_env(extra_path=()):
    """The Windows folders only, plus extra_path first (as make_build_kit): nothing from MSYS2, git or a Python."""
    sysroot = os.environ.get('SystemRoot', r'C:\Windows')
    keep = ('SystemRoot', 'SystemDrive', 'windir', 'TEMP', 'TMP', 'USERPROFILE', 'APPDATA', 'LOCALAPPDATA',
            'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE', 'PROCESSOR_IDENTIFIER', 'ComSpec', 'PATHEXT',
            'HOMEDRIVE', 'HOMEPATH', 'USERNAME', 'ProgramData', 'OS')
    env = {k: v for k, v in os.environ.items() if k in keep}
    env['PATH'] = os.pathsep.join(list(extra_path) + [os.path.join(sysroot, 'System32'), sysroot,
                                                      os.path.join(sysroot, 'System32', 'Wbem')])
    return env


def run(cmd, cwd=None, env=None, logfile=None):
    log('  $ ' + subprocess.list2cmdline(cmd))
    p = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True, encoding='utf-8', errors='replace')
    if logfile:
        with open(logfile, 'a', encoding='utf-8') as f:
            f.write('$ %s\n%s\n' % (subprocess.list2cmdline(cmd), p.stdout))
    if p.returncode != 0:
        sys.stderr.write(p.stdout[-6000:])
        raise ReleaseError('%s failed (exit %d)' % (os.path.basename(cmd[0]), p.returncode))
    return p.stdout


def link_tree(src, dst, skip=(), copy=()):
    """dst as a copy of src made of hard links (a copy where linking fails: another drive), but the relative paths in
    `copy` (files something will rewrite in place) copied, and the top-level names in `skip` left out."""
    for dirpath, dirs, names in os.walk(src):
        rel_dir = os.path.relpath(dirpath, src)
        if rel_dir == '.':
            dirs[:] = [d for d in dirs if d not in skip]
            names = [n for n in names if n not in skip]
        os.makedirs(os.path.join(dst, rel_dir), exist_ok=True)
        for n in names:
            s = os.path.join(dirpath, n)
            d = os.path.join(dst, rel_dir, n)
            rel = os.path.normpath(os.path.join(rel_dir, n)).replace('\\', '/')
            if rel in copy:
                shutil.copy2(s, d)
                continue
            try:
                os.link(s, d)
            except OSError:
                shutil.copy2(s, d)


def extract_tar_stream(cmd, dst, cwd):
    """A `git archive` (tar on stdout) unpacked into dst; links and absolute or parent paths refused."""
    p = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        with tarfile.open(fileobj=p.stdout, mode='r|') as tf:
            for m in tf:
                name = m.name
                if name.startswith('/') or '..' in name.split('/') or m.issym() or m.islnk():
                    raise ReleaseError('unexpected entry in the archive: %s' % name)
                target = os.path.join(dst, *name.split('/'))
                if m.isdir():
                    os.makedirs(target, exist_ok=True)
                elif m.isfile():
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    with tf.extractfile(m) as src, open(target, 'wb') as out:
                        shutil.copyfileobj(src, out)
                    if m.mode & 0o111:
                        os.chmod(target, 0o755)
                    os.utime(target, (m.mtime, m.mtime))
    except BaseException:
        p.kill()
        p.communicate()
        raise
    # Tar readers stop at the end marker before all record padding has necessarily left stdout. Drain stdout and
    # stderr together: waiting on stderr alone can deadlock git archive on Windows' small pipe buffer.
    _padding, err = p.communicate()
    err = err.decode('utf-8', 'replace')
    if p.returncode != 0:
        raise ReleaseError('%s: %s' % (' '.join(cmd[1:4]), err.strip()))


def audit_tree(folder, allow_pe=False, rel_prefix=''):
    """The problems that would put something of the game in the release: names (disc files, generated folders, the
    game's file types) and contents (ELF, a PS2 IRX, an ISO9660 volume, a pack's magic). Returns a list of strings."""
    problems = []
    for dirpath, _dirs, names in os.walk(folder):
        for n in names:
            p = os.path.join(dirpath, n)
            rel = os.path.relpath(p, folder).replace('\\', '/')
            low = n.lower()
            if low.endswith(GAME_EXTENSIONS) and not allowed_extension(rel):
                problems.append('%s: a file type of the game' % rel)
            if game_name(rel_prefix + rel):
                problems.append('%s: a disc file name or a generated / game folder' % rel)
            with open(p, 'rb') as f:
                head = f.read(4)
                if head == b'\x7fELF':
                    problems.append('%s: an ELF file' % rel)
                if not allow_pe and head[:2] == b'MZ':
                    problems.append('%s: an executable' % rel)
                if os.path.getsize(p) > 0x8006:
                    f.seek(0x8001)
                    if f.read(5) == b'CD001':
                        problems.append('%s: an ISO9660 volume' % rel)
    return problems


def allowed_extension(rel):
    """Files of the toolchain or the libraries that share an extension with the game's (none so far but these)."""
    return rel.startswith(('kit/ucrt64/', 'kit/deps/', 'kit/python/', 'kit/ffmpeg/', 'kit/sources/')) and \
        not rel.lower().endswith(('.elf', '.irx', '.iso', '.pack', '.pss', '.vag', '.cue'))


def parse_ini(path):
    out = {}
    if os.path.isfile(path):
        with open(path, encoding='utf-8') as f:
            for line in f:
                if '=' in line and not line.lstrip().startswith('#'):
                    k, v = line.split('=', 1)
                    out[k.strip()] = v.strip()
    return out


def pe_imports_of(path, tools_dir):
    sys.path.insert(0, tools_dir)
    try:
        from sc3_build import pe_imports
    finally:
        sys.path.pop(0)
    return pe_imports(path)


def download(url, dest, expect_sha256=None, allowed_prefix=None):
    if allowed_prefix and not url.startswith(allowed_prefix):
        raise ReleaseError('refusing to download %s (only %s)' % (url, allowed_prefix))
    if os.path.isfile(dest) and (not expect_sha256 or sha256_file(dest) == expect_sha256):
        return dest
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    log('  downloading %s' % url)
    tmp = dest + '.part'
    req = urllib.request.Request(url, headers={'User-Agent': 'SoulCaliburIII_Recomp-make_release'})
    with urllib.request.urlopen(req, timeout=60) as r, open(tmp, 'wb') as f:
        shutil.copyfileobj(r, f, 1 << 20)
    if expect_sha256 and sha256_file(tmp) != expect_sha256:
        os.remove(tmp)
        raise ReleaseError('%s: SHA-256 differs from the pinned one' % url)
    os.replace(tmp, dest)
    return dest


def url_exists(url):
    req = urllib.request.Request(url, method='HEAD', headers={'User-Agent': 'SoulCaliburIII_Recomp-make_release'})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status == 200
    except Exception:
        return False


def zip_dir(folder, zip_path, top, date_time, compress=True):
    """A reproducible zip: entries sorted, every timestamp date_time, fixed permissions; directories as entries."""
    tmp = zip_path + '.part'
    method = zipfile.ZIP_DEFLATED if compress else zipfile.ZIP_STORED
    entries = []
    for dirpath, dirs, names in os.walk(folder):
        dirs.sort()
        rel_dir = os.path.relpath(dirpath, folder).replace('\\', '/')
        rel_dir = '' if rel_dir == '.' else rel_dir + '/'
        entries.append((top + '/' + rel_dir, None))
        for n in sorted(names):
            entries.append((top + '/' + rel_dir + n, os.path.join(dirpath, n)))
    entries.sort(key=lambda e: e[0])
    with zipfile.ZipFile(tmp, 'w', method, compresslevel=9 if compress else None) as zf:
        for name, path in entries:
            info = zipfile.ZipInfo(name, date_time)
            info.create_system = 0  # MS-DOS: no Unix permissions to differ between machines
            if path is None:
                info.external_attr = 0x10  # directory
                zf.writestr(info, b'')
                continue
            info.compress_type = method
            if compress:  # a hand-made ZipInfo has no level: the ZipFile's is not applied to it
                info.compress_level = 9
                info._compresslevel = 9
            info.external_attr = 0x20  # archive bit
            with open(path, 'rb') as src, zf.open(info, 'w', force_zip64=True) as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
    os.replace(tmp, zip_path)


def write_sha256(path):
    digest = sha256_file(path)
    with open(path + '.sha256', 'w', newline='\n') as f:
        f.write('%s *%s\n' % (digest, os.path.basename(path)))
    return digest


def sha256_matches(path):
    """True when <path>.sha256 holds path's SHA-256."""
    with open(path + '.sha256') as f:
        return f.read().split()[0].lower() == sha256_file(path)


def write_text(path, text, crlf=False, bom=False):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8-sig' if bom else 'utf-8', newline='\r\n' if crlf else '\n') as f:
        f.write(text)


# ---- the release ----

class Release:
    def __init__(self, args):
        self.args = args
        self.out = os.path.abspath(args.out)
        root_n = os.path.normcase(ROOT) + os.sep
        if (os.path.normcase(self.out) + os.sep).startswith(root_n) and \
                not os.path.normcase(self.out).startswith(os.path.normcase(os.path.join(ROOT, 'build')) + os.sep):
            raise ReleaseError('--out inside the repository must be under build/ (a release is never committed)')
        self.commit = git('rev-parse', '--verify', (args.commit or 'HEAD') + '^{commit}')
        self.version = args.version or self.tag_version()
        if not VERSION_RE.match(self.version):
            raise ReleaseError('version %r: expected MAJOR.MINOR.PATCH[-prerelease]' % self.version)
        self.name = '%s-%s' % (NAME, self.version)
        self.downloads = os.path.abspath(args.downloads or os.path.join(self.out, 'downloads'))
        self.work = os.path.join(self.out, 'work', self.version)
        self.stage = os.path.join(self.out, self.name)
        self.source = os.path.join(self.stage, 'source')
        self.kit = os.path.join(self.stage, 'kit')
        self.zip = os.path.join(self.out, self.name + '.zip')
        self.sources_zip = os.path.join(self.out, self.name + '-sources.zip')
        self.commit_date = int(git('show', '-s', '--format=%ct', self.commit))
        self.engine_commit = None
        self.manifest = None
        self.msys_sources = []  # (package base, version, licence, url, file, sha256)

    def tag_version(self):
        try:
            tag = git('describe', '--tags', '--exact-match', self.commit)
        except ReleaseError:
            raise ReleaseError('the commit has no tag: pass --version')
        return tag[1:] if tag[:1] in 'vV' else tag

    def zip_time(self):
        t = time.gmtime(max(self.commit_date, 315532800 + 86400))  # a zip cannot hold dates before 1980
        return t[:6]

    # -- export

    def step_export(self):
        rmtree(self.stage)
        os.makedirs(self.source)
        extract_tar_stream([find_git(), 'archive', '--format=tar', self.commit], self.source, ROOT)
        entry = git('ls-tree', self.commit, ENGINE_DIR).split()
        if len(entry) < 3 or entry[0] != '160000':
            raise ReleaseError('%s is not a submodule at %s' % (ENGINE_DIR, self.commit))
        self.engine_commit = entry[2]
        engine_repo = os.path.join(ROOT, ENGINE_DIR)
        try:
            git('cat-file', '-e', self.engine_commit + '^{commit}', cwd=engine_repo)
        except ReleaseError:
            raise ReleaseError('the submodule does not have %s: git submodule update --init %s'
                               % (self.engine_commit, ENGINE_DIR))
        engine_dst = os.path.join(self.source, ENGINE_DIR)
        os.makedirs(engine_dst, exist_ok=True)
        extract_tar_stream([find_git(), 'archive', '--format=tar', self.engine_commit], engine_dst, engine_repo)
        for skip in ENGINE_SKIP:
            rmtree(os.path.join(engine_dst, skip))
        # The list of the source's files: sc3_build removes the ones a newer release unzipped over this one no longer
        # has (CMake globs runtime/*.cpp: a deleted file left behind would still be compiled).
        files = sorted(os.path.relpath(os.path.join(d, n), self.source).replace('\\', '/')
                       for d, _s, names in os.walk(self.source) for n in names)
        files += ['release.ini', 'release_files.txt']
        write_text(os.path.join(self.source, 'release_files.txt'), '\n'.join(sorted(set(files))) + '\n')
        write_text(os.path.join(self.source, 'release.ini'),
                   '# The SoulCaliburIII_Recomp release this folder is (scripts/make_release.py writes this file).\n'
                   'version = %s\ncommit = %s\nengine = BozoEngine\nengine_commit = %s\ndate = %s\n'
                   % (self.version, self.commit, self.engine_commit,
                      time.strftime('%Y-%m-%d', time.gmtime(self.commit_date))))
        problems = audit_tree(self.source, rel_prefix='source/')
        if problems:
            raise ReleaseError('the source export holds what looks like game data:\n  ' + '\n  '.join(problems[:20]))
        n = sum(len(f) for _d, _s, f in os.walk(self.source))
        log('  source: %s + BozoEngine %s, %d files, %.1f MB, audited' % (self.commit[:12], self.engine_commit[:12], n,
                                                                         dir_size(self.source) / 1e6))

    # -- the kit

    def kit_tool(self):
        return [sys.executable, '-I', os.path.join(self.source, 'scripts', 'make_build_kit.py')]

    def step_kit(self):
        rmtree(self.kit)
        if self.args.kit:
            src = os.path.abspath(self.args.kit)
            if not os.path.isfile(os.path.join(src, 'kit_manifest.json')):
                raise ReleaseError('%s has no kit_manifest.json (--kit)' % src)
            # Hard links: 460 MB not copied. What make_build_kit rewrites in place is copied, so the original kit
            # never changes; its tools/ folder is replaced as a whole when rebuilt (links only removed).
            parts = [os.path.join('.parts', n).replace('\\', '/') for n in os.listdir(os.path.join(src, '.parts'))] \
                if os.path.isdir(os.path.join(src, '.parts')) else []
            link_tree(src, self.kit, copy=set(['kit_manifest.json'] + parts))
        else:
            run(self.kit_tool() + ['--out', self.kit, '--msys', self.args.msys, '--downloads', self.downloads,
                                   '--work', os.path.join(self.work, 'kit-work'), '--jobs', str(self.args.jobs)])
        with open(os.path.join(self.kit, 'kit_manifest.json')) as f:
            self.manifest = json.load(f)
        if self.manifest.get('kit') != NAME + ' build kit' or self.manifest.get('engine') != 'BozoEngine':
            raise ReleaseError('%s is not a %s kit (%s, kit_version %s): make one with scripts/make_build_kit.py at '
                               'this commit' % (self.args.kit or self.kit, NAME, self.manifest.get('kit'),
                                                self.manifest.get('kit_version')))
        self.check_kit_pins()
        self.prune_kit_deps()
        rmtree(os.path.join(self.kit, '.parts'))
        log('  kit: %.1f MB' % (dir_size(self.kit) / 1e6))

    def prune_kit_deps(self):
        """What this commit's make_build_kit.py leaves out of the libraries' sources (DEP_PRUNE), out of the release's
        copy of a kit made before (its links removed; the kit itself unchanged), and the manifest's left_out with it."""
        text = open(os.path.join(self.source, 'scripts', 'make_build_kit.py'), encoding='utf-8').read()
        block = re.search(r'^DEP_PRUNE = (\{.*?^\})', text, re.M | re.S)
        prune = eval(block.group(1), {'__builtins__': {}}) if block else {}  # a dict literal of string lists
        removed = 0
        for name, rels in prune.items():
            for rel in rels:
                p = os.path.join(self.kit, 'deps', name + '-src', rel)
                if os.path.isdir(p):
                    rmtree(p)
                    removed += 1
                elif os.path.isfile(p):
                    os.remove(p)
                    removed += 1
            if name in self.manifest['deps']:
                self.manifest['deps'][name]['left_out'] = list(rels)
        if removed:
            with open(os.path.join(self.kit, 'kit_manifest.json'), 'w') as f:
                json.dump(self.manifest, f, indent=1)
            log('  kit deps: %d folders / files the build never reads left out (DEP_PRUNE)' % removed)

    def check_kit_pins(self):
        """The kit's FFmpeg, Python and library sources are the ones this commit's make_build_kit.py pins."""
        text = open(os.path.join(self.source, 'scripts', 'make_build_kit.py'), encoding='utf-8').read()
        m = self.manifest
        wrong = []
        for what, ver in (('FFMPEG', m['ffmpeg']['version']), ('PYTHON', m['python']['version'])):
            pinned = re.search(r"^%s = \{\s*'version': '([^']+)'" % what, text, re.M)
            if not pinned or pinned.group(1) != ver:
                wrong.append('%s %s (pinned %s)' % (what.lower(), ver, pinned.group(1) if pinned else '?'))
        for name, info in m['deps'].items():
            if not re.search(r"'name': '%s',[^}]*'sha256': '%s'" % (re.escape(name), re.escape(info['sha256'])), text):
                wrong.append('%s %s' % (name, info['version']))
        # The toolchain's MSYS2 packages: the versions scripts/build_kit_packages.json pins at this commit.
        pins_path = os.path.join(self.source, 'scripts', 'build_kit_packages.json')
        if os.path.isfile(pins_path):
            with open(pins_path, encoding='utf-8') as f:
                pinned = {e['name']: e['version'] for e in json.load(f)['packages']}
            for name, info in m['toolchain']['msys2_packages'].items():
                if pinned.get(name) != info['version']:
                    wrong.append('%s %s (pinned %s)' % (name, info['version'], pinned.get(name, 'none')))
        if wrong:
            raise ReleaseError('the kit does not match this commit\'s pins (%s): make a new kit (make_build_kit.py)'
                               % ', '.join(wrong))

    # -- the launcher

    def font_dir(self):
        if self.args.font_dir:
            return os.path.abspath(self.args.font_dir)
        d = os.path.join(self.downloads, 'exo2-' + EXO2_COMMIT[:8])
        raw = 'https://raw.githubusercontent.com/google/fonts/%s/ofl/exo2/' % EXO2_COMMIT
        download(raw + 'Exo2%5Bwght%5D.ttf', os.path.join(d, 'Exo2[wght].ttf'), EXO2_SHA256)
        download(raw + 'OFL.txt', os.path.join(d, 'OFL.txt'))
        return d

    def step_launcher(self):
        build = os.path.join(self.work, 'launcher-build')
        rmtree(build)
        kbin = os.path.join(self.kit, 'ucrt64', 'bin')
        b = lambda n: os.path.join(kbin, n + '.exe').replace('\\', '/')  # noqa: E731
        deps = os.path.join(self.kit, 'deps')
        logf = os.path.join(self.work, 'launcher.log')
        env = clean_env([kbin])
        run([b('cmake'), '-S', os.path.join(self.source, 'launcher'), '-B', build, '-G', 'Ninja',
             '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=' + b('gcc'), '-DCMAKE_CXX_COMPILER=' + b('g++'),
             '-DCMAKE_RC_COMPILER=' + b('windres'), '-DCMAKE_MAKE_PROGRAM=' + b('ninja'),
             '-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
             '-DFETCHCONTENT_SOURCE_DIR_SDL3=' + os.path.join(deps, 'sdl3-src').replace('\\', '/'),
             '-DFETCHCONTENT_SOURCE_DIR_IMGUI=' + os.path.join(deps, 'imgui-src').replace('\\', '/'),
             '-DLAUNCHER_FONT_DIR=' + self.font_dir().replace('\\', '/'),
             '-DLAUNCHER_VERSION=' + self.version, '-DLAUNCHER_COMMIT=' + self.commit] + self.rc_flags(),
            env=env, logfile=logf)
        run([b('cmake'), '--build', build, '-j', str(self.args.jobs)], env=env, logfile=logf)
        exe = os.path.join(build, 'SC3 Launcher.exe')
        imports = pe_imports_of(exe, os.path.join(self.source, 'tools'))
        local = [n for n in imports if os.path.isfile(os.path.join(kbin, n))]
        if local:
            raise ReleaseError('the launcher loads %s (it must be static)' % ', '.join(local))
        shutil.copy2(exe, os.path.join(self.stage, 'SC3 Launcher.exe'))
        self.launcher_fonts = os.path.join(build, 'fonts')
        log('  SC3 Launcher.exe: %.1f MB, imports %s' % (os.path.getsize(exe) / 1e6, ', '.join(sorted(imports))))

    def rc_flags(self):
        """sc3_build's windres preprocessor flags (a path with spaces), from the exported source."""
        text = open(os.path.join(self.source, 'scripts', 'sc3_build.py'), encoding='utf-8').read()
        m = re.search(r"^RC_FLAGS = '([^']*)'", text, re.M)
        return ['-DCMAKE_RC_FLAGS=' + m.group(1)] if m else []

    # -- licences and sources

    def msys_packages(self):
        """(name, info) of every MSYS2 package the kit holds files of, sorted."""
        return sorted(self.manifest['toolchain']['msys2_packages'].items())

    @staticmethod
    def source_names(base, version):
        v = version.replace(':', '~')  # pacman's epoch in a file name
        return ['%s-%s.src.tar.%s' % (base, v, ext) for ext in ('zst', 'gz', 'xz')]

    def step_licenses(self):
        lic = os.path.join(self.stage, 'LICENSES')
        rmtree(lic)
        # The code: SoulCaliburIII_Recomp's own files are MIT (the repository's LICENSE); BozoEngine, the recompiler and runtime
        # in source\BozoEngine, is MIT too.
        for name, src in (('SoulCaliburIII_Recomp', os.path.join(self.source, 'LICENSE')),
                          ('BozoEngine', os.path.join(self.source, ENGINE_DIR, 'LICENSE'))):
            if not os.path.isfile(src):
                raise ReleaseError('%s has no LICENSE at this commit (%s)' % (name, src))
            os.makedirs(os.path.join(lic, name))
            shutil.copy2(src, os.path.join(lic, name, 'LICENSE'))
        # The kit's components: their own licence folders (make_build_kit), then what MSYS2 installed elsewhere for
        # the packages without one (share/doc), with --msys.
        shutil.copytree(os.path.join(self.kit, 'licenses'), os.path.join(lic, 'kit'))
        tool_lic = os.path.join(lic, 'kit', 'toolchain')
        missing = []
        for name, info in self.msys_packages():
            d = os.path.join(tool_lic, name)
            if os.path.isdir(d) and os.listdir(d):
                continue
            found = self.msys_doc_licenses(info['package'])
            if found:
                os.makedirs(d, exist_ok=True)
                for src in found:
                    shutil.copy2(src, os.path.join(d, os.path.basename(src)))
            else:
                missing.append(name)
                os.makedirs(d, exist_ok=True)
                write_text(os.path.join(d, 'NOTE.txt'),
                           '%s %s: MSYS2 installs no licence file for this package. Its licence field: %s.\n'
                           'The full terms are in its source package: %s%s\n'
                           % (info['package'], info['version'], info['license'].replace('spdx:', ''), MSYS2_SOURCES,
                              self.source_names(info['base'], info['version'])[0]), crlf=True)
        # The libraries built into the game and the launcher (kit\deps), and the launcher's font.
        for name in sorted(os.listdir(os.path.join(self.kit, 'deps'))):
            src = os.path.join(self.kit, 'deps', name)
            files = [n for n in os.listdir(src) if re.match(r'(LICEN[CS]E|COPYING|NOTICE)', n, re.I)
                     and os.path.isfile(os.path.join(src, n))]
            if files:
                d = os.path.join(lic, 'libraries', name[:-4] if name.endswith('-src') else name)
                os.makedirs(d, exist_ok=True)
                for n in files:
                    shutil.copy2(os.path.join(src, n), d)
        os.makedirs(os.path.join(lic, 'launcher-font-exo2'))
        shutil.copy2(os.path.join(self.launcher_fonts, 'OFL.txt'), os.path.join(lic, 'launcher-font-exo2', 'OFL.txt'))
        self.missing_licenses = missing
        self.write_sources_txt(lic)
        write_text(os.path.join(lic, 'README.txt'), LICENSES_README.format(name=self.name), crlf=True)
        log('  LICENSES: %d files%s' % (sum(len(f) for _d, _s, f in os.walk(lic)),
                                        ('; no licence folder in MSYS2 for %s: their texts are in their source '
                                         'packages (SOURCES.txt)' % ', '.join(missing)) if missing else ''))

    def msys_doc_licenses(self, package):
        """The licence files pacman's file list gives a package outside share/licenses (share/doc/<x>/COPYING...)."""
        db = os.path.join(self.args.msys, 'var', 'lib', 'pacman', 'local')
        if not os.path.isdir(db):
            return []
        for d in os.listdir(db):
            if d.startswith(package + '-') and re.match(r'^[0-9]', d[len(package) + 1:]):
                out = []
                with open(os.path.join(db, d, 'files'), encoding='utf-8') as f:
                    for line in f:
                        line = line.strip()
                        if re.match(r'ucrt64/share/doc/[^/]+/(LICEN[CS]E|COPYING|Copyright)[^/]*$', line, re.I):
                            p = os.path.join(self.args.msys, *line.split('/'))
                            if os.path.isfile(p):
                                out.append(p)
                return out
        return []

    def write_sources_txt(self, lic):
        m = self.manifest
        lines = [
            '%s: where the source of everything in this download is' % self.name,
            '',
            'SoulCaliburIII_Recomp (source\\, this release\'s code; MIT, LICENSES\\SoulCaliburIII_Recomp)',
            '  https://github.com/%s/tree/%s' % (REPO, self.commit),
            'BozoEngine, the recompiler and runtime the game is built on (source\\BozoEngine; MIT, LICENSES\\BozoEngine)',
            '  https://github.com/%s/tree/%s' % (ENGINE_REPO, self.engine_commit),
            '  The whole source is in source\\ already, the launcher\'s too (source\\launcher).',
            '',
            'The build kit (kit\\) redistributes these programs and libraries unmodified. Their corresponding source:',
            '',
            '1. MSYS2 UCRT64 packages (GCC, binutils, mingw-w64, winpthreads, CMake, Ninja and the DLLs they load):',
            '   the binaries in kit\\ucrt64 are those packages\' files, unmodified (some files left out). Each source',
            '   package below is the exact source the binaries were built from (MSYS2\'s PKGBUILD, patches and the',
            '   upstream tarball). They are attached to the same GitHub release as %s-sources.zip;' % self.name,
            '   they are also at the addresses below on MSYS2\'s official repository.',
            '',
        ]
        by_file = {e[4]: e for e in self.msys_sources}
        for name, info in self.msys_packages():
            files = self.source_names(info['base'], info['version'])
            entry = next((by_file[f] for f in files if f in by_file), None)
            fname = entry[4] if entry else files[0]
            lines.append('   %-40s %-28s %s' % (info['package'], info['version'], info['license'].replace('spdx:', '')))
            lines.append('      %s%s' % (MSYS2_SOURCES, fname))
            if entry:
                lines.append('      SHA-256 %s' % entry[5])
        ff = m['ffmpeg']
        lines += [
            '',
            '2. FFmpeg %s (LGPL-2.1-or-later), built static into the game from the official release tarball:' % ff['version'],
            '   kit\\sources\\%s (also in %s-sources.zip)' % (os.path.basename(ff['url']), self.name),
            '   %s  SHA-256 %s' % (ff['url'], ff['sha256']),
            '   configured: %s' % ' '.join(ff['configure']),
            '',
            '3. Python %s (PSF-2.0, not copyleft): %s' % (m['python']['version'], m['python']['url']),
            '',
            '4. Libraries built from source on your PC (kit\\deps, the sources themselves):',
        ]
        for name, info in sorted(m['deps'].items()):
            lines.append('   %-6s %s (%s)' % (name, info['version'], info['license']))
            lines.append('          %s  SHA-256 %s' % (info['url'], info['sha256']))
        lines += [
            '',
            'Written offer: for at least three years from the date of this release, the authors (The Bozos,',
            'https://github.com/%s/issues) will provide anyone with a copy of the corresponding source' % REPO,
            'of the GPL / LGPL binaries in this download, on a medium customarily used for software interchange, for',
            'no more than the cost of providing it. The sources zip attached to the release is that source.',
            '',
        ]
        write_text(os.path.join(lic, 'SOURCES.txt'), '\n'.join(lines), crlf=True)

    def step_sources(self):
        """The MSYS2 source packages, downloaded from repo.msys2.org only, cached; then the sources zip."""
        cache = os.path.join(self.downloads, 'msys2-sources')
        os.makedirs(cache, exist_ok=True)
        seen = set()
        self.msys_sources = []
        for name, info in self.msys_packages():
            key = (info['base'], info['version'])
            if key in seen:
                continue
            seen.add(key)
            got = None
            for fname in self.source_names(*key):
                dest = os.path.join(cache, fname)
                if os.path.isfile(dest) or url_exists(MSYS2_SOURCES + fname):
                    download(MSYS2_SOURCES + fname, dest, allowed_prefix=MSYS2_SOURCES)
                    got = fname
                    break
            if not got:
                raise ReleaseError('no source package for %s %s on %s' % (key[0], key[1], MSYS2_SOURCES))
            self.msys_sources.append((info['base'], info['version'], info['license'], MSYS2_SOURCES + got, got,
                                      sha256_file(os.path.join(cache, got))))
        # The licence files of the packages MSYS2 installs none for, from their source packages.
        self.licenses_from_sources(cache)
        # SOURCES.txt again, with the hashes.
        self.write_sources_txt(os.path.join(self.stage, 'LICENSES'))
        root = os.path.join(self.work, 'sources-zip')
        rmtree(root)
        os.makedirs(os.path.join(root, 'msys2'))
        for e in self.msys_sources:
            link_tree_file(os.path.join(cache, e[4]), os.path.join(root, 'msys2', e[4]))
        os.makedirs(os.path.join(root, 'ffmpeg'))
        ff = os.path.join(self.kit, 'sources', os.path.basename(self.manifest['ffmpeg']['url']))
        link_tree_file(ff, os.path.join(root, 'ffmpeg', os.path.basename(ff)))
        shutil.copy2(os.path.join(self.stage, 'LICENSES', 'SOURCES.txt'), os.path.join(root, 'SOURCES.txt'))
        with open(os.path.join(root, 'SHA256SUMS'), 'w', newline='\n') as f:
            for dirpath, _d, names in sorted(os.walk(root)):
                for n in sorted(names):
                    if n != 'SHA256SUMS':
                        p = os.path.join(dirpath, n)
                        f.write('%s *%s\n' % (sha256_file(p), os.path.relpath(p, root).replace('\\', '/')))
        zip_dir(root, self.sources_zip, self.name + '-sources', self.zip_time(), compress=False)
        digest = write_sha256(self.sources_zip)
        log('  %s: %d MSYS2 source packages + FFmpeg, %.1f MB, SHA-256 %s'
            % (os.path.basename(self.sources_zip), len(self.msys_sources), os.path.getsize(self.sources_zip) / 1e6,
               digest))
        rmtree(root)

    def licenses_from_sources(self, cache):
        try:
            from compression import zstd  # noqa: F401  (Python 3.14: tarfile reads .tar.zst)
            has_zstd = True
        except ImportError:
            has_zstd = False
        tool_lic = os.path.join(self.stage, 'LICENSES', 'kit', 'toolchain')
        still = []
        for name in getattr(self, 'missing_licenses', []):
            info = self.manifest['toolchain']['msys2_packages'][name]
            e = next((x for x in self.msys_sources if x[0] == info['base'] and x[1] == info['version']), None)
            found = {}
            if e and (has_zstd or not e[4].endswith('.zst')):
                found = licence_files_in_source_package(os.path.join(cache, e[4]))
            if not found:
                still.append(name)
                continue
            d = os.path.join(tool_lic, name)
            os.makedirs(d, exist_ok=True)
            for fname, data in found.items():
                with open(os.path.join(d, fname), 'wb') as f:
                    f.write(data)
        self.missing_licenses = still
        if still:
            log('  no licence file found for %s' % ', '.join(still))

    # -- player files, zip, notes

    def step_readme(self):
        write_text(os.path.join(self.stage, 'README.txt'),
                   PLAYER_README.format(version=self.version, name=self.name, repo=REPO), crlf=True, bom=True)
        # The changelog at the top, for players (Notepad) and the launcher's "What's new": the commit's, CRLF.
        text = self.changelog_text()
        if text is not None:
            text = text.replace('\r\n', '\n')
            if changelog_section(text, self.version) is None and self.args.allow_missing_changelog:
                # A test build (--allow-missing-changelog): a section of its own, so "What's new" and the launcher's
                # self-check find one for this version.
                at = text.find('\n## ')
                at = at + 1 if at >= 0 else len(text)
                text = text[:at] + '## [%s] - test build\n\n%s\n\n' % (self.version, self.changelog_section) + text[at:]
            write_text(os.path.join(self.stage, 'CHANGELOG.md'), text, crlf=True)

    # -- the changelog

    def changelog_text(self):
        """CHANGELOG.md as committed at the release's commit (None when the commit has none)."""
        try:
            return git('show', '%s:CHANGELOG.md' % self.commit, binary=True).decode('utf-8')
        except ReleaseError:
            return None

    def changelog(self):
        """The section of CHANGELOG.md for this version, or a placeholder with --allow-missing-changelog."""
        text = self.changelog_text()
        section = changelog_section(text, self.version) if text is not None else None
        if section:
            return section
        why = ('the commit has no CHANGELOG.md' if text is None else
               'CHANGELOG.md has no section "## [%s]" with content (move the Unreleased entries under it, with the '
               'date)' % self.version)
        if not self.args.allow_missing_changelog:
            raise ReleaseError(why + ': a release needs its changelog (--allow-missing-changelog for a test build)')
        log('  WARNING: %s; a placeholder is used (--allow-missing-changelog)' % why)
        return '_A test build: no changelog entry for %s._' % self.version

    def step_zip(self):
        problems = [p for p in audit_tree(self.stage, allow_pe=True)]
        if problems:
            raise ReleaseError('the release folder holds what looks like game data:\n  ' + '\n  '.join(problems[:20]))
        t = time.time()
        # The folder in the zip is SoulCaliburIII_Recomp, not SoulCaliburIII_Recomp-<version>: a new release unzipped
        # where the last one was replaces it (and keeps build\ and game\, so Rebuild only does what changed).
        zip_dir(self.stage, self.zip, TOP, self.zip_time())
        # The names once more, as the zip has them.
        with zipfile.ZipFile(self.zip) as zf:
            names = zf.namelist()
        bad = [n for n in names if game_name(n.split('/', 1)[1]) or
               (n.lower().endswith(GAME_EXTENSIONS) and not allowed_extension(n.split('/', 1)[1]))]
        if bad:
            raise ReleaseError('the zip has names of the game\'s files: %s' % ', '.join(bad[:10]))
        self.zip_sha256 = write_sha256(self.zip)
        log('  %s: %d entries, %.1f MB (unpacked %.1f MB), %.0f s, SHA-256 %s'
            % (os.path.basename(self.zip), len(names), os.path.getsize(self.zip) / 1e6, dir_size(self.stage) / 1e6,
               time.time() - t, self.zip_sha256))

    def step_notes(self):
        sources = os.path.isfile(self.sources_zip + '.sha256')
        lines = [
            '# SoulCaliburIII_Recomp %s' % self.version, '',
            'Soulcalibur III (USA) as a native PC game, built on your PC from **your own disc**: nothing of the '
            'game is in this download.', '',
            "## What's new", '', self.changelog_section.strip(), '',
            '## Download', '',
            '1. `%s.zip` below. Check its SHA-256 before running anything:' % self.name,
            '   `Get-FileHash .\\%s.zip` (PowerShell) must print' % self.name, '',
            '   ```', '   %s' % self.zip_sha256.upper(), '   ```', '',
            'Requires Windows 10 version 1903 or newer (or Windows 11), a 64-bit CPU and OpenGL 3.3.', '',
            '2. Unzip it anywhere you like except Program Files, with about 15 GB free, and double-click '
            '**SC3 Launcher.exe**. It is not signed: on *Windows protected your PC*, click **More info**, then '
            '**Run anyway**.',
            '3. Select your USA disc image (SLUS-21216), **Prepare the game** (once; the texture and model '
            'conversion and the compile take a while), **Play**.', '',
            'Saves and settings are in `%APPDATA%\\SoulCaliburIII_Recomp`; they stay across updates.',
            '',
            '## Build information', '',
            '- Code: %s (`%s`), BozoEngine `%s`' % (REPO, self.commit, self.engine_commit),
            '- Build kit: GCC %s, CMake %s, Ninja %s (MSYS2 UCRT64), FFmpeg %s (LGPL, static), Python %s'
            % (self.pkg_version('gcc'), self.pkg_version('cmake'), self.pkg_version('ninja'),
               self.manifest['ffmpeg']['version'], self.manifest['python']['version']),
            '- `%s.zip`: %.1f MB, SHA-256 `%s`' % (self.name, os.path.getsize(self.zip) / 1e6, self.zip_sha256),
        ]
        if sources:
            with open(self.sources_zip + '.sha256') as f:
                sdig = f.read().split()[0]
            lines.append('- `%s-sources.zip`: %.1f MB, SHA-256 `%s`' % (self.name, os.path.getsize(self.sources_zip) / 1e6,
                                                                      sdig))
        lines += [
            '', '## Source code and licences', '',
            'The code is in the zip (`source\\`) and at the commit above: our own code and BozoEngine (the recompiler '
            'and runtime it is built on) under the MIT licence. The build kit redistributes unmodified '
            'GPL / LGPL programs (GCC, binutils, mingw-w64, the DLLs CMake loads) and FFmpeg (LGPL-2.1+): '
            '`LICENSES\\` in the zip has their licences, and `LICENSES\\SOURCES.txt` lists each package with its exact '
            'source on repo.msys2.org.' +
            (' Their corresponding source is attached to this release as `%s-sources.zip`.' % self.name
             if sources else ' **Attach `%s-sources.zip` (make_release.py --with-sources) before publishing.**'
             % self.name) +
            ' For three years from this release we also provide it on request (GitHub issues).',
            '',
            'Soulcalibur III © Bandai Namco Entertainment, developed by Namco. An unofficial fan project by The Bozos '
            '(boubou666 & Arzaroth), not affiliated with or endorsed by them.', '']
        write_text(os.path.join(self.out, 'RELEASE_NOTES.md'), '\n'.join(lines))
        log('  RELEASE_NOTES.md (the changelog section for %s, the hashes, the source offer)' % self.version)

    def pkg_version(self, name):
        info = self.manifest['toolchain']['msys2_packages'].get(name)
        return info['version'] if info else '?'

    def publish(self):
        assets = [self.zip, self.zip + '.sha256']
        if os.path.isfile(self.sources_zip):
            assets += [self.sources_zip, self.sources_zip + '.sha256']
        else:
            log('  WARNING: no %s: the GPL source would not be attached' % os.path.basename(self.sources_zip))
        cmd = ['gh', 'release', 'create', 'v' + self.version, '--repo', REPO, '--draft', '--target', self.commit,
               '--title', '%s %s' % (NAME, self.version), '--notes-file', os.path.join(self.out, 'RELEASE_NOTES.md')]
        if '-' in self.version:
            cmd.append('--prerelease')
        cmd += assets
        if self.args.dry_run:
            log('  dry run, not run:\n  ' + subprocess.list2cmdline(cmd))
            return
        gh = shutil.which('gh')
        if not gh:
            raise ReleaseError('gh (GitHub CLI) is needed for --publish')
        # A second run for the same tag would make a second draft: refuse while one exists (draft or published).
        p = subprocess.run([gh, 'release', 'view', 'v' + self.version, '--repo', REPO, '--json', 'isDraft'],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if p.returncode == 0:
            raise ReleaseError('a release v%s exists already (%s): delete the draft on GitHub first'
                               % (self.version, p.stdout.decode('utf-8', 'replace').strip()))
        # The commit has to be on GitHub for --target: say so instead of pushing anything.
        log('  creating a DRAFT release (nothing is pushed; the commit must already be on GitHub)')
        run([gh] + cmd[1:])

    def publish_made(self):
        """--publish-only: the draft from what an earlier run left in --out, after its checks."""
        notes = os.path.join(self.out, 'RELEASE_NOTES.md')
        for p in (self.zip, self.zip + '.sha256', notes):
            if not os.path.isfile(p):
                raise ReleaseError('%s is missing: make the release first (without --publish-only)' % p)
        with open(notes, encoding='utf-8') as f:
            if not f.readline().strip().endswith(' ' + self.version):
                raise ReleaseError('%s is not the notes of %s' % (notes, self.version))
        problems = verify_zip(self.zip)
        if problems:
            raise ReleaseError('%s:\n  %s' % (self.zip, '\n  '.join(problems[:40])))
        if not sha256_matches(self.zip):
            raise ReleaseError('%s does not match its .sha256' % self.zip)
        self.publish()

    def main(self):
        os.makedirs(self.work, exist_ok=True)
        log('%s from %s' % (self.name, self.commit))
        # First, so a release without its changelog stops before the long steps.
        self.changelog_section = self.changelog()
        steps = [('export', self.step_export), ('kit', self.step_kit), ('launcher', self.step_launcher),
                 ('licenses', self.step_licenses), ('readme', self.step_readme)]
        if self.args.with_sources:
            steps.append(('sources', self.step_sources))
        steps += [('zip', self.step_zip), ('notes', self.step_notes)]
        if self.args.publish:
            steps.append(('publish', self.publish))
        t0 = time.time()
        for name, fn in steps:
            log('[%s]' % name)
            t = time.time()
            fn()
            log('  %.1f s' % (time.time() - t))
        if not self.args.keep_work:
            rmtree(self.work)
        log('done in %.0f s: %s' % (time.time() - t0, self.zip))


CHANGELOG_HEADING = re.compile(r'^##[ \t]+\[?([^\]\s]+)\]?(.*)$', re.M)


def changelog_section(text, version):
    """The body of the "## [version] - date" section of a Keep a Changelog text (up to the next "## " heading), the
    link references removed, stripped; None when missing, empty, or the version is "Unreleased"."""
    if version.lower() == 'unreleased':
        return None
    text = text.replace('\r\n', '\n')
    heads = list(CHANGELOG_HEADING.finditer(text))
    for i, m in enumerate(heads):
        if m.group(1).lstrip('vV') != version:
            continue
        end = heads[i + 1].start() if i + 1 < len(heads) else len(text)
        body = re.sub(r'^\[[^\]]+\]:\s*\S+\s*$', '', text[m.end():end], flags=re.M).strip()
        return body if re.search(r'\w', body) else None
    return None


def verify_zip(path, top=TOP):
    """The zip audited as the release folder is (--verify-zip): names of the game's files or the build's folders, ELF
    and ISO9660 contents, the top folder, the files a release must have. Problems as a list of strings."""
    problems = []
    with zipfile.ZipFile(path) as zf:
        names = zf.namelist()
        tops = set(n.split('/', 1)[0] for n in names)
        if tops != {top}:
            problems.append('the zip\'s top folders are %s, expected %s only' % (sorted(tops), top))
        for info in zf.infolist():
            n = info.filename
            rel = n.split('/', 1)[1] if '/' in n else n
            if rel.replace('\\', '/').lower().startswith('source/ps2recomp/'):
                problems.append('%s: PS2Recomp in a BozoEngine release' % n)
            if n.endswith('/'):
                if game_name(rel):
                    problems.append('%s: a generated / game folder' % n)
                continue
            if game_name(rel) or (rel.lower().endswith(GAME_EXTENSIONS) and not allowed_extension(rel)):
                problems.append('%s: named like a file of the game' % n)
            with zf.open(info) as f:
                head = f.read(0x8006)
            if head[:4] == b'\x7fELF':
                problems.append('%s: an ELF file' % n)
            if len(head) >= 0x8006 and head[0x8001:0x8006] == b'CD001':
                problems.append('%s: an ISO9660 volume' % n)
        for need in ('SC3 Launcher.exe', 'README.txt', 'CHANGELOG.md', 'LICENSES/SoulCaliburIII_Recomp/LICENSE',
                     'LICENSES/BozoEngine/LICENSE', 'source/release.ini', 'kit/kit_manifest.json',
                     'kit/python/python.exe', 'source/scripts/sc3_build.py', 'source/BozoEngine/CMakeLists.txt',
                     'kit/deps/sdl3-src/CMakeLists.txt', 'kit/deps/imgui-src/imgui.cpp'):
            if '%s/%s' % (top, need) not in names:
                problems.append('%s/%s is missing' % (top, need))
    return problems


def link_tree_file(src, dst):
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def licence_files_in_source_package(path):
    """{name: bytes} of the licence files at the top of the upstream tarball inside an MSYS2 source package (the
    package's folder holds the PKGBUILD, patches and that tarball)."""
    found = {}
    try:
        with tarfile.open(path) as outer:
            for m in outer.getmembers():
                base = m.name.rsplit('/', 1)[-1]
                if not m.isfile():
                    continue
                if re.match(r'(LICEN[CS]E|COPYING|COPYRIGHT)', base, re.I) and m.name.count('/') <= 1:
                    found[base] = outer.extractfile(m).read()
                elif re.search(r'\.tar\.(gz|xz|bz2|zst)$|\.tgz$', base) and m.size < 400 << 20:
                    data = outer.extractfile(m).read()
                    try:
                        with tarfile.open(fileobj=io.BytesIO(data)) as inner:
                            for im in inner.getmembers():
                                ib = im.name.rsplit('/', 1)[-1]
                                if im.isfile() and im.name.count('/') == 1 and \
                                        re.match(r'(LICEN[CS]E|COPYING|COPYRIGHT)', ib, re.I):
                                    found[ib] = inner.extractfile(im).read()
                    except (tarfile.TarError, EOFError, OSError):
                        pass
    except (tarfile.TarError, EOFError, OSError):
        return {}
    return found


LICENSES_README = """{name}: licences

SoulCaliburIII_Recomp\\LICENSE    SoulCaliburIII_Recomp's own code in source\\ (MIT)
BozoEngine\\LICENSE    BozoEngine, the recompiler and runtime in source\\BozoEngine (MIT)
kit\\                 the build kit's components: toolchain\\<MSYS2 package> (GCC, binutils, mingw-w64,
                      winpthreads, CMake, Ninja and the DLLs they load), ffmpeg\\, python\\
libraries\\            the libraries compiled into the game and the launcher (SDL3, Dear ImGui)
launcher-font-exo2\\   the launcher's font, Exo 2 (SIL Open Font License 1.1)
SOURCES.txt           where the source of every component is, with the exact MSYS2 source packages

Soulcalibur III is (c) Bandai Namco Entertainment. Nothing of the game is in this download.
"""

PLAYER_README = """Soulcalibur III - PC port ({name})
by The Bozos (boubou666 & Arzaroth)

[English]

What this is
  A native PC version of Soulcalibur III (PS2), built on your own PC from YOUR OWN disc.
  Nothing of the game is in this download: no game files, no code taken from the game.
  Requires Windows 10 version 1903 or newer (or Windows 11), a 64-bit CPU and OpenGL 3.3.
  You need a disc image of the USA release (SLUS-21216): a .iso, or a .bin with its .cue.
  Discs of the other regions are not supported.

Before you start: check the download
  The release page gives the SHA-256 of {name}.zip. In PowerShell:
      Get-FileHash .\\{name}.zip
  The hash printed must be the same as on the release page (and in {name}.zip.sha256).

Install
  1. Unzip the whole zip anywhere you like, with about 15 GB free, for example into D:\\Games: that makes
     D:\\Games\\SoulCaliburIII_Recomp. Not in "Program Files" (the launcher writes the game next to itself).
     Keep the folders together: SC3 Launcher.exe, source\\ and kit\\.
  2. Double-click "SC3 Launcher.exe". It is not signed, so Windows may show
     "Windows protected your PC": click "More info", then "Run anyway".
  3. Click "Select your disc image..." and pick your USA disc image.
  4. Click "Prepare the game". It takes a while, once (converting the textures and models, then compiling).
     You can stop it; it continues later.
  5. Click "Play". In the game, F2 opens the settings (controls, video).

Game speed
  The game updates at 60 Hz; movies keep their original cadence.

Your saves and settings
  %APPDATA%\\SoulCaliburIII_Recomp (the launcher's Maintenance tab opens it). They stay when you update.

Updates
  The launcher tells you when a new version is out (Settings: turn that check off; it only asks GitHub
  for the latest version number). Download the new zip and unzip it where you unzipped this one
  (replace the files when Windows asks), then click "Rebuild now" in the launcher: only what changed
  is built again. Your saves stay.

Uninstall
  Delete this folder. Your saves and settings are in %APPDATA%\\SoulCaliburIII_Recomp: delete it too to remove everything.

Help
  https://github.com/{repo}/issues
  When preparing fails, the launcher's "Copy details" button copies what we need to know.

Licences and source code: LICENSES\\ (SOURCES.txt says where every source is). Our code is MIT, and so is
BozoEngine, the recompiler and runtime it is built on.
Soulcalibur III (c) Bandai Namco Entertainment, developed by Namco.
An unofficial fan project, not affiliated with or endorsed by them.

[Français]

De quoi s'agit-il
  Une version PC native de Soulcalibur III (PS2), construite sur votre PC à partir de VOTRE PROPRE disque.
  Rien du jeu n'est dans ce téléchargement : aucun fichier du jeu, aucun code issu du jeu.
  Nécessite Windows 10 version 1903 ou ultérieure (ou Windows 11), un processeur 64 bits et OpenGL 3.3.
  Il vous faut une image disque de la version américaine (SLUS-21216) : un .iso, ou un .bin avec son .cue.
  Les disques des autres régions ne sont pas pris en charge.

Avant de commencer : vérifiez le téléchargement
  La page de la version donne le SHA-256 de {name}.zip. Dans PowerShell :
      Get-FileHash .\\{name}.zip
  L'empreinte affichée doit être la même que sur la page de la version (et dans {name}.zip.sha256).

Installation
  1. Décompressez tout le zip où vous voulez, avec environ 15 Go libres, par exemple dans D:\\Jeux : cela donne
     D:\\Jeux\\SoulCaliburIII_Recomp. Pas dans « Program Files » (le lanceur écrit le jeu à côté de lui).
     Gardez les dossiers ensemble : SC3 Launcher.exe, source\\ et kit\\.
  2. Double-cliquez sur « SC3 Launcher.exe ». Il n'est pas signé : si Windows affiche
     « Windows a protégé votre ordinateur », cliquez sur « Informations complémentaires », puis « Exécuter quand même ».
  3. Cliquez sur « Choisir votre image disque... » et choisissez votre image du disque américain.
  4. Cliquez sur « Préparer le jeu ». Cela prend un moment, une seule fois (conversion des textures et modèles, puis
     compilation). Vous pouvez l'arrêter ; la suite reprendra.
  5. Cliquez sur « Jouer ». En jeu, F2 ouvre les réglages (commandes, vidéo).

Vitesse
  Le jeu tourne à 60 Hz ; les vidéos conservent leur cadence d'origine.

Vos sauvegardes et réglages
  %APPDATA%\\SoulCaliburIII_Recomp (l'onglet Maintenance du lanceur l'ouvre). Ils restent lors d'une mise à jour.

Mises à jour
  Le lanceur vous prévient quand une nouvelle version sort (Réglages : désactivez cette vérification ; elle demande
  seulement à GitHub le numéro de la dernière version). Téléchargez le nouveau zip et décompressez-le au même endroit
  que celui-ci (remplacez les fichiers quand Windows le demande), puis cliquez sur « Reconstruire maintenant » dans le
  lanceur : seul ce qui a changé est refait. Vos sauvegardes restent.

Désinstallation
  Supprimez ce dossier. Vos sauvegardes et réglages sont dans %APPDATA%\\SoulCaliburIII_Recomp : supprimez-le aussi pour tout enlever.

Aide
  https://github.com/{repo}/issues
  Si la préparation échoue, le bouton « Copier les détails » du lanceur copie ce dont nous avons besoin.

Licences et code source : LICENSES\\ (SOURCES.txt indique où se trouve chaque source). Notre code est sous
licence MIT, tout comme BozoEngine, le recompilateur et le moteur sur lesquels il repose.
Soulcalibur III (c) Bandai Namco Entertainment, développé par Namco.
Un projet de fans non officiel, ni affilié ni approuvé par eux.

Version {version}
"""


def main():
    ap = argparse.ArgumentParser(description='Makes the SoulCaliburIII_Recomp release zip (see the docstring).')
    ap.add_argument('--version', help='MAJOR.MINOR.PATCH[-pre] (default: the commit\'s tag, v stripped)')
    ap.add_argument('--commit', help='the commit to release (default HEAD); only committed files count')
    ap.add_argument('--out', help='the folder for the zips and the work (outside the repository)')
    ap.add_argument('--kit', help='an existing build kit to ship (default: assembled with make_build_kit.py)')
    ap.add_argument('--msys', default='C:/msys64', help='MSYS2 (the kit\'s toolchain, its licence files)')
    ap.add_argument('--downloads', help='download cache: the font, the MSYS2 source packages (default <out>/downloads)')
    ap.add_argument('--font-dir', help='a folder with Exo2[wght].ttf and OFL.txt (default: downloaded, SHA-256 checked)')
    ap.add_argument('--with-sources', action='store_true', help='download the MSYS2 source packages and write '
                    '<name>-sources.zip (the GPL / LGPL corresponding source, attached to the release)')
    ap.add_argument('--publish', action='store_true', help='create a DRAFT GitHub release with gh (never published, '
                    'nothing pushed)')
    ap.add_argument('--dry-run', action='store_true', help='with --publish: print the gh command only')
    ap.add_argument('--publish-only', action='store_true', help='the draft from a release already made in --out (the '
                    'zips and RELEASE_NOTES.md of this version): the release workflow checks the zip in between')
    ap.add_argument('--keep-work', action='store_true', help='keep <out>/work (the launcher build)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--allow-missing-changelog', action='store_true', help='a test build: a placeholder when '
                    'CHANGELOG.md has no section for the version (never for a real release)')
    ap.add_argument('--check-changelog', action='store_true', help='only print CHANGELOG.md\'s section for the version '
                    '(--version or the commit\'s tag) at the commit; exit 1 when it is missing')
    ap.add_argument('--verify-zip', metavar='ZIP', help='only audit a release zip (no game file, the expected files)')
    args = ap.parse_args()
    try:
        if args.verify_zip:
            problems = verify_zip(args.verify_zip)
            if problems:
                raise ReleaseError('%s:\n  %s' % (args.verify_zip, '\n  '.join(problems[:40])))
            log('%s: no game file, the expected files present' % args.verify_zip)
            return
        if args.check_changelog:
            commit = git('rev-parse', '--verify', (args.commit or 'HEAD') + '^{commit}')
            version = args.version
            if not version:
                try:
                    tag = git('describe', '--tags', '--exact-match', commit)
                except ReleaseError:
                    raise ReleaseError('the commit has no tag: pass --version')
                version = tag[1:] if tag[:1] in 'vV' else tag
            try:
                text = git('show', '%s:CHANGELOG.md' % commit, binary=True).decode('utf-8')
            except ReleaseError:
                raise ReleaseError('%s has no CHANGELOG.md' % commit[:12])
            section = changelog_section(text, version)
            if not section:
                raise ReleaseError('CHANGELOG.md at %s has no section "## [%s]" with content' % (commit[:12], version))
            log(section)
            return
        if not args.out:
            ap.error('--out is needed')
        if args.publish_only:
            Release(args).publish_made()
            return
        Release(args).main()
    except ReleaseError as e:
        sys.exit('make_release: %s' % e)


if __name__ == '__main__':
    main()
