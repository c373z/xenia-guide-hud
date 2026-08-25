"""Reverse call-graph for a XEX basefile.

  python callers.py <file.pe> <runtime_addr> [levels]

Scans .text for "bl" sites whose computed target is the address, attributes
each to its containing function via .pdata, and repeats outward. All addresses
in and out are RUNTIME (ghidra - 0x7200 for xam .text).

Caveats, both of which can hide a real caller:
  - only direct "bl" is followed. Virtual calls (bctrl through a vtable) and
    calls through a function pointer are invisible here.
  - .pdata omits leaf functions, so a caller may be attributed to the function
    before it. Cross-check the disassembly before relying on a boundary.
"""
import struct, bisect, sys

def sections(d):
    pe = struct.unpack('<I', d[0x3c:0x40])[0]
    ns = struct.unpack('<H', d[pe + 6:pe + 8])[0]
    opt = struct.unpack('<H', d[pe + 20:pe + 22])[0]
    ib = struct.unpack('<I', d[pe + 24 + 28:pe + 24 + 32])[0]
    o = pe + 24 + opt
    sec = {}
    for i in range(ns):
        e = d[o + i * 40:o + i * 40 + 40]
        n = e[:8].rstrip(b'\0').decode()
        vs, va, rs, ra = struct.unpack('<IIII', e[8:24])
        sec[n] = (va, vs, ra)
    return ib, sec

def main():
    d = open(sys.argv[1], 'rb').read()
    target = int(sys.argv[2], 16)
    levels = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    ib, sec = sections(d)
    va, vs, ra = sec['.pdata']
    starts = sorted(a for off in range(ra, ra + vs, 8)
                    for a in [struct.unpack('>I', d[off:off + 4])[0]]
                    if 0x81000000 < a < 0x82000000)
    def fn(rt):
        i = bisect.bisect_right(starts, rt) - 1
        return starts[i] if i >= 0 else 0
    tva, tvs, tra = sec['.text']
    edges = {}
    for off in range(0, tvs - 4, 4):
        w = struct.unpack('>I', d[tra + off:tra + off + 4])[0]
        if (w >> 26) == 18 and (w & 1):
            li = w & 0x3fffffc
            if li & 0x2000000:
                li -= 0x4000000
            rt = (ib + tva + off) - 0x7200
            edges.setdefault(rt + li, []).append(rt)
    seen = {target}
    frontier = [target]
    for lvl in range(1, levels + 1):
        nxt = []
        for t in frontier:
            for site in edges.get(t, []):
                f = fn(site)
                print('  L%d  %08X calls %08X  (site %08X)' % (lvl, f, t, site))
                if f and f not in seen:
                    seen.add(f)
                    nxt.append(f)
        if not nxt:
            print('  L%d: no further callers' % lvl)
            break
        frontier = nxt

main()
