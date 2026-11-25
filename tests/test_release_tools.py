"""The release tools (scripts/sc3_build.py, make_release.py, apply_update.py) without game data."""
import io
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import apply_update
import make_release
import sc3_build

REQUIRED = (
    'SC3 Launcher.exe', 'README.txt', 'CHANGELOG.md', 'LICENSES/SoulCaliburIII_Recomp/LICENSE',
    'LICENSES/BozoEngine/LICENSE', 'source/release.ini', 'kit/kit_manifest.json',
    'kit/python/python.exe', 'source/scripts/sc3_build.py', 'source/BozoEngine/CMakeLists.txt',
    'kit/deps/sdl3-src/CMakeLists.txt', 'kit/deps/imgui-src/imgui.cpp',
)
ENGINE = Path(os.environ.get('BOZO_ENGINE_DIR') or ROOT / 'BozoEngine')


def release_zip(path, extra=()):
    with zipfile.ZipFile(path, 'w') as archive:
        for name in REQUIRED:
            archive.writestr('SoulCaliburIII_Recomp/' + name, b'fixture')
        for name, data in extra:
            archive.writestr(name, data)


class BuildTests(unittest.TestCase):
    def test_configure_starts_afresh_after_a_toolchain_change(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            cache = root / 'CMakeCache.txt'
            cache.write_text('CMAKE_HOME_DIRECTORY:INTERNAL=' + sc3_build.ROOT +
                             '\nBOZO_ENGINE_DIR:PATH=engine\nHAVE_XINPUT_H:INTERNAL=\n', encoding='utf-8')
            (root / 'CMakeFiles').mkdir()
            (root / 'CMakeFiles/old-check').write_bytes(b'failed check')
            stamp = root / '.sc3-toolchain-key'
            stamp.write_text('before', encoding='utf-8')
            build = sc3_build.Build.__new__(sc3_build.Build)
            build.cmake_build = folder
            build.disc_dir = folder
            build.args = SimpleNamespace(build_type='Release', debug_info='no')
            build.ffmpeg_root = folder
            build.toolchain_key = lambda: 'after'
            build.tool = lambda name: name
            build.elf = lambda: str(root / 'own-elf')
            build.fetch_args = lambda: []
            commands = []

            def configure(command):
                self.assertFalse(cache.exists())
                self.assertFalse((root / 'CMakeFiles').exists())
                commands.append(command)
            build.run = configure
            build.step_configure()
            self.assertEqual(stamp.read_text(encoding='utf-8').strip(), 'after')
            self.assertIn('-DSC3_WINDOW=ON', commands[0])
            self.assertIn('-DSC3_GUI_SUBSYSTEM=ON', commands[0])
            self.assertIn('-DBOZO_FFMPEG_ROOT=' + folder.replace('\\', '/'), commands[0])

    def test_extract_takes_only_what_the_build_reads(self):
        build = sc3_build.Build.__new__(sc3_build.Build)
        build.state, build.disc_dir = {'boot': 'SLUS_212.16'}, ''
        wanted = [n for n in ('SLUS_212.16', 'SCES_533.12', 'ROOT.OLK', 'IRX/CRI_ADXI.IRX', 'IRX/NUSOUND.IRX',
                              'IRX/SDRDRV.IRX', 'IRX\\SDRDRV.IRX', 'MOVIE.AFS', 'BGM.AFS', 'IRX/LIBSD.IRX', 'SYSTEM.CNF')
                  if build.wanted(n)]
        self.assertEqual(wanted, ['SLUS_212.16', 'ROOT.OLK', 'IRX/CRI_ADXI.IRX', 'IRX/NUSOUND.IRX', 'IRX/SDRDRV.IRX',
                                  'IRX\\SDRDRV.IRX'])

    def test_source_cleanup_cannot_escape_the_installation(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'source'
            root.mkdir()
            outside = Path(folder) / 'outside'
            outside.mkdir()
            (outside / 'keep.txt').write_bytes(b'outside source')
            (root / 'release_files.txt').write_text('../outside/keep.txt\n', encoding='utf-8')
            with patch.object(sc3_build, 'ROOT', str(root)), self.assertRaises(sc3_build.BuildError):
                sc3_build.remove_stale_sources()
            self.assertEqual((outside / 'keep.txt').read_bytes(), b'outside source')

    def test_update_removes_files_a_release_dropped_and_keeps_player_data(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'source'
            root.mkdir()
            (root / 'release_files.txt').write_text('scripts/kept.py\n', encoding='utf-8')
            for name in ('scripts/kept.py', 'scripts/removed.py', 'custom/untouched.txt', 'game/save.dat'):
                file = root / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(b'player data or source')
            with patch.object(sc3_build, 'ROOT', str(root)):
                sc3_build.remove_stale_sources()
            self.assertFalse((root / 'scripts/removed.py').exists())
            for name in ('scripts/kept.py', 'custom/untouched.txt', 'game/save.dat'):
                self.assertEqual((root / name).read_bytes(), b'player data or source')

    def test_update_never_follows_a_source_directory_link(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'source'
            external = Path(folder) / 'external'
            root.mkdir()
            external.mkdir()
            (root / 'scripts').mkdir()
            (root / 'release_files.txt').write_text('scripts/kept.py\n', encoding='utf-8')
            (external / 'keep.txt').write_bytes(b'outside source')
            if sys.platform == 'win32':
                import _winapi
                _winapi.CreateJunction(str(external), str(root / 'scripts/linked'))
            else:
                (root / 'scripts/linked').symlink_to(external, target_is_directory=True)
            with patch.object(sc3_build, 'ROOT', str(root)):
                sc3_build.remove_stale_sources()
            self.assertEqual((external / 'keep.txt').read_bytes(), b'outside source')
            self.assertTrue((root / 'scripts/linked').exists())

    def disc_folder(self, folder, name, data=b'elf'):
        (Path(folder) / name).write_bytes(data)
        build = sc3_build.Build.__new__(sc3_build.Build)
        build.args, build.state, build.disc_dir = SimpleNamespace(disc=folder), {}, str(Path(folder) / 'work')
        return build

    def test_verify_takes_the_european_disc_and_its_recomp_inputs(self):
        with tempfile.TemporaryDirectory() as folder:
            build = self.disc_folder(folder, 'SCES_533.12')
            digest = sc3_build.hashlib.sha1(b'elf').hexdigest()
            releases = dict(sc3_build.RELEASES, **{'SCES_533.12': (digest,) + sc3_build.RELEASES['SCES_533.12'][1:]})
            with patch.object(sc3_build, 'RELEASES', releases), redirect_stdout(io.StringIO()):
                self.assertEqual(build.step_verify(), 'SCES_533.12')
                self.assertEqual(build.boot(), 'SCES_533.12')
                self.assertEqual(build.elf(), str(Path(folder) / 'work' / 'SCES_533.12'))
                self.assertEqual(build.symbol_map(), str(ROOT / 'recomp' / 'pal' / 'sc3_symbols.txt'))
                self.assertTrue(build.wanted('SCES_533.12'))
                self.assertFalse(build.wanted('SLUS_212.16'))

    def test_verify_names_the_disc_a_damaged_copy_should_be(self):
        with tempfile.TemporaryDirectory() as folder:
            build = self.disc_folder(folder, 'SCES_533.12')
            with self.assertRaises(sc3_build.BuildError) as raised:
                build.step_verify()
            self.assertIn('SHA-1', str(raised.exception))
            self.assertIn('SCES-53312', str(raised.exception))

    def test_verify_refuses_another_release(self):
        with tempfile.TemporaryDirectory() as folder:
            build = self.disc_folder(folder, 'SLES_533.70')
            with self.assertRaises(sc3_build.BuildError) as raised:
                build.step_verify()
            self.assertIn('boots SLES_533.70', str(raised.exception))
            self.assertIn('SLUS-21216', str(raised.exception))
            self.assertIn('SCES-53312', str(raised.exception))

    def test_a_work_folder_without_a_verify_record_builds_the_extracted_disc(self):
        with tempfile.TemporaryDirectory() as folder:
            build = self.disc_folder(folder, 'ignored')
            Path(build.disc_dir).mkdir()
            (Path(build.disc_dir) / 'SCES_533.12').write_bytes(b'')
            self.assertEqual(build.boot(), 'SCES_533.12')

    @unittest.skipUnless((ENGINE / 'tools/native_pack/ps2_disc.py').is_file(), 'no BozoEngine checkout')
    def test_check_refuses_a_file_that_is_not_a_disc(self):
        with tempfile.TemporaryDirectory() as folder:
            image = Path(folder) / 'zeros.iso'
            image.write_bytes(bytes(1 << 20))
            out = subprocess.run([sys.executable, '-I', str(ROOT / 'scripts/sc3_build.py'), '--engine', str(ENGINE),
                                  '--check', '--disc', str(image)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 universal_newlines=True)
            self.assertEqual(out.returncode, 1)
            self.assertIn('ERROR verify', out.stdout)
            self.assertIn('is not an ISO9660 disc image', out.stdout)


class ReleaseTests(unittest.TestCase):
    def test_source_export_drains_archive_padding_before_waiting(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            archive = base / 'source.tar'
            content = b'native source archive\n'
            with tarfile.open(archive, 'w') as tar:
                entry = tarfile.TarInfo('source.txt')
                entry.size = len(content)
                tar.addfile(entry, io.BytesIO(content))
            producer = base / 'producer.py'
            producer.write_text('import sys\nfrom pathlib import Path\n'
                                'sys.stdout.buffer.write(Path(sys.argv[1]).read_bytes())\n'
                                'sys.stdout.buffer.write(bytes(1048576))\n', encoding='utf-8')
            command = [sys.executable, str(producer), str(archive)]
            code = ('import sys; sys.path.insert(0, %r); import make_release; '
                    'make_release.extract_tar_stream(%r, %r, %r)' %
                    (str(ROOT / 'scripts'), command, str(base / 'out'), folder))
            subprocess.run([sys.executable, '-c', code], check=True, timeout=15)
            self.assertEqual((base / 'out/source.txt').read_bytes(), content)

    def test_release_zip_has_one_folder_and_the_files_a_release_needs(self):
        with tempfile.TemporaryDirectory() as folder:
            good = Path(folder) / 'good.zip'
            release_zip(good)
            self.assertEqual(make_release.verify_zip(good), [])
            other = Path(folder) / 'other.zip'
            release_zip(other, [('SoulCaliburIII_Recomp-0.1.0-bozo/README.txt', b'x')])
            self.assertTrue(any('top folders' in p for p in make_release.verify_zip(other)))

    def test_release_rejects_game_files(self):
        with tempfile.TemporaryDirectory() as folder:
            cases = [('SoulCaliburIII_Recomp/source/hidden.dat', b'\x7fELF' + b'\0' * 100, 'an ELF file'),
                     ('SoulCaliburIII_Recomp/game/SLUS_212.16', b'x', 'named like a file of the game'),
                     ('SoulCaliburIII_Recomp/source/packs/sc3_meshes.pack', b'x', 'named like a file of the game'),
                     ('SoulCaliburIII_Recomp/source/PS2Recomp/runtime.cpp', b'x', 'PS2Recomp')]
            for name, data, problem in cases:
                path = Path(folder) / 'bad.zip'
                release_zip(path, [(name, data)])
                self.assertTrue(any(problem in p for p in make_release.verify_zip(path)), name)

    def test_update_refuses_a_zip_of_another_layout(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'other.zip'
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr('TMB_Recomp_Bozo/SC3 Launcher.exe', b'x')
            archive = apply_update.Archive(str(path))
            with self.assertRaises(apply_update.UpdateError):
                apply_update.checked_names(archive, '0.1.0')
            archive.close()

    def linux_release(self, folder, extra=()):
        stage = Path(folder) / 'stage'
        for name in make_release.LINUX_REQUIRED:
            file = stage / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(b'\x7fELF launcher' if name == 'sc3-launcher' else b'version = 0.1.1\n')
        (stage / 'sc3-launcher').chmod(0o755)
        for name, data in extra:
            (stage / name).parent.mkdir(parents=True, exist_ok=True)
            (stage / name).write_bytes(data)
        path = Path(folder) / 'SoulCaliburIII_Recomp-0.1.1-linux.tar.gz'
        make_release.tar_dir(str(stage), str(path), 'SoulCaliburIII_Recomp', 1700000000)
        return path

    def test_linux_release_has_one_folder_the_launcher_and_no_other_elf(self):
        with tempfile.TemporaryDirectory() as folder:
            good = self.linux_release(folder)
            self.assertEqual(make_release.verify_tar(str(good)), [])
            with tarfile.open(good) as tf:
                if sys.platform != 'win32':
                    self.assertEqual(tf.getmember('SoulCaliburIII_Recomp/sc3-launcher').mode, 0o755)
                self.assertEqual(tf.getmember('SoulCaliburIII_Recomp/README.txt').mode, 0o644)
            bad = self.linux_release(folder, [('source/tool', b'\x7fELF other'), ('game/SLUS_212.16', b'x')])
            problems = make_release.verify_tar(str(bad))
            self.assertTrue(any('source/tool: an ELF file' in p for p in problems), problems)
            self.assertTrue(any('SLUS_212.16' in p for p in problems), problems)

    def test_linux_release_is_reproducible(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            self.assertEqual(self.linux_release(a).read_bytes(), self.linux_release(b).read_bytes())

    def test_update_installs_a_linux_release_with_its_launcher_executable(self):
        with tempfile.TemporaryDirectory() as folder:
            path = self.linux_release(folder)
            make_release.write_sha256(str(path))
            install = Path(folder) / 'install'
            (install / 'game').mkdir(parents=True)
            (install / 'game/keep.txt').write_bytes(b'player')
            out = subprocess.run([sys.executable, '-I', str(ROOT / 'scripts/apply_update.py'), '--zip', str(path),
                                  '--sha256', str(path) + '.sha256', '--version', '0.1.1', '--install', str(install),
                                  '--keep-zip'], stdout=subprocess.PIPE, universal_newlines=True)
            self.assertEqual(out.returncode, 0, out.stdout)
            self.assertIn('done 0.1.1', out.stdout)
            self.assertTrue(os.access(install / 'sc3-launcher', os.X_OK) or sys.platform == 'win32')
            self.assertEqual((install / 'game/keep.txt').read_bytes(), b'player')

    def test_update_refuses_a_link_in_a_linux_release(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'linked.tar.gz'
            with tarfile.open(path, 'w:gz') as tf:
                link = tarfile.TarInfo('SoulCaliburIII_Recomp/source/escape')
                link.type, link.linkname = tarfile.SYMTYPE, '/etc'
                tf.addfile(link)
            with self.assertRaises(apply_update.UpdateError):
                apply_update.Archive(str(path))


class LinuxBuildTests(unittest.TestCase):
    def test_missing_tools_are_named_with_the_distribution_command(self):
        with patch.object(sc3_build.shutil, 'which', lambda name, **_k: None if name == 'ninja' else '/usr/bin/' + name), \
                patch.object(sc3_build.subprocess, 'run', lambda cmd, **_k: SimpleNamespace(
                    returncode=1 if cmd[-1] in ('libswscale', 'x11', 'wayland-client') else 0)):
            missing = sc3_build.linux_missing()
        self.assertEqual(missing, ['ninja', 'FFmpeg (libswscale)', 'X11 or Wayland (x11 or wayland-client)'])
        with patch.object(sc3_build, 'linux_family', lambda: 'fedora'):
            self.assertIn('sudo dnf install', sc3_build.linux_missing_message(missing))
        with patch.object(sc3_build, 'linux_family', lambda: None):
            self.assertIn('sudo apt install', sc3_build.linux_missing_message(missing))

    def test_a_linux_kit_gives_the_libraries_sources_and_keeps_the_system_toolchain(self):
        with tempfile.TemporaryDirectory() as folder:
            kit = Path(folder) / 'kit'
            (kit / 'deps').mkdir(parents=True)
            (kit / 'kit_manifest.json').write_text('{"platform": "linux"}')
            args = SimpleNamespace(out=str(Path(folder) / 'out'), game=None, toolchain=None, fetch_cache=None,
                                   debug_info=None, kit=str(kit), no_kit=False)
            build = sc3_build.Build(args)
            self.assertEqual(build.kit, str(kit))
            self.assertIsNone(args.toolchain)
            self.assertIsNone(build.ffmpeg_root)
            self.assertEqual(args.fetch_cache, str(kit / 'deps'))


if __name__ == '__main__':
    unittest.main()
