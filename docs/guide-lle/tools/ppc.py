"""PowerPC BE disassembler for the converted firmware PEs.

Written because two findings in NEXT.md were invalidated by hand-decoding
errors (wrong branch polarity, and decoding against the wrong xam build).
Anything quoted from here should also state which image it came from --
see Image.label.

Usage:
    python -m tools.ppc <image> <va> [count]
    python tools/ppc.py work/xam17489.pe 819F4488 60

Branch targets are absolute and already resolved, which is the part that was
being got wrong by hand.
"""
import struct
import sys

BO_COND = {
    (12, 2): 'beq', (4, 2): 'bne',
    (12, 0): 'blt', (4, 0): 'bge',
    (12, 1): 'bgt', (4, 1): 'ble',
    (12, 3): 'bso', (4, 3): 'bns',
}


class Img:
    def __init__(self, path):
        self.path = path
        self.label = path.replace('\\', '/').rsplit('/', 1)[-1]
        self.d = open(path, 'rb').read()
        pe = struct.unpack_from('<I', self.d, 0x3c)[0]
        self.ib = struct.unpack_from('<I', self.d, pe + 24 + 28)[0]

    def word(self, va):
        return struct.unpack_from('>I', self.d, va - self.ib)[0]

    def cstr(self, va, limit=200):
        o = va - self.ib
        e = self.d.find(b'\0', o, o + limit)
        return self.d[o:e].decode('latin1', 'replace')

    def wstr(self, va, limit=200):
        o = va - self.ib
        out = []
        for i in range(limit):
            c = struct.unpack_from('<H', self.d, o + i * 2)[0]
            if c == 0:
                break
            out.append(chr(c))
        return ''.join(out)


def decode(w, a):
    """Return (text, branch_target_or_None)."""
    op = w >> 26
    rt = (w >> 21) & 31
    ra = (w >> 16) & 31
    rb = (w >> 11) & 31
    si = w & 0xFFFF
    s = si - 0x10000 if si & 0x8000 else si
    xo = (w >> 1) & 0x3FF

    if op == 14:
        return (('li     r%d,%d' % (rt, s)) if ra == 0 else
                ('addi   r%d,r%d,%d' % (rt, ra, s))), None
    if op == 15:
        return (('lis    r%d,0x%04X' % (rt, si)) if ra == 0 else
                ('addis  r%d,r%d,0x%04X' % (rt, ra, si))), None
    if op == 24:
        return 'ori    r%d,r%d,0x%04X' % (ra, rt, si), None
    if op == 25:
        return 'oris   r%d,r%d,0x%04X' % (ra, rt, si), None
    if op == 10:
        return 'cmpli  cr%d,r%d,0x%X' % ((w >> 23) & 7, ra, si), None
    if op == 11:
        return 'cmpi   cr%d,r%d,%d' % ((w >> 23) & 7, ra, s), None
    if op == 32:
        return 'lwz    r%d,%d(r%d)' % (rt, s, ra), None
    if op == 34:
        return 'lbz    r%d,%d(r%d)' % (rt, s, ra), None
    if op == 40:
        return 'lhz    r%d,%d(r%d)' % (rt, s, ra), None
    if op == 36:
        return 'stw    r%d,%d(r%d)' % (rt, s, ra), None
    if op == 38:
        return 'stb    r%d,%d(r%d)' % (rt, s, ra), None
    if op == 44:
        return 'sth    r%d,%d(r%d)' % (rt, s, ra), None
    if op == 21:
        mb = (w >> 6) & 31
        me = (w >> 1) & 31
        sh = (w >> 11) & 31
        return 'rlwinm r%d,r%d,%d,%d,%d' % (ra, rt, sh, mb, me), None
    if op == 58:
        ds = w & 0xFFFC
        if ds & 0x8000:
            ds -= 0x10000
        return ('ld     r%d,%d(r%d)' % (rt, ds, ra)) if (w & 3) == 0 else \
               ('ldu/lwa r%d,%d(r%d)' % (rt, ds, ra)), None
    if op == 62:
        ds = w & 0xFFFC
        if ds & 0x8000:
            ds -= 0x10000
        return 'std    r%d,%d(r%d)' % (rt, ds, ra), None

    if op == 16:  # bc
        bo = (w >> 21) & 31
        bi = (w >> 16) & 31
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        tgt = bd if (w & 2) else a + bd
        m = BO_COND.get((bo & 0x1E, bi & 3))
        name = m if m else 'bc(bo=%d,bi=%d)' % (bo, bi)
        if (bi >> 2) != 0:
            name += ' cr%d,' % (bi >> 2)
        else:
            name += ' '
        return '%-6s 0x%08X' % (name, tgt), tgt
    if op == 18:  # b / bl
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        tgt = li if (w & 2) else a + li
        return '%-6s 0x%08X' % ('bl' if (w & 1) else 'b', tgt), tgt
    if op == 19:
        if w == 0x4E800020:
            return 'blr', None
        if w == 0x4E800420:
            return 'bctr', None
        if w == 0x4E800421:
            return 'bctrl', None
        return 'b?19   xo=%d' % xo, None
    if op == 31:
        if xo == 444:
            return ('mr     r%d,r%d' % (ra, rt)) if rt == rb else \
                   ('or     r%d,r%d,r%d' % (ra, rt, rb)), None
        if xo == 0:
            return 'cmp    cr%d,r%d,r%d' % ((w >> 23) & 7, ra, rb), None
        if xo == 32:
            return 'cmpl   cr%d,r%d,r%d' % ((w >> 23) & 7, ra, rb), None
        if xo == 40:
            return 'subf   r%d,r%d,r%d' % (rt, ra, rb), None
        if xo == 266:
            return 'add    r%d,r%d,r%d' % (rt, ra, rb), None
        if xo == 23:
            return 'lwzx   r%d,r%d,r%d' % (rt, ra, rb), None
        if xo == 151:
            return 'stwx   r%d,r%d,r%d' % (rt, ra, rb), None
        if xo == 339:
            spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
            return 'mfspr  r%d,%d%s' % (rt, spr, ' (lr)' if spr == 8 else ''), None
        if xo == 467:
            spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
            return 'mtspr  %d,r%d%s' % (spr, rt, ' (lr)' if spr == 8 else
                                        (' (ctr)' if spr == 9 else '')), None
        return 'x31    xo=%-4d r%d,r%d,r%d' % (xo, rt, ra, rb), None
    return '.long  0x%08X   (op=%d xo=%d)' % (w, op, xo), None


def dis(img, va, n=40, mark=None):
    print('=== %s @ 0x%08X (%d instrs) ===' % (img.label, va, n))
    for i in range(n):
        a = va + i * 4
        w = img.word(a)
        t, tgt = decode(w, a)
        flag = ' <<<' if mark and a == mark else ''
        rel = ''
        if tgt is not None:
            d = tgt - a
            rel = '   (%+d)' % d
        print('  %08X: %08X  %s%s%s' % (a, w, t, rel, flag))


if __name__ == '__main__':
    p = sys.argv[1]
    va = int(sys.argv[2], 16)
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    dis(Img(p), va, n)
