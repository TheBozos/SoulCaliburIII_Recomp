"""sc3_region_map: where an address of the NTSC executable (SLUS_212.16) lives in another release's (SCES_533.12,
SCKA_200.59).

    python scripts/sc3_region_map.py build <SLUS_212.16> <SCES_533.12 or SCKA_200.59> <out.map>
    python scripts/sc3_region_map.py translate <map> <address>...
    python scripts/sc3_region_map.py symbols <map> <ntsc "name 0xADDR" list> <out>
    python scripts/sc3_region_map.py patches <map> <ntsc patches> <SLUS_212.16> <its other ELF> <reviewed> <out>
    python scripts/sc3_region_map.py addresses <bozo/port/sc3_addresses.txt> <map> [--column pal|kor] [--write]
    python scripts/sc3_region_map.py header <sc3_region.h> --title <title> --elf <boot ELF> --table <sc3_addresses.txt>
                                     [--column ntsc|pal|kor] [--symbols <symbol map>] [--sources <folder>...]

build lines the two executables up and writes the ranges that moved as a block:
  code     function by function (a jal target to the next): bodies equal once every relocated field is masked (jump
           targets, the immediates of lui, addiu, ori, the loads and the stores) and unique on both sides match first,
           then the callees the matched functions name, then the functions left between two matched neighbours, in order.
           A function whose body differs is aligned inside by a diff of the two.
  data     the initialised sections, word by word, a diff with every word that points into the image masked
  bss      .sbss and .bss have no bytes to line up: their ranges come from the addresses the matched code and data name
           (lui pairs, $gp offsets, pointers), grouped while the offset stays the same
  changed  the functions whose code differs, and the data words that point elsewhere: an address the port names there has
           to be read again for that release, its place in the map is the diff's guess

A map line is `<kind> <ntsc begin> <ntsc end> <offset>`, the end excluded. An address in no range has no translation.
symbols and patches carry SC3's recomp inputs over with the map; patches takes the reviewed lines (`<ntsc address>
<address> <expected> <replacement>`) in place of what it cannot carry itself. addresses fills or checks a release's column
of the port's address table from its map; a row inside a function that changed is read by hand and marked
reviewed=<column>.
header writes the build's sc3_region.h from that table's column.
"""
import argparse
import bisect
import difflib
import re
import struct
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

IMAGE_LOW, IMAGE_HIGH = 0x00100000, 0x02000000
MASK_IMMEDIATE = {0x09, 0x0D, 0x0F, 0x1E, 0x1F} | set(range(0x20, 0x40))
STORES = {0x1F, 0x39, 0x3D, 0x3F} | set(range(0x28, 0x30))


class Elf:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        d = self.data
        shoff, = struct.unpack_from('<I', d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2E)
        raw = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]
        strtab = raw[shstrndx][4]
        self.sections = {}
        for s in raw:
            name = d[strtab + s[0]:d.index(b'\0', strtab + s[0])].decode()
            self.sections[name] = (s[1], s[3], s[4], s[5])
        self.gp, = struct.unpack_from('<I', d, self.sections['.reginfo'][2] + 20)
        _, addr, off, size = self.sections['.text']
        self.text = (addr, list(struct.unpack_from('<%dI' % (size // 4), d, off)))
        loaded = sorted((a, o, z) for t, a, o, z in self.sections.values() if t == 1 and a and a != self.text[0])
        self.initialised = [(a, list(struct.unpack_from('<%dI' % (z // 4), d, o))) for a, o, z in loaded]
        self.zeroed = sorted((a, a + z) for t, a, o, z in self.sections.values() if t == 8 and z)

    def bss(self, address):
        return any(lo <= address < hi for lo, hi in self.zeroed)


def code_key(w):
    op = w >> 26
    if op in (2, 3):
        return w & 0xFC000000
    if op in MASK_IMMEDIATE:
        return w & 0xFFFF0000
    return w


def data_key(w):
    return 'p' if IMAGE_LOW <= w < IMAGE_HIGH else '%08x' % w


def aligned_runs(a, b):
    """What a diff of the two key lists keeps in place: (index in a, index in b, length, same), where same is False for
    a hunk that replaces words with as many others (the layout holds, the content differs)."""
    with tempfile.TemporaryDirectory() as tmp:
        fa, fb = Path(tmp, 'a'), Path(tmp, 'b')
        fa.write_text(''.join('%s\n' % k for k in a))
        fb.write_text(''.join('%s\n' % k for k in b))
        done = subprocess.run(['diff', '--minimal', str(fa), str(fb)], capture_output=True, text=True)
        if done.returncode not in (0, 1):
            raise SystemExit('diff failed: %s' % done.stderr.strip())
        out = done.stdout
    runs, ia, ib = [], 0, 0
    for line in out.splitlines():
        if not line or not line[0].isdigit():
            continue
        (a1, a2), kind, (b1, b2) = _hunk(line)
        if kind == 'a':
            a1, a2 = a1 + 1, a1
        if kind == 'd':
            b1, b2 = b1 + 1, b1
        if a1 - 1 > ia:
            runs.append((ia, ib, a1 - 1 - ia, True))
        if kind == 'c' and a2 - a1 == b2 - b1:
            runs.append((a1 - 1, b1 - 1, a2 - a1 + 1, False))
        ia, ib = a2, b2
    if ia < len(a):
        runs.append((ia, ib, len(a) - ia, True))
    return runs


def _hunk(line):
    for kind in 'acd':
        if kind in line:
            left, right = line.split(kind)
            return _span(left), kind, _span(right)
    raise ValueError(line)


def _span(text):
    first, _, last = text.partition(',')
    return int(first), int(last or first)


def addresses_named(start, words_a, words_b, gp_a, gp_b):
    """The addresses two aligned instruction runs compute, lui with its low half and $gp offsets: (a, b, where)."""
    high_a, high_b, pairs = {}, {}, []
    for i, (wa, wb) in enumerate(zip(words_a, words_b)):
        op, rs, rt = wa >> 26, (wa >> 21) & 31, (wa >> 16) & 31
        if op == 0x0F:
            high_a[rt], high_b[rt] = (wa & 0xFFFF) << 16, (wb & 0xFFFF) << 16
            continue
        if op in MASK_IMMEDIATE:
            sa, sb = _signed(wa & 0xFFFF), _signed(wb & 0xFFFF)
            if rs == 28:
                pairs.append(((gp_a + sa) & 0xFFFFFFFF, (gp_b + sb) & 0xFFFFFFFF, start + 4 * i))
            elif rs in high_a and op != 0x0D:
                pairs.append(((high_a[rs] + sa) & 0xFFFFFFFF, (high_b[rs] + sb) & 0xFFFFFFFF, start + 4 * i))
            elif rs in high_a:
                pairs.append((high_a[rs] | (wa & 0xFFFF), high_b[rs] | (wb & 0xFFFF), start + 4 * i))
            if op not in STORES:
                high_a.pop(rt, None)
                high_b.pop(rt, None)
        elif op == 0:
            high_a.pop((wa >> 11) & 31, None)
            high_b.pop((wa >> 11) & 31, None)
        if op in (2, 3) or (op == 0 and (wa & 0x3F) in (8, 9)):
            high_a.clear()
            high_b.clear()
    return pairs


def _signed(v):
    return v - 0x10000 if v & 0x8000 else v


def vote(pairs, kind, shift):
    """Address pairs with no range yet: each address takes the offset most of its pairs give, and a run of addresses
    with the same offset is a range, from the first to the word past the last. An offset that strays from the section's
    own shift by 32 KiB or more, or a lone address whose offset no neighbour shares, is left out (a lui that pairs with
    another low half than it seems to)."""
    votes = defaultdict(lambda: defaultdict(int))
    for x, y in pairs:
        if abs(y - x - shift) < 0x8000:
            votes[x][y - x] += 1
    best = sorted((x, max(c, key=c.get)) for x, c in votes.items())
    runs = []
    for x, offset in best:
        if runs and runs[-1][3] == offset:
            runs[-1][1] = x
            runs[-1][2] += 1
        else:
            runs.append([x, x, 1, offset])
    kept = [r for i, r in enumerate(runs)
            if r[2] > 1 or (i and runs[i - 1][3] == r[3]) or (i + 1 < len(runs) and runs[i + 1][3] == r[3])]
    return [(kind, lo, (hi & ~3) + 4, offset) for lo, hi, _, offset in kept]


def functions(elf):
    """The functions of .text as (start, end): from a jal target to the next."""
    base, words = elf.text
    end = base + 4 * len(words)
    starts = sorted({(w & 0x3FFFFFF) << 2 for w in words if w >> 26 == 3} | {base})
    starts = [x for x in starts if base <= x < end] + [end]
    return list(zip(starts, starts[1:]))


def match_code(a, b):
    """Every NTSC function's place in the other release: (start, end, its start there, aligned runs inside it) and the
    functions whose code differs. Identical bodies (relocations masked) that are unique on both sides match first, then
    the calls the matched functions make name their callees, then a function between two matched neighbours takes the
    places they leave, in order, when as many functions sit between them on both sides."""
    base_a, words_a = a.text
    base_b, words_b = b.text
    fa, fb = functions(a), functions(b)
    end_a = dict(fa)
    ka = {lo: tuple(code_key(w) for w in words_a[(lo - base_a) // 4:(hi - base_a) // 4]) for lo, hi in fa}
    kb = {lo: tuple(code_key(w) for w in words_b[(lo - base_b) // 4:(hi - base_b) // 4]) for lo, hi in fb}
    by_a, by_b = defaultdict(list), defaultdict(list)
    for lo, k in ka.items():
        by_a[k].append(lo)
    for lo, k in kb.items():
        by_b[k].append(lo)
    match = {los[0]: by_b[k][0] for k, los in by_a.items() if len(los) == 1 and len(by_b.get(k, ())) == 1}
    order_b = [lo for lo, _ in fb]
    index_b = {lo: i for i, lo in enumerate(order_b)}

    def take(x, y):
        if x not in match and y in kb and y not in used:
            match[x] = y
            used.add(y)
            return 1
        return 0

    used = set(match.values())
    while True:
        votes = defaultdict(lambda: defaultdict(int))
        for x, y in match.items():
            if ka[x] != kb[y]:
                continue
            for wa, wb in zip(words_a[(x - base_a) // 4:(end_a[x] - base_a) // 4], words_b[(y - base_b) // 4:]):
                if wa >> 26 == 3:
                    votes[(wa & 0x3FFFFFF) << 2][(wb & 0x3FFFFFF) << 2] += 1
        new = 0
        for t, c in votes.items():
            best = max(c, key=c.get)
            if t in ka and ka[t] == kb.get(best):
                new += take(t, best)
        for i, (lo, _) in enumerate(fa):
            if lo in match:
                continue
            prev, nxt = fa[i - 1][0] if i else None, fa[i + 1][0] if i + 1 < len(fa) else None
            for y in (index_b.get(match.get(prev), -2) + 1, index_b.get(match.get(nxt), -1) - 1):
                if 0 <= y < len(order_b) and ka[lo] == kb[order_b[y]]:
                    new += take(lo, order_b[y])
                    break
        if not new:
            break
    i = 0
    while i < len(fa):
        if fa[i][0] in match:
            i += 1
            continue
        j = i
        while j < len(fa) and fa[j][0] not in match:
            j += 1
        if i and j < len(fa):
            after, before = index_b[match[fa[i - 1][0]]] + 1, index_b[match[fa[j][0]]]
            if before - after == j - i:
                for k in range(j - i):
                    take(fa[i + k][0], order_b[after + k])
        i = j

    pairs, changed = [], []
    for lo, hi in fa:
        y = match.get(lo)
        if y is None:
            changed.append((lo, hi))
            continue
        if ka[lo] == kb[y]:
            pairs.append((lo, hi, y, [(0, 0, len(ka[lo]), True)]))
            continue
        changed.append((lo, hi))
        runs = []
        for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, ka[lo], kb[y], autojunk=False).get_opcodes():
            if tag == 'equal' or (tag == 'replace' and i2 - i1 == j2 - j1):
                runs.append((i1, j1, i2 - i1, tag == 'equal'))
        pairs.append((lo, hi, y, runs))
    return pairs, changed


def merge(ranges):
    out = []
    for r in sorted(ranges, key=lambda r: r[1]):
        if out and out[-1][0] == r[0] and out[-1][2] == r[1] and out[-1][3] == r[3]:
            out[-1] = (r[0], out[-1][1], r[2], r[3])
        else:
            out.append(r)
    return out


def build(ntsc_path, pal_path, out):
    a, b = Elf(ntsc_path), Elf(pal_path)
    ranges, anchors = [], []

    base_a, text_a = a.text
    base_b, text_b = b.text
    pairs, changed = match_code(a, b)
    for lo, hi, y, runs in pairs:
        for ia, ib, n, identical in runs:
            ranges.append(('code', lo + ia * 4, lo + (ia + n) * 4, y - lo + (ib - ia) * 4))
            if not identical:
                continue
            xa, xb = (lo - base_a) // 4 + ia, (y - base_b) // 4 + ib
            anchors += addresses_named(lo + ia * 4, text_a[xa:xa + n], text_b[xb:xb + n], a.gp, b.gp)
            for i, (wa, wb) in enumerate(zip(text_a[xa:xa + n], text_b[xb:xb + n])):
                if wa >> 26 in (2, 3):
                    anchors.append(((wa & 0x3FFFFFF) << 2, (wb & 0x3FFFFFF) << 2, lo + (ia + i) * 4))

    flat_a = [(addr + 4 * i, w) for addr, words in a.initialised for i, w in enumerate(words)]
    flat_b = [(addr + 4 * i, w) for addr, words in b.initialised for i, w in enumerate(words)]
    for ia, ib, n, identical in aligned_runs([data_key(w) for _, w in flat_a], [data_key(w) for _, w in flat_b]):
        start = 0
        for k in range(1, n + 1):
            if k == n or flat_a[ia + k][0] != flat_a[ia + k - 1][0] + 4 or flat_b[ib + k][0] != flat_b[ib + k - 1][0] + 4:
                ranges.append(('data', flat_a[ia + start][0], flat_a[ia + k - 1][0] + 4, flat_b[ib + start][0] - flat_a[ia + start][0]))
                start = k
        for k in range(n if identical else 0):
            wa, wb = flat_a[ia + k][1], flat_b[ib + k][1]
            if IMAGE_LOW <= wa < IMAGE_HIGH and wa != wb:
                anchors.append((wa, wb, flat_a[ia + k][0]))

    ranges = merge(ranges)
    starts = [r[1] for r in ranges]

    def covering(address):
        i = bisect.bisect_right(starts, address) - 1
        return ranges[i] if i >= 0 and ranges[i][1] <= address < ranges[i][2] else None

    fa = functions(a)
    function_starts = [lo for lo, _ in fa]
    differs, loose = set(), []
    for x, y, where in anchors:
        r = covering(x)
        if r is None:
            if a.bss(x) and b.bss(y) and x != y:
                loose.append((x, y, where))
            continue
        if x == y or y - x == r[3] or (r[0] == 'code' and x & 3):
            continue
        differs.add(where)
    bss = vote([(x, y) for x, y, where in loose if where not in differs], 'bss', b.zeroed[0][0] - a.zeroed[0][0])
    bss_starts = [r[1] for r in bss]
    for x, y, where in loose:
        i = bisect.bisect_right(bss_starts, x) - 1
        if i >= 0 and bss[i][1] <= x < bss[i][2] and y - x != bss[i][3]:
            differs.add(where)
    ranges = merge(ranges + bss)

    changed = set(changed)
    words = set()
    for where in differs:
        if base_a <= where < base_a + 4 * len(text_a):
            i = bisect.bisect_right(function_starts, where) - 1
            changed.add(fa[i])
        else:
            words.add((where, where + 4))
    changed = sorted(changed | words)

    with open(out, 'w') as f:
        f.write('# %s -> %s: sc3_region_map.py build. <kind> <ntsc begin> <ntsc end> <offset>, the end excluded.\n'
                % (Path(ntsc_path).name, Path(pal_path).name))
        for kind, lo, hi, offset in ranges:
            f.write('%s %#08x %#08x %s%#x\n' % (kind, lo, hi, '-' if offset < 0 else '+', abs(offset)))
        for lo, hi in changed:
            f.write('changed %#08x %#08x\n' % (lo, hi))
    print('%d code runs, %d data runs, %d bss runs; %d of %d functions and %d data words differ beyond their place'
          % (sum(r[0] == 'code' for r in ranges), sum(r[0] == 'data' for r in ranges), len(bss),
             len(changed) - len(words), len(fa), len(words)))


class Map:
    def __init__(self, path):
        self.ranges, self.changed = [], []
        for line in Path(path).read_text().splitlines():
            parts = line.split('#')[0].split()
            if not parts:
                continue
            if parts[0] == 'changed':
                self.changed.append((int(parts[1], 16), int(parts[2], 16)))
            else:
                self.ranges.append((parts[0], int(parts[1], 16), int(parts[2], 16), int(parts[3], 16)))
        self.ranges.sort(key=lambda r: r[1])
        self.starts = [r[1] for r in self.ranges]

    def range(self, address):
        i = bisect.bisect_right(self.starts, address) - 1
        if i >= 0 and self.ranges[i][1] <= address < self.ranges[i][2]:
            return self.ranges[i]
        return None

    def translate(self, address):
        r = self.range(address)
        return None if r is None else address + r[3]

    def in_changed(self, address):
        return next(((lo, hi) for lo, hi in self.changed if lo <= address < hi), None)


class Addresses:
    """bozo/port/sc3_addresses.txt, the game's addresses the port names: a row a name (a `name[]` row per element of an
    array), its address in each release (COLUMNS, `-` for none yet), `reviewed=<column>,...` for the ones read by hand,
    then `# note`."""

    COLUMNS = ('ntsc', 'pal', 'kor')

    def __init__(self, path):
        self.path = Path(path)
        self.lines = self.path.read_text(encoding='utf-8').splitlines()
        self.rows = []
        for n, line in enumerate(self.lines):
            code, _, note = line.partition('#')
            parts = code.split()
            if not parts:
                continue
            where = '%s:%d' % (self.path.name, n + 1)
            cells = [v for v in parts[1:] if not v.startswith('reviewed=')]
            flags = [v[len('reviewed='):] for v in parts[1:] if v.startswith('reviewed=')]
            reviewed = set(','.join(flags).split(',')) - {''}
            if len(cells) > len(self.COLUMNS):
                raise SystemExit('%s: %s has %d addresses, the table has %d columns (%s)'
                                 % (where, parts[0], len(cells), len(self.COLUMNS), ', '.join(self.COLUMNS)))
            if reviewed - set(self.COLUMNS[1:]):
                raise SystemExit('%s: reviewed=%s names no column of %s' % (where, ','.join(sorted(reviewed)),
                                                                             ', '.join(self.COLUMNS[1:])))
            cells += ['-'] * (len(self.COLUMNS) - len(cells))
            row = {'line': n, 'name': parts[0], 'reviewed': reviewed, 'note': note.strip()}
            for c, v in zip(self.COLUMNS, cells):
                try:
                    row[c] = int(v, 16) if v != '-' else None
                except ValueError:
                    raise SystemExit('%s: %s: %r is not an address, `-` or reviewed=<column>,...' % (where, parts[0], v))
            self.rows.append(row)

    def values(self, column):
        """name -> address, or the list of an array's, in that column."""
        out = {}
        for r in self.rows:
            if r['name'].endswith('[]'):
                out.setdefault(r['name'][:-2], []).append(r[column])
            else:
                out[r['name']] = r[column]
        return out

    def write(self):
        width = max(len(r['name']) for r in self.rows)
        for r in self.rows:
            cells = ['0x%08X' % r[c] if r[c] is not None else '-' for c in self.COLUMNS]
            while len(cells) > 1 and cells[-1] == '-':
                cells.pop()
            reviewed = [c for c in self.COLUMNS if c in r['reviewed']]
            text = '%-*s %s' % (width, r['name'], ' '.join('%-10s' % c for c in cells).rstrip())
            if reviewed:
                text += ' reviewed=' + ','.join(reviewed)
            self.lines[r['line']] = text + ('  # ' + r['note'] if r['note'] else '')
        self.path.write_text('\n'.join(self.lines) + '\n', encoding='utf-8')


def addresses(argv):
    """A release's column of the table from its map: checked (the default) or written (--write). A row inside a function
    that changed has to be reviewed: its address is read by hand and kept."""
    ap = argparse.ArgumentParser(prog='sc3_region_map.py addresses')
    ap.add_argument('table')
    ap.add_argument('map')
    ap.add_argument('--column', choices=Addresses.COLUMNS[1:], default='pal')
    ap.add_argument('--write', action='store_true')
    args = ap.parse_args(argv)
    table, m, column = Addresses(args.table), Map(args.map), args.column
    errors, stale = [], []
    for r in table.rows:
        if column in r['reviewed']:
            if r[column] is None:
                errors.append('%s: reviewed with no address' % r['name'])
            continue
        changed = m.in_changed(r['ntsc'])
        here = m.translate(r['ntsc'])
        if changed:
            errors.append('%s %#x: inside %#x-%#x, which changed: read it and mark the row reviewed=%s'
                          % ((r['name'], r['ntsc']) + changed + (column,)))
        elif here is None:
            errors.append('%s %#x: no place in the map' % (r['name'], r['ntsc']))
        elif here != r[column]:
            stale.append('%s %#x: %s in the table, %#x by the map'
                         % (r['name'], r['ntsc'], '-' if r[column] is None else '%#x' % r[column], here))
            r[column] = here
    for line in errors + ([] if args.write else stale):
        print(line)
    if args.write and not errors:
        table.write()
    return 1 if errors or (stale and not args.write) else 0


FN = re.compile(r'\bfn::(\w+)')


def serial(elf):
    return elf.replace('_', '-').replace('.', '')


def header(argv):
    """sc3_region.h: the build's release, the table's column for it as sc3::addr, and as sc3::fn the generated function
    at each address the port calls (fn::Name in its sources)."""
    ap = argparse.ArgumentParser(prog='sc3_region_map.py header')
    ap.add_argument('out')
    ap.add_argument('--title', required=True)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--table', required=True)
    ap.add_argument('--column', choices=Addresses.COLUMNS, default='ntsc')
    ap.add_argument('--symbols', help='the symbol map bozo-recomp names the functions with')
    ap.add_argument('--sources', nargs='*', default=[])
    args = ap.parse_args(argv)
    values = Addresses(args.table).values(args.column)
    names = {}
    if args.symbols:
        for line in Path(args.symbols).read_text(encoding='utf-8').splitlines():
            parts = line.split()
            if len(parts) >= 2 and not line.startswith('#'):
                names[int(parts[1], 16)] = parts[0]
    called, errors = set(), []
    for folder in args.sources:
        for f in sorted(Path(folder).rglob('*')):
            if f.suffix in ('.cpp', '.h'):
                called |= set(FN.findall(f.read_text(encoding='utf-8')))
    missing = sorted(n for n, v in values.items() if v is None or (isinstance(v, list) and None in v))
    for name in sorted(called):
        if name not in values or isinstance(values[name], list):
            errors.append('fn::%s: no such address in %s' % (name, args.table))
    if missing:
        errors.append('%s has no %s address for %s' % (args.table, args.column, ', '.join(missing)))
    if errors:
        print('\n'.join(errors))
        return 1
    rows = []
    for name, v in values.items():
        if isinstance(v, list):
            rows.append('    inline constexpr uint32_t %s[] = {%s};\n' % (name, ', '.join('0x%08Xu' % a for a in v)))
        else:
            rows.append('    inline constexpr uint32_t %s = 0x%08Xu;\n' % (name, v))
    functions = ''.join('    inline constexpr GuestFunction %s = &::%s_0x%x;\n' % (name, names.get(values[name], 'func'), values[name])
                        for name in sorted(called))
    text = ('// %s for %s: scripts/sc3_region_map.py header from %s. Do not edit.\n'
            '#pragma once\n\n#include "sc3_functions.h"\n\n#include <cstdint>\n\n'
            'namespace sc3::region\n{\n'
            '    inline constexpr const char *kTitle = "%s";\n'
            '    inline constexpr const char *kBootElf = "%s";\n'
            '    inline constexpr const char *kSerial = "%s";\n}\n\n'
            'namespace sc3::addr\n{\n%s}\n\n'
            'namespace sc3::fn\n{\n'
            '    using GuestFunction = void (*)(uint8_t *, R5900Context *, Runtime *);\n%s}\n'
            % (Path(args.out).name, args.elf, Path(args.table).name, args.title, args.elf, serial(args.elf), ''.join(rows),
               functions))
    out = Path(args.out)
    if not out.exists() or out.read_text(encoding='utf-8') != text:
        out.write_text(text, encoding='utf-8', newline='\n')
    return 0


def translate(path, addresses):
    m = Map(path)
    for text in addresses:
        a = int(text, 16)
        r, c = m.range(a), m.in_changed(a)
        note = ' (changed function %#x-%#x)' % c if c else ''
        print('%#08x -> %s%s' % (a, '%#08x %s' % (a + r[3], r[0]) if r else 'none', note))


def word_at(elf, address):
    base, words = elf.text
    i = (address - base) // 4
    return words[i] if 0 <= i < len(words) else None


def symbols(path, ntsc_list, out):
    """A "name 0xADDR" list (a symbol map, a reviewed list) with its addresses translated, its comments kept."""
    m, lines, missing = Map(path), [], []
    for line in Path(ntsc_list).read_text().splitlines():
        parts = line.split()
        if len(parts) < 2 or line.startswith('#'):
            lines.append(line)
            continue
        y = m.translate(int(parts[1], 16))
        if y is None:
            missing.append(parts[0])
            continue
        lines.append(line.replace(parts[1], '%#x' % y, 1))
    Path(out).write_text('\n'.join(lines) + '\n')
    for name in missing:
        print('%s: no place for %s' % (ntsc_list, name))
    return 1 if missing else 0


def patches(path, ntsc_patches, ntsc_path, pal_path, reviewed, out):
    """An NTSC patch list (address, expected word, replacement) for the other release: each address translated, the
    expected word read from that release (it may name another data address), a replacement jump's target translated.
    Inside a function that changed, the translated place is a guess: when its word is not the patched instruction, the
    same instruction is searched for in the 32 words around it, and a line with no single answer is left for review."""
    m, a, b = Map(path), Elf(ntsc_path), Elf(pal_path)
    by_hand = defaultdict(list)
    for line in Path(reviewed).read_text().splitlines():
        parts = line.split('#')[0].split()
        if len(parts) >= 4:
            y, expected = int(parts[1], 16), int(parts[2], 16)
            if word_at(b, y) != expected:
                raise SystemExit('%s: %#x holds %08x, not %08x' % (reviewed, y, word_at(b, y), expected))
            by_hand[int(parts[0], 16)].append('%#x %#010x %#010x' % (y, expected, int(parts[3], 16)))
    lines = ['# %s carried to %s by sc3_region_map.py patches, with %s: change those and run it again.'
             % (Path(ntsc_patches).name, Path(pal_path).name, Path(reviewed).name)]
    review = 0
    for line in Path(ntsc_patches).read_text().splitlines():
        parts = line.split('#')[0].split()
        if len(parts) < 3:
            lines.append(line)
            continue
        x, expected, replacement = (int(p, 16) for p in parts[:3])
        if word_at(a, x) != expected:
            raise SystemExit('%s: %#x holds %08x, not %08x' % (ntsc_patches, x, word_at(a, x), expected))
        if x in by_hand:
            lines += by_hand.pop(x)
            continue
        y = m.translate(x)
        if replacement >> 26 in (2, 3):
            target = m.translate((replacement & 0x3FFFFFF) << 2)
            if target is None:
                lines.append('# REVIEW %s: the jump target has no place' % line.strip())
                review += 1
                continue
            replacement = (replacement & 0xFC000000) | (target >> 2)
        found = word_at(b, y) if y is not None else None
        if found is None or code_key(found) != code_key(expected):
            near = [y + 4 * k for k in range(-32, 33) if word_at(b, y + 4 * k) == expected] if y is not None else []
            if len(near) != 1:
                lines.append('# REVIEW %s: %s' % (line.strip(), 'no place' if y is None else '%#x holds %08x' % (y, found)))
                review += 1
                continue
            y, found = near[0], expected
        lines.append('%#x %#010x %#010x' % (y, found, replacement))
    for x in by_hand:
        print('%s: %#x is no patch of %s' % (reviewed, x, ntsc_patches))
        review += 1
    Path(out).write_text('\n'.join(lines) + '\n')
    print('%s: %d lines to review' % (out, review))
    return 1 if review else 0


def main(argv):
    if len(argv) >= 4 and argv[0] == 'build':
        build(*argv[1:4])
    elif len(argv) >= 2 and argv[0] == 'translate':
        translate(argv[1], argv[2:])
    elif len(argv) >= 1 and argv[0] == 'header':
        return header(argv[1:])
    elif len(argv) >= 1 and argv[0] == 'addresses':
        return addresses(argv[1:])
    elif len(argv) >= 4 and argv[0] == 'symbols':
        return symbols(*argv[1:4])
    elif len(argv) >= 7 and argv[0] == 'patches':
        return patches(*argv[1:7])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
