"""Correct address handling for the converted firmware PEs (xam/hud/dash).

The single most important fact, and the one this project got wrong for a long
time: **these files are memory images**. A section's bytes live at file offset
`VA - ImageBase`, and the section header's `PointerToRawData` is NOT usable -
for xam's `.text` it is 0x7200 too low, which is where the "runtime + 0x7200 =
file VA" folklore came from. That rule was a bug, not a property of the image:
it happened to cancel out for `.text` and produced garbage for `.rdata`, which
is exactly the symptom recorded against it ('rpOutputLocation', 'ice (0x%08X)').

With `off()` below, VA == the address the emulator logs, for every section, and
no per-section fudge is needed.

Verified: `.pdata` begins land on real prologues, tile `.text` contiguously, and
`.rdata` strings read correctly at their logged runtime addresses.
"""
import struct, sys, bisect

class Image:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        pe = struct.unpack('<I', self.d[0x3c:0x40])[0]
        nsec = struct.unpack('<H', self.d[pe+6:pe+8])[0]
        optsz = struct.unpack('<H', self.d[pe+20:pe+22])[0]
        self.ib = struct.unpack('<I', self.d[pe+24+28:pe+24+32])[0]  # PE32: +28
        so = pe + 24 + optsz
        self.secs = []
        for i in range(nsec):
            e = self.d[so+i*40:so+i*40+40]
            vs, va, rs, ra = struct.unpack('<IIII', e[8:24])
            self.secs.append((e[:8].rstrip(b'\0').decode(), self.ib+va, vs))
        self._funcs = None

    def off(self, va):
        return va - self.ib

    def sec(self, name):
        for s in self.secs:
            if s[0] == name:
                return s
        raise SystemExit(f'no {name} section')

    def word(self, va):
        o = self.off(va)
        return struct.unpack('>I', self.d[o:o+4])[0]

    def cstr(self, va, limit=200):
        o = self.off(va)
        e = self.d.find(b'\0', o, o+limit)
        return self.d[o:e].decode('latin1')

    def funcs(self):
        """(begin, length) from .pdata, sorted. Packed PPC RUNTIME_FUNCTION:
        PrologLen:8 | FunctionLen:22 | ThirtyTwoBit:1 | ExceptionFlag:1,
        read from the low bits up."""
        if self._funcs is None:
            _, pva, pvs = self.sec('.pdata')
            base = self.off(pva)
            out = []
            for o in range(base, base+pvs-7, 8):
                b, f = struct.unpack('>II', self.d[o:o+8])
                if b:
                    out.append((b, ((f >> 8) & 0x3FFFFF) * 4))
            out.sort()
            self._funcs = out
        return self._funcs

    def containing(self, va):
        fs = self.funcs()
        i = bisect.bisect_right([f[0] for f in fs], va) - 1
        if i < 0:
            return None
        b, ln = fs[i]
        return (b, ln) if va < b + ln + 16 else None

    def _text_words(self):
        _, tva, tvs = self.sec('.text')
        n = tvs // 4
        o = self.off(tva)
        return tva, struct.unpack(f'>{n}I', self.d[o:o+n*4])

    def callers(self, entry):
        """Every `bl` in .text targeting `entry`."""
        tva, ws = self._text_words()
        out = []
        for i, w in enumerate(ws):
            if (w >> 26) != 18 or not (w & 1) or (w & 2):
                continue
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            va = tva + i*4
            if (va + li) & 0xffffffff == entry:
                out.append(va)
        return out

    def xrefs(self, target, window=64):
        """.text sites that materialise `target` via lis + addi/ori.

        Does NOT assume the two halves are adjacent, and does NOT assume the
        destination register matches the source: xam emits
        `lis r8,0x8164` ... `addi r11,r8,0x6938` with the pair dozens of
        instructions apart. Assuming either has silently missed most matches.
        """
        tva, ws = self._text_words()
        hi = {}
        out = []
        for i, w in enumerate(ws):
            va = tva + i*4
            op = w >> 26
            rt, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xffff
            if op == 15 and ra == 0:
                hi[rt] = (va, imm << 16)
                continue
            if op == 14 and ra in hi:
                hva, hv = hi[ra]
                s = imm - 0x10000 if imm & 0x8000 else imm
                if va - hva <= window*4 and (hv + s) & 0xffffffff == target:
                    out.append((hva, va))
            elif op == 24 and rt in hi:
                hva, hv = hi[rt]
                if va - hva <= window*4 and (hv | imm) == target:
                    out.append((hva, va))
            if op in (14, 15, 24, 28, 20, 21):
                hi.pop(ra if op in (24, 28, 20, 21) else rt, None)
            elif op in (32, 34, 36, 40, 44, 7, 8, 12, 13):
                hi.pop(rt, None)
            elif op == 31:
                hi.pop(rt, None)
        return out

if __name__ == '__main__':
    img = Image(sys.argv[1] if len(sys.argv) > 2 else 'work/xam17489.pe')
    va = int(sys.argv[-1], 16)
    f = img.containing(va)
    print(f"{va:08X} in function {f[0]:08X} (len {f[1]:#x})" if f else f"{va:08X}: no .pdata function")
    if f:
        cs = img.callers(f[0])
        print(f"callers of {f[0]:08X}: {len(cs)}")
        for c in cs:
            cf = img.containing(c)
            print(f"  bl at {c:08X}   in {cf[0]:08X}" if cf else f"  bl at {c:08X}")
