#!/usr/bin/env python3
"""apply_update: installs a downloaded release over this one, for the launcher's Update button.

  python -I scripts/apply_update.py --zip <SoulCaliburIII_Recomp-x.y.z.zip> --sha256 <its .sha256 file> --version x.y.z
                                  --install <the release folder> [--keep-zip]
  python3 -I scripts/apply_update.py --zip <SoulCaliburIII_Recomp-x.y.z-linux.tar.gz> ...       (the Linux release)

Checks the zip's SHA-256 against the release's .sha256, checks every name in it (one top folder SoulCaliburIII_Recomp/, no
absolute or parent paths, nothing under build/, game/ or update/, the launcher and source/release.ini of the version
asked for), extracts it whole to <install>/update/staging, and only then moves its files over the release folder.
A file in use (the launcher that started this, the kit's Python running it) is renamed to <name>.sc3-old first, as
Windows lets a running program's file be renamed but not replaced; those are listed in update/old_files.txt for the
launcher to delete at its next start. The player's own folders (build/, game/) and saves (%APPDATA%) are never
touched; sc3_build removes the source files a release no longer has (source/release_files.txt).

Prints one line per event for the launcher: "step <verify|extract|install|clean>", "progress <done> <total>" (bytes),
"done <version>", or "error <message>" (exit code 1).
"""
import argparse
import hashlib
import os
import shutil
import sys
import tarfile
import zipfile

TOP = 'SoulCaliburIII_Recomp/'  # scripts/make_release.py's TOP
LAUNCHERS = {'zip': 'SC3 Launcher.exe', 'tar': 'sc3-launcher'}
PROTECTED = ('build/', 'game/', 'update/')  # the player's build and game, and this update's own folder


class UpdateError(Exception):
    pass


def say(*parts):
    print(*parts, flush=True)


def sha256_of(path):
    h = hashlib.sha256()
    total = os.path.getsize(path)
    done = 0
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
            done += len(block)
            if done % (16 << 20) < (1 << 20):
                say('progress', done, total)
    return h.hexdigest()


def expected_sha256(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        token = f.read().split()
    if not token or len(token[0]) != 64 or any(c not in '0123456789abcdefABCDEF' for c in token[0]):
        raise UpdateError('the .sha256 file does not hold a SHA-256')
    return token[0].lower()


class Archive:
    """A release's zip (Windows) or .tar.gz (Linux): its entries as (name, is_dir, size, mode), read the same way."""

    def __init__(self, path):
        if tarfile.is_tarfile(path):
            self.kind, self.tf = 'tar', tarfile.open(path, 'r:gz')
            self.members = {}
            for m in self.tf.getmembers():
                if not (m.isfile() or m.isdir()):
                    raise UpdateError('unexpected entry in the archive: %r (a link or device)' % m.name)
                name = m.name + ('/' if m.isdir() and not m.name.endswith('/') else '')
                self.members[name] = m
        else:
            self.kind, self.zf = 'zip', zipfile.ZipFile(path)

    def entries(self):
        if self.kind == 'tar':
            return [(n, m.isdir(), m.size, m.mode) for n, m in self.members.items()]
        return [(i.filename, i.filename.endswith('/'), i.file_size, None) for i in self.zf.infolist()]

    def open(self, name):
        return self.tf.extractfile(self.members[name]) if self.kind == 'tar' else self.zf.open(name)

    def read(self, name):
        with self.open(name) as f:
            return f.read()

    def close(self):
        (self.tf if self.kind == 'tar' else self.zf).close()


def checked_names(archive, version):
    """The archive's file entries as (name, size, mode, path under the top folder), after checking every name."""
    files = []
    for name, is_dir, size, mode in archive.entries():
        if not name.startswith(TOP) or '\\' in name or ':' in name or name.startswith('/'):
            raise UpdateError('unexpected name in the zip: %r' % name)
        rel = name[len(TOP):]
        if any(part in ('..', '.') for part in rel.split('/')):
            raise UpdateError('unexpected name in the zip: %r' % name)
        if rel.startswith(PROTECTED) or rel.rstrip('/') in ('build', 'game', 'update'):
            raise UpdateError('the zip holds %r, a folder of the player\'s own' % name)
        if not is_dir:
            files.append((name, size, mode, rel))
    rels = {f[3] for f in files}
    if LAUNCHERS[archive.kind] not in rels or 'source/release.ini' not in rels:
        raise UpdateError('this is not a SoulCaliburIII_Recomp release (no launcher or source/release.ini)')
    stamp = archive.read(TOP + 'source/release.ini').decode('utf-8', 'replace')
    found = ''
    for line in stamp.splitlines():
        key, _, value = line.partition('=')
        if key.strip() == 'version':
            found = value.strip()
    if found != version:
        raise UpdateError('the zip is version %r, not %r' % (found, version))
    return files


def remove_tree(path):
    def onerror(func, p, _exc):
        os.chmod(p, 0o666)
        func(p)
    if os.path.isdir(path):
        shutil.rmtree(path, onerror=onerror)


def replace(src, dst, old_files):
    """Moves src over dst; a dst in use is renamed aside first (<dst>.sc3-old, or .sc3-old2... if that one is too)."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    try:
        os.replace(src, dst)
        return
    except PermissionError:
        pass
    for n in range(1, 100):
        aside = dst + ('.sc3-old' if n == 1 else '.sc3-old%d' % n)
        try:
            if os.path.exists(aside):
                os.remove(aside)
            os.rename(dst, aside)
            break
        except OSError:
            continue
    else:
        raise UpdateError('%s is in use and could not be set aside' % dst)
    old_files.append(aside)
    os.replace(src, dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--zip', required=True)
    ap.add_argument('--sha256', required=True)
    ap.add_argument('--version', required=True)
    ap.add_argument('--install', required=True)
    ap.add_argument('--keep-zip', action='store_true')
    args = ap.parse_args()
    install = os.path.abspath(args.install)
    work = os.path.join(install, 'update')
    staging = os.path.join(work, 'staging')

    say('step verify')
    want = expected_sha256(args.sha256)
    got = sha256_of(args.zip)
    if got != want:
        raise UpdateError('the download is damaged (SHA-256 %s, expected %s): try again' % (got, want))

    say('step extract')
    remove_tree(staging)
    archive = Archive(args.zip)
    try:
        files = checked_names(archive, args.version)
        total = sum(f[1] for f in files)
        if shutil.disk_usage(install).free < total + (256 << 20):
            raise UpdateError('not enough free space: %d MB needed' % ((total >> 20) + 256))
        done = 0
        for name, size, mode, rel in files:
            target = os.path.join(staging, *rel.split('/'))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with archive.open(name) as src, open(target, 'wb') as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
            if mode is not None:
                os.chmod(target, 0o755 if mode & 0o111 else 0o644)
            done += size
            say('progress', done, total)
    finally:
        archive.close()

    # Everything is extracted and checked: only moves on the same disk from here.
    say('step install')
    old_files = []
    for n, (_name, _size, _mode, rel) in enumerate(files):
        replace(os.path.join(staging, *rel.split('/')), os.path.join(install, *rel.split('/')), old_files)
        if n % 200 == 0:
            say('progress', n, len(files))
    say('progress', len(files), len(files))
    if old_files:
        with open(os.path.join(work, 'old_files.txt'), 'a', encoding='utf-8') as f:
            f.write(''.join(path + '\n' for path in old_files))

    say('step clean')
    remove_tree(staging)
    if not args.keep_zip:
        for path in (args.zip, args.sha256):
            try:
                os.remove(path)
            except OSError:
                pass
    say('done', args.version)


if __name__ == '__main__':
    try:
        main()
    except (UpdateError, OSError, zipfile.BadZipFile, tarfile.TarError) as e:
        say('error', e)
        sys.exit(1)
