#!/usr/bin/env python3
"""Soulcalibur III mesh pack from the user's own disc, with the standard library only (python3 -I).

usage: scripts/sc3_mesh_pack.py <disc dir> <out.pack> [--only /4/204,...] [--jobs N]

Walks ROOT.OLK to its leaf files (scripts/sc3_texture_pack.py's walk). A leaf is loaded whole into EE memory by the game
and relocated in place, so the pack's image of a leaf is its bytes on the disc, and every object is keyed by its offset
in the leaf.

Objects (PS2Recomp's generic mesh pack, PS2XMSH1, tools/native_pack/mesh.py), one per model part the game hands its
part builder 0x10A178 (a REF to raw VIF data): STCYCL 1,1 and an UNPACK of its GIF tag to VU TOP + 0, then STCYCL 4,1 and
one UNPACK a vertex field (V2-32 ST at TOP + 1, V4-32 or S-32 colour at + 2, V4-32 position at + 3, V3-32 normal at + 4,
four quadwords a vertex). kind 1; positions x, y, z and the vertex flags (the position's w word: bit 15 ADC, bits 4-11
the GS F) as a float; one strip (the tag's PRIM, its word 3 as the material word); and section 'SC3V' with the exact
words the UNPACKs leave in VU memory: u32 vertex count, u32 fields (bit 0 normals, bit 1 one colour for all, an S-32
UNPACK), the 16-byte GIF tag, then per vertex 16 u32: ST, colour, position, normal (normal zero without them).

Register blocks: every GS A+D block of a leaf (a PACKED GIF tag of 1-8 A+D writes, the model's materials, the block the
game REFs to VU TOP + 0x19F), at the tag's offset, with its writes (the register in the address's low byte: the game
keeps other bits above it) and, as register 0x100 (no GS register), the tag's
REGS word: the game keeps a texture's used size there (bits 32-47 width, 48-63 height).

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
from native_pack import mesh  # noqa: E402
from sc3_texture_pack import MODEL_MAGIC, olk_leaves  # noqa: E402

STCYCL_11 = 0x01000101
TAG_UNPACK = 0x6C018000
KIND_RAW_PART = 1
KIND_CHAIN_PART = 2
KIND_VU_BLOCK = 3
TAG_REF, TAG_CNT, TAG_RET = 3, 1, 6
CHARACTER_MAGIC = 0x0B1C0E09
GS_REGISTER_LIMIT = 0x63
TAG_REGS = 0x100


def raw_part_size(d, q):
    """Bytes of the raw VIF part at q (its tag UNPACK, then the vertex field UNPACKs), or None."""
    if q + 32 > len(d) or struct.unpack_from('<II', d, q) != (STCYCL_11, TAG_UNPACK):
        return None
    pos, last, fields = q + 24, q + 24, 0
    while pos + 4 <= len(d):
        code = struct.unpack_from('<I', d, pos)[0]
        cmd, num, imm = (code >> 24) & 0x7F, (code >> 16) & 0xFF, code & 0xFFFF
        if code == 0:
            pos += 4
            continue
        if cmd == 1:
            if code == STCYCL_11 and pos + 8 <= len(d) and struct.unpack_from('<I', d, pos + 4)[0] == TAG_UNPACK:
                break
            pos += 4
            continue
        if cmd >= 0x60 and (cmd & 3) == 0 and (imm & 0x8000) and (imm & 0x3FF) in (1, 2, 3, 4):
            pos += 4 + (((cmd >> 2) & 3) + 1) * 4 * (num or 256)
            last = pos
            fields += 1
            continue
        break
    if fields < 2 or last > len(d):
        return None
    return ((last + 15) & ~15) - q


def decode_raw_part(d, q, size):
    """(tag, vertices, fields): the GIF tag words and per vertex the 16 words VU memory holds after the part's UNPACKs."""
    tag = list(struct.unpack_from('<4I', d, q + 8))
    count = tag[0] & 0x7FFF
    vertices = [[0] * 16 for _ in range(count)]
    fields = 0
    pos, end = q + 24, q + size
    while pos + 4 <= end:
        code = struct.unpack_from('<I', d, pos)[0]
        pos += 4
        cmd, num, imm = (code >> 24) & 0x7F, (code >> 16) & 0xFF, code & 0xFFFF
        if cmd < 0x60:
            continue
        comps = ((cmd >> 2) & 3) + 1
        n = num or 256
        field = (imm & 0x3FF) - 1
        for i in range(min(n, count)):
            words = list(struct.unpack_from('<%dI' % comps, d, pos + i * comps * 4))
            if comps == 1:
                words = words * 4
                fields |= 2
            row = vertices[i]
            for c, word in enumerate(words):
                row[field * 4 + c] = word
        if field == 3:
            fields |= 1
        pos += comps * 4 * n
    return tag, vertices, fields


def as_float(word):
    return struct.unpack('<f', struct.pack('<I', word))[0]


def add_raw_part(f, d, q, size):
    tag, vertices, fields = decode_raw_part(d, q, size)
    o = f.add_object(q, kind=KIND_RAW_PART, attributes=struct.pack('<I', size))
    o.positions = [(as_float(v[8]), as_float(v[9]), as_float(v[10]), float(v[11] & 0xFFFF)) for v in vertices]
    prim = (tag[1] >> 15) & 0x7FF
    strip = []
    for k, v in enumerate(vertices):
        rgba = tuple(max(0, min(255, int(as_float(w)))) for w in v[4:8])
        s, t = (max(-32768, min(32767, int(as_float(w) * 4096.0))) for w in v[0:2])
        strip.append((k, 0, 0, 0, mesh.VERTEX_ADC if v[11] & 0x8000 else 0, rgba, s, t))
    o.add_strip(prim, ((tag[1] >> 14) & 1) | (((tag[0] >> 15) & 1) << 1), tag[3], -1, strip)
    blob = [struct.pack('<II4I', len(vertices), fields, *tag)]
    blob.extend(struct.pack('<16I', *v) for v in vertices)
    o.add_section('SC3V', b''.join(blob))


def vu_block_codes(d, q):
    """Offset of the STCYCL 1,1 and V4-32 UNPACK to VU TOP + 0 a block of VU data a chain part REFs starts with (after
    up to two NOPs), or None."""
    for at in (q, q + 8):
        if q < 0 or at + 8 > len(d):
            return None
        stcycl, unpack = struct.unpack_from('<II', d, at)
        if stcycl == STCYCL_11 and (unpack & 0xFF00FFFF) == 0x6C008000:
            return at
        if at == q and struct.unpack_from('<II', d, q) != (0, 0):
            return None
    return None


def vu_block(d, q):
    return vu_block_codes(d, q) is not None


def model_starts(d):
    """Where a leaf's models may start: the leaf, a container's entries (u32 count, then that many ascending offsets),
    the model headers (03 04 04 00; 09 0e 1c 0b for characters)."""
    starts = {0}
    count = struct.unpack_from('<I', d, 0)[0] if len(d) >= 4 else 0
    if 0 < count < 64 and len(d) >= 4 + 4 * count:
        offsets = struct.unpack_from('<%dI' % count, d, 4)
        if all(0 < o < len(d) and o % 16 == 0 for o in offsets) and list(offsets) == sorted(offsets):
            starts.update(offsets)
    starts.update(q for q in range(0, len(d) - 4, 16) if struct.unpack_from('<I', d, q)[0] in (MODEL_MAGIC, CHARACTER_MAGIC))
    return sorted(starts)


def chain_list(d, q):
    """[(relative address, qwc)] of a chain list at q (REF, CNT per batch, then a RET), or None."""
    batches = []
    at = q
    while at + 32 <= len(d):
        ref, address = struct.unpack_from('<II', d, at)
        cnt = struct.unpack_from('<I', d, at + 16)[0]
        if (ref >> 28) & 7 != TAG_REF or ref & 0x0FFF0000 or not ref & 0xFFFF or cnt != TAG_CNT << 28:
            break
        batches.append((address, ref & 0xFFFF))
        at += 32
    if batches and at + 4 <= len(d) and struct.unpack_from('<I', d, at)[0] == TAG_RET << 28:
        return batches
    return None


def chain_parts(d):
    """(offset, [(data offset, qwc)]) of the chain parts in a leaf: the lists 0x10A4E8 CALLs, a REF to a block of VU data
    and a CNT (the game puts the MSCAL in it at run time) per batch, ended by a RET. A REF's address is relative to the
    model the list is in: the nearest model start before it whose REFs all land on VU data blocks."""
    starts = model_starts(d)
    out = []
    q = 0
    while q + 48 <= len(d):
        batches = chain_list(d, q)
        found = None
        if batches:
            for base in reversed([b for b in starts if b <= q]):
                if all(vu_block(d, base + address) for address, _ in batches):
                    found = [(base + address, qwc) for address, qwc in batches]
                    break
        if found:
            out.append((q, found))
            q += 32 * len(found) + 16
        else:
            q += 16
    return out


def add_chain_part(f, q, batches):
    f.add_object(q, kind=KIND_CHAIN_PART, attributes=struct.pack('<I', len(batches)))
    f.objects[-1].add_section('SC3C', b''.join(struct.pack('<II', offset, qwc) for offset, qwc in batches))


def add_vu_block(f, d, q, qwc):
    """A chain part's block that is not a raw part (program B's): the quadwords its UNPACK writes from VU TOP + 0."""
    at = vu_block_codes(d, q)
    num = (struct.unpack_from('<I', d, at + 4)[0] >> 16) & 0xFF or 256
    data = d[at + 8:at + 8 + 16 * num]
    if len(data) != 16 * num:
        return
    o = f.add_object(q, kind=KIND_VU_BLOCK, attributes=struct.pack('<I', qwc * 16))
    o.add_section('SC3B', data)


def register_blocks(d):
    """(offset, writes) of the leaf's A+D blocks: a PACKED tag of 1-8 A+D writes to GS registers."""
    out = []
    for q in range(0, len(d) - 32, 16):
        lo, hi = struct.unpack_from('<QQ', d, q)
        nloop = lo & 0x7FFF
        if (lo >> 58) & 3 or ((lo >> 60) & 0xF) != 1 or (hi & 0xF) != 0xE or not 1 <= nloop <= 8:
            continue
        if q + 16 * (nloop + 1) > len(d):
            continue
        writes = []
        for i in range(nloop):
            value, reg = struct.unpack_from('<QQ', d, q + 16 * (i + 1))
            reg &= 0xFF
            if reg > GS_REGISTER_LIMIT:
                writes = None
                break
            writes.append((reg, value))
        if writes and any(r in (0x06, 0x07, 0x42, 0x43, 0x47, 0x48) for r, _ in writes):
            out.append((q, writes + [(TAG_REGS, hi)]))
    return out


def convert_leaf(job):
    olk, path, offset, size = job
    with open(olk, 'rb') as f:
        f.seek(offset)
        d = f.read(size)
    parts = []
    q = 0
    while q < len(d) - 32:
        n = raw_part_size(d, q)
        if n:
            parts.append((q, n))
            q += n
        else:
            q += 16
    chains = chain_parts(d)
    if not parts and not chains:
        return path, offset, size, None
    f = mesh.MeshFile(path, d)
    for q, n in parts:
        add_raw_part(f, d, q, n)
    raw = {q for q, _ in parts}
    blocks = set()
    for q, batches in chains:
        add_chain_part(f, q, batches)
        blocks.update((at, qwc) for at, qwc in batches if at not in raw)
    for at, qwc in sorted(blocks):
        add_vu_block(f, d, at, qwc)
    for q, writes in register_blocks(d):
        f.add_register_block(q, writes)
    return path, offset, size, f.encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('disc')
    parser.add_argument('out')
    parser.add_argument('--only', default='', help='comma-separated leaf paths (/4/204,...)')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 1, help='leaves converted at once (default: all cores)')
    args = parser.parse_args()
    olk = os.path.join(args.disc, 'ROOT.OLK')
    only = set(p for p in args.only.split(',') if p)
    with open(olk, 'rb') as f:
        listed = olk_leaves(f)
    jobs = [(olk, path, offset, size) for path, offset, size in listed if size and (not only or path in only)]
    files, leaves = [], []
    with multiprocessing.Pool(max(1, args.jobs)) as pool:
        for n, (path, offset, size, encoded) in enumerate(pool.imap(convert_leaf, jobs, chunksize=4)):
            if encoded is not None:
                files.append(encoded)
                leaves.append('%s %x %x' % (path, offset, size))
            if n % 500 == 0:
                print('%d/%d leaves, %d with parts' % (n, len(jobs), len(files)), file=sys.stderr)
    with open(args.out + '.tmp', 'wb') as out:
        out.write(mesh.MAGIC + struct.pack('<I', len(files)))
        for encoded in files:
            out.write(encoded)
    os.replace(args.out + '.tmp', args.out)
    index = os.path.splitext(args.out)[0] + '.leaves'
    with open(index + '.tmp', 'w') as out:
        out.write('\n'.join(leaves) + '\n')
    os.replace(index + '.tmp', index)
    print('%s: %d leaves with parts' % (args.out, len(files)))


if __name__ == '__main__':
    main()
