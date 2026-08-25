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

## The render-target binding, located

`device+32A0` is not a single field: it is an array of four render-target
slots, with `device+32B0` the depth-stencil. No `stw` in xam ever targets
`32A0` directly - three sites take its address with `addi rX,r31,0x32A0` and
store through the pointer, which is why a naive store scan finds nothing.

Function `819F4C00` makes the roles unambiguous. It resets the device:

```
r29 = &dev[32A0]; r30 = 0
  if ([r29] != [dev+3F78]) call 819F31A8(dev, r30, 0)   ; per render target
  r29 += 4; r30++; while (r30 < 4)
if ([dev+32B0] != [dev+3F70]) call 819F38C8(dev, 0)     ; depth-stencil
```

Both callees confirm their own signatures rather than being inferred from
that loop:

- **`819F31A8` = `SetRenderTarget(device, index, surface)`**. It asserts
  (`twi 31,r0,25`) when `index > 3`, which is the four-slot bound check, and
  when `surface` is non-null it validates bit 30 of `[surface+0]` and asserts
  if clear. It also asserts unless `[device+3118] == 0`; xam's device has 0
  there, so that guard passes for us.
- **`819F38C8` = `SetDepthStencilSurface(device, surface)`**, the single
  writer of `32B0`.

Neither appears in any vtable in `.rdata` or `.data`, so they are called
statically inside xam - which means they are reachable from the host with a
direct `processor->Execute`, the same way the bootstrap already calls xam
functions.

### What is still missing

A surface object. Binding needs one that xam's D3D will accept, and the
constraint is now known precisely: bit 30 of its first word must be set, and
the present path reads a packed fetch constant from `[surface+24]`
(`rlwinm r10,r9,14,18,31` and `rlwinm r9,r9,29,17,31` pull width and height
out of it).

Two candidate sources, neither tested:

1. `VdSwap`'s second argument is described in Xenia's own signature as the
   "frontbuffer Direct3D 9 texture header fetch" - a real descriptor for the
   buffer already on screen.
2. The title's current render target. Note this cannot be lifted by offset:
   the device diff shows the title's device carries the same value patterns
   at exactly `+80` from xam's (`title+3004..3014` == `xam+3084..3094`,
   `title+3024..302C` == `xam+30A4..30AC`), so dash and xam link different
   D3D builds with different `D3DDevice` layouts. `D3DSurface` may well be
   compatible where `D3DDevice` is not, but that is an assumption, not a
   finding.

### One reading that is NOT established

Sampling `32A0`/`32B0` on the *title's* device also returns zero. That is
consistent with two different stories - that a render target is simply never
bound on xam's device, or that nothing is bound on any device at `VdSwap`
time because it falls between frames - and the differing struct layouts mean
the title sample may not even be reading the same fields. Do not build on
either reading until a surface has actually been bound and the result
observed.

## Why there is nothing for a surface to be bound to

Both of the device's own default surfaces are null:

```
default surface [3F70] = 00000000
default surface [3F78] = 00000000
```

So there is no ready-made surface to borrow, and xam's device has no
ringbuffer either - nothing in the run ever calls `VdInitializeRingBuffer`.
The device was created but never brought up. Binding a render target on its
own could not produce pixels while that is true.

`81A04570` is the only function in xam that calls `VdInitializeRingBuffer`
(also `VdEnableRingBufferRPtrWriteBack` and the GPU-identifier setup). It
never ran. `callers.py` (added in this commit) gives its reverse call graph.

### What CreateDevice actually does

`8178F748`, the function the Guide button calls, opens with the hardware gate:

```
r11 = [[815F048C]]              ; XboxHardwareInfo flags
if ((r11 & 0x200) == 0) { r3 = 0; return; }    ; S_OK, having done nothing
```

That is the `xbox_hardware_info_flags = 0x220` requirement from CONFIG.md,
seen from xam's side. It then calls `819F4D28(0, 2, 0, 0, 0, out)` and, on
success, allocates and stores the device pointer.

### What is established, and what is not

Measured, from `DemandFunction` coverage (7911 functions compiled on call in
that run, so absence is meaningful):

- `8178F748` ran; `819F4D28` ran.
- `819F4A00`, `81A0FE48` and `81A04570` never ran.

Read from the disassembly:

- `819F4A00` is reached from `8178F748` only at `81796A4C`, which is inside
  CreateDevice's error path (after `r26 = 0x8007000E`), and from `819F4D28`
  only at a site guarded by a failed call. It is teardown, not setup.
- `81A0FE48` is called at `819FC08C`, and that site *is* on a success path -
  its non-zero result leads to storing the device and returning 0.
- `81A16968`, called just before at `819FC054`, is straight-line with no
  branches and ends `addi r3,r0,1`: it always returns 1. The test at
  `819FC05C` branches when that is non-zero, straight to "store device,
  return 0" - skipping the bring-up.

**Not established:** that the bring-up is therefore unreachable. The block
containing the `81A0FE48` call has a loop-back at `819FC094` into `819FC060`,
so it has at least one entry that has not been traced, somewhere at or above
`819FC008`. Reading 20-instruction windows is not sufficient to settle this;
it needs a real CFG of `819F4D28`. Do not write "xam can never bring up its
device" until that exists.

### A correction to the previous section

An earlier note here said the path from CreateDevice to the bring-up "exists
but bails somewhere". That was wrong twice over: one of the two edges is a
teardown call on an error path, and a reverse call graph cannot tell a setup
edge from a teardown edge at all. `callers.py` carries that caveat in its
docstring.

## The missing ring buffer is deliberate: xam has two device modes

The previous section left open whether the GPU bring-up was reachable at all.
`cfg.py` (added in this commit) settles it: all 102 instructions of `819F4D28`
are reachable, and the call to `81A0FE48` at `819F4E8C` sits on a normal path.
Reading 20-instruction windows had missed the branch that gets there.

The deciding test is:

```
819F4D34  cmpwi cr6, r29, 2
819F4D38  bne cr6 -> 819F4D6C       ; mode != 2: take the GPU bring-up path
                                     ; mode == 2: set a flag bit, skip it
```

`r29` is `819F4D28`'s second argument, and the function asserts unless it is 1
or 2. Both of xam's device creators funnel into it:

| creator | mode | result |
|---|---|---|
| `8178F748` - what the Guide button calls | 2 | no ring buffer, by design |
| `8178E9F0` | 1 | takes the bring-up: `81A0FE48` -> `81A04570` -> `VdInitializeRingBuffer` |

So **the device the Guide creates is not a failed bring-up. It is a mode-2
device, and mode 2 exists precisely to skip owning the GPU.** The previous
section's "the device was created but never brought up" was the wrong reading
of a deliberate design.

`8178E9F0` has no callers anywhere inside xam, which fits: on hardware the
mode-1 device is created by the system boot, from outside the module.

### Trying mode 1

`guide_create_primary_device` calls `8178E9F0` instead. Mode 1 first calls
`KeGetCurrentProcessType` and asserts unless the matching device global is
still empty - SYSTEM (2) checks `VdGlobalXamDevice`, anything else checks
`VdGlobalDevice`, which the title has already filled in. `guide_system_process_type`
was added to force the former, but it turned out to be unnecessary: the thread
the Guide handler runs on **already reports process type 2**. That check was
never the obstacle, and the cvar is kept only because it makes the property
explicit and testable.

With mode 1, `81A0FE48` and `81A04570` are entered for the first time - the
bring-up genuinely runs. xam also starts enumerating hardware
(`WRN[XAM]: Found Unknown in HD DVD drive`), so mode 1 is a much fuller
initialisation than anything reached before.

It then **hangs**. Two runs, 50s and 100s, both stop at exactly the same place:
the last function entered is `8177C328`, and nothing executes afterwards -
`VdInitializeRingBuffer` is never actually reached. `8177C328` is small (30
instructions, ends before `8177C3A0`) and calls `817815D0` and `81780710`,
both already compiled, so the block is at or below one of those.

This is a hang, not a crash: no exception, no assert. The likely shape is a
wait on something the system boot would satisfy, or a lock the title also
needs - but which of those it is has NOT been determined, and there is no
evidence here to choose between them. Diagnosing it needs guest thread/wait
state at the moment of the hang, which Xenia does not currently dump.

Both cvars default off. With them off the known-good configuration in
CONFIG.md is unchanged.

## Mode 1 reaches the GPU, and the trade it forces

Correcting the previous section: mode 1 does not hang at `8177C328`. That
reasoning was an invalid instrument. `DemandFunction` logs only the FIRST
compilation of a function, so a thread looping in already-compiled code
produces no further lines. `8177C328` was merely the last thing newly
compiled, and it is on a different thread (`F80000E8`) than the Guide's
(`01000028`). For the record, the last function newly compiled on the Guide
thread, `819F3FC8`, is a delay loop - `mtctr 4`, eight `or rX,rX,rX` nops,
`bdnz`, `blr` - which plainly returns.

What mode 1 actually does, with `log_high_frequency_kernel_calls` on so that
`Vd` calls are visible, is reach the GPU:

```
VdGetSystemCommandBuffer #1442 [XAM CREATEDEVICE] from ''
VdSwap(FE03E284, ..., 709DEF48(00000280), 709DEF44(000001E0))
Hardware scaler: width ratio 1:1, height ratio 1:1, final aspect ratio 16:9
VdGetSystemCommandBuffer #1443 [XAM CREATEDEVICE] from ''
VdSwap(FE041FA4, ...)
```

Those tags are trustworthy: `in_xam_createdevice_scope` is `thread_local`, so
they mean thread `01000028` was genuinely inside the CreateDevice call. This
is the first time in this investigation that anything belonging to xam has
reached Xenia's display path.

It is exactly **two** swaps, at 640x480, against 1441 from the title's render
thread in the same run - so it is not a loop, it is an initialisation that
presents two frames and then stops. The complete set of kernel calls that
thread makes is: `VdGetSystemCommandBuffer` twice, `VdGlobalDevice` once,
`VdCallGraphicsNotificationRoutines` once. `VdInitializeRingBuffer` is never
reached, and `CreateDevice` never returns - in a 50s, a 70s and a 100s run.

That is the trade mode 1 forces:

| | mode 2 (`8178F748`) | mode 1 (`8178E9F0`) |
|---|---|---|
| bootstrap completes | yes | no - CreateDevice never returns |
| device can present | no | yes, demonstrably |

Under mode 1 the rest of the Guide bootstrap - render host, DC, scene, draw
hook - never runs, so the Guide has no opportunity to draw. The two frames
presented are xam's own, not the Guide's.

Where it stops has NOT been established. The shape of `819F3FC8` (a spin
delay) and the fact that the two swaps precede the stall are *consistent* with
polling for a swap completion that Xenia never signals for a second device,
but no test here distinguishes that from any other wait. Do not write it down
as the cause.

### A screenshot that proves nothing

A capture taken 20s after the button press under mode 1 shows a fully rendered
"sign in or out" profile screen. It is tempting to read that as the Guide.

It is not. Running the identical harness with the Guide press removed produces
a **byte-identical** file (`2EF6B4B7`). That screen is the dashboard's own
sign-in UI at that point in its boot, and has nothing to do with the Guide.
`work/noguideshot.ps1` is that control; run it before believing any screenshot
in this project.

## Where mode 1 stops: measured

`guide_stall_probe_seconds` samples the guest PC of the thread running the
Guide's device creation, by suspending the host thread, reading RIP, and
resolving it through the same code-cache lookup the crash handler uses. RIPs
are collected first and resolved only after resuming, because `LookupFunction`
takes the code cache lock.

Sixteen samples across two runs all land in the same place:

```
StallProbe[0]: host A080D26E -> guest 819F4004
StallProbe[1]: host A080D268 -> guest 819F3FF8
StallProbe[5]: host A080D20B -> guest 819F3FC8
...
```

`819F3FC8`..`819F4004` is the `bdnz` delay loop. The thread is **spinning, not
blocked** - it makes no kernel calls at all while there. Its only caller is
`819F4488`, a poll routine of the shape `do { delay(); } while (!ready)`.

So the answer to "where does mode 1 stop" is: xam submits two frames through
the system command buffer, then busy-waits for the GPU to acknowledge them,
and the acknowledgement never comes.

Both halves of that acknowledgement are stubbed in Xenia:

- `VdGetSystemCommandBuffer` zeroes 0x94 bytes and returns the constants
  `0xBEEF0000` and `0xBEEF0001`. Marked `kStub`.
- `VdSetSystemCommandBufferGpuIdentifierAddress` has an empty body. Marked
  `kStub`. Its own comment in Xenia reads `// r3 = 0x2B10(d3d?) + 8`.

### What is NOT established

That comment made `[device+2B10]` look like the polled word, and `819F4488`
does read `[[r29+2B10]]` on one of its paths. Measuring killed it: with mode 1
running, `[81D43684] = 40870D00` and `[device+2B10] = 00000000`. A null there
would fault if it were dereferenced, and nothing faults - so either that path
is not the one being taken (there is a flag test at `819F4488+34` on
`[r29+2B3D]` bit 1 that skips the poll body), or `r29` is not this device.

So: the thread spins in a poll, and the two exports that would let a poll of
this kind finish are stubs. **Which** word it polls is unresolved, and the
`2B10` reading is specifically disproven for this run. Pinning it down needs
the guest registers at spin time; the probe currently captures `CONTEXT_CONTROL`
only, so it has RIP and nothing else.
