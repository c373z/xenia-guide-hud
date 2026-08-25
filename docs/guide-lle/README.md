# Guide / LLE-xam research record

Working notes from making the Xbox 360 Guide (hud.xex) run under Xenia with a
real xam.xex loaded as a guest module.

| file | what it is |
|---|---|
| `SUMMARY.md` | read this first - what pressing the Xbox button does, why nothing renders, what was fixed, what is still broken |
| `TRACE.md` | verbatim log of a button press from a default-configuration run |
| `FINDINGS.md` | the full record, 149 phases, including every retraction |
| `fnlookup.py` | function boundaries from a XEX basefile's `.pdata` (see its docstring - it omits leaf functions) |
| `ppcdis.py` | PowerPC disassembler, PE-section addressing (xam) |
| `ppcdisflat.py` | same, flat addressing (hud, as Xenia maps it) |

The tools take a **decrypted XEX basefile**, which is not included here - these
are Microsoft binaries and belong to whoever owns the console they came from.
Extract your own with xextool.

## Address conventions

Two spaces are in play and confusing them costs hours:

- xam `.text`: **runtime = ghidra - 0x7200**; `.rdata` and `.pdata` are 1:1
- hud: Xenia maps XEX basefiles **flat**, so runtime = 913E0000 + file offset,
  which does not match the PE section table
- pointers stored in guest data are **runtime** addresses
- strings in xam and hud are **UTF-16BE**

## Reading FINDINGS.md

It is chronological and includes conclusions that were later withdrawn. Where a
phase was corrected, a later phase says so explicitly; the retractions are kept
because the reasoning that produced them is the useful part.
