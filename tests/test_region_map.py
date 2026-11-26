"""scripts/sc3_region_map.py on two small executables: one function changed, the next one moved, data and bss moved."""
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import sc3_region_map as region

NOP, JR_RA = 0, 0x03E00008
V0, V1 = 2, 3


def jal(target):
    return 0x0C000000 | target >> 2


def lui(rt, high):
    return 0x3C000000 | rt << 16 | high


def lw(rt, offset, rs):
    return 0x8C000000 | rs << 21 | rt << 16 | offset & 0xFFFF


def sw(rt, offset, rs):
    return 0xAC000000 | rs << 21 | rt << 16 | offset & 0xFFFF


def addiu(rt, rs, value):
    return 0x24000000 | rs << 21 | rt << 16 | value & 0xFFFF


def write_elf(path, text, data_address, data, bss_address, bss_size):
    names = b'\0.text\0.reginfo\0.data\0.bss\0.shstrtab\0'
    blobs = [struct.pack('<%dI' % len(text), *text), struct.pack('<6I', 0, 0, 0, 0, 0, 0x128000),
             struct.pack('<%dI' % len(data), *data), b'', names]
    offsets, at = [], 0x40
    for blob in blobs:
        offsets.append(at)
        at += (len(blob) + 3) & ~3
    sections = [(0, 0, 0, 0, 0, 0),
                (1, 1, 0x100000, offsets[0], len(blobs[0]), 4),
                (7, 0x70000006, 0, offsets[1], len(blobs[1]), 4),
                (16, 1, data_address, offsets[2], len(blobs[2]), 4),
                (22, 8, bss_address, offsets[3], bss_size, 4),
                (27, 3, 0, offsets[4], len(blobs[4]), 1)]
    out = bytearray(at)
    struct.pack_into('<I', out, 0x20, at)
    struct.pack_into('<HHH', out, 0x2E, 40, len(sections), 5)
    for blob, offset in zip(blobs, offsets):
        out[offset:offset + len(blob)] = blob
    for name, kind, address, offset, size, align in sections:
        out += struct.pack('<10I', name, kind, 0, address, offset, size, 0, 0, align, 0)
    Path(path).write_bytes(bytes(out))


def releases(folder):
    """NTSC: f0 calls f1 and f2; f1 loads data; f2 stores to bss. PAL: f1 gains an instruction, so f2 moves by 4;
    the data moves by 0x10, the bss by 0x20."""
    f1, f2 = 0x100018, 0x100028
    ntsc = [jal(f1), NOP, jal(f2), NOP, JR_RA, NOP,
            lui(V0, 0x11), lw(V0, 0, V0), JR_RA, NOP,
            lui(V1, 0x12), sw(0, 0, V1), lui(V1, 0x12), sw(0, 8, V1), JR_RA, NOP]
    pal = [jal(f1), NOP, jal(f2 + 4), NOP, JR_RA, NOP,
           lui(V0, 0x11), lw(V0, 0x10, V0), addiu(V0, V0, 1), JR_RA, NOP,
           lui(V1, 0x12), sw(0, 0x20, V1), lui(V1, 0x12), sw(0, 0x28, V1), JR_RA, NOP]
    write_elf(folder / 'ntsc.elf', ntsc, 0x110000, [0x11111111, f2, 0x22222222], 0x120000, 0x100)
    write_elf(folder / 'pal.elf', pal, 0x110010, [0x11111111, f2 + 4, 0x22222222], 0x120020, 0x100)
    return folder / 'ntsc.elf', folder / 'pal.elf'


class RegionMapTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.folder = Path(self.tmp.name)
        self.ntsc, self.pal = releases(self.folder)
        self.map = self.folder / 'pal.map'
        with redirect_stdout(StringIO()):
            region.build(self.ntsc, self.pal, self.map)
        self.m = region.Map(self.map)

    def tearDown(self):
        self.tmp.cleanup()

    def test_moved_function_translates(self):
        self.assertEqual(self.m.translate(0x100000), 0x100000)
        self.assertEqual(self.m.translate(0x100028), 0x10002C)
        self.assertEqual(self.m.translate(0x100034), 0x100038)
        self.assertIsNone(self.m.in_changed(0x100028))

    def test_changed_function_is_flagged(self):
        self.assertEqual(self.m.in_changed(0x10001C), (0x100018, 0x100028))
        self.assertEqual(self.m.translate(0x10001C), 0x10001C)

    def test_data_and_bss_translate(self):
        self.assertEqual(self.m.translate(0x110008), 0x110018)
        self.assertEqual(self.m.range(0x110008)[0], 'data')
        self.assertEqual(self.m.translate(0x120004), 0x120024)
        self.assertEqual(self.m.range(0x120004)[0], 'bss')
        self.assertIsNone(self.m.translate(0x120010))

    def test_symbols_translate(self):
        names, out = self.folder / 'names.txt', self.folder / 'names.pal.txt'
        names.write_text('# a list\nf2 0x100028  # why\nf0 0x100000\n')
        with redirect_stdout(StringIO()):
            self.assertEqual(region.symbols(self.map, names, out), 0)
        self.assertEqual(out.read_text(), '# a list\nf2 0x10002c  # why\nf0 0x100000\n')

    def test_patches_carry_over(self):
        patches, reviewed, out = self.folder / 'patches.txt', self.folder / 'reviewed.txt', self.folder / 'out.txt'
        patches.write_text('# stores\n0x10002c 0x%08x 0x%08x\n0x10001c 0x%08x 0x00000000\n0x100020 0x%08x 0x00000000\n'
                           % (sw(0, 0, V1), jal(0x100028), lw(V0, 0, V0), JR_RA))
        reviewed.write_text('0x100020 0x100024 0x%08x 0x00000000  # f1 grew\n' % JR_RA)
        with redirect_stdout(StringIO()):
            self.assertEqual(region.patches(self.map, patches, self.ntsc, self.pal, reviewed, out), 0)
        self.assertEqual(out.read_text().splitlines()[1:], [
            '# stores',
            '0x100030 0x%08x 0x%08x' % (sw(0, 0x20, V1), jal(0x10002C)),
            '0x10001c 0x%08x 0x00000000' % lw(V0, 0x10, V0),
            '0x100024 0x%08x 0x00000000' % JR_RA])

    def test_patch_without_a_place_is_left_for_review(self):
        patches, reviewed, out = self.folder / 'patches.txt', self.folder / 'reviewed.txt', self.folder / 'out.txt'
        patches.write_text('0x100020 0x%08x 0x00000000\n' % JR_RA)
        reviewed.write_text('')
        with redirect_stdout(StringIO()):
            self.assertEqual(region.patches(self.map, patches, self.ntsc, self.pal, reviewed, out), 0)
        self.assertEqual(out.read_text().splitlines()[1:], ['0x100024 0x%08x 0x00000000' % JR_RA])
        patches.write_text('0x100020 0x%08x 0x00000000\n' % JR_RA)
        (self.folder / 'pal.elf').write_bytes((self.folder / 'pal.elf').read_bytes().replace(
            struct.pack('<I', JR_RA), struct.pack('<I', 0x03E00009)))
        with redirect_stdout(StringIO()):
            self.assertEqual(region.patches(self.map, patches, self.ntsc, self.pal, reviewed, out), 1)
        self.assertTrue(out.read_text().splitlines()[1].startswith('# REVIEW 0x100020'))

    def test_jump_to_a_target_without_a_place_is_left_for_review(self):
        patches, reviewed, out = self.folder / 'patches.txt', self.folder / 'reviewed.txt', self.folder / 'out.txt'
        patches.write_text('0x10002c 0x%08x 0x%08x\n' % (sw(0, 0, V1), jal(0x120010)))
        reviewed.write_text('')
        with redirect_stdout(StringIO()):
            self.assertEqual(region.patches(self.map, patches, self.ntsc, self.pal, reviewed, out), 1)
        self.assertEqual(out.read_text().splitlines()[1:],
                         ['# REVIEW 0x10002c 0x%08x 0x%08x: the jump target has no place' % (sw(0, 0, V1), jal(0x120010))])

    def write_table(self, text):
        table = self.folder / 'sc3_addresses.txt'
        table.write_text('# name ntsc pal\n' + text)
        return table

    def call(self, command, *args):
        with redirect_stdout(StringIO()) as printed:
            code = command([str(a) for a in args])
        return code, printed.getvalue()

    def test_addresses_fill_the_pal_column_from_the_map(self):
        table = self.write_table('F2 0x00100028 -  # a note\nData[] 0x00110000 -\nData[] 0x00110008 0x00000001\n')
        self.assertEqual(self.call(region.addresses, table, self.map)[0], 1)
        self.assertEqual(self.call(region.addresses, table, self.map, '--write')[0], 0)
        self.assertEqual(table.read_text().splitlines()[1:], ['F2     0x00100028 0x0010002C  # a note',
                                                              'Data[] 0x00110000 0x00110010',
                                                              'Data[] 0x00110008 0x00110018'])
        self.assertEqual(self.call(region.addresses, table, self.map)[0], 0)

    def test_addresses_refuse_an_unreviewed_row_in_a_changed_function(self):
        table = self.write_table('Inside 0x0010001C -\n')
        code, printed = self.call(region.addresses, table, self.map, '--write')
        self.assertEqual(code, 1)
        self.assertIn('Inside 0x10001c: inside 0x100018-0x100028, which changed: read it and mark the row reviewed=pal',
                      printed)
        table = self.write_table('Inside 0x0010001C 0x00100020 reviewed=pal  # read\n')
        self.assertEqual(self.call(region.addresses, table, self.map)[0], 0)

    def test_header_names_the_columns_addresses_and_the_calls(self):
        table = self.write_table('F2 0x00100028 0x0010002C\nData[] 0x00110000 0x00110010\nData[] 0x00110008 0x00110018\n')
        port = self.folder / 'port'
        port.mkdir()
        (port / 'a.cpp').write_text('fn::F2(r, c, t);\n')
        symbols = self.folder / 'symbols.txt'
        symbols.write_text('f2 0x10002c\n')
        out = self.folder / 'sc3_region.h'
        for column, expected in (('ntsc', ('F2 = 0x00100028u;', 'Data[] = {0x00110000u, 0x00110008u};', '&::func_0x100028;')),
                                 ('pal', ('F2 = 0x0010002Cu;', 'Data[] = {0x00110010u, 0x00110018u};', '&::f2_0x10002c;'))):
            code, _ = self.call(region.header, out, '--title', 'T', '--elf', 'SCES_533.12', '--table', table, '--column',
                               column, '--symbols', symbols, '--sources', port)
            self.assertEqual(code, 0)
            text = out.read_text()
            for line in expected + ('kSerial = "SCES-53312";',):
                self.assertIn(line, text)

    def test_header_refuses_a_call_the_table_does_not_name(self):
        table = self.write_table('F2 0x00100028 0x0010002C\n')
        port = self.folder / 'port'
        port.mkdir()
        (port / 'a.cpp').write_text('fn::Other(r, c, t);\n')
        code, printed = self.call(region.header, self.folder / 'h.h', '--title', 'T', '--elf', 'E', '--table', table,
                                 '--sources', port)
        self.assertEqual(code, 1)
        self.assertIn('fn::Other: no such address', printed)

    def test_vote_drops_a_stray_offset(self):
        pairs = [(0x120000, 0x120020), (0x120008, 0x120028), (0x120008, 0x120028), (0x120008, 0x130028),
                 (0x120040, 0x110040), (0x120080, 0x1200A0)]
        self.assertEqual(region.vote(pairs, 'bss', 0x20), [('bss', 0x120000, 0x120084, 0x20)])


class CommittedTableTest(unittest.TestCase):
    def test_the_pal_column_is_the_maps(self):
        with redirect_stdout(StringIO()) as printed:
            code = region.addresses([str(ROOT / 'bozo/port/sc3_addresses.txt'), str(ROOT / 'recomp/pal/address_map.txt')])
        self.assertEqual(code, 0, printed.getvalue())


if __name__ == '__main__':
    unittest.main()
