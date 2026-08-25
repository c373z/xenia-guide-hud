"""Exact function lookup for a XEX basefile using its .pdata table.

Xenon PE images carry a RUNTIME_FUNCTION table in .pdata - 18,301 entries for
xam - giving exact start addresses for every function THAT HAS UNWIND DATA.

IMPORTANT: that is not every function. Leaf functions which never touch LR and
allocate no frame need no unwind record and are absent from .pdata, so a lookup
will report the previous function and silently swallow them. Runtime 8174FFD0 -
the XUI command dispatcher - is exactly such a function: it begins with
"lwz r11,4(r3)", has no prologue, and .pdata attributes its body to the function
before it.

So use this to confirm a boundary, not to discover one, and cross-check against
the disassembly: a blr followed by a 00000000 padding word is a function end
whether or not .pdata agrees.

  python fnlookup.py <file.pe> <addr>      which function contains addr
  python fnlookup.py <file.pe> --count     how many functions
"""
import bisect, struct, sys


def load_starts(path):
    d = open(path, 'rb').read()
    pe = struct.unpack('<I', d[0x3c:0x40])[0]
    ns = struct.unpack('<H', d[pe + 6:pe + 8])[0]
    opt = struct.unpack('<H', d[pe + 20:pe + 22])[0]
    o = pe + 24 + opt
    for i in range(ns):
        e = d[o + i * 40:o + i * 40 + 40]
        if e[:8].rstrip(b'\0').decode() != '.pdata':
            continue
        vs, va, rs, ra = struct.unpack('<IIII', e[8:24])
        starts = []
        for off in range(ra, ra + min(vs, rs), 8):
            a = struct.unpack('>I', d[off:off + 4])[0]
            if 0x81000000 < a < 0x82000000:
                starts.append(a)
        starts.sort()
        return starts
    raise SystemExit('no .pdata in ' + path)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    starts = load_starts(sys.argv[1])
    if sys.argv[2] == '--count':
        print(len(starts))
        return
    addr = int(sys.argv[2], 16)
    i = bisect.bisect_right(starts, addr) - 1
    if i < 0:
        print('no function contains %08X' % addr)
        return
    end = starts[i + 1] if i + 1 < len(starts) else None
    print('%08X is in function %08X%s' %
          (addr, starts[i], ' (ends before %08X)' % end if end else ''))


main()
