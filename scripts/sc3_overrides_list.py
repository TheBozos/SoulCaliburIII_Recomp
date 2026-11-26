"""SC3's override list for BozoEngine's bozo-externals: every address bozo/port/*.cpp registers
(runtime.registerFunction(addr::Name, ...), or each element of an addr:: array a loop registers), whatever environment
toggle guards it, and every store the store patch tells apart (a "// bozo:store addr::Name" line next to it).

    python scripts/sc3_overrides_list.py <port folder> <out.txt> [--column ntsc|pal|kor]

The names are bozo/port/sc3_addresses.txt's, in the column of the disc the build is for.

The generated code must route every one of them to the runtime: BozoEngine checks at startup that each registered
override is an external of the generated code (bozo::port::undeclared) and refuses to run otherwise.
"""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sc3_region_map

LOOP = re.compile(r'for\s*\(\s*uint32_t\s+(\w+)\s*:\s*addr::(\w+)\s*\)')
REGISTER = re.compile(r'registerFunction\(\s*([^,]+?)\s*,')
STORE = re.compile(r'//\s*bozo:store\s+addr::(\w+)')
NATIVE = re.compile(r'//\s*bozo:native\s+addr::(\w+)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--column', choices=sc3_region_map.Addresses.COLUMNS, default='ntsc')
    args = ap.parse_args()
    folder, out = args.folder, args.out
    values = sc3_region_map.Addresses(folder / 'sc3_addresses.txt').values(args.column)
    overrides, stores, native, errors = {}, {}, {}, []

    def resolve(f, name, array=False):
        v = values.get(name)
        if v is None or isinstance(v, list) != array:
            errors.append(f'{f.name}: addr::{name} is no {"array" if array else "address"} of sc3_addresses.txt')
            return [] if array else None
        return v

    for f in sorted(folder.glob('*.cpp')):
        text = f.read_text(encoding='utf-8')
        loops = {var: arr for var, arr in LOOP.findall(text)}
        for expr in REGISTER.findall(text):
            expr = expr.strip()
            if expr in loops:
                for a in resolve(f, loops[expr], True):
                    overrides.setdefault(a, f'{f.name}: addr::{loops[expr]} (a loop)')
            elif expr.startswith('addr::'):
                a = resolve(f, expr[len('addr::'):])
                if a is not None:
                    overrides.setdefault(a, f'{f.name}: {expr}')
            else:
                errors.append(f'{f.name}: registerFunction({expr}): not an addr:: name')
        for name in STORE.findall(text):
            a = resolve(f, name)
            if a is not None:
                stores[a] = f'{f.name}: addr::{name}'
        for name in NATIVE.findall(text):
            for a in resolve(f, name, True):
                native[a] = f'{f.name}: addr::{name}'
    if errors:
        sys.exit('\n'.join(errors))
    lines = ['# SC3\'s overrides and patched stores (scripts/sc3_overrides_list.py from bozo/port/*.cpp). Do not edit.']
    lines += [f'override 0x{a:x}  # {where}' for a, where in sorted(overrides.items())]
    lines += [f'store 0x{a:x}  # {where}' for a, where in sorted(stores.items())]
    lines += [f'native 0x{a:x}  # {where}' for a, where in sorted(native.items())]
    text = '\n'.join(lines) + '\n'
    if not out.exists() or out.read_text(encoding='utf-8') != text:
        out.write_text(text, encoding='utf-8', newline='\n')
    print(f'sc3_overrides_list: {len(overrides)} overrides, {len(stores)} patched stores, {len(native)} native instructions -> {out}')


if __name__ == '__main__':
    main()
