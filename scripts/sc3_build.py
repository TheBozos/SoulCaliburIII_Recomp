#!/usr/bin/env python3
"""sc3_build: SC3 on BozoEngine (cmake/bozo.cmake), from the player's own disc files, on Linux and on Windows (MSYS2
UCRT64 GCC). Nothing derived from the game is distributed: the generated code and the executables are made here.

  python scripts/sc3_build.py --elf <SLUS_212.16> [--iso <disc image>] [--out build/bozo]
                              [--engine <BozoEngine>] [--symbol-map <file>] [--no-window] [--target <target>]...

Steps:
  verify     SLUS_212.16's SHA-1 (the NTSC-U release the port is made for)
  configure  cmake (Ninja) with SC3_BOZO=ON
  compile    bozo-recomp and bozo-externals, the generated code, then sc3, sc3-headless and sc3-check

--toolchain <dir>: a folder whose bin/ has g++, cmake and ninja (an MSYS2 prefix such as C:/msys64/ucrt64); else the
PATH, else C:/msys64/ucrt64 on Windows. Standard library only (Python 3.8+).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ELF_SHA1 = '9bbb67a70137052931bc245855eedeee6bbe5b49'


def sha1_file(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def toolchain_env(toolchain):
    env = dict(os.environ)
    candidates = [toolchain] if toolchain else []
    if os.name == 'nt' and not shutil.which('g++'):
        candidates.append('C:/msys64/ucrt64')
    for folder in candidates:
        bin_dir = Path(folder) / 'bin'
        if not (bin_dir / ('g++.exe' if os.name == 'nt' else 'g++')).exists():
            sys.exit(f'ERROR toolchain: no g++ in {bin_dir}')
        env['PATH'] = str(bin_dir) + os.pathsep + env.get('PATH', '')
        break
    for tool in ('cmake', 'ninja'):
        if not shutil.which(tool, path=env['PATH']):
            sys.exit(f'ERROR toolchain: {tool} not found (--toolchain)')
    return env


def run(cmd, env):
    print('+ ' + ' '.join(str(c) for c in cmd), flush=True)
    if subprocess.call([str(c) for c in cmd], env=env) != 0:
        sys.exit(f'ERROR {cmd[0]} failed')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--elf', required=True, help='SLUS_212.16 from your disc')
    ap.add_argument('--iso', help='your disc image (the default --iso of the programs)')
    ap.add_argument('--out', default=str(ROOT / 'build' / 'bozo'), help='the build folder (default build/bozo)')
    ap.add_argument('--engine', help="BozoEngine's source tree (default: the BozoEngine submodule)")
    ap.add_argument('--symbol-map', help="SC3's symbol map (name 0xADDR lines) for bozo-externals --symbols")
    ap.add_argument('--toolchain', help='compiler folder with bin/g++, cmake, ninja')
    ap.add_argument('--build-type', default='Release', choices=['Release', 'RelWithDebInfo', 'Debug'])
    ap.add_argument('--no-window', action='store_true', help='no sc3 (no SDL3): sc3-headless and sc3-check only')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--target', action='append', help='build only these targets (sc3_recompile: the generated code)')
    args = ap.parse_args()

    elf = Path(args.elf).resolve()
    if not elf.is_file():
        sys.exit(f'ERROR verify: no file {elf}')
    digest = sha1_file(elf)
    if digest != ELF_SHA1:
        print(f'warning: {elf.name} SHA-1 is {digest}, the port is made for {ELF_SHA1}', flush=True)
    engine = Path(args.engine).resolve() if args.engine else ROOT / 'BozoEngine'
    if not (engine / 'CMakeLists.txt').exists():
        sys.exit(f'ERROR configure: no BozoEngine at {engine} (git submodule update --init BozoEngine, or --engine)')

    env = toolchain_env(args.toolchain)
    out = Path(args.out).resolve()
    configure = ['cmake', '-S', ROOT, '-B', out, '-G', 'Ninja', '-DSC3_BOZO=ON', f'-DCMAKE_BUILD_TYPE={args.build_type}',
                 f'-DBOZO_ENGINE_DIR={engine.as_posix()}', f'-DSC3_ELF={elf.as_posix()}',
                 f'-DSC3_WINDOW={"OFF" if args.no_window else "ON"}']
    if args.iso:
        configure.append(f'-DSC3_ISO={Path(args.iso).resolve().as_posix()}')
    configure.append(f'-DSC3_SYMBOL_MAP={Path(args.symbol_map).resolve().as_posix() if args.symbol_map else ""}')
    run(configure, env)
    build = ['cmake', '--build', out, '-j', str(args.jobs)]
    for target in args.target or []:
        build += ['--target', target]
    run(build, env)
    print(f'RESULT {out}', flush=True)


if __name__ == '__main__':
    main()
