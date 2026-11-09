#!/usr/bin/env python3
"""Compile, statically link and run a C++ probe with only the bundled Windows toolchain.

The release workflow extracts the kit under a path with spaces and accents. No game data is needed.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_release import clean_env
from sc3_build import configure_gcc_paths, is_ascii, make_alias, remove_alias


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolchain', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    alias = None
    prefix = args.toolchain.resolve()
    try:
        if os.name == 'nt' and not is_ascii(str(prefix)):
            alias = make_alias(str(prefix), 'compiler-check')
            prefix = Path(alias)
        env = clean_env([str(prefix / 'bin')])
        configure_gcc_paths(env, str(prefix / 'bin'))
        source = args.out / 'native-toolchain.cpp'
        exe = args.out / 'native-toolchain.exe'
        source.write_text('#include <iostream>\n#include <vector>\n'
                          'int main(){std::vector<int> v{1,2,3}; std::cout << "native toolchain " << v.size() << "\\n";}\n')
        result = subprocess.run([str(prefix / 'bin/g++.exe'), '-std=c++20', '-static', str(source), '-o', str(exe)],
                                env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        (args.out / 'compiler.log').write_text(result.stdout, encoding='utf-8')
        if result.returncode:
            print(result.stdout)
            return result.returncode
        output = subprocess.check_output([str(exe)], env=env, text=True).strip()
        print(output)
        return int(output != 'native toolchain 3')
    finally:
        if alias:
            remove_alias(alias)


if __name__ == '__main__':
    raise SystemExit(main())
