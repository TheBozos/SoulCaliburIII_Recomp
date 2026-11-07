#!/usr/bin/env python3
"""The reference runner's named calls (SC3_CALL_NAMES, reference/README.md): each address the PS2Recomp build (main
da8a761) answers as a stub, named from recomp/sc3_symbols.txt, sub_<address> when the map has no name. Only the
addresses are read from main's sc3.toml, never its names.

    python scripts/sc3_call_names.py <main checkout>/recomp/sc3.toml recomp/sc3_symbols.txt <out.txt>
"""
import re
import sys


def main():
    toml, symbols, out = sys.argv[1:4]
    text = open(toml, encoding='utf-8').read()
    start = text.index('stubs = [')
    addresses = {int(a, 16) for a in re.findall(r'@0x([0-9a-fA-F]+)', text[start:text.index(']', start)])}
    names = {}
    for line in open(symbols, encoding='utf-8'):
        fields = line.split('#')[0].split()
        if len(fields) == 2:
            names.setdefault(int(fields[1], 16), fields[0])
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        for a in sorted(addresses):
            f.write(f'{names.get(a, f"sub_{a:x}")} 0x{a:x}\n')
    print(f'sc3_call_names: {len(addresses)} addresses, {len(addresses & names.keys())} named -> {out}')


if __name__ == '__main__':
    main()
