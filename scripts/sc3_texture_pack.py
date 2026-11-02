#!/usr/bin/env python3
"""Soulcalibur III texture pack from the user's own disc, with the standard library only (python3 -I).

usage: scripts/sc3_texture_pack.py <disc dir> <out.pack> [--only /2/0,/4/12] [--report FILE] [--jobs N]

Walks ROOT.OLK (an "olnk" archive of archives) to its leaf files and each leaf to its models (a 03 04 04 00 header;
a model's DMA addresses are relative to it). A model's texture uploads are CALL lists the game CALLs at run time
(runs of DMA CALL tags into upload units, ended by RET; a unit is a CNT with GIF A+D BITBLTBUF / TRXPOS / TRXREG /
TRXDIR and an IMAGE tag, then a REF to the texels) and upload chains it CALLs itself (animated textures). Each list
is played into a model of GS memory (offline only), and every texture a material of the model selects (its TEX0, at
the used size the material's GIF tag keeps) whose texels and CLUT the list wrote becomes a record.

Writes <out.pack>, PS2Recomp's generic texture pack (PS2XTEX1, tools/native_pack/texture.py): one file per CALL list,
named '<leaf>@<list offset in the leaf>', its records the textures (indexed: an index a byte; CT32) each followed by
its palette for an indexed one (CT32, index order, alpha 0x80 = 1.0). Beside it, <out>.keys, SC3's key scheme
(write_keys): where each list sits in ROOT.OLK, its entries, and which records a TEX0 and used size select.

The output lives in the build tree and is never committed: it is the user's game data.
"""

import argparse
import multiprocessing
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'PS2Recomp', 'tools'))
from native_pack import texture  # noqa: E402

TEX0_KEY_MASK = (0x3FFF | (0x3F << 14) | (0x3F << 20) | (0xF << 26) | (0xF << 30) | (0x3FFF << 37) | (0xF << 51) |
                 (1 << 55) | (0x1F << 56))

BLOCK32 = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23], [8, 9, 12, 13, 24, 25, 28, 29],
           [10, 11, 14, 15, 26, 27, 30, 31]]
COLUMN32 = [[0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15], [16, 17, 20, 21, 24, 25, 28, 29],
            [18, 19, 22, 23, 26, 27, 30, 31], [32, 33, 36, 37, 40, 41, 44, 45], [34, 35, 38, 39, 42, 43, 46, 47],
            [48, 49, 52, 53, 56, 57, 60, 61], [50, 51, 54, 55, 58, 59, 62, 63]]
BLOCK8 = BLOCK32
COLUMN8 = [
    [0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54],
    [8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62],
    [33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23],
    [41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31],
    [96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86],
    [104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94],
    [65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119],
    [73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127],
    [128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182],
    [136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190],
    [161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151],
    [169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159],
    [224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214],
    [232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222],
    [193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247],
    [201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255],
]
BLOCK4 = [[0, 2, 8, 10], [1, 3, 9, 11], [4, 6, 12, 14], [5, 7, 13, 15], [16, 18, 24, 26], [17, 19, 25, 27],
          [20, 22, 28, 30], [21, 23, 29, 31]]
COLUMN4 = [
    [0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110],
    [16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126],
    [65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47],
    [81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63],
    [192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174],
    [208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190],
    [129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239],
    [145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255],
    [256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366],
    [272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382],
    [321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303],
    [337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319],
    [448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430],
    [464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446],
    [385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495],
    [401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511],
]

GS_BYTES = 4 * 1024 * 1024
PSM_CT32, PSM_CT24, PSM_T8, PSM_T4, PSM_T8H, PSM_T4HL, PSM_T4HH = 0x00, 0x01, 0x13, 0x14, 0x1B, 0x24, 0x2C


def addr32(bp, bw, x, y):
    page = (bp >> 5) + (y >> 5) * max(bw, 1) + (x >> 6)
    block = (bp & 0x1F) + BLOCK32[(y >> 3) & 3][(x >> 3) & 7]
    return ((page << 13) + ((block >> 5) << 13) + (block & 0x1F) * 256 + COLUMN32[y & 7][x & 7] * 4) % GS_BYTES


def addr8(bp, bw, x, y):
    page = (bp >> 5) + (y >> 6) * max(bw >> 1, 1) + (x >> 7)
    block = (bp & 0x1F) + BLOCK8[(y >> 4) & 3][(x >> 4) & 7]
    return ((page << 13) + ((block >> 5) << 13) + (block & 0x1F) * 256 + COLUMN8[y & 15][x & 15]) % GS_BYTES


def addr4(bp, bw, x, y):
    page = (bp >> 5) + (y >> 7) * max(bw >> 1, 1) + (x >> 7)
    block = (bp & 0x1F) + BLOCK4[(y >> 4) & 7][(x >> 5) & 3]
    return ((page << 14) + ((block >> 5) << 14) + (block & 0x1F) * 512 + COLUMN4[y & 15][x & 31]) % (GS_BYTES * 2)


class GsMemory:
    """GS local memory as a CALL list's uploads leave it, with the list entry (1-based) that last wrote each byte
    (offline model only)."""

    def __init__(self):
        self.mem = bytearray(GS_BYTES)
        self.written = bytearray(GS_BYTES)
        self.entry = 1

    def write_ct32(self, bp, bw, sx, sy, w, h, data):
        mem, written = self.mem, self.written
        mark = bytes([self.entry]) * 4
        i = 0
        for y in range(sy, sy + h):
            for x in range(sx, sx + w):
                a = addr32(bp, bw, x, y)
                mem[a:a + 4] = data[i:i + 4]
                written[a:a + 4] = mark
                i += 4

    def write_high(self, psm, bp, bw, sx, sy, w, h, data):
        mem, written = self.mem, self.written
        i = 0
        for y in range(sy, sy + h):
            for x in range(sx, sx + w):
                a = addr32(bp, bw, x, y) + 3
                if psm == PSM_T8H:
                    mem[a] = data[i]
                else:
                    v = (data[i >> 1] >> ((i & 1) * 4)) & 0xF
                    mem[a] = (mem[a] & 0xF0) | v if psm == PSM_T4HL else (mem[a] & 0x0F) | (v << 4)
                written[a] = self.entry
                i += 1

    def ct32(self, bp, bw, x, y, touched):
        a = addr32(bp, bw, x, y)
        e = self.written[a]
        touched.add(e)
        return struct.unpack_from('<I', self.mem, a)[0]


class Unsupported(Exception):
    pass


def clut_ct32(gs, tex0, entries, touched):
    cbp = (tex0 >> 37) & 0x3FFF
    csm = (tex0 >> 55) & 1
    if csm:
        raise Unsupported('CSM2 CLUT')
    out = []
    for e in range(entries):
        s = (e & ~0x18) | ((e & 0x08) << 1) | ((e & 0x10) >> 1)
        out.append(gs.ct32(cbp, 1, s & 15, s >> 4, touched))
    return out


def decode(gs, tex0, w, h):
    """The w x h top-left part of the texture TEX0 selects, as a pack record (PSM, texels: an index a byte for the
    indexed formats, CT32 four bytes) with its palette for an indexed one (its CT32 colours in index order, as the GS
    looks them up: CSM1 order, 16 entries from CBP for T4 whatever CSA, alpha 0x80 = 1.0), and the list entries
    (1-based, 0 for bytes no entry wrote) its texels and CLUT were read from."""
    tbp = tex0 & 0x3FFF
    tbw = (tex0 >> 14) & 0x3F
    psm = (tex0 >> 20) & 0x3F
    tw = (tex0 >> 26) & 0xF
    th = (tex0 >> 30) & 0xF
    cpsm = (tex0 >> 51) & 0xF
    if tw > 10 or th > 10:
        raise Unsupported('size %d x %d' % (tw, th))
    mem, written = gs.mem, gs.written
    touched = set()
    if psm in (PSM_T8, PSM_T4, PSM_T8H, PSM_T4HL, PSM_T4HH):
        if cpsm != PSM_CT32:
            raise Unsupported('CLUT psm %#x' % cpsm)
        four = psm in (PSM_T4, PSM_T4HL, PSM_T4HH)
        palette = clut_ct32(gs, tex0, 16 if four else 256, touched)
        out = bytearray(w * h)
        i = 0
        for y in range(h):
            for x in range(w):
                if psm == PSM_T8:
                    a = addr8(tbp, tbw, x, y)
                    touched.add(written[a])
                    out[i] = mem[a]
                elif psm == PSM_T4:
                    n = addr4(tbp, tbw, x, y)
                    touched.add(written[n >> 1])
                    out[i] = (mem[n >> 1] >> ((n & 1) * 4)) & 0xF
                else:
                    a = addr32(tbp, tbw, x, y)
                    touched.add(written[a + 3])
                    v = mem[a + 3]
                    out[i] = v if psm == PSM_T8H else (v & 0xF if psm == PSM_T4HL else v >> 4)
                i += 1
        return psm, bytes(out), palette, touched
    if psm == PSM_CT32:
        out = bytearray(w * h * 4)
        i = 0
        for y in range(h):
            for x in range(w):
                struct.pack_into('<I', out, i, gs.ct32(tbp, tbw, x, y, touched))
                i += 4
        return psm, bytes(out), None, touched
    raise Unsupported('texture psm %#x' % psm)


def olk_entries(f, base):
    f.seek(base)
    h = f.read(0x20)
    if len(h) < 0x20 or h[4:8] != b'olnk':
        return None
    count = struct.unpack_from('<I', h, 0)[0]
    data = struct.unpack_from('<I', h, 0x10)[0]
    f.seek(base + 0x20)
    return [(base + data + offset, size) for offset, size, _, _ in (struct.unpack('<IIII', f.read(16)) for _ in range(count))]


def olk_leaves(f):
    """(path, byte offset in ROOT.OLK, size) of every leaf: ROOT.OLK holds sub-archives, they hold the files."""
    out = []
    for i, (base, _) in enumerate(olk_entries(f, 0) or []):
        for j, (offset, size) in enumerate(olk_entries(f, base) or []):
            out.append(('/%d/%d' % (i, j), offset, size))
    return out


def dma_tag(d, q):
    lo = struct.unpack_from('<Q', d, q)[0]
    return lo & 0xFFFF, (lo >> 28) & 7, (lo >> 32) & 0x7FFFFFF0


def walk_chain(d, start, limit=1 << 16):
    """GIF data of the DMA chain from `start` (file-relative addresses), and whether it is a whole chain."""
    out = []
    stack = []
    q = start
    for _ in range(limit):
        if q < 0 or q + 16 > len(d):
            return None
        qwc, tid, addr = dma_tag(d, q)
        if tid == 1:  # CNT
            out.append(d[q + 16:q + 16 + qwc * 16])
            q += 16 + qwc * 16
        elif tid == 3 or tid == 4:  # REF, REFS
            if addr + qwc * 16 > len(d):
                return None
            out.append(d[addr:addr + qwc * 16])
            q += 16
        elif tid == 0:  # REFE
            out.append(d[addr:addr + qwc * 16])
            return b''.join(out)
        elif tid == 2:  # NEXT
            out.append(d[q + 16:q + 16 + qwc * 16])
            q = addr
        elif tid == 5:  # CALL
            out.append(d[q + 16:q + 16 + qwc * 16])
            if len(stack) == 2:
                return None
            stack.append(q + 16 + qwc * 16)
            q = addr
        elif tid == 6:  # RET
            out.append(d[q + 16:q + 16 + qwc * 16])
            if not stack:
                return b''.join(out)
            q = stack.pop()
        elif tid == 7:  # END
            out.append(d[q + 16:q + 16 + qwc * 16])
            return b''.join(out)
    return None


def play_gif(gs, data):
    """Plays the GIF stream's image transfers into gs. Returns the number of transfers, or raises Unsupported."""
    regs = {}
    q = 0
    transfers = 0
    while q + 16 <= len(data):
        lo, hi = struct.unpack_from('<QQ', data, q)
        nloop = lo & 0x7FFF
        flg = (lo >> 58) & 3
        nreg = (lo >> 60) & 0xF or 16
        q += 16
        if flg == 0:
            if nreg != 1 or (hi & 0xF) != 0xE:
                raise Unsupported('PACKED tag regs %#x' % hi)
            for _ in range(nloop):
                value, reg = struct.unpack_from('<QQ', data, q)
                regs[reg & 0xFF] = value
                q += 16
        elif flg == 2:
            payload = data[q:q + nloop * 16]
            q += nloop * 16
            bitbltbuf, trxpos, trxreg = regs.get(0x50, 0), regs.get(0x51, 0), regs.get(0x52, 0)
            if regs.get(0x53, 0) & 3:
                raise Unsupported('TRXDIR %d' % (regs.get(0x53, 0) & 3))
            dbp, dbw, dpsm = (bitbltbuf >> 32) & 0x3FFF, (bitbltbuf >> 48) & 0x3F, (bitbltbuf >> 56) & 0x3F
            dsx, dsy = (trxpos >> 32) & 0x7FF, (trxpos >> 48) & 0x7FF
            w, h = trxreg & 0xFFF, (trxreg >> 32) & 0xFFF
            if dpsm not in (PSM_CT32, PSM_T8H, PSM_T4HL, PSM_T4HH) or ((trxpos >> 59) & 3):
                raise Unsupported('upload psm %#x dir %d' % (dpsm, (trxpos >> 59) & 3))
            bits = 32 if dpsm == PSM_CT32 else 8 if dpsm == PSM_T8H else 4
            if len(payload) * 8 < w * h * bits:
                raise Unsupported('short IMAGE')
            if dpsm == PSM_CT32:
                gs.write_ct32(dbp, dbw, dsx, dsy, w, h, payload)
            else:
                gs.write_high(dpsm, dbp, dbw, dsx, dsy, w, h, payload)
            transfers += 1
        else:
            raise Unsupported('GIF format %d' % flg)
    return transfers


def texture_units(d):
    """Offsets of the upload units (CNT tag, A+D BITBLTBUF..TRXDIR, IMAGE tag) in a leaf."""
    units = set()
    for q in range(0, len(d) - 0x70, 16):
        qwc, tid, _ = dma_tag(d, q)
        if tid != 1 or qwc != 6:
            continue
        lo, hi = struct.unpack_from('<QQ', d, q + 16)
        if (lo & 0x7FFF) != 4 or ((lo >> 58) & 3) != 0 or (hi & 0xFF) != 0xE:
            continue
        if all(struct.unpack_from('<Q', d, q + 40 + 16 * i)[0] & 0xFF == 0x50 + i for i in range(4)):
            units.add(q)
    return units


def call_lists(d, units):
    """The leaf's CALL lists: runs of CALL tags into upload units, ended by RET. A game CALLs one at any entry, and the
    uploads run from there to the list's RET. The RETs right after a list are empty lists (one entry, no target)."""
    lists = []
    q = 0
    while q < len(d) - 16:
        qwc, tid, addr = dma_tag(d, q)
        if tid == 5 and qwc == 0 and addr in units:
            entries = []
            while q < len(d) - 16:
                qwc, tid, addr = dma_tag(d, q)
                if tid != 5 or qwc != 0 or addr not in units:
                    break
                entries.append((q, addr))
                q += 16
            if dma_tag(d, q)[1] == 6:
                lists.append(entries)
                q += 16
                while q < len(d) - 16 and dma_tag(d, q)[1] == 6:
                    lists.append([(q, None)])
                    q += 16
                continue
        q += 16
    return lists


def material_textures(d):
    """(TEX0, width, height) of the model's materials: an A+D GIF tag whose spare REGS bits carry the texture's used
    size (bits 32-47 width, 48-63 height, as the game stores it), followed by its register writes, TEX0 among them."""
    out = set()
    for q in range(0, len(d) - 32, 16):
        lo, hi = struct.unpack_from('<QQ', d, q)
        nloop = lo & 0x7FFF
        if (lo >> 58) & 3 or ((lo >> 60) & 0xF) != 1 or (hi & 0xF) != 0xE or not 1 <= nloop <= 8 or q + 16 * (nloop + 1) > len(d):
            continue
        for i in range(nloop):
            value, reg = struct.unpack_from('<QQ', d, q + 16 * (i + 1))
            if (reg & 0xFF) not in (0x06, 0x07):
                continue
            psm, tw, th = (value >> 20) & 0x3F, (value >> 26) & 0xF, (value >> 30) & 0xF
            if psm not in (PSM_CT32, PSM_T8, PSM_T4, PSM_T8H, PSM_T4HL, PSM_T4HH) or tw > 10 or th > 10:
                continue
            w, h = (hi >> 32) & 0xFFFF, (hi >> 48) & 0xFFFF
            if not w or w > (1 << tw):
                w = 1 << tw
            if not h or h > (1 << th):
                h = 1 << th
            out.add((value & TEX0_KEY_MASK, w, h))
    return out


def convert_model(d, report):
    """[(entries, textures, complete)] per CALL list of one model (its addresses are relative to its start), and per
    upload chain no list CALLs (the game CALLs those itself: animated textures), as a list of one entry at its start: entries are the CALL tags' offsets; textures (tex0, w, h, rgba,
    first entry) are those whose texels and CLUT the entries from `first entry` to the list's end all wrote; complete[k]
    says every material texture reading what entries k.. wrote is among them (their uploads can be left out)."""
    units = texture_units(d)
    if not units:
        return []
    materials = sorted(material_textures(d))
    out = []
    lists = call_lists(d, units)
    called = {target for entries in lists for _, target in entries if target is not None}
    heads = [u for u in sorted(units) if u not in called and (u < 16 or dma_tag(d, u - 16)[1] != 3)]
    for entries in lists + [[(u, u)] for u in heads]:
        if len(entries) > 254:
            report.append('list %#x: %d entries' % (entries[0][0], len(entries)))
            continue
        gs = GsMemory()
        try:
            for k, (_, target) in enumerate(entries):
                if target is None:
                    continue
                gs.entry = k + 1
                data = walk_chain(d, target)
                if data is None:
                    raise Unsupported('entry %d: chain does not end' % k)
                play_gif(gs, data)
        except Unsupported as e:
            report.append('list %#x: %s' % (entries[0][0], e))
            continue
        textures, reads = [], []
        for tex0, w, h in materials:
            try:
                psm, texels, palette, touched = decode(gs, tex0, w, h)
            except Unsupported as e:
                report.append('list %#x tex0 %#x: %s' % (entries[0][0], tex0, e))
                continue
            written = sorted(e for e in touched if e)
            if not written:
                continue
            reads.append((0 in touched, written[0] - 1, written[-1] - 1))
            if 0 not in touched:
                textures.append((tex0, w, h, psm, texels, palette, written[0] - 1))
        complete = [all(not (last >= k) or (not unwritten and first >= k) for unwritten, first, last in reads)
                    for k in range(len(entries))]
        out.append(([q for q, _ in entries], textures, complete))
    return out


MODEL_MAGIC = 0x00040403


def convert_leaf(d, report):
    """The leaf's models (a 03 04 04 00 header, at the leaf's start or inside a container) converted one by one; a
    model's DMA addresses are relative to its own start. CALL tag offsets come back relative to the leaf."""
    starts = sorted({0} | {q for q in range(0, len(d) - 4, 16) if struct.unpack_from('<I', d, q)[0] == MODEL_MAGIC})
    out = []
    for i, start in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else len(d)
        model_report = []
        for calls, textures, complete in convert_model(d[start:end], model_report):
            out.append(([start + q for q in calls], textures, complete))
        report += ['model %#x %s' % (start, line) for line in model_report]
    return out


KEYS_MAGIC = b'SC3KEYS1'


def write_keys(path, olk_size, leaves):
    """The SC3 key index beside the texture pack: per pack file (one per CALL list, named '<leaf>@<list offset>'), the
    leaf's offset and size in ROOT.OLK, its list's entries (CALL offset in the leaf, complete) and its textures (TEX0
    bits that select texels, used size, first entry, record number of the texels and of the palette or 0xffffffff).

    "SC3KEYS1", u64 ROOT.OLK size, u32 list count, then per list: u16 name length, name, u64 leaf offset, u32 leaf
    size, u32 entry count, u32 texture count, entries (u32 offset, u32 complete), textures (u64 tex0, u16 width,
    u16 height, u32 first entry, u32 texel record, u32 palette record)."""
    with open(path + '.tmp', 'wb') as out:
        lists = [(name, offset, size, calls, keys, complete) for name, offset, size, calls, keys, complete in leaves]
        out.write(KEYS_MAGIC + struct.pack('<QI', olk_size, len(lists)))
        for name, offset, size, calls, keys, complete in lists:
            encoded = name.encode()
            out.write(struct.pack('<H', len(encoded)) + encoded + struct.pack('<QIII', offset, size, len(calls), len(keys)))
            for q, done in zip(calls, complete):
                out.write(struct.pack('<II', q, 1 if done else 0))
            for tex0, w, h, first, texel_record, palette_record in keys:
                out.write(struct.pack('<QHHIII', tex0, w, h, first, texel_record, palette_record))
    os.replace(path + '.tmp', path)


def convert_file(job):
    olk, path, offset, size = job
    with open(olk, 'rb') as f:
        f.seek(offset)
        d = f.read(size)
    report = []
    return path, offset, size, convert_leaf(d, report), report


def keys_path(pack_path):
    return os.path.splitext(pack_path)[0] + '.keys'


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('disc')
    parser.add_argument('out')
    parser.add_argument('--only', default='', help='comma-separated leaf paths (/2/0,...)')
    parser.add_argument('--report', default='', help='file listing the upload shapes not converted')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 1, help='leaves converted at once (default: all cores)')
    args = parser.parse_args()
    olk = os.path.join(args.disc, 'ROOT.OLK')
    only = set(p for p in args.only.split(',') if p)
    report = []
    pack = texture.TexturePackWriter()
    lists = []
    with open(olk, 'rb') as f:
        olk_size = os.fstat(f.fileno()).st_size
        listed = olk_leaves(f)
    jobs = [(olk, path, offset, size) for path, offset, size in listed if size and (not only or path in only)]
    with multiprocessing.Pool(max(1, args.jobs)) as pool:
        for n, (path, offset, size, converted, leaf_report) in enumerate(pool.imap(convert_file, jobs, chunksize=4)):
            report += ['%s %s' % (path, line) for line in leaf_report]
            for calls, textures, complete in converted:
                name = '%s@%x' % (path, calls[0])
                records = pack.add_file(name)
                keys = []
                for tex0, w, h, psm, texels, palette, first in textures:
                    records.append((psm, w, h, texels))
                    texel_record = len(records) - 1
                    palette_record = 0xFFFFFFFF
                    if palette is not None:
                        records.append((texture.PSMCT32, len(palette), 1, struct.pack('<%dI' % len(palette), *palette)))
                        palette_record = len(records) - 1
                    keys.append((tex0, w, h, first, texel_record, palette_record))
                lists.append((name, offset, size, calls, keys, complete))
            if n % 200 == 0:
                print('%d/%d leaves, %d upload lists' % (n, len(jobs), len(lists)), file=sys.stderr)
    pack.write(args.out + '.tmp')
    os.replace(args.out + '.tmp', args.out)
    write_keys(keys_path(args.out), olk_size, lists)
    print('%s: %d upload lists, %d CALL entries, %d records; %d shapes not converted' %
          (args.out, len(lists), sum(len(c) for _, _, _, c, _, _ in lists), pack.record_count(), len(report)))
    if args.report:
        with open(args.report, 'w') as f:
            f.write('\n'.join(report) + '\n')


if __name__ == '__main__':
    main()
