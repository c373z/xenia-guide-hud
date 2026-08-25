"""Find every static reference to an address in a XEX basefile.

  python refs.py <file.pe> <runtime_addr> [text_shift]

text_shift defaults to 0x7200 (xam). Pass 0 for dash/hud, whose runtime
addresses are their PE addresses.

Reports three kinds of reference, because using only one of them has produced
three wrong conclusions in this investigation:

  direct    a "bl" whose computed target is the address
  data      the address stored as a word in .rdata/.data - vtables, tables of
            function pointers
  register  "lis rX,hi" followed by "addi rY,rX,lo" forming the address in a
            register - thread start routines, callbacks passed as arguments,
            anything handed to a function rather than stored

A function with no direct caller is NOT therefore an entry point: 4,494 of
xam's 18,301 functions have no direct caller, about half of those are reached
through stored pointers, and of the remainder some are reached through
register-formed addresses. 81750FA8 looked unreferenced by the first two tests
and turned out to be a thread routine found only by the third.

Absence of all three still is not proof - an address assembled by arithmetic
other than lis/addi would evade this - but it is the strongest static statement
available.
"""
import bisect, struct, sys


def load(path):
    d = open(path, 'rb').read()
    pe = struct.unpack('<I', d[0x3c:0x40])[0]
    ns = struct.unpack('<H', d[pe + 6:pe + 8])[0]
    opt = struct.unpack('<H', d[pe + 20:pe + 22])[0]
    ib = struct.unpack('<I', d[pe + 24 + 28:pe + 24 + 32])[0]
    o = pe + 24 + opt
    secs = []
    for i in range(ns):
        e = d[o + i * 40:o + i * 40 + 40]
        n = e[:8].rstrip(b'\0').decode()
        vs, va, rs, ra = struct.unpack('<IIII', e[8:24])
        secs.append((n, va, vs, ra, rs))
    return d, ib, secs


def main():
    path = sys.argv[1]
    target = int(sys.argv[2], 16)
    shift = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x7200
    d, ib, secs = load(path)
    S = {n: (va, vs, ra, rs) for n, va, vs, ra, rs in secs}
    pva, pvs, pra, prs = S['.pdata']
    starts = sorted({a for off in range(pra, pra + pvs, 8)
                     for a in [struct.unpack('>I', d[off:off + 4])[0]]
                     if 0x81000000 < a < 0x93000000})

    def fn(rt):
        i = bisect.bisect_right(starts, rt) - 1
        return starts[i] if i >= 0 else 0

    tva, tvs, tra, trs = S['.text']
    hi, lo = (target >> 16) & 0xffff, target & 0xffff
    if lo & 0x8000:                 # addi sign-extends, so the lis half is +1
        hi = (hi + 1) & 0xffff
    found = 0
    for off in range(0, tvs - 4, 4):
        w = struct.unpack('>I', d[tra + off:tra + off + 4])[0]
        site = (ib + tva + off) - shift
        if (w >> 26) == 18 and (w & 1):
            li = w & 0x3fffffc
            if li & 0x2000000:
                li -= 0x4000000
            if site + li == target:
                print('  direct    %08X  in fn %08X' % (site, fn(site)))
                found += 1
        elif (w >> 26) == 15 and (w & 0xffff) == hi:
            rD = (w >> 21) & 31
            for k in range(1, 12):
                if off + 4 * k >= tvs:
                    break
                w2 = struct.unpack('>I', d[tra + off + 4 * k:tra + off + 4 * k + 4])[0]
                if (w2 >> 26) == 14 and ((w2 >> 16) & 31) == rD:
                    if (w2 & 0xffff) == lo:
                        s2 = (ib + tva + off + 4 * k) - shift
                        print('  register  %08X  in fn %08X' % (s2, fn(s2)))
                        found += 1
                    break
    for n, va, vs, ra, rs in secs:
        if n in ('.text', '.pdata'):
            continue
        for off in range(0, min(vs, rs) - 4, 4):
            if struct.unpack('>I', d[ra + off:ra + off + 4])[0] == target:
                print('  data      %s+%06X (VA %08X)' % (n, off, ib + va + off))
                found += 1
    print('  total references: %d' % found)


main()
