#!/usr/bin/env python3
"""Soulcalibur III 2D material pack from the user's own disc, with the standard library only (python3 -I).

usage: scripts/sc3_2d_pack.py <disc dir> <out.pack> [--only /2/17,...] [--jobs N]

The 2D builders (runtime/sc3_2d.cpp) draw sprites whose texture registers the game REFs from a block inside a loaded
ROOT.OLK leaf (a texture file: a GIF A+D block with TEX1, TEX0 and CLAMP per texture page). Those blocks are converted
here into PS2Recomp's generic mesh pack (PS2XMSH1), one file per leaf with register blocks only, each at its offset in
the leaf with its writes and, as register 0x100, the tag's REGS word (the game keeps the texture's used size there: bits
32-47 width, 48-63 height), as scripts/sc3_mesh_pack.py records them for the 3D. Leaves without such blocks are left out.

A page's texture is uploaded by the file's own CALL lists, its texels by one list and its CLUT by another (the 3D's
texture pack keys a texture by one list and so has none of these): every upload list of the file (each model of the
leaf: a 03 04 04 00 header) is played into one offline GS memory model (scripts/sc3_texture_pack.py's), and each page's
texture is decoded from it at its used size into <out without extension>_textures.pack (PS2XTEX1, one file per leaf
named as the leaf, a texel record and for an indexed texture a CT32 palette record, alpha 0x80 = 1.0). The page's block
then carries the records' numbers as registers 0x101 (texels) and 0x102 (palette, 0xFFFFFFFF none); a page whose
texels or CLUT the file does not upload itself has neither.

An indexed page's other colours: the game may draw it with another CLUT the file uploads (a TEX2 write changing CBP: the
text renderer's colours are the font's 16 CLUTs); each CLUT block the file's lists write besides the page's own gets
a CT32 palette record of the page's size, named in the block as register 0x103 = CBP << 32 | record.

Texture files inside the game's ELF (SLUS_212.16 in the disc dir: the font at 0x4763B0, the menu icons at 0x4AF170 and
nine more) are converted as well, named elf/<address>, as the game has them before 0x118E18 relocates their pointers,
each up to the end of its tables and of the data its upload lists reach (at most the next one's start).

Beside it, <out>.leaves: one line per pack file, its name, the leaf's byte offset in ROOT.OLK and its size (hex).

The output lives in the build tree and is never committed: it is the user's game data.
"""

import argparse
import multiprocessing
import os
import struct
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'PS2Recomp', 'tools'))
sys.path.insert(0, HERE)
from native_pack import mesh, texture  # noqa: E402
from sc3_mesh_pack import register_blocks  # noqa: E402
from sc3_texture_pack import (MODEL_MAGIC, PSM_CT32, GsMemory, Unsupported, call_lists, clut_ct32, decode,  # noqa: E402
                              dma_tag, olk_leaves, play_gif, texture_units, walk_chain)

TEXELS_REG = 0x101
PALETTE_REG = 0x102
CLUT_REG = 0x103
NO_RECORD = 0xFFFFFFFF
ELF_NAME = 'SLUS_212.16'
ELF_FILES = [0x473E60, 0x4742D0, 0x474630, 0x4749E0, 0x474DF0, 0x475200, 0x475610, 0x475970, 0x475DE0, 0x4763B0, 0x4AF170]


def clut_uploads(data):
    """The DBPs of the stream's small CT32 image transfers (256 texels or fewer: CLUTs)."""
    regs, out, q = {}, set(), 0
    while q + 16 <= len(data):
        lo, hi = struct.unpack_from('<QQ', data, q)
        nloop, flg = lo & 0x7FFF, (lo >> 58) & 3
        q += 16
        if flg == 0 and (hi & 0xF) == 0xE:
            for _ in range(nloop):
                value, reg = struct.unpack_from('<QQ', data, q)
                regs[reg & 0xFF] = value
                q += 16
        elif flg == 2:
            q += nloop * 16
            bitbltbuf, trxreg = regs.get(0x50, 0), regs.get(0x52, 0)
            if (bitbltbuf >> 56) & 0x3F == PSM_CT32 and (trxreg & 0xFFF) * ((trxreg >> 32) & 0xFFF) <= 256:
                out.add((bitbltbuf >> 32) & 0x3FFF)
        else:
            break
    return out


def page_textures(d, blocks):
    """{block offset: (psm, w, h, texels, palette)} for the blocks whose texture the leaf's own upload lists write."""
    starts = sorted({0} | {q for q in range(0, len(d) - 4, 16) if struct.unpack_from('<I', d, q)[0] == MODEL_MAGIC})
    out = {}
    for i, start in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else len(d)
        m = d[start:end]
        units = texture_units(m)
        if not units:
            continue
        gs = GsMemory()
        cluts = set()
        try:
            for k, entries in enumerate(call_lists(m, units)):
                for _, target in entries:
                    if target is None:
                        continue
                    gs.entry = k + 1
                    data = walk_chain(m, target)
                    if data is not None:
                        play_gif(gs, data)
                        cluts |= clut_uploads(data)
        except Unsupported:
            continue
        for q, writes in blocks:
            if not start <= q < end:
                continue
            regs = dict(writes)
            tex0 = regs.get(0x07, regs.get(0x06))
            tag = regs.get(0x100, 0)
            if tex0 is None:
                continue
            tw, th = (tex0 >> 26) & 0xF, (tex0 >> 30) & 0xF
            w, h = (tag >> 32) & 0xFFFF, (tag >> 48) & 0xFFFF
            if tw > 10 or th > 10:
                continue
            if not w or w > (1 << tw):
                w = 1 << tw
            if not h or h > (1 << th):
                h = 1 << th
            try:
                psm, texels, palette, touched = decode(gs, tex0, w, h)
            except Unsupported:
                continue
            if not touched or 0 in touched:
                continue
            others = []
            if palette is not None:
                for cbp in sorted(cluts - {(tex0 >> 37) & 0x3FFF}):
                    seen = set()
                    other = clut_ct32(gs, (tex0 & ~(0x3FFF << 37)) | (cbp << 37), len(palette), seen)
                    if seen and 0 not in seen:
                        others.append((cbp, other))
            out[q] = (psm, w, h, texels, palette, others)
    return out


def elf_image(path, address, size, partial=False):
    """size bytes of the ELF's loaded image at address (its PT_LOAD segments; partial: up to the segment's end)."""
    with open(path, 'rb') as f:
        elf = f.read()
    phoff, = struct.unpack_from('<I', elf, 0x1C)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x2A)
    for i in range(phnum):
        kind, off, vaddr, _, filesz = struct.unpack_from('<5I', elf, phoff + phentsize * i)
        if kind == 1 and vaddr <= address and (address + size <= vaddr + filesz or partial and address < vaddr + filesz):
            return elf[off + address - vaddr:off + min(address + size, vaddr + filesz) - vaddr]
    raise ValueError('%s: %#x + %#x not in a loaded segment' % (path, address, size))


def elf_file_size(path, address):
    """How far the texture file at address reaches: its CALL and page tables, its upload units and their data."""
    d = elf_image(path, address, 0x40000, partial=True)
    calls, pages = d[4], d[5] + d[7] * 256
    call_table, page_table = struct.unpack_from('<II', d, 8)
    end = max(0x20, call_table + 20 * calls, page_table + 4 * pages)
    for p in range(pages):
        block = struct.unpack_from('<I', d, page_table + 4 * p)[0]
        end = max(end, (block & ~0xF) + 16 * ((block & 0xF) + 2))
    for unit in texture_units(d[:end + 0x40000]):
        q = unit
        while q + 16 <= len(d):
            qwc, tid, addr = dma_tag(d, q)
            if tid == 1:
                q += 16 * (qwc + 1)
            elif tid == 3:
                end = max(end, addr + 16 * qwc)
                q += 16
            else:
                break
        end = max(end, q + 16)
    following = [a - address for a in ELF_FILES if a > address]
    return min([(end + 15) & ~15] + following)


def convert_leaf(job):
    olk, path, offset, size = job
    if path.startswith('elf/'):
        d = elf_image(olk, offset, size)
    else:
        with open(olk, 'rb') as f:
            f.seek(offset)
            d = f.read(size)
    blocks = register_blocks(d)
    if not blocks:
        return path, offset, size, None, []
    pages = page_textures(d, blocks)
    records = []
    f = mesh.MeshFile(path, d)
    for q, writes in blocks:
        if q in pages:
            psm, w, h, texels, palette, others = pages[q]
            records.append((psm, w, h, texels))
            writes = writes + [(TEXELS_REG, len(records) - 1)]
            if palette is not None:
                records.append((texture.PSMCT32, len(palette), 1, struct.pack('<%dI' % len(palette), *palette)))
                writes.append((PALETTE_REG, len(records) - 1))
            else:
                writes.append((PALETTE_REG, NO_RECORD))
            for cbp, other in others:
                records.append((texture.PSMCT32, len(other), 1, struct.pack('<%dI' % len(other), *other)))
                writes.append((CLUT_REG, (cbp << 32) | (len(records) - 1)))
        f.add_register_block(q, writes)
    return path, offset, size, f.encode(), records


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('disc')
    parser.add_argument('out')
    parser.add_argument('--only', default='', help='comma-separated leaf paths (/2/17,...)')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 1, help='leaves converted at once (default: all cores)')
    args = parser.parse_args()
    olk = os.path.join(args.disc, 'ROOT.OLK')
    only = set(p for p in args.only.split(',') if p)
    with open(olk, 'rb') as f:
        listed = olk_leaves(f)
    jobs = [(olk, path, offset, size) for path, offset, size in listed if size and (not only or path in only)]
    elf = os.path.join(args.disc, ELF_NAME)
    if os.path.exists(elf):
        jobs += [(elf, 'elf/%x' % address, address, elf_file_size(elf, address)) for address in ELF_FILES
                 if not only or 'elf/%x' % address in only]
    files, leaves = [], []
    pack = texture.TexturePackWriter()
    textured = 0
    with multiprocessing.Pool(max(1, args.jobs)) as pool:
        for n, (path, offset, size, encoded, records) in enumerate(pool.imap(convert_leaf, jobs, chunksize=4)):
            if encoded is not None:
                files.append(encoded)
                leaves.append('%s %x %x' % (path, offset, size))
            if records:
                pack.add_file(path).extend(records)
                textured += 1
            if n % 500 == 0:
                print('%d/%d leaves, %d with register blocks' % (n, len(jobs), len(files)), file=sys.stderr)
    with open(args.out + '.tmp', 'wb') as out:
        out.write(mesh.MAGIC + struct.pack('<I', len(files)))
        for encoded in files:
            out.write(encoded)
    os.replace(args.out + '.tmp', args.out)
    index = os.path.splitext(args.out)[0] + '.leaves'
    with open(index + '.tmp', 'w') as out:
        out.write('\n'.join(leaves) + '\n')
    os.replace(index + '.tmp', index)
    textures = os.path.splitext(args.out)[0] + '_textures.pack'
    pack.write(textures + '.tmp')
    os.replace(textures + '.tmp', textures)
    print('%s: %d leaves with register blocks, %d with page textures in %s' % (args.out, len(files), textured, textures))


if __name__ == '__main__':
    main()
