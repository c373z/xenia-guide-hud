"""Control-flow graph and reachability for one function in a XEX basefile.

  python cfg.py <file.pe> <fn_runtime> [target_runtime]

Addresses in and out are RUNTIME. Prints the basic blocks with their edges,
and if a target is given, whether it is reachable from the entry block and one
concrete path to it.

Why this exists: reading fixed-size disassembly windows cannot tell you whether
a block is reachable, because a block's entry can be a branch from anywhere in
the function. Two claims in this research were nearly made on window-reading
alone.

Indirect branches (bctr) are shown as edges to UNKNOWN and are NOT followed, so
"unreachable" from this tool means "unreachable by direct branches" - a jump
table would defeat it. That limitation is reported in the output when any
bctr is present.
"""
import struct, bisect, sys, collections

def load(path):
    d = open(path, 'rb').read()
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
    return d, ib, sec

def main():
    d, ib, sec = load(sys.argv[1])
    start = int(sys.argv[2], 16)
    target = int(sys.argv[3], 16) if len(sys.argv) > 3 else None
    pva, pvs, pra = sec['.pdata']
    starts = sorted(a for off in range(pra, pra + pvs, 8)
                    for a in [struct.unpack('>I', d[off:off + 4])[0]]
                    if 0x81000000 < a < 0x82000000)
    i = bisect.bisect_right(starts, start)
    end = starts[i] if i < len(starts) else start + 0x2000
    tva, tvs, tra = sec['.text']
    def word(rt):
        off = tra + (rt + 0x7200) - (ib + tva)
        return struct.unpack('>I', d[off:off + 4])[0]

    # pass 1: successors per instruction
    succ, leaders, has_bctr = {}, {start}, False
    for a in range(start, end, 4):
        w = word(a)
        op = w >> 26
        s = []
        if op == 18:                                  # b / bl
            li = w & 0x3fffffc
            if li & 0x2000000: li -= 0x4000000
            t = (li if (w >> 1) & 1 else a + li)
            if w & 1:                                 # bl: call, falls through
                s = [a + 4]
            else:
                s = [t]; leaders.add(t)
        elif op == 16:                                # bc
            bd = w & 0xfffc
            if bd & 0x8000: bd -= 0x10000
            t = (bd if (w >> 1) & 1 else a + bd)
            bo = (w >> 21) & 31
            uncond = (bo & 0x14) == 0x14
            if w & 1:
                s = [a + 4]
            elif uncond:
                s = [t]; leaders.add(t)
            else:
                s = [t, a + 4]; leaders.add(t); leaders.add(a + 4)
        elif op == 19:
            xo = (w >> 1) & 0x3ff
            if xo == 16:  s = []                      # blr
            elif xo == 528:
                has_bctr = True
                s = [] if not (w & 1) else [a + 4]    # bctr tail vs bctrl call
            else: s = [a + 4]
        else:
            s = [a + 4]
        succ[a] = [x for x in s if start <= x < end]
        if not s and a + 4 < end:
            leaders.add(a + 4)
    # pass 2: reachability over instructions (blocks are cosmetic here)
    seen, order, q = {start}, [], collections.deque([start])
    prev = {start: None}
    while q:
        a = q.popleft(); order.append(a)
        for t in succ.get(a, []):
            if t not in seen:
                seen.add(t); prev[t] = a; q.append(t)
    print('function %08X .. %08X  (%d instructions, %d reachable)'
          % (start, end, (end - start) // 4, len(seen)))
    if has_bctr:
        print('NOTE: function contains bctr - indirect targets are not followed,')
        print('      so "unreachable" here means "not reachable by direct branch".')
    if target is None:
        return
    if target not in seen:
        print('TARGET %08X: NOT reachable from entry by direct branches' % target)
        unreached = [a for a in range(start, end, 4) if a not in seen]
        print('  (%d instructions unreachable overall)' % len(unreached))
        return
    path, a = [], target
    while a is not None:
        path.append(a); a = prev[a]
    path.reverse()
    print('TARGET %08X: reachable. Path (%d steps, branch points only):'
          % (target, len(path)))
    for a in path:
        w = word(a); op = w >> 26
        if op in (16, 18, 19) or a in (start, target):
            print('   %08X  %08X' % (a, w))

main()
