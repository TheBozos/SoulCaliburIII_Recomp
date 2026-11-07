"""The release tools (scripts/sc3_build.py, make_release.py, apply_update.py) without game data."""
import io
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
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
            self.assertIn('-DSC3_BOZO=ON', commands[0])
            self.assertIn('-DSC3_GUI_SUBSYSTEM=ON', commands[0])
            self.assertIn('-DBOZO_FFMPEG_ROOT=' + folder.replace('\\', '/'), commands[0])

    def test_extract_takes_only_what_the_build_reads(self):
        wanted = [n for n in ('SLUS_212.16', 'ROOT.OLK', 'IRX/CRI_ADXI.IRX', 'IRX/NUSOUND.IRX', 'IRX/SDRDRV.IRX',
                              'IRX\\SDRDRV.IRX', 'MOVIE.AFS', 'BGM.AFS', 'IRX/LIBSD.IRX', 'SYSTEM.CNF')
                  if sc3_build.Build.wanted(n)]
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
            with zipfile.ZipFile(path) as archive, self.assertRaises(apply_update.UpdateError):
                apply_update.checked_names(archive, '0.1.0')


if __name__ == '__main__':
    unittest.main()
