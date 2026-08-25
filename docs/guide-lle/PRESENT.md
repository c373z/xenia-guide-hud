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
  when `surface` is non-null it tests bit 30 of `[surface+0]`. **See the
  correction at the end of this file: the assert fires when that bit is SET,
  not clear.** It also asserts unless `[device+3118] == 0`; xam's device has 0
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

## What the Xbox button does, end to end

Retracting the previous section's retraction. It said the `2B10` reading was
"specifically disproven". That was wrong, and wrong for an avoidable reason:
the probe read `[81D43684]`, the device global, when the poll loop holds its
object in `r29`. They are different objects. Reading the field off the wrong
one produced `[2B10]=00000000` and I treated a measurement error as a
disproof.

Extending the probe to capture guest registers - the x64 backend keeps the
`PPCContext` pointer in `rsi`, so the GPRs can be read straight out of host
memory while the thread is held - gives the real values:

```
StallProbe: poll arg r31=709DEC70  [r31+8]=00000003
StallProbe: device=40883A80  [2B10]=FE474000
StallProbe: polled word [FE474000] = 00000003 then 00000003 (UNCHANGED)
```

`[dev+2B10] + 8` is `FE474008`, which is exactly the third argument Xenia logs
in `VdSwap(FE03E284, 709DEF60, FE474008, ...)` - the system writeback pointer.
Xenia's own stub comment, `// r3 = 0x2B10(d3d?) + 8`, was correct all along.

`819F4488` is a timeout-guarded wait: it reads a tick, tracks when the polled
word last changed, and once the elapsed time passes `[81D31D60]` it prints two
strings. They are xam's own words:

```
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
A deadlock has occurred in InsertAsyncCommandBufferCall,
because 64 Async Command Buffer Call objects have been inserted,
```

So the whole sequence, measured rather than argued:

1. The button dispatches to hud, which is loaded and creates its Guide object.
2. Under mode 1 the device creator `8178E9F0` asks `819F4D28` for a mode-1
   device, which takes the GPU bring-up branch.
3. xam acquires the system command buffer and presents two 640x480 frames -
   real `VdSwap` calls that Xenia's hardware scaler processes.
4. xam then waits for the GPU to acknowledge that work, by polling the system
   writeback word at `FE474000`.
5. That word holds 3 and never advances. Nothing consumes xam's command
   buffer: `VdGetSystemCommandBuffer` is a stub returning `0xBEEF0000` /
   `0xBEEF0001`, and `VdSetSystemCommandBufferGpuIdentifierAddress` - the call
   by which the guest registers exactly this writeback address - has an empty
   body. Both are `kStub`.
6. So xam spins in its async-command-buffer wait, `CreateDevice` never
   returns, and the rest of the Guide bootstrap never runs.

That is a specific, named gap in Xenia, reached by the guest's own diagnostic
text rather than by inference. Implementing those two exports so that the
writeback word advances as the system command buffer is consumed is the next
piece of work, and it is a Xenia change, not a Guide one.

## The obvious fix does not work

The previous section ended by naming the next piece of work: make the system
writeback word advance. That has now been tried, and it does not release the
wait.

`guide_fake_gpu_writeback` locates the polled word through the stall probe and
then advances it from the host, re-sampling the guest PC after each write:

```
StallProbe: polled word [FE474000] = 00000003 then 00000003 (UNCHANGED)
FakeWriteback[0]:  wrote 00000004, guest PC 819F4000 (still spinning)
FakeWriteback[6]:  wrote 0000000A, guest PC 819F3FF0 (still spinning)
FakeWriteback[12]: wrote 00000010, guest PC 819F4000 (still spinning)
FakeWriteback[18]: wrote 00000016, guest PC 819F3FF0 (still spinning)
```

24 writes over 2.4s, and the thread never leaves `819F3FC8..819F4004`.

Re-reading `819F4488` with that result in hand, the word is not the exit
condition at all. It is a *change* tracker feeding the timeout heuristic:
`[r31+8]` holds the last-seen value and `[r31+12]` when it last moved, and if
nothing moves for `[81D31D60]` the InsertAsyncCommandBufferCall deadlock
message prints. The only path that returns 0 - done - is the flag test at the
top of the function, on bit 1 of `[device+2B3D]`.

So a working writeback would silence the deadlock diagnostic without ending
the wait. Anyone implementing `VdSetSystemCommandBufferGpuIdentifierAddress`
expecting that to unblock the Guide should read this first.

### Where the completion actually comes from is still open

Exactly one instruction in xam sets that bit - `81A03E68`, inside `81A03D40` -
found by scanning for a `stb` to `2B3D` preceded by `ori rX,rY,2`. That sounds
decisive and is not: `81A03D40` has 19 callers across roughly 150 call sites
and is plainly a shared state helper, not a GPU completion handler. Naming it
does not name the path.

Also measured: no `VdSetGraphicsInterruptCallback` call occurs anywhere in the
mode-1 run, by xam or by the title. Whatever sets the bit in a real system is
not reached through an interrupt callback that xam has registered by this
point.

## Where [dc+134] comes from, and a correction to the surface check

### The null-render flag is inherited, not decided

The DC initialiser is `818FDE98`, called with the DC in r3 and a creation
parameter object in r4. It fills in the fields the present path later reads:

```
818FDE98+2C  stw r27,[dc+1C8]        ; the parameter object
818FDE98+30  lwz r3,[r27+8]
818FDE98+34  stw r3,[dc+1CC]         ; -> 4088A0E0, what Present dispatches on
...
818FDE98+58  lwz r11,[r27+1C]
818FDE98+7C  stw r11,[dc+134]        ; <-- the null-render flag
```

So `[dc+134]` is not a decision the DC makes. It is copied verbatim from
`[param+1C]`, whichever creation parameters the caller supplied. Our bootstrap
calls `XuiRenderCreateDC` with only an out-pointer and xam builds the
parameter block internally from the XUI render context at `81D6C978`; the DC
the draw actually uses is a different one that hud creates. So the value comes
from hud's parameters, by way of xam's context, and zeroing it after the fact
- which is what `guide_force_real_present` does - is treating the symptom.

### The surface type check is the other way round

An earlier section here said `SetRenderTarget` "validates bit 30 of
`[surface+0]` and asserts if clear". That is backwards. The code is:

```
819FA3E4  lwz     r11,0(r22)
819FA3E8  rlwinm. r11,r11,0,1,1     ; isolate bit 30; Rc=1, so CR0 is set
819FA3EC  beq     cr0,+8            ; skip the trap when the bit is ZERO
819FA3F0  twi     31,r0,25
```

The trap fires when the bit is **set**. A surface xam will accept has bit 30
**clear**.

The practical consequence is that the bit is worthless as a search filter -
nearly anything passes it. A sweep of the device wrapper and the D3D device
for "valid surfaces" on the inverted test returned twelve hits, including
`819E9750`, whose first word is `7D8802A6` - the `mfspr r12,8` of a function
prologue. Any surface has to be recognised by the packed fetch constant at
`+0x24`, not by the type bit.

## The two modes need each other

### Clearing the flag at its source changes nothing

`[dc+134]` is inherited from `[param+1C]`, and the parameter object turns out
to be the XUI render context itself - the singleton at `81D6C978`. Measured:

```
pre-draw: dc=40899D40 [134]=00000001 dev=4088A0E0
          param=[1C8]=4088A0A0 [param+1C]=00000001 [param+8]=4088A0E0
```

`4088A0A0` is the same value the bootstrap already logs as "XUI ctx". So the
flag can be cleared at the source, before hud builds its DC, rather than
patched per-frame afterwards. `guide_clear_null_render` does that, and it
works mechanically - the DC is then constructed with `[134]=00000000` and the
scene still creates:

```
XUI ctx 4088A0A0 [1C] 00000001 -> 00000000 (null-render flag cleared at source)
scene creator 913EB940 -> 00000000, scene=00010000
pre-draw: dc=40899D40 [134]=00000000 ... [param+1C]=00000000
```

The reason for doing it at the source was that `XuiRenderBegin` skips its call
to `dc->vtable[20]` when the flag is set, and a DC built non-null would run
that call - which looked like where render-target setup would happen. It is
not. The frame faults at `819DE94C` with `fault_addr 0x24`, byte for byte the
same crash as clearing `[dc+134]` per frame. That expectation is disproven.

### Nothing on the mode-2 path ever binds a render target

Counting `DemandFunction` entries for `SetRenderTarget` (`819F31A8`) and all
eleven of its callers across a mode-2 session: **every one is zero**.
`SetRenderTarget` is never so much as compiled. So the Guide's frame is not
failing to bind a target, it never attempts to.

The same count over the mode-1 run:

| function | mode 2 | mode 1 |
|---|---|---|
| `819F31A8` SetRenderTarget | 0 | 1 |
| `819FCE78` (caller) | 0 | 1 |
| `81A0FA80` (caller) | 0 | 1 |

So render-target setup belongs to the mode-1 side - the device that owns the
GPU - and the Guide's drawing belongs to the mode-2 side, which is the only
side whose bootstrap completes.

That is the shape of the problem, stated from both ends and measured on both:

- **mode 1** brings the GPU up, binds render targets, and presents two frames,
  then blocks forever in the async command buffer wait, because the system
  command buffer is stubbed.
- **mode 2** completes the Guide bootstrap, creates the scene, and runs a full
  XUI frame, but never binds a render target, so the present dereferences null.

Neither is a bug in the other. They are two halves of one system that, on
hardware, is brought up by the boot code Xenia does not have. Making the Guide
draw means supplying that: a device that owns the GPU *and* a bootstrap that
returns.

## Running both modes at once

### An ordering bug hid the whole experiment

Under `guide_create_primary_device` the Guide bootstrap was never running at
all, for a dull reason: the mode-1 creator never returns, and
`QueueGuideBootstrap` is called *after* it in the button handler. Everything
below the device call was dead code in that configuration.

`guide_bootstrap_before_device` queues it first. With that, mode 1 and the
bootstrap both run:

```
Guide button: queueing bootstrap BEFORE device creation
Guide button: calling device creator 8178E9F0
GuideBootstrap: render host -> 00000000, XUI ctx 4088B760, provider 81D22A54
GuideBootstrap: scene creator 913EB940 -> 00000000, scene=00010000
Guide composite draw #1 -> 00000000; draw dc=4089B400 ... [1CC]=4088B7A0
```

The bootstrap is consumed inside the `VdSwap` that mode 1's own initialisation
performs, so it runs on the button thread rather than the title thread.

Adding `guide_clear_null_render` on top makes the present take the real path,
and the crash register dump shows `r31=40870D00` - the mode-1, GPU-owning
device. So with all three, the Guide's frame does reach the right device. It
still faults at `819DE94C` on a null render target.

### SetRenderTarget really does bind, measured

A call count could not settle whether mode 1 binds or unbinds, since the reset
loop `819F4C00` calls `SetRenderTarget(dev, i, 0)`. `guide_trace_setrendertarget`
puts a breakpoint on `819F31A8` and reads the arguments:

```
SetRenderTarget #1: dev=40883A80 index=0 surface=4088B3E0 lr=81A0FD2C  <- BIND
```

A real surface, index 0, from `81A0FA80`. So mode 1 genuinely binds a render
target, and the earlier claim - made from a call count - happens to hold.

### What this does NOT show

The bind above is `dev=40883A80`; the present in the combined run dispatched to
`40870D00`. That looks like the Guide presenting to a different device than the
one that got the target, which would be exactly the missing link.

It is not evidence. Those two numbers come from **different runs**, and guest
heap addresses are not stable between runs - the same object has appeared at
`40883A80` in one session and `40870D00` in another. Nothing here compares them
within a single session.

Confirming or killing that idea needs one run that logs both the bind and the
faulting `r31`. The breakpoint perturbs execution enough that the run which
produced the bind never reached a `VdSwap`, so the bootstrap was never consumed
and no draw happened - the two events have not yet been observed together.

## Two xam devices, and only one has a render target

The previous section flagged a cross-run address comparison as inadmissible.
Here is the same question answered inside one run, by logging every device
object in play with its render-target slots at pre-draw time:

```
device xam dev [81D43684]           = 40870D00  RT0=00000000 RT1=00000000 depth=00000000
device VdGlobalXamDevice [801E6FC8] = 40883A80  RT0=4088D130 RT1=00000000 depth=00000000
device VdGlobalDevice [801E6FC4]    = 40952400  RT0=00000000 RT1=00000000 depth=00000000
device dc wrapper [dc+1CC]          = 4088A000  RT0=00000000 RT1=00000000 depth=00000000
GUEST CRASH: ... r31=40870D00
```

Under mode 1 there are **two** xam devices. `801E6FC8` holds the one with a
real surface bound to RT0. `81D43684` - the global the DC is built from, and
the object the faulting present used, `r31=40870D00` - has nothing bound. The
Guide was presenting to the wrong device. The suspicion from the previous
section was right, and this is the evidence it lacked.

### Redirecting the global moves the crash

`guide_use_bound_device` points `81D43684` at whatever `801E6FC8` holds, before
the bootstrap builds the DC:

```
GuideBootstrap: xam device global 40870D00 (RT0=00000000) -> 40883A80 (RT0=4088B3E0)
GuideBootstrap: scene creator 913EB940 -> 00000000, scene=00010000
device xam dev [81D43684] = 40883A80  RT0=4088B3E0
```

The frame then gets **past** the null render target. The fault moves from
`819DE94C` / `fault_addr 0x24` - where it had sat for this entire
investigation - to `819F5EC4` / `fault_addr 0x20`, inside `819F5D18`.

### All 32 GPRs on a crash

The new fault is `lwz r11,32(r14)` with `r14` null, and the dump only covered
r27-r31 - so the one register that mattered was the one missing. The crash
reporter now prints every GPR, which immediately showed:

```
r8 -r15  00000000 00000000 00000001 4088B3E0 03FF1C88 301BC000 00000000 00000000
r24-r31  00000000 00000000 00000000 00000000 00000000 00000000 00000000 40883A80
```

`r11 = 4088B3E0` is the bound surface, loaded successfully out of
`device[idx]` two instructions earlier and passing its non-null assert - so the
render target is now genuinely reaching the code that wants it. `r14`, and
every register from r15 to r30, is zero, and `lr` is only `+0x54` into a
function that runs to `+0x2208`; `r14` was simply never assigned on this path.

This is forward progress, not a fix. The Guide still does not draw.

## Chasing the new fault, and two instrument limits

The fault after the device redirect is `lwz r11,32(r14)` with `r14` null. Only
three instructions in `819F5D18` write r14, and the one that matters is the
third instruction of the function:

```
819F5D50  or r14,r8,r8
```

So `r14` is the function's sixth argument, and it arrived null. The log
ordering places the fault firmly inside the Guide's draw, not inside device
creation - `pre-draw` is emitted immediately before the draw is invoked:

```
GuideBootstrap: draw hook installed on title thread
Guide pre-draw: dc=4089B400 [134]=00000000 ...
GUEST CRASH: access violation at guest PC 819F5EC4 ... fault_addr 0x20
```

### Breakpoints cannot observe this path

`819F7F20` looked like the answer - `callers.py` reported it as the only caller
of `819F5D18`, and it passes `or r8,r31,r31`, its own fourth argument. A
breakpoint there would have confirmed it.

It cannot. Installing any breakpoint changes scheduling enough that mode 1's
`VdSwap` never happens, so the queued bootstrap is never consumed, no draw runs
and no crash occurs. Two runs confirmed this: the breakpoint installs, reports
`SetRenderTarget ... BIND`, and then the session goes into the mode-1 spin
having never drawn. Argument capture and the crash path are mutually exclusive
observations here.

### A backtrace that does not perturb

The crash reporter now scans the guest stack for words in xam's `.text` range.
On this fault:

```
stack code refs: 819F2CB0(+8) 81D70000(+44) 81957740(+48) 81A020DC(+A8)
                 81A03588(+D8) 819541C0(+E8)
```

Resolved, outermost first: `81954190` -> `81A03168` -> `81A02058` ->
`81957598` -> `819F2A40` -> the crashing function. (`81D70000` is a data
address that fell inside the range - the scan is a heuristic, not a real
unwinder.)

### Which corrects the caller analysis

`819F7F20` does not appear in that chain. So the call into `819F5D18` on this
path is **indirect**, and the "single caller" result was an artifact of
`callers.py` following only direct `bl` - a limitation its own docstring
states and that I did not apply when reading its output. Everything derived
from that reading - that the null originates as `819F7F20`'s fourth argument -
is unsupported. What survives is narrower: the sixth argument to `819F5D18` is
null, and the real caller is reached through a function pointer from somewhere
in the chain above.

## What the null sixth argument is not, and what it looks like

### It is not the depth-stencil surface

The reading was: the faulting `lwz r11,32(r14)` takes the low 6 bits of
`[r14+20]`, a caller compares that same field against `0x3D`, and `0x3D` in the
low bits of a surface word reads like a format enum. The device has a colour
surface on RT0 and nothing on depth in every run, and `SetDepthStencilSurface`
(`819F38C8`) is never called in any run. So `r14` looked like the depth
surface.

`guide_bind_depth_copy` tests it. Before the first draw it clones the bound
colour surface - a structurally valid object, rather than a fabricated one -
and binds the clone:

```
Guide: SetDepthStencilSurface(dev 40883A80, clone 301C1000 of RT0 4088B3E0)
       -> 40883A80; depth now 301C1000
GUEST CRASH: access violation at guest PC 819F5EC4 ... fault_addr 0x20
```

The bind succeeds and the device now has a depth surface. The crash is
byte-identical - same PC, same fault address. **`r14` is not the depth
surface.** The reading is disproven, not merely unconfirmed.

### What r14 does look like

Every access through `r14` in `819F5D18` is a read, at exactly six consecutive
word offsets and nowhere else:

```
+0x01C  lwz  x1
+0x020  lwz  x10
+0x024  lwz  x3
+0x028  lwz  x3
+0x02C  lwz  x1
+0x030  lwz  x6
```

`0x1C` through `0x30` inclusive is 24 bytes - six dwords, the exact footprint
of a Xenos fetch constant. So `r14` points at a texture or surface descriptor
with its fetch constant at `+0x1C`, which is consistent with the earlier fault
at `819DE94C` reading width and height out of `[surface+0x24]`.

What that descriptor *is* - a source texture for the composite, some other
bound resource - is not established, and the depth guess is a warning against
picking the nearest plausible candidate. What is established: it is a
surface-shaped object, it is the sixth argument, it arrives null, and the
device having both colour and depth bound does not supply it.

## The Guide is genuinely presenting

### Retracting the retraction

The previous section withdrew the `819F7F20` caller analysis because that
function did not appear in the stack scan. That withdrawal was wrong, and for a
measurable reason: `819F5D18`'s prologue is `stwu r1,-0x1A0(r1)`, a 416-byte
frame, so its caller's return address sits at `[r1+0x198]` - word 102 - and the
scan covered 96 words. It missed by eight words. Widened, the address is
exactly where the ABI says it should be:

```
stack code refs: ... 819F7FB4(+198) ...
```

`819F7F20` is the direct caller after all. Absence from a truncated window is
not absence.

### A real unwinder

The scan was the wrong instrument anyway - it cannot distinguish a live frame
from a stale word left by an earlier, deeper call, and it had duly produced
`XuiRenderEnd` frames that are not on the live chain. These prologues save LR
with `stw r12,-8(r1)` before the `stwu`, and PPC keeps a back chain at `[sp]`,
so a frame's return address is exactly `[caller_sp - 8]`. The crash reporter
now walks that:

```
GUEST CRASH: unwind (back chain): 819F7FB4 819FEC68 8191B438 818F930C 818FB020 913EABC4
```

Six frames, resolved innermost to outermost:

| return address | function | what it is |
|---|---|---|
| `819F7FB4` | `819F7F20` | direct caller of the faulting function |
| `819FEC68` | `819FEB78` | |
| `8191B438` | `8191B418` | |
| `818F930C` | `818F9290` | the DC's `vtable[21]` - Present's implementation |
| `818FB020` | `818FAFC8` | `XuiRenderPresent`, the export |
| `913EABC4` | hud's draw | the instruction after `bl XuiRenderPresent` |

### What that establishes

The outermost frame is hud, returning from `XuiRenderPresent`. So the Guide's
per-frame draw is now running `XuiRenderPresent` for real, dispatching through
`vtable[21]` exactly as the static reading predicted, and reaching six frames
into xam's actual presentation code before it dies.

That is the whole chain of this investigation validated from the inside:
Present no longer short-circuits to S_OK (`guide_clear_null_render`), it no
longer faults on a missing render target (`guide_use_bound_device`), and it is
now executing real D3D work. What stops it is one null resource descriptor -
six dwords of fetch constant at `+0x1C` - passed as the sixth argument.

## The null resource is the front buffer

Following the verified unwind down, the null sixth argument comes from one
load:

```
819FEC24  lwz r6,0x3F74(r31)      ; r31 is the device (it calls the same
                                  ; (device,1) helper SetRenderTarget does)
```

Two functions write `[device+3F74]`, and one of them, `81A0FA80`, is the same
function whose `SetRenderTarget` call binds RT0. Its store is preceded by:

```
81A16EB0  bl 819E7310             ; allocate
81A16EB4  cmplwi cr0,r3,0
81A16EB8  bne  -> 81A16ED4        ; success: store it
81A16EC0  addi r3,r11,13264       ; -> "Couldn't allocate front buffer.\n"
81A16EC4  bl 81A0C2A8
81A16ED0  b    -> 81A17040        ; return 0
81A16ED4  stw r3,0x3F74(r29)
```

xam names it itself: **`[device+3F74]` is the front buffer**. The Guide's
present dereferences it six frames down and faults because it is null.

### The device is the right one

`8191B418` does `lwz r3,12(r31)` and passes that to `819FEB78`, so the present
path takes its device from `[wrapper+12]`, not from the `81D43684` global that
`guide_use_bound_device` rewrites. Measured, they agree:

```
device xam dev [81D43684]        = 40883A80  RT0=4088B3E0 ... [3F74]=00000000
device PRESENT PATH [wrapper+12] = 40883A80  RT0=4088B3E0 ... [3F74]=00000000
```

So the redirect is correct and the wrapper points at the device that has a
render target. That device simply has no front buffer.

### A mid-analysis inference of mine that was wrong

I reasoned: the failure path returns before the `SetRenderTarget` call, the
bind is observed, therefore the store must have executed and `r29` must be some
third object. A breakpoint on the store address settles it - **it never fires**,
while `SetRenderTarget` from `lr=81A0FD2C` does. The bind is reachable without
the store, so "bind happened therefore store happened" was invalid.

What is measured, and what is not:

- No run prints `Couldn't allocate front buffer`, so the failure path is not
  taken either.
- The allocator `819E7310` *is* entered in the run where the draw happens
  (`DemandFunction` count 1), and not entered in breakpoint runs.
- `[device+3F74]` is null on every device object visible: both xam devices,
  the displaced pre-redirect one, and the present path's own.

Those do not yet form a consistent story, and I am not going to invent one to
join them. The obstacle is methodological: the store can only be observed with
a breakpoint, and any breakpoint stops the draw path from being taken at all.

## Past the last crash, and still no pixels

### The front buffer is never written, confirmed without a breakpoint

`guide_watch_front_buffer` polls `[device+3F74]` from a host thread every
500us for both xam device globals. Across a full session it logs both devices
appearing and **no front-buffer transition at all**. The field is null the
whole time, not written-then-cleared. That settles what a code breakpoint
could not.

### The redirect was racy

The same watch exposed something else. In one run the bootstrap logged
`device global unchanged (40870D00, bound=00000000)` - `VdGlobalXamDevice` was
still 0 at that moment - and that run faulted at the *old* site, `819DE94C`,
while the watch later saw the bound device appear. So doing the redirect once,
at bootstrap, wins or loses a race, and **some of the run-to-run differences
earlier in this file are that race rather than the cvars**. Merely adding a
host polling thread was enough to flip it.

It is now done per frame against the pointer the present path actually reads,
`[wrapper+12]`, which is race-free.

### Supplying a front buffer removes the crash entirely

`guide_fake_front_buffer` clones the bound colour surface into `+3F74`:

```
Guide: front buffer [dev 40883A80 +3F74] = clone 301C5000 of RT0 4088B570
```

Result: **zero guest crashes**. The fault that has ended every run since the
device redirect is gone. The title thread then executes 315 further log lines,
compiling xam D3D functions it had never reached before (`81A06148`,
`81A05F48`), and does not return from the draw inside the capture window.

So the Guide's present now runs without faulting, several layers deeper than
anything reached before.

### And the screen does not change

A capture 22s after the button press is `2EF6B4B7` - byte for byte the same
file as the no-press control from `work/noguideshot.ps1`. No pixels.

That is consistent with what the async-command-buffer wait already said many
sections ago: xam's device submits through the system command buffer, and
`VdGetSystemCommandBuffer` is a stub that hands back `0xBEEF0000` and
`0xBEEF0001`. Whatever the Guide is now drawing goes into a buffer nothing
executes. Removing the null dereferences got the software to run; it did not
connect it to the GPU, and no amount of further work on this side will.

## What VdGetSystemCommandBuffer has to return

The system command buffer is the terminal blocker: the Guide's present now
runs without faulting and submits, and nothing executes what it submits. Xenia
stubs the export - zero `0x94` bytes, write `0xBEEF0000` to p0 and
`0xBEEF0001` to p1. Anyone implementing it needs the guest's expectations, so
here is what the only caller does with the result.

Exactly one function in xam calls it: `819FE138`, at `819FE600`, as
`VdGetSystemCommandBuffer(p0 = r1+304, p1 = r1+156)`. Immediately after:

```
81A05804  lwz    r11,[r31+59A8]
81A0580C  beq    -> 81A05858          ; branch on [device+59A8]
          ; --- taken when [device+59A8] is non-zero ---
81A05810  lwz    r11,[r1+156]         ; the p1 value
81A05818  lwz    r10,[r31+2B10]       ; the writeback block (FE474000)
81A0581C  stw    r11,8(r10)           ; [writeback+8] = p1
81A05820  bl     81A0B3F0             ; result -> [r1+128]
81A0583C  bl     81A088E0  (r3=device, r4=&[r1+128])
81A0584C  bl     81A08990  (r3=device, r4=&[r1+128], r5=0)
          ; --- otherwise ---
81A0585C  lwz    r10,[r1+352]         ; p0 + 0x30
81A05864  cmplwi r10,0x500
81A05870  lwz    r11,[r1+356]         ; p0 + 0x34
81A05874  cmplwi r11,0x5BE
```

So the contract has two halves:

- **p1 is a GPU identifier value.** The guest stores it at `[device+2B10] + 8`
  - which is `FE474008`, precisely the "system writeback ptr" argument Xenia
  already logs in `VdSwap`, and precisely the address Xenia's own stub comment
  for `VdSetSystemCommandBufferGpuIdentifierAddress` names as `0x2B10(d3d?) + 8`.
  Three independent things agree on that address.
- **p0 is a 0x94-byte descriptor**, not a scalar. The guest reads `+0x30` and
  `+0x34` and compares them against `0x500` and `0x5BE`. Xenia zeroes the whole
  block, so both comparisons fail and the guest takes a path built for a
  descriptor it did not get.

What `0x500` and `0x5BE` mean is **not** established - they are the right
magnitude for Xenos register indices, but that is a guess and is written here
as one. The rest of the `0x94` bytes are likewise unmapped; only `+0x30` and
`+0x34` have observed readers.

This is where the work stops being about the Guide. Everything from the button
press to the draw is now understood and runs; what remains is a GPU-side
feature of Xenia that no title has ever needed, because only system software
uses this path.

## The present completes

Filling the two descriptor fields the guest actually reads - `p0+0x30 = 0x500`
and `p0+0x34 = 0x5BE`, behind `guide_syscmdbuf_fields` - changes the outcome:

```
VdGetSystemCommandBuffer: filled p0+30=0x500 p0+34=0x5BE
Guide: front buffer [dev 40883A80 +3F74] = clone 301C1000 of RT0 4088B3E0
Guide composite draw #1 -> 00000000; draw dc=4089B400 [11C]=00000000
                           [134]=00000000 [1CC]=4088B7A0
```

`[134]=00000000` means Present did not take the S_OK short-circuit, and the
draw **returned**. That is the first time the composite draw has completed on
the real present path - every previous run either faulted or never came back.
Zero guest crashes.

So the whole guest-side sequence now runs to completion: button press, handler,
Guide object, xam device, render host, DC, scene, XUI frame, and a present that
returns.

### It still does not draw anything

- Exactly one draw completes. The title thread stops afterwards, so `VdSwap`
  never comes round again and there is no draw #2.
- A capture 18s after the press is `2EF6B4B7`, byte for byte the no-press
  control.

Which is what the rest of this file predicts. Completing the present means the
guest wrote its commands and was satisfied with the answers it got back; it
does not mean anything executed those commands. `VdGetSystemCommandBuffer`
still hands out a zeroed descriptor with two plausible-looking fields, and
Xenia's command processor never sees xam's buffer.

The two values are worth being careful about. `0x500` and `0x5BE` were taken
from the guest's own comparisons, so satisfying them is not a guess - but
*why* those values, and what the other `0x8C` bytes of the descriptor mean, is
still unknown. A descriptor that passes two checks is not a command buffer.

## The descriptor, field by field

Scanning `819FE138` for every access to the `p0` block (`r1+304`) and to `p1`
(`r1+156`) gives the whole observed contract, not just the two compared fields:

| field | accesses | what is known |
|---|---|---|
| `p0+0x00` | **none** | no reader at all - Xenia's `0xBEEF0000` there is read by nobody |
| `p0+0x04` | 1 read | |
| `p0+0x08` | 3 reads | compared against 0, drives a three-way branch |
| `p0+0x1C` | 1 read | |
| `p0+0x30` | 1 read | compared against `0x500` |
| `p0+0x34` | 1 read | compared against `0x5BE` |
| `p0+0x90` | 2 reads | on a path that asserts if it is zero |
| `p1` | 4 reads | stored to `[device+2B10]+8`, the writeback |

Two things worth taking from that table.

The magic Xenia writes at `p0+0x00` has no consumer in the only function that
calls this export. Whatever it was chosen to satisfy, it is not this.

And at `81A05A88`, immediately after the `p0+0x90` check, the guest copies
**56 bytes from `p0+0x20`** with a memcpy. `0x30` and `0x34` sit inside that
block, so they are two fields of a 56-byte structure the guest lifts out
wholesale - which is why satisfying them individually is not the same as
providing the structure.

### The p0+0x90 assert never fires

It reads as a hard requirement - `twi 31,r0,25` when the field is zero, and
Xenia zeroes it. It is not one, at least not on our path. With
`log_guest_asserts` on, a full run logs **zero** guest asserts. The branch
carrying that assert is not taken, and the guest is entirely satisfied with
what it gets: no traps, no crashes, and a present that returns.

That is worth stating plainly because it cuts against the obvious story. The
guest is not limping past a series of checks it should have failed. On the path
it actually takes, everything it inspects is acceptable to it. What is missing
is not validation the guest performs - it is execution that Xenia does not do.
