import struct,sys
import sys as _sy
d=open(_sy.argv[3] if len(_sy.argv)>3 else 'work/xam17489.pe','rb').read()
def dec(w):
    op=w>>26; rt=(w>>21)&31; ra=(w>>16)&31; rb=(w>>11)&31
    si=w&0xffff; s=si-0x10000 if si&0x8000 else si
    if op==32: return f"lwz r{rt},{s}(r{ra})"
    if op==36: return f"stw r{rt},{s}(r{ra})"
    if op==34: return f"lbz r{rt},{s}(r{ra})"
    if op==14: return f"addi r{rt},r{ra},{s}"
    if op==15: return f"lis r{rt},0x{si:04x}"
    if op==11: return f"cmpwi r{ra},{s}"
    if op==10: return f"cmplwi r{ra},0x{si:x}"
    if op==21:
        mb=(w>>6)&31; me=(w>>1)&31; sh=(w>>11)&31
        return f"rlwinm r{ra},r{rt},{sh},{mb},{me}"
    if op==20:
        mb=(w>>6)&31; me=(w>>1)&31; sh=(w>>11)&31
        return f"rlwimi r{ra},r{rt},{sh},{mb},{me}"
    if op==7: return f"mulli r{rt},r{ra},{s}"
    if op==28: return f"andi. r{ra},r{rt},0x{si:x}"
    if op==24: return f"ori r{ra},r{rt},0x{si:x}"
    if op==31:
        xo=(w>>1)&0x3ff
        if xo==339: return f"mfspr r{rt},{((rb<<5)|ra)}"
        if xo==467: return f"mtspr {((rb<<5)|ra)},r{rt}"
        if xo==444: return f"or r{ra},r{rt},r{rb}"
        if xo==0:   return f"cmpw r{ra},r{rb}"
        if xo==32:  return f"cmplw r{ra},r{rb}"
        if xo==23:  return f"lwzx r{rt},r{ra},r{rb}"
        if xo==87:  return f"lbzx r{rt},r{ra},r{rb}"
        return f"op31 xo={xo} r{rt},r{ra},r{rb}"
    if op==16:
        bd=w&0xfffc; bd=bd-0x10000 if bd&0x8000 else bd
        return f"bc {rt},{ra},{bd:#x}"
    if op==18:
        li=w&0x3fffffc; li=li-0x4000000 if li&0x2000000 else li
        return ("bl " if w&1 else "b ")+f"{li:#x}"
    if op==19:
        xo=(w>>1)&0x3ff
        if xo==16: return "blr"
        if xo==528: return "bctr"
        return f"op19 xo={xo}"
    return f".long 0x{w:08x}"
import struct as _s
_pe=_s.unpack('<I',d[0x3c:0x40])[0]
_ns=_s.unpack('<H',d[_pe+6:_pe+8])[0]
_opt=_s.unpack('<H',d[_pe+20:_pe+22])[0]
_ib=_s.unpack('<I',d[_pe+24+28:_pe+24+32])[0]
_secs=[]
_o=_pe+24+_opt
for _i in range(_ns):
    _e=d[_o+_i*40:_o+_i*40+40]
    _n=_e[:8].rstrip(bytes([0])).decode()
    _vs,_va,_rs,_ra=_s.unpack('<IIII',_e[8:24])
    _secs.append((_n,_va,_vs,_ra))
def tofile(v):
    return v-0x913E0000  # flat map, runtime hud base
    r=v-_ib
    for _n,_va,_vs,_ra in _secs:
        if _va<=r<_va+_vs: return _ra+(r-_va)
    raise SystemExit(f'{v:08X} not in any section')
va=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 40
base=tofile(va)
for i in range(n):
    w=struct.unpack('>I',d[base+i*4:base+i*4+4])[0]
    a=va+i*4; t=dec(w)
    if t.split()[0] in ('bl','b','bc'):
        
        try:
            off=int(t.split()[-1].split(",")[-1],16); t+=f"   -> {a+off:08x}"
        except Exception: pass
    print(f"{a:08x}  {w:08x}  {t}")
