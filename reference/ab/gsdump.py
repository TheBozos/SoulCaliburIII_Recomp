#!/usr/bin/env python3
# gsdump.py <x.gs>: a PCSX2 GS dump as text, one line per run of equal state (reference analysis only, never used by the port)
import struct, sys

d = open(sys.argv[1], 'rb').read()
p = 0


def u32():
    global p
    v = struct.unpack_from('<I', d, p)[0]
    p += 4
    return v


crc = u32()
if crc == 0xFFFFFFFF:
    hs = u32()
    h = struct.unpack_from('<%dI' % (hs // 4), d, p)
    p += hs
    print('hdr', h, file=sys.stderr)
    state_size = h[1]
    serial_size = h[3]
    shot_size = h[8] if len(h) > 8 else 0
    bios_size = h[10] if len(h) > 10 else 0
    p += state_size
    p_after = p
else:
    ss = u32()
    p += ss
regs = d[p:p + 8192]
p += 8192


def dumpregs(r, tag):
    q = lambda a: struct.unpack_from('<Q', r, a)[0]
    print('%s PMODE=%x SMODE2=%x DISPFB1=%x DISPLAY1=%x DISPFB2=%x DISPLAY2=%x EXTBUF=%x EXTDATA=%x EXTWRITE=%x BGCOLOR=%x' % (
        tag, q(0), q(0x20), q(0x70), q(0x80), q(0x90), q(0xa0), q(0xb0), q(0xc0), q(0xd0), q(0xe0)))


dumpregs(regs, 'regs0')
R = {}
names = {0: 'PRIM', 1: 'RGBAQ', 2: 'ST', 3: 'UV', 6: 'TEX0_1', 7: 'TEX0_2', 8: 'CLAMP_1', 9: 'CLAMP_2', 0xa: 'FOG',
         0x14: 'TEX1_1', 0x15: 'TEX1_2', 0x16: 'TEX2_1', 0x17: 'TEX2_2', 0x18: 'XYOFFSET_1', 0x19: 'XYOFFSET_2',
         0x1a: 'PRMODECONT', 0x1b: 'PRMODE', 0x1c: 'TEXCLUT', 0x22: 'SCANMSK', 0x3b: 'TEXA', 0x3d: 'FOGCOL',
         0x3f: 'TEXFLUSH', 0x40: 'SCISSOR_1', 0x41: 'SCISSOR_2', 0x42: 'ALPHA_1', 0x43: 'ALPHA_2', 0x44: 'DIMX',
         0x45: 'DTHE', 0x46: 'COLCLAMP', 0x47: 'TEST_1', 0x48: 'TEST_2', 0x49: 'PABE', 0x4a: 'FBA_1', 0x4b: 'FBA_2',
         0x4c: 'FRAME_1', 0x4d: 'FRAME_2', 0x4e: 'ZBUF_1', 0x4f: 'ZBUF_2', 0x50: 'BITBLTBUF', 0x51: 'TRXPOS',
         0x52: 'TRXREG', 0x53: 'TRXDIR'}
prim = 0
run = None
frame = 0


def key():
    c = (prim >> 9) & 1
    g = lambda n: R.get(n + '_%d' % (c + 1), 0)
    tme = prim & 0x10
    return (prim & 0x7ff, g('FRAME'), g('ZBUF'), g('TEST'), g('ALPHA'), g('TEX0') if tme else 0, g('TEX1') if tme else 0,
            g('SCISSOR'), g('XYOFFSET'), R.get('FOGCOL', 0), R.get('COLCLAMP', 0), R.get('DTHE', 0), R.get('PABE', 0),
            g('FBA'), R.get('TEXA', 0), g('CLAMP') if tme else 0, R.get('RGBAQ', 0) if not prim & 8 else 0)


def flush():
    global run
    if not run:
        return
    x = run
    k = x['k']
    pr, fb, z, te, al, t = k[:6]
    print('f%d n=%d prim=%d iip=%d tme=%d fge=%d abe=%d fst=%d ctx=%d FBP=%x FBW=%d PSM=%x FBMSK=%08x Z=%x ZPSM=%x ZMSK=%d TEST=%x ALPHA=%x(fix %x) '
          'TBP=%x TBW=%d TPSM=%x TW=%d TH=%d TCC=%d TFX=%d CBP=%x CPSM=%x TEX1=%x SCI=%x OFS=%x FOGCOL=%x CC=%x DTHE=%x PABE=%x FBA=%x TEXA=%x CLAMP=%x | '
          'rgba=%08x first=%s last=%s uv=%s uvlast=%s bbox=%d,%d,%d,%d A=%s' % (
              frame, x['n'], pr & 7, pr >> 3 & 1, pr >> 4 & 1, pr >> 5 & 1, pr >> 6 & 1, pr >> 8 & 1, pr >> 9 & 1,
              fb & 0x1ff, fb >> 16 & 0x3f, fb >> 24 & 0x3f, fb >> 32, z & 0x1ff, z >> 24 & 0xf, z >> 32 & 1, te, al & 0xff, al >> 32 & 0xff,
              t & 0x3fff, t >> 14 & 0x3f, t >> 20 & 0x3f, t >> 26 & 0xf, t >> 30 & 0xf, t >> 34 & 1, t >> 35 & 3, t >> 37 & 0x3fff, t >> 51 & 0xf,
              k[6], k[7], k[8], k[9], k[10], k[11], k[12], k[13], k[14], k[15], x['c'], x['f'], x['l'], x['uv'], x['uvl'], (x['b'][0] - (k[8] & 0xffff)) // 16, (x['b'][1] - (k[8] >> 32 & 0xffff)) // 16, (x['b'][2] - (k[8] & 0xffff)) // 16, (x['b'][3] - (k[8] >> 32 & 0xffff)) // 16, sorted(set(c >> 24 for c in x['cs']))[:12]))
    run = None


def setreg(a, v):
    global prim, run
    if a == 0:
        prim = v & 0x7ff
        return
    if a in (4, 5, 0xc, 0xd):
        xy = (v & 0xffff, v >> 16 & 0xffff)
        k = key()
        if run and run['k'] == k:
            run['n'] += 1
            run['l'] = xy
            run['uvl'] = (R.get('UV', 0) & 0xffff, R.get('UV', 0) >> 16 & 0xffff)
            run['cs'].append(R.get('RGBAQ', 0) & 0xffffffff)
            run['b'] = [min(run['b'][0], xy[0]), min(run['b'][1], xy[1]), max(run['b'][2], xy[0]), max(run['b'][3], xy[1])]
        else:
            flush()
            run = {'cs': [R.get('RGBAQ', 0) & 0xffffffff], 'b': [xy[0], xy[1], xy[0], xy[1]], 'k': k, 'n': 1, 'f': xy, 'l': xy, 'c': R.get('RGBAQ', 0), 'uv': (R.get('UV', 0) & 0xffff, R.get('UV', 0) >> 16 & 0xffff), 'uvl': None}
        return
    if a == 0x53:
        flush()
        print('f%d TRX dir=%d BLT=%x POS=%x REG=%x' % (frame, v & 3, R.get('BITBLTBUF', 0), R.get('TRXPOS', 0), R.get('TRXREG', 0)))
    R[names.get(a, 'R%x' % a)] = v


def walk(b):
    i = 0
    while i + 16 <= len(b):
        lo, hi = struct.unpack_from('<QQ', b, i)
        i += 16
        nloop = lo & 0x7fff
        pre = lo >> 46 & 1
        pr = lo >> 47 & 0x7ff
        flg = lo >> 58 & 3
        nreg = lo >> 60 & 0xf or 16
        if pre:
            setreg(0, pr)
        if flg == 0:
            for _ in range(nloop):
                for r in range(nreg):
                    if i + 16 > len(b):
                        return
                    a = hi >> (4 * r) & 0xf
                    u, w = struct.unpack_from('<QQ', b, i)
                    i += 16
                    if a == 0xe:
                        setreg(w & 0xff, u)
                    elif a == 0xf:
                        pass
                    elif a == 0:
                        setreg(0, u & 0x7ff)
                    elif a == 1:
                        R['RGBAQ'] = (u & 0xff) | ((u >> 32 & 0xff) << 8) | ((w & 0xff) << 16) | ((w >> 32 & 0xff) << 24)
                    elif a == 2:
                        R['ST'] = u & 0xffffffff
                    elif a == 3:
                        R['UV'] = (u & 0x3fff) | ((u >> 32 & 0x3fff) << 16)
                    elif a in (4, 5, 0xc, 0xd):
                        setreg(5, (u & 0xffff) | ((u >> 32 & 0xffff) << 16))
                    else:
                        setreg(a, u)
        elif flg == 1:
            tot = nloop * nreg
            for j in range(tot):
                if i + 8 > len(b):
                    return
                u = struct.unpack_from('<Q', b, i)[0]
                i += 8
                a = hi >> (4 * (j % nreg)) & 0xf
                if a == 1:
                    R['RGBAQ'] = u & 0xffffffff
                elif a == 2:
                    R['ST'] = u & 0xffffffff
                elif a == 3:
                    R['UV'] = u & 0x3fff3fff
                elif a in (4, 5, 0xc, 0xd):
                    setreg(5, u)
                elif a == 0:
                    setreg(0, u & 0x7ff)
                elif a < 0xe:
                    setreg(a, u)
            if tot & 1:
                i += 8
        else:
            i += nloop * 16


while p < len(d):
    t = d[p]
    p += 1
    if t == 0:
        p += 1
        sz = u32()
        walk(d[p:p + sz])
        p += sz
    elif t == 1:
        flush()
        fld = d[p]
        p += 1
        frame += 1
        print('--- vsync', frame, fld)
    elif t == 2:
        print('readfifo', u32())
    elif t == 3:
        dumpregs(d[p:p + 8192], 'regs')
        p += 8192
    else:
        print('tag?', t, p)
        break
flush()
