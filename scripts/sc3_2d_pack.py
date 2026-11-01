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
from sc3_texture_pack import (MODEL_MAGIC, GsMemory, Unsupported, call_lists, decode, olk_leaves, play_gif,  # noqa: E402
                              texture_units, walk_chain)

TEXELS_REG = 0x101
PALETTE_REG = 0x102
NO_RECORD = 0xFFFFFFFF


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
        try:
            for k, entries in enumerate(call_lists(m, units)):
                for _, target in entries:
                    if target is None:
                        continue
                    gs.entry = k + 1
                    data = walk_chain(m, target)
                    if data is not None:
                        play_gif(gs, data)
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
            if touched and 0 not in touched:
                out[q] = (psm, w, h, texels, palette)
    return out


def convert_leaf(job):
    olk, path, offset, size = job
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
            psm, w, h, texels, palette = pages[q]
            records.append((psm, w, h, texels))
            writes = writes + [(TEXELS_REG, len(records) - 1)]
            if palette is not None:
                records.append((texture.PSMCT32, len(palette), 1, struct.pack('<%dI' % len(palette), *palette)))
                writes.append((PALETTE_REG, len(records) - 1))
            else:
                writes.append((PALETTE_REG, NO_RECORD))
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
