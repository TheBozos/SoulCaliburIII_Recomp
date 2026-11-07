"""Rewrites a bozo-externals list so a tail call (a direct jump) reaches SC3's overrides as a call does: "stub" is the
kind whose every transfer goes to the runtime; the guest code stays generated for the overrides that call it.

    python scripts/sc3_externals_all_transfers.py <in.txt> <out.txt>
"""
import re
import sys
from pathlib import Path


def main():
    text = Path(sys.argv[1]).read_text(encoding='utf-8')
    Path(sys.argv[2]).write_text(re.sub(r'^override ', 'stub ', text, flags=re.M), encoding='utf-8')


if __name__ == '__main__':
    main()
