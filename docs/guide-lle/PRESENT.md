# What actually happens when you press the Xbox button

This supersedes the part of FINDINGS.md that treated
`Guide composite draw -> 00000000` as evidence that the Guide's frame was
being presented. It was not evidence of anything.

## The per-frame draw

The draw hook calls hud `913EAB28` once per `VdSwap`, with the Guide's render
object. Disassembled, it is:

```
913eab44  lwz  r3,12(r31)          ; dc = obj+12
913eab48  bl   XuiRenderBegin(dc)
913eab4c  cmpwi r3,0
913eab50  blt  -> epilogue          ; bail if Begin failed
          ... XuiSendMessage(scene, msg)      ; build msg via 913EA888
          ... XuiBubbleMessage(scene, msg)    ; build msg via 913EA890
913eabac  bl   XuiRenderEnd(dc)
913eabc0  bl   XuiRenderPresent(dc,0,0,0)
913eabc4  li   r3,0                 ; <-- return value hardcoded to 0
```

The last two instructions are the important ones: **hud discards Present's
HRESULT**. The `00000000` this has always logged is that `li r3,0`, not a
successful present. Any claim resting on that return code is void.

## Where the frame dies

The three XUI render exports are thin dispatchers onto the DC's vtable
(`dc->vtable[18]`, `[19]`, `[21]` for Begin/End/Present). The DC's vtable is
`8163E4E8`; Present's implementation is runtime `818F9290`:

```
if ([dc+11C] != 0) return 0x8000FFFF     ; nesting guard
if ([dc+1CC] == 0) twi 31,r0,25          ; assert
if ([dc+134] != 0) return 0              ; S_OK, having presented NOTHING
[dc+1CC]->vtable[24](...)                ; the real present
```

Measured at every frame: `[dc+11C]=0`, `[dc+1CC]=4088A0E0`, **`[dc+134]=1`**.
So Present takes the third branch and returns S_OK without doing anything.
`XuiRenderBegin` behaves the same way - with `[dc+134]` non-zero it skips its
call to `dc->vtable[20]` entirely. The whole XUI frame runs in a null
rendering mode that reports success.

`guide_force_real_present` zeroes `[dc+134]` before each draw to force the
real path.

## What the real path hits

With the field cleared, the draw reaches xam's D3D device and faults:

```
GUEST CRASH: access violation at guest PC 819DE94C, fault_addr 100000024
GUEST CRASH: r31=40883A80
```

`40883A80` is xam's D3D device, the one `xam CreateDevice` returned. The
faulting code is:

```
819de938  lwz  r8,[r31+32A0]
819de944  bne  cr0 -> 819de94c        ; use r8 if non-null
819de948  lwz  r11,[r31+32B0]         ; otherwise this
819de94c  lwz  r9,36(r11)             ; faults, r11 == 0
```

Both fields are zero on the device. **xam's D3D device has no render target
bound.** That is the current blocker, and it is a specific missing
initialisation on a specific object rather than a general "the system boot is
missing".

Note `[dc+1CC]` (`4088A0E0`) and the D3D device (`40883A80`) are *different*
objects; the present chain passes through the former to reach the latter. That
was established by dumping r31 at the fault, after an inference that they were
the same predicted a fault at `0x84` and the observed fault was at `0x24`.

## Tooling correction

`ppcdis.py` and `ppcdisflat.py` were dropping the CR field from `cmpwi`/
`cmplwi` and printing `bc` as two bare numbers. `2f0b0000` is
`cmpwi cr6,r11,0` and `419a0014` is `beq cr6`, but they rendered as `cmpwi
r11,0` and `bc 12,26` - which reads naturally as a cr0 test and inverts the
sense of the branch. Both are fixed to print the CR field and a named
condition. Any branch logic in FINDINGS.md derived from these tools before
this commit should be re-checked against the fixed output.

## Open

- What binds `[dev+32A0]`/`[dev+32B0]` on real hardware, and whether the
  title's existing back buffer can be bound into xam's device instead.
- What sets `[dc+134]` to 1 in the first place. Two writers sit in the XUI
  render range (runtime `818FDF14`, `81901224`); neither has been traced to a
  caller yet. Do not assume clearing it is correct until that is known - it
  may be a legitimate "no device" flag whose real fix is upstream.
