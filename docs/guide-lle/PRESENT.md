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

## How the system command buffer would be implemented

The loop is closed by an argument correspondence that is easy to miss. In one
run:

```
VdGetSystemCommandBuffer #1 from '' p0=7042FA40 p1=7042F9E4
VdSwap(BF9D06E8, 7042F9F0, FFBE2008, 7042FA40, BEEF0001, ...)
                                     ^^^^^^^^  ^^^^^^^^
```

`VdSwap`'s fourth argument **is** the descriptor `VdGetSystemCommandBuffer`
filled, and its fifth is the `p1` identifier value. Xenia's own signature
comments already say as much - "buffer from VdGetSystemCommandBuffer",
"from VdGetSystemCommandBuffer (0xBEEF0001)" - and the code then ignores both.

So the mechanism is: the kernel hands the guest a command buffer, the guest
writes into it, and the guest hands it back at swap time for the GPU to
execute. Xenia stubs the first half and discards the second.

The primitive for the missing half already exists:

```
void ExecuteIndirectBuffer(uint32_t ptr, uint32_t count)   // pm4_command_processor_declare.h
```

That executes an arbitrary PM4 buffer, which is exactly what a system command
buffer is. So an implementation is:

1. `VdGetSystemCommandBuffer` allocates (once) a guest-visible PM4 buffer and
   describes it in the `0x94`-byte block, rather than zeroing it. Which fields
   carry the pointer and size is the open question - `p0+0x04` is stored by the
   guest into `[device+0x60C0]`, and `p0+0x08` gates roughly `0xE0` bytes of
   processing that is skipped entirely while it is zero, so those two are the
   candidates.
2. `VdSwap` executes the buffer named by its fourth argument with
   `ExecuteIndirectBuffer`.
3. The identifier written to `[device+2B10]+8` advances as the buffer is
   consumed, which is what the `InsertAsyncCommandBufferCall` wait in mode 1 is
   watching.

Two cautions for whoever does it. This path is not Guide-specific - the title's
own `VdSwap` carries the same pair (`7042FA40` / `BEEF0001`), so changing these
exports affects every title, and the stub's current constants are load-bearing
in the sense that everything today is built around them being inert. And
`p0+0x08` should not be filled with a guessed value: unlike `0x500` and
`0x5BE`, which were read out of the guest's own comparisons, nothing observed
says what belongs there.

## What 0x500 and 0x5BE actually are

Earlier this file recorded, as an explicit guess, that `0x500` and `0x5BE` were
"the right magnitude for Xenos register indices". That guess was wrong, and
there is now evidence.

Three functions in xam reference both constants: `819FE138`, the caller of
`VdGetSystemCommandBuffer`; `81A0F3B0`; and `81A14110`. In `81A0F3B0` they are
not compared but **loaded as arguments**:

```
81A0F46C  li  r4,0x5BE
81A0F470  li  r3,0x500
81A0F474  li  r7,0
81A0F478  li  r6,2
81A0F47C  bl  81D1009C          -> VdSetDisplayModeOverride
```

Xenia's own declaration is
`VdSetDisplayModeOverride(width, height, refresh_rate, unk3, unk4)`, so
`0x500` = 1280 and `0x5BE` = 1470 are a **display mode**, not register
indices. (Xenia's export is itself a stub, so the parameter names are its
reading rather than documented fact - but a width of 1280 is hard to argue
with.)

Which reinterprets the descriptor. `819FE138` compares `p0+0x30` against 1280
and `p0+0x34` against 1470, so those two fields hold the display width and
height, and the 56-byte block at `p0+0x20` that the guest memcpys out is a
**video mode structure**, not command-buffer plumbing.

That makes the earlier probe make more sense than it did at the time. Filling
those two fields let the present complete because the guest could finally match
the mode it was looking for - not because anything about command submission had
been satisfied. It also means the two fields say nothing about where a command
buffer lives, so they are no help with `p0+0x04` and `p0+0x08`, which remain
the open question.

## The descriptor is opaque to titles

dash.xex is an independent user of this export, and it maps cleanly: its image
base is `92000000` and, unlike xam, its runtime addresses are the PE addresses
with **no shift** (derived from its entry point, `92196660`, matching the
header exactly).

It calls `VdGetSystemCommandBuffer` exactly once, at `922810C0` in function
`92281058`, with `p0 = r1+416`. Scanning that whole function for reads of the
`0x94`-byte block finds **none**. What it does instead is:

```
922810C0  bl   -> 9293BA64          ; VdGetSystemCommandBuffer
922810C4  addi r9,r1,416            ; the descriptor
922810D0  stw  r9,188(r1)
922810E8  stw  r9,164(r1)
9228110C  stw  r9,140(r1)
```

It stashes the pointer into argument slots and hands it on - which is why
`VdSwap`'s fourth argument is that same address. **The title treats the
descriptor as an opaque handle.** Only xam, the system software, reads fields
out of it.

### Which simplifies the implementation

The plan earlier in this file said the open question was "which fields carry
the pointer and size". That framing is wrong, or at least unnecessary. Since
titles never inspect the block, the command buffer's address and length **do
not have to be in it at all**. Xenia can allocate the buffer, remember it
host-side, hand back a descriptor as a key, and execute the buffer when
`VdSwap` presents that key back:

```
VdGetSystemCommandBuffer(p0, p1)  ->  fill p0, record {p0 -> buffer}
VdSwap(..., p0, p1, ...)          ->  ExecuteIndirectBuffer(buffer, used)
```

What still has to be right is the subset of fields xam reads, because xam is
not a title: `+0x04`, `+0x08`, `+0x1C`, `+0x30` and `+0x34` (display width and
height, now known), and `+0x90`. Those five plus the mode pair are the real
specification, and three of them still have no observed meaning.

That is a smaller and better-shaped problem than "reverse a 0x94-byte kernel
structure".

## p0+0x04 and p0+0x08 are not a buffer pointer and size

The opacity finding above left a gap: dash never reads the descriptor, but xam
must learn where to write its commands from *somewhere*, and the two fields
with unexplained readers are the obvious place. `p0+0x04` is stored by the
guest into `[device+0x60C0]`, which is what you would do with a buffer address
you need to keep; `p0+0x08` gates roughly `0xE0` bytes of processing that is
skipped entirely while it is zero, which is what a submission path would look
like. That is a hypothesis worth testing rather than a guess.

`guide_syscmdbuf_buffer_kb` tests it: allocate a buffer once, hand it back as
`p0+0x04 = address` and `p0+0x08 = size`, and then check whether the guest
writes anything into it. PM4 type-3 packets begin `0xC0......`, so the check is
mechanical.

```
VdGetSystemCommandBuffer: handing guest buffer 301AF000 size 65536
SysCmdBuf 301AF000: 0 non-zero words, 0 type-3 headers, first=00000000
Guide composite draw #1 -> 00000000; ...
```

**Not one word.** The draw still completes and nothing crashes, so the values
were accepted - the guest simply does not treat them as a place to write.

So the hypothesis is disproven. Either those fields mean something else, or the
guest writes its command stream somewhere it already knows about and the
descriptor plays no part in locating it. Nothing observed distinguishes those
two, and the buffer-pointer reading should not be carried forward as if the
absence of a better idea made it likely.

What survives from this and the previous section:

- Titles never read the descriptor; only xam does.
- xam reads `+0x04`, `+0x08`, `+0x1C`, `+0x90`, and the display mode at `+0x30`
  and `+0x34`.
- Filling the display mode lets the present complete.
- Filling `+0x04` and `+0x08` with a buffer and its size produces no writes.

## Observing the writes instead of guessing at them

Every attempt to locate xam's command stream through the descriptor has
failed, so `guide_diff_draw_writes` checksums guest memory in 64KB blocks
either side of the composite draw and reports which blocks changed.

Two false starts worth recording, because both look like the instrument
working when it is not:

- The range was first `0x30000000-0x50000000`. Hashing 512MB forces commit of
  a great deal of untouched guest address space; the run died inside the
  snapshot, before it could log that it had started. The symptom was "zero
  draws" in a configuration that normally produces hundreds, which reads as a
  broken draw path rather than a broken probe.
- The deep configuration is a poor host for this. It produces **0 or 1** draws
  non-deterministically - three runs installed the draw hook and only one drew
  - so an instrument that fires once per draw may never fire at all. The
  stable six-cvar configuration draws continuously and is the right place.

Narrowed to `0x40800000-0x40A00000`, in the stable configuration:

```
DrawDiff: snapshot of 32 blocks taken
DrawDiff: block 40880000 changed, 141 type-3-looking words
DrawDiff: block 40890000 changed, 265
DrawDiff: block 408A0000 changed, 358
DrawDiff: block 408B0000 changed, 5800
DrawDiff: block 40950000 changed, 234
DrawDiff: block 40970000 changed, 56
DrawDiff: 6 of 32 blocks changed across the draw
```

and on the following draw, one block changes rather than six.

So the Guide's frame **does** write to guest memory, heavily on its first draw
and lightly thereafter - and this is the *stable* configuration, where
`[dc+134]` is 1 and `XuiRenderPresent` returns without presenting. The XUI
scene is doing real work regardless of whether the present goes anywhere.

### Do not read the packet counts as packet counts

The "type-3-looking words" figure tests `(word & 0xC0000000) == 0xC0000000`,
which matches **a quarter of all random data**. 5800 of 16384 words is 35%,
which is above chance but nowhere near evidence of a PM4 stream. The column is
a hint about where to look next, not a finding. Identifying whether any of
these blocks holds a command stream needs the packet headers decoded - type,
opcode and count checked for self-consistency - not a bitmask.

## The draw writes no command stream

The previous section flagged the "type-3-looking words" count as unreliable and
said the real test was decoding packet headers. Done, and the answer is no.

The first attempt at a packet walker was wrong in the same direction as the
bitmask. It advanced through the buffer honouring each header's length, which
sounds rigorous, but `0x00000000` parses as a **type-0 packet with count 1** -
so a run of zeros walks forever and reports a long, perfectly consistent chain.
It produced "1423 packets" for a block that is mostly empty.

Replaced with a test against Xenia's own opcode table (`PM4_*` in
`src/xenia/gpu/xenos.h`, 47 opcodes): count only type-3 packets whose opcode
Xenia recognises and whose length stays in bounds, and never treat a zero word
as a packet.

Dumping the six changed blocks and scanning them:

- **Stable configuration** (Present short-circuits): 3 recognised packets in
  one block, 1 in another, 0 elsewhere. At or below what chance produces.
- **Deep configuration** (Present executes for real, `[134]=00000000`, draw
  returns): **0 recognised packets in every block.**

So the Guide's frame writes a great deal of data - six 64KB blocks change on
the first draw - and none of it is a PM4 command stream. The `5800`
type-3-looking words were noise, exactly as suspected.

### What this does and does not establish

It does not establish that the Guide emits no commands anywhere. The diff only
covers `0x40800000-0x40A00000`, chosen because that is where its device, DC and
wrapper objects live, and the earlier attempt to cover 512MB killed the run. A
command buffer could sit in physical memory, in the `FE4xxxxx` region xam's
`VdSwap` calls reference, or anywhere else outside that 2MB.

What it does establish is that the memory the draw demonstrably touches holds
scene data, not commands - which is consistent with everything else here: xam's
device has no command buffer to write into, because `VdGetSystemCommandBuffer`
never gave it one.

## The Guide does emit a command stream - it is in FE03xxxx/FE04xxxx

The previous section reported no PM4 in the memory the draw writes, and
carefully scoped that to `0x40800000-0x40A00000`, noting a buffer could sit in
the `FE4xxxxx` region xam's `VdSwap` arguments reference. Extending the diff to
`0xFE000000-0xFE500000` finds exactly that.

Three more blocks change across the draw: `FE030000`, `FE040000` and
`FE460000`. The first two are where xam's own `VdSwap` front-buffer pointers
point (`FE03E284`, `FE041FA4`).

Raw contents of `FE040000`, repeating:

```
C0054500 00000007 00001925 00000000 FFFFFFFF 00001922 00000002 00001925 ...
```

`C0054500` is a type-3 header, count 6, opcode `0x45` - `COND_WRITE` in Xenia's
own table - and `0x1925` / `0x1922` are Xenos register indices. `0x80000000`,
the type-2 NOP filler, appears 290 times in 263 contiguous runs, which is what
padding in a real command buffer looks like.

Opcode histograms:

| `FE040000` | | `FE030000` | |
|---|---|---|---|
| COND_WRITE | 256 | DRAW_INDX_2 | 25 |
| INDIRECT_BUFFER | 28 | DRAW_INDX | 15 |
| EVENT_WRITE_SHD | 22 | WAIT_REG_MEM | 10 |
| WAIT_REG_MEM | 19 | EVENT_WRITE_SHD | 9 |
| INVALIDATE_STATE | 11 | INVALIDATE_STATE | 7 |
| **DRAW_INDX** | **5** | IM_LOAD_IMMEDIATE | 5 |

Draws, shader loads, state invalidation, event writes and conditional register
writes, in sensible proportions. Random data does not produce that - the
earlier bitmask "5800 type-3-looking words" produced no such structure when
decoded, and this does.

**So the Guide is rendering.** It builds a real GPU command stream containing
real draw calls, every frame, into buffers of its own. What it never gets is
anything that executes them.

That also settles the question the descriptor investigation kept failing to
answer. xam does not learn where to write from `VdGetSystemCommandBuffer` at
all - it already has its buffers, which is why handing it one at `p0+0x04` /
`p0+0x08` produced no writes. The missing piece was never "where does it
write"; it is "who executes what it wrote".

Next step, and it is now a concrete one: point `ExecuteIndirectBuffer` at these
buffers and see what reaches the screen.

## Executing the stream: mechanism works, test inconclusive

`ExecuteIndirectBuffer` is `protected` on `CommandProcessor`, reachable only
from packet dispatch on the command processor thread. `ExecuteGuestBufferUnsafe`
is a public wrapper added for this experiment, named to make its status
obvious: driving the command processor from the title thread is not thread
safe, and this is investigation code.

`guide_execute_command_stream` walks the Guide's buffers after the draw and
submits what it finds:

```
GuideExec: FE030000 chain starts FE038000, 41 words
GuideExec: submitted FE038000 +41 words
GuideExec: FE040000 chain starts FE040144, 12 words
GuideExec: submitted FE040144 +12 words
```

Nothing crashes - the command processor accepts the packets - and the screen is
unchanged.

**That result is inconclusive, not negative.** The offline analysis found 370
plausible type-3 headers in `FE040000` and 86 in `FE030000`; the runtime walker
submitted **53 words in total**. It starts at the first plausible header and
stops at the first inconsistency, and the packets are scattered between long
runs of `0x80000000` NOP padding, so it captures a fragment and stops. Fifty-
three words of a several-hundred-packet stream rendering nothing says nothing
about the stream.

What this actually exposes is that locating the stream's true start and extent
is the problem. A command buffer's bounds are not discoverable by pattern
matching - they come from the ring buffer's base, size and write pointer, which
is precisely what `VdInitializeRingBuffer` would have established and what mode
1 never reaches before it stalls.

So the same missing bring-up that leaves xam's device without a ring buffer
also leaves the stream it writes without discoverable bounds. Guessing the
extent by walking headers is not a substitute, and the fragment submitted here
should not be read as evidence either way.

## Measuring the stream's extent, and executing it

Header walking cannot find a command buffer's bounds, but the words the draw
writes *are* its bounds. `guide_word_diff` snapshots `FE030000-FE050000` word
by word around the draw and reports contiguous runs of change:

```
WordDiff: run FE03E284 .. FE03E6B8  (269 words)
WordDiff: run FE040A00 .. FE040E30  (268 words)
WordDiff: run FE0424A0 .. FE04582C  (2388 words)
...
WordDiff: 19 runs, 4201 words changed
```

The largest run in the first block starts at **`FE03E284`** - which is exactly
the pointer xam passes to `VdSwap` as its buffer argument, the one Xenia's own
signature comments call "ptr into primary ringbuffer". The measurement and the
guest's own parameter agree on the address independently. The header walker had
guessed `FE038000` and captured 41 words; it was wrong by 24KB.

Feeding the measured runs to the command processor:

```
GuideExec: submitted measured run FE03E284 +269 words
GuideExec: submitted measured run FE040A00 +268 words
GuideExec: submitted measured run FE0424A0 +2388 words
... 9 runs
```

Roughly 4000 words of the Guide's own command stream, executed. **Zero
crashes** - the command processor consumed it without complaint, which is
itself a check on the stream being real; malformed packets would not survive
that. And the screen is unchanged, still byte-identical to the no-press
control.

### What is left unresolved

This is a far stronger test than the 53-word fragment, but it still does not
show the commands did nothing. The most likely reading is that they render into
xam's own render target - the surface bound at RT0 - and that getting that
surface onto the display is a separate step this has never performed. The
screenshot captures what the display scans out, not what the GPU drew.

The next measurement is therefore not another execution attempt but a check on
the target: diff the memory behind the RT0 surface across the execution. If it
changes, the Guide has been drawn and the remaining problem is purely
composition. If it does not, the commands are being executed without effect and
the state they depend on is missing.

## A functional test that undercuts the command-stream reading

A screenshot cannot say whether the command processor interpreted the packets -
it shows scanout, not GPU work. The register file can: if packets execute,
registers move. `guide_execute_command_stream` now checksums a set of registers
either side of each submission.

```
run FE03E284 +269  words; register checksum 540A0866 -> 540A0866 (UNCHANGED)
run FE040A00 +268  words; 540A0866 -> 540A0866 (UNCHANGED)
run FE041248 +498  words; 540A0866 -> 540A0866 (UNCHANGED)
run FE0424A0 +2388 words; 540A0866 -> 540A0866 (UNCHANGED)
run FE0451C0 +48   words; 540A0866 -> 140A0838 (changed)
run FE04E064 +78   words; 140A0838 -> 140A0838 (UNCHANGED)
```

Eight of nine runs move nothing. One 48-word run does - which is the control
that matters, because it shows the mechanism works: the command processor
reacts to packets when it is given packets.

**This weakens the previous section's conclusion.** I wrote that the Guide
"builds a real GPU command stream containing real draw calls", on the strength
of a decoded opcode histogram - DRAW_INDX, COND_WRITE, IM_LOAD_IMMEDIATE in
plausible proportions. That was a statistical argument about byte patterns. The
register probe is a functional one, and it disagrees: the command processor
consumes those 4000 words and changes no state.

Two readings survive, and nothing here separates them:

1. The runs are real packets but do not begin on packet boundaries. The
   word-diff bounds where the draw *wrote*, which need not be where a packet
   *starts* - and the 48-word run that did work may simply have been aligned by
   luck.
2. The bytes are largely data - vertex or constant buffers - whose scattered
   type-3-looking headers decoded into a plausible-looking histogram without
   being a command stream at all.

The honest position is that "the Guide is rendering" is not established. What
is established: it writes ~4200 words per frame into the region its `VdSwap`
pointer names, one 48-word run of that is something the command processor acts
on, and the rest is inert to it.

## Correcting the correction: the stream does execute

The previous section used a register probe to argue that the command processor
consumed 4000 words and changed no state, and concluded that "the Guide is
rendering" was not established. **That probe was broken**, and the conclusion
with it.

It checksummed eight registers chosen by hand: `0x2000`, `0x2100`, `0x2200`,
`0x1925`, `0x1922`, `0x2001`, `0x2010`, `0x2280`. Logging the head of each run
next to its verdict exposed the problem immediately - runs begin
`00000A31 01000000 ...`, a type-0 write to register **`0x0A31`**, which is not
among the eight. A probe that cannot see the writes it is looking for reports
absence whatever happens.

Checksumming the whole register file (`kRegisterCount` = `0x5003`) instead:

```
run FE03E284 +269  words; 0  registers written
run FE040A00 +268  words; 0  registers written
run FE041248 +498  words; 1  register  written
run FE041B48 +332  words; 58 registers written
run FE0424A0 +2388 words; 0  registers written
run FE0451C0 +48   words; 2  registers written
```

**58 registers changed by a single run.** The command processor is executing
these buffers. The "real command stream" reading from two sections ago stands;
the retraction of it was itself the error.

### The metric still under-reports

It counts registers whose *value* differs, not registers *written*. A packet
that writes a register the value it already holds is invisible, which is
exactly what repeated identical state-setting between frames looks like - so
the zeros are ambiguous and should not be read as "nothing executed here".
`FE040A00` and `FE0451C0` share a byte-identical head yet report 0 and 2, which
is only explicable that way.

Three probes in a row on this one question have been wrong in different
directions: a bitmask that accepted noise, a walker that accepted zeros, and a
register sample too narrow to see its target. The pattern is the same each
time - a test that cannot fail is not evidence - and it is worth more caution
than the result currently deserves.

## A test with a control that passes

Three probes on this question had failed by being unable to return a negative.
This one counts draw packets actually dispatched inside the command processor -
`guide_draw_count_`, incremented at the `PM4_DRAW_INDX` and `PM4_DRAW_INDX_2`
dispatch sites - and is paired with a control that must read zero:

```
GuideExec CONTROL: 512 zero words -> 0 draws (must be 0)
run FE03E284 +269  words; 0  regs,  0 DRAWS
run FE040A00 +268  words; 0  regs,  0 DRAWS
run FE041248 +498  words; 1  reg,   0 DRAWS
run FE041B48 +332  words; 58 regs,  0 DRAWS
run FE0424A0 +2388 words; 0  regs,  0 DRAWS
run FE0451C0 +48   words; 2  regs,  0 DRAWS
```

The control passes, so the counter is not measuring noise. And every run
dispatches **zero draws**, while one of them genuinely writes 58 registers - so
the command processor is parsing real packets out of this memory, and none of
them are draws.

That settles the question the opcode histogram raised and could not answer. The
histogram reported `DRAW_INDX` 5 and `DRAW_INDX_2` 25 in these blocks; executed,
they dispatch nothing. Scattered bytes that decode as draw headers are not draw
packets, and the statistical reading was wrong where the functional one is
controlled.

### The honest summary of the whole question

- The Guide writes ~4200 words per frame into the region its `VdSwap` pointer
  names. Measured.
- Some of it is genuine GPU state: one 332-word run writes 58 registers through
  the real command processor. Measured, with a control.
- None of it dispatches a draw. Measured, with a control.

So what the Guide produces is GPU **state**, not drawing. Whether the draws
exist somewhere this has not looked, or are never emitted because the device
was never brought up, is not settled here - but "the Guide is rendering and
only compositing is missing" is not supported, and I stated it twice before
having a test that could contradict it.

## The sharpest measurement available: the Guide adds no draws

The draw counter sees everything the command processor dispatches, including
the dashboard's own rendering. That makes a within-run control possible: watch
the draw rate across the button press.

```
swaps  200-> 400: +5800 draws (29.0/swap)
swaps 1400->1600: +5800 draws (29.0/swap)   <- button pressed around here
swaps 2000->2200: +5800 draws (29.0/swap)   <- draw hook installed, composite
swaps 3000->3200: +5800 draws (29.0/swap)      draws running
swaps 4400->4600: +5800 draws (29.0/swap)
```

**Exactly 5800 every interval, with no variance at all**, through the press,
through the bootstrap, and through hundreds of composite draws. The dashboard
renders a fixed scene deterministically, which makes this an unusually
sensitive instrument: a single extra draw anywhere would break the constant.

The Guide contributes **zero** draws to the GPU.

That is the cleanest statement of where this ends. Not "no pixels appeared",
which a compositing failure would also explain - the Guide's frame runs to
completion, writes GPU state, and issues no drawing work whatsoever.

### Why that is the expected answer, and what it is worth

This run is the stable configuration, where `[dc+134]` is 1 and
`XuiRenderPresent` returns without presenting, so zero draws is what the rest
of this file predicts. The value is not the surprise; it is that the prediction
is now checked against a measurement sharp enough to have caught a single draw,
after four earlier probes on this question failed by being unable to return a
negative at all.

It also leaves a regression test behind. If any future change makes the Guide
render, this number stops being 5800.

## The answer, measured in both configurations

`GuideDrawGPU` counts GPU draws dispatched across the guest's own draw call,
which the per-swap sampler cannot see in the deep configuration - there the
composite draw happens after the title thread has stopped swapping.

Deep configuration, `[dc+134] = 00000000`, Present executing for real, draw
returning `00000000`:

```
GuideDrawGPU: the guest draw dispatched 0 GPU draws
Guide composite draw #1 -> 00000000; ... [134]=00000000 ...
```

Stable configuration, from the per-swap series: the dashboard's rate holds at
exactly 29.0 draws per swap through the press and 3300 composite draws, so the
Guide's contribution there is zero as well.

**In both configurations, with the entire present path executing, the Guide
dispatches no GPU draws.** The counter has a control that reads zero for a
buffer of zeros, and is sharp enough that one extra draw would show.

So the answer to what happens when you press the Xbox button, as far as this
work can establish it:

1. The button reaches hud, which creates the Guide object and stores it.
2. xam creates a device, an XUI render context, a device context and a scene -
   `scene=00010000`, every time.
3. The per-frame draw runs: `XuiRenderBegin`, the scene messages,
   `XuiRenderEnd`, `XuiRenderPresent`, six frames deep into xam's real
   presentation code, returning `00000000`.
4. It writes about 4200 words of GPU state per frame into the region its
   `VdSwap` pointer names, some of which the command processor accepts and
   acts on.
5. It never issues a draw.

The software runs end to end. What it does not do is render, and the reason is
upstream of drawing: the device was never brought up by the system boot Xenia
does not have, so the path that would emit geometry is inert even though every
call along it succeeds.

## Confirming the wait's exit condition by forcing it

`819F4488` has exactly one path that returns "done": the test on bit 1 of
`[device+2B3D]` at the top. Everything else returns "still waiting". That
reading came from the disassembly and had never been tested.

`guide_force_cmdbuf_complete` sets the bit from a host thread - no breakpoint,
no perturbation of the guest's scheduling:

```
CmdBufComplete: armed, 8s
CmdBufComplete: set bit1 of [40870D00+2B3D]
CmdBufComplete: set bit1 of [40883A80+2B3D]
```

The behaviour changes immediately and unmistakably. The log goes from ~20,000
lines to **815,441**, ending in:

```
VdGetSystemCommandBuffer #787293 [XAM CREATEDEVICE] from '' p0=709DEFF0 p1=709DEF5C
VdGetSystemCommandBuffer #787294 [XAM CREATEDEVICE] ...
VdGetSystemCommandBuffer #787295 [XAM CREATEDEVICE] ...
```

787,295 calls, roughly 20,000 a second. So the reading was right: **the wait
does end when that bit is set**, and the guest leaves it and carries on. This
is a causal confirmation of a static reading, which most of the conclusions in
this file do not have.

What it also shows is the shape of the next obstacle. Released from the wait,
xam immediately asks for the system command buffer again, gets Xenia's zeroed
descriptor again, submits, waits, is released again, and repeats. The stall
becomes a spin. `CreateDevice` still never returns and
`VdInitializeRingBuffer` is still never called.

So forcing the exit converts a deadlock into a livelock, which is what you
would expect when the thing being waited for is real work that never happens.
It is not a step toward the device coming up; it is a demonstration that the
wait is not what stands in the way - the missing command buffer is.

## Re-testing the buffer hypothesis where the guest is desperate for one

The disproof of `p0+0x04` / `p0+0x08` as a command buffer pointer and size was
measured in the stable configuration, where the guest asks for the buffer once
and is not retrying. That is a weak place to test it: a guest that has what it
needs has no reason to write.

The forced-wait livelock is the opposite condition. There xam asks for the
system command buffer roughly 20,000 times a second, having just been released
from a wait it could not otherwise leave. If the descriptor were how it learns
where to write, that is where it would show.

Handing it a real 64KB buffer in those fields and sampling every 50,000
acquisitions:

```
SysCmdBufAcq #1:      guest has written 0 non-zero words
SysCmdBufAcq #50000:  guest has written 0 non-zero words
SysCmdBufAcq #100000: guest has written 0 non-zero words
...
SysCmdBufAcq #300000: guest has written 0 non-zero words
```

Three hundred thousand acquisitions, not one word written.

So the disproof holds in the strongest condition available, not just the
convenient one. `p0+0x04` and `p0+0x08` are not where xam learns about a
command buffer, and the retry loop is not a search for one - it is waiting for
something about the descriptor it already has to change.

Which is consistent with the rest: xam writes to its own buffers in
`FE03xxxx`/`FE04xxxx`, and what it wants from `VdGetSystemCommandBuffer` is
something else entirely, still unidentified.

## The retry loop is a kernel wait on three objects

The livelock's `VdGetSystemCommandBuffer` storm is not a tight poll inside
`819FE138` - none of that function's back-edges span the call. The loop is one
level up, in mode-1 `CreateDevice` itself:

```
8178ECBC  <- loop head
  ...
8178EDD4  bl 819FE138            ; acquires the system command buffer
  ...
8178EE08  bl KeWaitForMultipleObjects(3, &objects, 1, 3, 1, 0, 0, &timeout)
8178EE0C  cmpwi cr0,r3,0
8178EE10  bne  -> 8178ECBC       ; loop while the wait does not succeed
```

`81D0FF3C` is `KeWaitForMultipleObjects` in xam's import table. The first
argument is **3**, and `r4` points at an array of three objects on the stack.

So the shape of mode 1's stall is different from what the inner spin suggested.
At the inner level, `819F4488` polls a flag and never leaves - that part is
established, and forcing the flag does release it. But the outer level is a
proper kernel wait on three dispatcher objects, retried indefinitely because
the wait keeps failing.

That reframes the blocker usefully. It is not "xam is busy-waiting on memory
Xenia never writes". It is "xam is waiting on three kernel objects that nothing
ever signals", which is a far more tractable thing to chase: the objects have
identities, and whatever signals them on hardware is a specific piece of the
system that Xenia either stubs or never runs.

The next step is to identify the three. Xenia implements
`KeWaitForMultipleObjects`, so logging the handles when the call arrives from
this site names them, and from there the question becomes which code is
supposed to signal each one.

## A failing wait, and why it fails

Xenia's `KeWaitForMultipleObjects` returns `X_STATUS_INVALID_PARAMETER`
**immediately** when any object will not resolve - it does not wait. So a
caller that loops on the result spins at full speed, which is what tens of
thousands of `VdGetSystemCommandBuffer` calls a second look like from the
outside.

Logging the object that fails:

```
KeWaitForMultipleObjects #1: object 0 of 2 at 81D424A8 will not resolve;
  dispatch type 9 -> returning INVALID_PARAMETER without waiting
```

Dispatch type **9** is a synchronisation timer. Xenia's `GetNativeObject`
handles timers only when `guest_native_timers` is set, and it defaults off:

```
if (!cvars::guest_native_timers) { result = nullptr; break; }
```

So the object is a timer the emulator declines to resolve, the wait fails
instantly rather than waiting, and the caller busy-loops.

Enabling the cvar confirms it causally: **wait failures drop from thousands to
zero**.

### What that costs

It immediately crashes elsewhere:

```
GUEST CRASH: access violation at guest PC 817286C0, fault_addr 0
GUEST CRASH: lr=81728678 r3=FFFFFECC
GUEST CRASH: unwind: 81731538 81731760 817319F8 81779D54 8177AA9C 8177AE48
```

`r3 = 0xFFFFFECC` is -308 - a negative value being used where a pointer or
index is expected. This is in xam code unrelated to the Guide, reached through
six frames that have nothing to do with device bring-up, and it is presumably
why the cvar defaults off in the first place.

### Scope, stated carefully

The wait that was instrumented is a **2-object** wait. The retry loop in mode-1
`CreateDevice` is a **3-object** wait at `8178EE08`. They are different call
sites, and nothing here shows the 3-object wait fails for the same reason - or
at all. What is established is narrower and still worth having: at least one
wait in this run fails instantly because a guest timer will not resolve, that
failure mode makes any looping caller spin, and the emulator's timer support is
the reason.

## The three-object wait is not the retry loop

The previous section identified the `KeWaitForMultipleObjects` at `8178EE08` as
the loop driving the `VdGetSystemCommandBuffer` storm, from a static reading of
the back-edges in mode-1 `CreateDevice`. Instrumenting that specific wait -
logging three-object waits whether they succeed or fail - shows it is not.

In one run:

| | count |
|---|---|
| three-object waits | **36** |
| two-object waits failing on a timer | 1,983 |
| `VdGetSystemCommandBuffer` calls | **606,117** |

Thirty-six iterations cannot produce six hundred thousand acquisitions. And the
three objects resolve without trouble:

```
Wait3 #1: 81D433C8(type 0) 81D43398(type 0) 81D433A8(type 0)
```

Type 0 is a notification event; none of them is the type-9 timer that fails
elsewhere, and none of them appears in the failure log at all.

So the loop identified from the back-edge analysis runs 36 times and is not the
source of the storm. Where the 606,117 calls come from is unidentified. They
are tagged `[XAM CREATEDEVICE]`, so they are inside the `CreateDevice` call on
the button thread, but the enclosing loop is not the one at `8178EE08`.

This is the same error as several before it: a back-edge that spans a call site
shows a loop *exists*, not that it is the loop *running*. The counts were
available the whole time and settle it in one measurement.

## Retracting that retraction: it was my own rate limit

The previous section claimed the three-object wait runs 36 times and therefore
cannot drive 606,117 acquisitions. That comparison was invalid. **36 was the
number of log lines**, emitted under `if (wn <= 6 || (wn % 20000) == 0)`, and
it was compared against an unlimited count. The highest sequence number in
those lines is:

```
Wait3 #600000
```

So the wait ran 600,000 times, matching the 606,117 acquisitions almost
exactly. The loop identified from the back-edges was right all along.

Asking the caller directly settles it independently. Logging the return address
at the shim gives `lr 819FE604 x598576` - inside `819FE138` - and unwinding the
guest stack from there gives one frame:

```
SysCmdBufCaller #200000: unwind 8178EDD8
```

`8178EDD8` is the return address from `8178EDD4`, the call site inside mode-1
`CreateDevice`. Three independent measurements - the sequence number, the LR
histogram and the unwind - agree.

### So what the bring-up is actually waiting for

```
8178ECBC  <- loop head
8178EDD4  bl 819FE138                              ; acquire system command buffer
8178EE08  bl KeWaitForMultipleObjects(3, &objs, ...)
8178EE10  bne -> 8178ECBC
```

with

```
Wait3: 81D433C8(type 0) 81D43398(type 0) 81D433A8(type 0)
```

Three **notification events**, which resolve without trouble - they are not the
type-9 timer that fails elsewhere. They are simply never signalled, so the wait
times out and the loop runs forever.

That is the most specific statement of the blocker this investigation has
produced: **mode-1 device bring-up waits on three notification events at
`81D433C8`, `81D43398` and `81D433A8`, and nothing in Xenia ever signals them.**

### The lesson, since it is the third of its kind

I misread my own rate-limited log as a count. The earlier failures were tests
that could not return a negative; this one is a number that did not mean what
it appeared to. Both come from not asking what an instrument is actually
reporting before reasoning from it.

## Who is supposed to signal the three events

Scanning xam for code that forms the addresses `81D433C8`, `81D43398` and
`81D433A8` finds nine sites in four functions. Three of them build the wait
array in `8178E9F0` itself. Of the rest:

| function | references | `KeSetEvent` calls | ran? |
|---|---|---|---|
| `817915A0` | all three | **2** | no |
| `81795548` | two | 0 | no |
| `8178E8C0` | one | 0 | no |

So `817915A0` is the signaller, and it never executes. Following its callers:

```
81751428  ->  81792880  ->  817915A0  ->  KeSetEvent on the three events
```

`81792880` has exactly one caller, `81751428`, and **`81751428` has no caller
anywhere inside xam**. It is an entry point invoked from outside the module -
the same shape as `8178E9F0`, the mode-1 device creator, which also has no
in-module caller. Both are things the system boot calls and Xenia does not.

That is the chain, end to end: mode-1 bring-up waits on three notification
events; the only code that signals them is reachable only from an entry point
nothing in the emulator ever calls.

### Calling it directly does not work

`guide_call_boot_entry` invokes `81751428` with no arguments, as
`8178E9F0` is invoked:

```
Guide button: calling boot entry 81751428
GUEST CRASH: access violation at guest PC 81778508, fault_addr 4
```

It faults immediately on a null dereference, and `817915A0` still never runs.
The entry point takes arguments, and nothing here says what they are. Guessing
them would be the same mistake as guessing `p0+0x08`, so the experiment stops
at a recorded negative rather than a search.

What it establishes is still worth having: the events are not signalled because
a specific, named function is never called, and that function is the system
boot's job.

## Correction: the boot entry takes no arguments

The previous section said `81751428` "takes arguments, and nothing here says
what they are", and stopped rather than guess. The stopping was right; the
reason was wrong. Its prologue:

```
81751428+0C  addi r11,r1,80
81751428+10  addi r31,r0,0
81751428+14  addi r5,r0,0
81751428+18  addi r4,r0,19
81751428+1C  addi r3,r1,80        ; r3, r4, r5 all WRITTEN before any read
81751428+30  bl 817A90E0
81751428+38  lis r30,0x81D4
81751428+3C  lwz r3,[81D3FBC0]    ; works from globals
```

Every argument register is written before it is read. **The function takes no
arguments.** It operates on globals and locals.

So the crash is not a signature problem. Unwinding it:

```
GUEST CRASH: access violation at 81778508, fault_addr 4
GUEST CRASH: r3=00000000
GUEST CRASH: unwind: 817271D8 817512F8 8175149C
```

`8175149C` is inside `81751428` itself, so the chain is
`81751428` -> `817512D8` -> `817271B8` -> `817784F8`, which dereferences
`[r3+4]` with `r3` null.

A null object four calls deep, in a function that reads globals rather than
parameters, means **the state it depends on was never set up**. `81751428` is
one step of a boot sequence, and calling it in isolation finds the globals that
earlier steps would have initialised still empty.

That is a better characterisation than "unknown arguments", and it points
somewhere different: the question is not how to call this function but what has
to have happened before it. Which is the same answer the rest of this file
keeps arriving at from other directions - there is a system boot, Xenia does
not run it, and its individual pieces do not work when lifted out of it.

## The boot entries form a chain, and it does not terminate anywhere useful

`81751428` crashed on a null global at `81D3C8E8`. That global lives in the
zero-filled part of `.data`, so it starts null and must be written at runtime.

Searching for writers by the obvious pattern - `lis 0x81D4` then `stw` at that
displacement - finds **only reads**. That would have been the wrong answer: the
same pattern-limited search failed earlier for `device+0x32A0`, which turned out
to be written through a taken address. Searching for the address-taken form
instead finds one site:

```
81727530  addi r31,r11,-14104     ; r31 = &81D3C8E8
81727534  or   r3,r31,r31
81727538  bl   8177C9E8           ; hands it to an initialiser
```

`81727500` is reached only from `81750FA8`, which - like `81751428` and
`8178E9F0` - has **no caller anywhere inside xam**. So the dependency is
derived rather than guessed: `81750FA8` initialises what `81751428` needs.

Running them in that order:

```
GUEST CRASH: access violation at guest PC 81747DE8, fault_addr F4
```

`81750FA8` now crashes too, before it returns, and `817915A0` still never runs.
The initialiser has its own predecessors.

### What that settles

These are not a handful of entry points that can be called in the right order
to stand the system up. They are a sequence whose dependencies keep receding:
`8178E9F0` needs events signalled by `817915A0`, reachable only from
`81751428`, which needs a global initialised by `81750FA8`, which needs
something earlier still. Each attempt moves the crash one level back rather
than closer to working.

So the approach of lifting individual entry points out of the boot and calling
them is not a way in, and this is the evidence for that rather than an opinion
about it. Anyone tempted to try the same thing should read this section first -
it costs a build and a run to rediscover.

## How much weight "no caller inside xam" can carry

Several conclusions above lean on a function having no caller inside xam, and
read that as "the system boot calls it". Worth measuring how distinctive that
property actually is:

```
xam functions with unwind data:        18,301
never the target of a direct bl:        4,494  (24.6%)
indirect call sites (bctrl) in .text:   5,950
```

**A quarter of xam's functions are never directly called**, and there are
almost six thousand indirect call sites. xam is C++ with vtables throughout -
this file has already followed several dispatches through `dc->vtable[21]` and
`device->vtable[24]`. So "no direct caller" is a common property with an
obvious innocent explanation, and on its own it does **not** establish that a
function is a boot entry point.

That weakens how `81750FA8`, `81751428` and `8178E9F0` were described. Calling
them "entry points the system boot invokes" was an inference from a property
shared by 4,494 functions, and `callers.py` cannot see indirect calls - a
limitation in its own docstring that I have now failed to apply twice.

What survives for those three is the direct evidence, which is unaffected:

- `8178E9F0` produces a device that reaches the GPU where `8178F748` does not.
  Measured.
- `81751428` and `81750FA8` both fault when called cold, on null state, in
  chains that read globals rather than parameters. Measured.
- The three notification events are never signalled and `817915A0`, the only
  code that signals them, never runs. Measured.

Those stand. The claim that should not be repeated without better evidence is
the architectural one - that these particular functions are *the* interface the
system boot drives. They may equally be reached through a function pointer from
somewhere that itself never runs.

## Sharpening the test: called, dispatched, or neither

The previous section was right that "no direct caller" is common - 4,494 of
18,301 functions - and wrong to leave it there, because the obvious innocent
explanation is testable. A function reached through a vtable or a function
pointer must have its address **stored somewhere as data**. Splitting the 4,494
on that:

```
functions:                                18,301
never a direct bl target:                  4,494
  ...address IS stored as data:            2,294   <- vtable / fn-pointer dispatch
  ...and not stored anywhere either:       2,200   <- unreachable from inside xam
```

Half of them are ordinary C++ indirect dispatch, exactly as suspected. The
other half are reachable by no static means at all: nothing calls them and
nothing holds their address.

The three in question fall in the second set:

| function | never called | never stored |
|---|---|---|
| `81750FA8` | yes | **yes** |
| `81751428` | yes | **yes** |
| `8178E9F0` | yes | **yes** |
| `817915A0` | no - directly called | - |
| `81792880` | no - directly called | - |

So the entry-point reading is restored, now on evidence rather than on a
property shared by a quarter of the module: `81750FA8`, `81751428` and
`8178E9F0` cannot be reached from within xam by any static path. Something
outside the module calls them, which is what an entry point is.

Two honest limits. 2,200 functions share that property, so it identifies a
class rather than singling these three out - much of that class is likely dead
code. And an address computed at runtime rather than stored would not show up
here, though for a function that is also never directly called that is a
stretch.

## Correction: 81750FA8 already runs

Cross-referencing the 2,200 unreachable-from-inside functions against what
actually executed in a session shows 238 of them running - xam's entry point
`817519D8` among them, and most of the rest simply exports the title calls
through the import table. That is the mundane explanation for the class, and it
is worth stating.

It also turned up something I had asserted without measuring. `81750FA8` is in
the list, and checking it directly across three traces:

| trace | `81750FA8` | `81751428` | `817915A0` |
|---|---|---|---|
| stable config | **1** | 0 | 0 |
| mode 1 | **1** | 0 | 0 |
| boot-entry experiment | **1** | 1 | 0 |

**It runs in every session, including a plain one.** The section above
described it as an entry point "the system boot invokes and Xenia does not",
and built an experiment on calling it. That premise was false: something
already calls it, every time, and my experiment called it a *second* time.

So the crash that experiment produced says nothing about boot ordering - it is
what re-entering an already-completed initialiser does. The "receding chain"
conclusion drawn from it is not supported by that evidence.

What survives, still measured: `81751428` and `817915A0` genuinely never run,
the three notification events are never signalled, and `[81D3C8E8]` is null
when `81751428` reads it - which is the interesting part, because `81750FA8`
*did* run and the global is still empty. So `81750FA8` takes a path that does
not reach `81727500`, and why is now the open question.

That is a better question than the one I was chasing, and I only reached it by
checking a claim I had already written down as fact.

## Why the initialiser is skipped

`81750FA8` runs every session and `81727500`, which initialises `81D3C8E8`,
never does. The call to it sits at `81751294`, and a linear read of the code
before it suggests it is unreachable - it follows a `twi` and an unconditional
branch. `cfg.py` disagrees, and is right: the block is entered by a jump.

```
8175125C  cmpwi cr6,r29,0
81751260  bne   cr6 -> 81751294      ; only when r29 is non-zero
81751294  bl    81727500             ; initialises 81D3C8E8
```

and the single write to `r29` in the whole function is its third instruction:

```
81750FC8  or r29,r3,r3               ; r29 = the first argument
```

So **the initialiser runs only when `81750FA8` is called with a non-zero first
argument**, and since it never runs, the natural caller passes zero.

That also indicts the earlier experiment twice over. It called `81750FA8` a
second time - the function already runs - and passed `0`, which is exactly the
argument that skips the initialiser it was trying to reach.

### Testing the other argument does not work either

Calling it with `1` produces the same crash as before, at `81747DE8`, and
`81727500` still never runs. The fault happens early in the function, before
the `r29` test, so this says nothing about the argument: re-entering an
already-completed initialiser fails regardless of what it is passed.

The static finding stands on its own and is the useful part: the path to
`81D3C8E8`'s initialisation is gated on an argument that the real caller sets
to zero. What decides that argument, in whatever calls `81750FA8` normally, is
the next thing to find - and it is a question about the caller, not about
re-invoking the callee.

## What actually calls it, and what that corrects

`81750FA8` runs on thread `F80000E0`, immediately after `LLE xam: DllMain
returned`. It is a **thread start routine**, and that explains everything the
static scans could not: nothing calls it with `bl` and nothing stores its
address, because the address is formed in a register and handed to a thread
creator.

Scanning for that form - `lis` then `addi` producing the address - finds it:

```
81758ADC  addi r4,r0,0            ; the thread parameter: zero, hardcoded
81758AE8  addi r3,r10,4008        ; r3 = 81750FA8
81758AEC  bl   8177C8E0           ; thread creation
```

in function `81751718`. So the answer to "what decides that argument" is: it is
a compile-time constant `0` at the only site that creates the thread. `r29` is
zero because it is passed zero, deliberately, and the `81D3C8E8` initialisation
this chain leads to is simply not on the path that runs - on hardware either.

### The classification was wrong for two of three

The same scan finds `81751428`'s address formed at `81751534`, inside
`81751500`. So:

| function | direct call | stored as data | formed in a register |
|---|---|---|---|
| `81750FA8` | no | no | **yes** - thread routine |
| `81751428` | no | no | **yes** |
| `8178E9F0` | no | no | no |

Two of the three are ordinary function pointers, not entry points. The
"unreachable from inside xam" test had a caveat written into it - "an address
computed at runtime rather than stored would not show up here, though for a
function that is also never directly called that is a stretch" - and the
stretch is exactly what happened, twice.

`8178E9F0` survives all three tests and remains genuinely unreferenced, which
is now a much narrower and better-supported claim than the one made for all
three together.

### And the chain it was built on

Since `81750FA8` is called with zero by design, `81727500` never runs on
hardware either, so `81D3C8E8` must be initialised by something else entirely.
The chain from the three notification events back through this function was
real as disassembly and useless as an explanation: it traced a path that does
not execute.

## All three are thread routines; the entry-point reading is fully withdrawn

`refs.py` (added here) looks for all three ways an address can be referenced -
a direct `bl`, a word stored in data, and `lis`/`addi` forming it in a register
- and handles the sign extension that `addi` applies. Run against the three
functions:

```
81750FA8:  register 817518E8 in fn 81751718
81751428:  register 81751534 in fn 81751500
8178E9F0:  register 8179171C in fn 817915A0
```

**All three are referenced.** The previous section said `8178E9F0` "survives
all three tests and remains genuinely unreferenced"; that was wrong, because my
hand-written scan looked for `lis 0x8178` when `0xE9F0` has bit 15 set and
`addi` sign-extends, so the `lis` half is `0x8179`:

```
8179890C  lis  r11,0x8179
81798918  addi r4,r0,0            ; thread parameter: zero
8179891C  addi r3,r11,-5648       ; r3 = 8178E9F0
81798920  bl   8177C8E0           ; the same thread creator as 81750FA8
```

So `8178E9F0` is a thread start routine too, created by `817915A0` - the very
function that signals the three notification events - with parameter zero,
through the same helper `8177C8E0`.

That reorganises everything cleanly. `817915A0` is the startup for this
subsystem: it signals the events *and* starts the device thread. It never runs,
so neither happens. There are no external entry points here at all; there is
one function that would start the whole thing and does not execute.

### Four versions of one mistake

The entry-point narrative was built and rebuilt on scans that each missed a
form of reference: direct calls only, then direct plus stored, then a
register scan with a sign-extension bug. Each time the gap was in the tool
rather than the reasoning, and each time the conclusion looked stronger than it
was. `refs.py` covers all three forms and documents what it still cannot see -
an address assembled by arithmetic other than `lis`/`addi`.

## A real Xenia bug on the path: kernel_debug_monitor never worked

Tracing what would start `817915A0` leads to `817439D0`, which registers
callbacks by dispatching through a kernel global:

```
8174ABE8  lwz r11,[815F044C]      ; KeDebugMonitorData
8174ABEC  lwz r11,0(r11)
8174ABF0  cmplwi cr6,r11,0
8174ABF4  beq  cr6 -> return      ; null: register nothing, silently
8174ABF8  lwz r11,24(r11)         ; vtable slot at +0x18
8174AC00  addi r3,r0,50
8174AC04  bctrl
```

Measured, `[815F044C] = 80207A64` - a valid pointer - but `[[815F044C]] = 0`.
Xenia's own comment at that address says "Offset 0x18 is a 4b pointer to a
handler function that seems to take two arguments", which matches the
disassembly exactly.

Xenia has a `kernel_debug_monitor` cvar for this, and **it did not work**:

```cpp
uint32_t pKeDebugMonitorData = memory_->SystemHeapAlloc(...);
xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(pKeDebugMonitorData),
                             pKeDebugMonitorData);      // into the block
auto lp = memory_->TranslateVirtual<...>(pKeDebugMonitorData);
std::memset(lp, 0, sizeof(...));                        // ...then erased
```

The pointer was written into the freshly allocated block rather than into the
exported variable, and the `memset` immediately after erased even that. So
`KeDebugMonitorData` stayed zero and the cvar had no guest-visible effect at
all. Fixed by writing the pointer to `KeDebugMonitorData` after the block is
initialised.

With the fix: `[[815F044C]]` is a real object, and the guest starts using it -
`KeDebugMonitorCallback` is invoked **53,525 times** in one session, where
previously it was unreachable. No crashes, and the Guide bootstrap is
unaffected.

### It is not the Guide's missing piece

`81723D70`, `81751500`, `81751428` and `817915A0` still do not run. Registering
a callback is not the same as it being invoked, and the invocations that do
happen are for something else.

There is a larger caution here. This entire chain is gated on the **debug
monitor**, which is a devkit facility. On retail hardware it is absent too, so
a path that only runs under it cannot be how the Guide normally starts. The
chain is real as disassembly and was worth following to its end - it found a
genuine emulator bug - but it is a debugging facility, not the Guide's startup
path, and should not be pursued further on the assumption that it is.

## The same bug in KeCertMonitorData, and verification of both

`KeCertMonitorData` had the identical defect - allocate a block, write the
block's address into the block, memset it away, never touch the exported
variable - so `kernel_cert_monitor` was equally inert. Fixed the same way.

Verified together:

| | with both cvars off | with both on |
|---|---|---|
| dashboard framebuffer | `2EF6B4B7` | `2EF6B4B7` |
| guest crashes | 0 | 0 |
| `KeDebugMonitorCallback` invocations | unreachable | **21,411** |
| `KeCertMonitorCallback` invocations | unreachable | **1** |

The framebuffer hash is the same as the long-standing no-press control, so
neither fix changes rendering. Both callbacks go from unreachable to actually
used by the guest.

These two are worth separating from the Guide work when it comes to
upstreaming. They are not Guide-specific and not LLE-specific: any title that
probes `KeDebugMonitorData` or `KeCertMonitorData` sees null today regardless
of the cvar, and silently takes its "no monitor present" path. The fixes are
four lines and independent of everything else on this branch.

## The scene is not empty: it is laying out real geometry

A draw count of zero has two possible explanations, and only one of them has
been examined. Either the Guide cannot submit its drawing, or **it has nothing
to draw** - an empty scene issues no draws quite legitimately, and every
conclusion in this file would read differently if that were the case.

The words the frame writes settle it. Decoding the heads of the runs the word
diff found, as IEEE floats:

```
BF000000 BF000000 3F7F0001  ->  -0.50  -0.50   1.00
43800000 00000000 43800000 4279C190  ->  256.00  0.00  256.00  62.44
42280000 00000000 42280000 42280000  ->   42.00  0.00   42.00  42.00
```

`-0.5, -0.5` is the Direct3D 9 half-pixel offset, the correction every 2D UI
renderer applies to align texels to pixels. The others are rectangles: one
256 x 62.44, one 42 x 42. These are laid-out UI elements in screen
coordinates.

So the scene has content, the layout runs, the transforms are applied and the
geometry is computed - every frame, into the buffers the `VdSwap` pointer
names. The Guide is doing everything up to and including working out where its
elements go on screen.

What it never does is issue a draw packet to rasterise any of it. That is a
narrower and better-supported statement than the one this file started with,
and it rules out the alternative explanation rather than leaving it open.

## Locating the draw emitter

A `DRAW_INDX` packet carries opcode `0x22` in bits 8-14 of its header, so code
that builds one has `0x2200` as an immediate. Scanning xam for that finds 19
sites in 17 functions, and one of them is already familiar:

```
fn 819F5D18  x2      <- the function that faulted on the null r14 descriptor
```

`819F5D18` is the **draw emitter**. That identifies the crash from several
sections ago as happening inside the code that would have issued the Guide's
draw calls, which is a much more specific thing than "somewhere in xam's D3D".

Whether it runs at all differs by configuration:

| configuration | composite draws | `819F5D18` reached |
|---|---|---|
| stable (`[dc+134]=1`) | 14+ | **no** |
| deep (`[dc+134]=0`) | 1 | **yes**, then faults |

Both halves are consistent with everything else here. In the stable
configuration the null-render flag makes `XuiRenderBegin` skip its device call
and `XuiRenderPresent` return without presenting, so the emitter is never
reached and zero draws is the correct outcome, not a symptom. In the deep
configuration the path is live, the emitter *is* reached, and it dies on a
missing resource descriptor before emitting anything.

So the earlier draw-count measurements were right but were measuring two
different situations. The one that matters is the deep configuration, where
exactly one thing stands between the Guide and real draw packets: the null
sixth argument to `819F5D18`, the six-dword fetch constant traced earlier to
`[device+3F74]`.

That is the narrowest the problem has been. It is one pointer, in one call, in
a function now known to be the thing that emits draws.

## The emitter is reached and emits nothing - and why that proves less than it looks

With the full deep configuration and `guide_create_xam_device` restored (it
gates the whole device-creation block, and turning it off during unrelated work
had been silently disabling mode 1):

```
device global 40870D00 (RT0=00000000) -> 40883A80 (RT0=4088B3E0)
front buffer [dev 40883A80 +3F74] = clone 301C1000 of RT0 4088B3E0
GuideDrawGPU: the guest draw dispatched 0 GPU draws
composite draw #1 -> 00000000
```

No crashes, `819F5D18` reached, zero draws. The emitter runs and returns
without emitting, so it bails on an internal condition - the read of
`[r14+0x20]`, whose low six bits it extracts as a format and passes to
`819FC1E0`, is the obvious candidate.

**But this configuration cannot bear that weight.** Reaching the emitter at all
required three pieces of hand-fabricated state:

- `guide_use_bound_device` - repointing the DC at a different device
- `guide_fake_front_buffer` - a **clone of the colour surface** standing in for
  a front buffer that nothing allocates
- `guide_syscmdbuf_fields` - display-mode values written into a descriptor
  Xenia otherwise zeroes

The front buffer in particular is a colour surface wearing a front buffer's
hat. If `819F5D18` inspects its format and decides there is nothing to do, that
is a perfectly reasonable response to the object it was handed - and says
nothing about what the real system would do.

So "the emitter emits zero draws" is a measurement of **my configuration**, not
of xam. The honest reading is that the deep configuration gets far enough to
reach the code that would draw, and that everything past that point is being
fed fabricated inputs. Diagnosing the bail condition would be diagnosing my own
stand-ins.

That is a limit worth naming clearly, because several sections here have leaned
on deep-configuration measurements without it.

## Re-checking the geometry finding without fabricated state

The previous section warned that deep-configuration results measure my
stand-ins rather than xam. The geometry evidence was one of those - the floats
came from `GuideExec` logs in the deep configuration. Worth re-establishing
where nothing is faked.

Stable configuration, every fabrication cvar off (`create_primary_device`,
`clear_null_render`, `use_bound_device`, `fake_front_buffer`,
`syscmdbuf_fields` all false), diffing the composite draw and decoding the
changed blocks as floats:

```
dump_408B0000 @+0x285C:  0.00  4.35  0.00  4.32  287.00  350.00  1.00  425.00
dump_40950000 @+0x0058:  5.16 22.88  4.66  576.00  640.00  768.00  0.00  0.00
dump_40880000 @+0x21FC:  3.41 68.73  4.25  4.25   4.25   4.29    0.00  0.00
```

`287`, `350`, `425` are screen coordinates. `576`, `640`, `768` are resolution
values. The small clustered numbers around 4.2-4.8 look like per-element
metrics repeated across a list.

So the finding holds in the clean configuration: **the Guide lays out real UI
geometry every frame, with no fabricated state involved.** The scene is
populated, the layout runs, and coordinates are computed - all of that is
genuine xam behaviour, not an artefact of the stand-ins.

This is worth having separated. Of the major claims in this file:

- **Stable configuration, nothing faked**: the software runs end to end; the
  scene is created; a full XUI frame executes per swap; real geometry is laid
  out; the Guide contributes exactly zero GPU draws; the draw emitter is never
  reached because the null-render flag disables the path.
- **Deep configuration, three pieces of fabricated state**: the present path
  executes; the emitter is reached; it emits nothing.

The first group stands on its own. The second describes what happens to a
system I have partly assembled by hand, and should be read that way.

## The clean-configuration blocker, and where it comes from

Stripping away everything fabricated, the stable configuration has exactly one
thing stopping the render path: `[dc+0x134] = 1`, inherited from
`[xui_ctx+0x1C]`. Clearing it is what `guide_clear_null_render` does, and doing
so drags in the rest of the fabricated state. So the clean question is what
sets it legitimately.

The XUI context global `81D6C978` has three writers, found only by searching
for *any* base register with displacement `0xC978` - the `lis 0x81D7` form
finds only reads:

```
818FADD0  in fn 818FAD98
818FF278  in fn 818FF140    <- XUI ordinal 0x351
818FF3F4  in fn 818FF2C8    <- XUI ordinal 0x352
```

`818FF140` is the context creator, and it contains **no store to `+0x1C`** at
all. Yet the bootstrap's own log shows the flag already set the moment the
render host returns:

```
GuideBootstrap: XUI ctx 4088A0A0 [1C] 00000001 -> 00000000
```

So it is set somewhere inside `8178DC58`'s call tree rather than at context
construction.

### Which suggests the flag is a symptom

A field that means "render nothing, report success" is the sort of thing a
renderer sets when it finds it has no usable device - not a switch someone
forgot to flip. If that reading is right, the flag is downstream of the same
missing device bring-up that everything else in this file converges on, and
clearing it by hand is treating the symptom, which is exactly what the
fabricated deep configuration turns out to be doing.

That reading is **not** established - no test here distinguishes "set because
the device is missing" from "set for some unrelated reason". But it is the
first explanation that accounts for why clearing the flag by hand leads
immediately to needing a device redirect, then a front buffer, then display
mode fields: each stand-in replaces something the same absent bring-up would
have provided.

## A function that clears the flag, and how far that goes

If `[xui_ctx+0x1C]` is set by something, the complementary question is whether
anything clears it. Scanning the XUI range for stores to `+0x1C` whose source
register was just loaded with a constant finds exactly one:

```
8190F7D4  stw r30(=0),0x1C(r31)     in fn 8190F7A0
```

One site, storing **zero**. Nothing in the XUI range stores a constant `1`
there, so the `1` the flag carries is computed rather than a literal default.

`8190F7A0` never runs, and neither does its chain:

```
81914740 -> 81914578 -> 8190F7A0      none of the three execute
```

### What this is not

It is tempting to read that as "the function that would clear the flag never
runs", and the temptation should be resisted. `8190F7A0` writes `0x1C` of
whatever object arrives in `r3`, and nothing here shows that object is the XUI
context. Plenty of objects have a field at `+0x1C`; this investigation has
already mistaken one object's offset for another's more than once.

The only circumstantial support is the call site:

```
8191B81C  lwz r4,-13952(r31)     ; = [81D6C980] with r31 = 81D70000
8191B820  bl  8190F7A0
```

`81D6C980` sits eight bytes from the XUI context global `81D6C978`, so the
caller is working in that neighbourhood. That is suggestive of the right
subsystem and says nothing about `r3`.

So: there is exactly one constant-zero store to a `+0x1C` field in the XUI
code, it is in a function that never executes, and whether that field belongs
to the XUI context is unverified. Confirming it needs the object in `r3` at
runtime - which cannot be read with a breakpoint, since the function never runs
to break on.

## That candidate is a dead end, resolved statically

The `r3` question can be answered without running anything. Reading `81914578`
from its start to the call site, every write to `r3` is a call return, and the
last one before the call is:

```
8191B810  bl    81944A70          ; r3 = its return value
8191B814  cmplwi cr0,r3,0
8191B818  beq   cr0,+0x14         ; skip if null
8191B81C  lwz   r4,-13952(r31)
8191B820  bl    8190F7A0          ; r3 unchanged
```

So the object whose `+0x1C` gets zeroed is **whatever `81944A70` returns** -
a freshly obtained object, null-checked on the spot. It is not the XUI context
that already exists and carries the flag.

So `8190F7A0` is initialising a new object's field, not clearing the Guide's.
The lead is closed: the fact that it never runs is irrelevant to `[dc+0x134]`,
and chasing why it does not run would have been chasing nothing.

Recording it because the shape was persuasive - the only constant-zero store to
that offset anywhere in the XUI code, in a function that never executes, called
from code touching a global eight bytes from the context pointer. Three
coincidences pointing the same way, and the answer was one instruction's worth
of dataflow away the whole time.

What remains true and unexplained: `[xui_ctx+0x1C]` is `1` by the time the
render host returns, no constant `1` is stored to that offset anywhere in the
XUI range, and nothing observed clears it.

## The flag is set before the context is published

Of the 22 functions in the XUI range that store to a `+0x1C` field, five
actually execute: `818F5288`, `818F5488`, `818FCE38`, `818FD0E8`, `81903580`.
Rather than read all five, watch the value appear.

`guide_watch_null_render` polls the context global every 250us from a host
thread and logs every transition:

```
NullRenderWatch: ctx 00000000 -> 4088A0A0
NullRenderWatch: [ctx+1C] FFFFFFFF -> 00000001
```

`FFFFFFFF` is the watcher's "not yet sampled" marker, so that second line is
the **first** read of the field, not a transition. There is no `0 -> 1` at all:
the flag already reads `1` the moment the context pointer becomes visible.

So the value is written during construction, before the object is published to
`81D6C978`. That is consistent with `818FF140` - the function that publishes
the pointer - containing no store to `+0x1C`: by the time it stores the
pointer, the field is already set by whatever built the object.

It also means this particular instrument cannot go further. A host-side watch
can only see a field once something tells it where the object is, and here the
write happens strictly before that. Narrowing to which of the five wrote it
needs the construction path read directly, not sampled.

Small negative result, but it removes an approach: the flag cannot be caught in
the act by polling, however fast.

## What the null-render flag does not depend on

The flag is computed, so the useful question is what it is computed *from*.
Two inputs can be varied cheaply, and neither moves it.

**The hardware-info word.** Logging `[ctx+1C]` read-only while varying
`xbox_hardware_info_flags`:

| `XboxHardwareInfo` | `[ctx+1C]` |
|---|---|
| `0x220` (documented) | `1` |
| `0x2A0` | `1` |
| `0x620` | `1` |
| `0x20` (Xenia default) | no context at all |

At `0x20` there is no XUI context to read - xam gates device creation on bit
`0x200`, so the render host has nothing to build on, which is the already
documented reason `0x220` is required. Above that threshold, adding bits
`0x80` or `0x400` changes nothing.

**The device.** In the deep configuration the flag is also `1`, and that
configuration has a mode-1 device with a render target actually bound. So the
presence, mode and readiness of a device do not decide it either.

So two plausible inputs are ruled out by measurement rather than argument. That
matters mainly because the reading offered earlier - that the flag is a
renderer's response to finding no usable device - now has evidence against it:
the flag is `1` whether the device is a ring-buffer-less mode-2 device or a
mode-1 device with a bound colour surface.

That reading should be treated as weakened, not merely unproven. Whatever the
flag is computed from, it is something neither of those levers touches.

## The flag is a constructor default, not a computed value

`818FF140` passes `&slot` to `818FD0E8`, which builds the XUI context:

```
81904318  addi r3,r0,44          ; 44 bytes
8190431C  bl   81944A70          ; allocate
81904320  addi r28,r0,1          ; r28 = 1, a literal
81904324  cmplwi cr0,r3,0
8190432C  beq   cr0,+0x40        ; skip on allocation failure
81904334  stw  r28,4(r3)         ; [obj+04] = 1
81904338  stw  r28,28(r3)        ; [obj+1C] = 1     <- the null-render flag
```

`[ctx+0x1C] = 1` is written unconditionally, from a literal, immediately after
the allocation succeeds. **It is a constructor default.**

### Correcting several sections above

This file has said repeatedly that the flag is "computed, not a constant",
that "no constant 1 is stored to that offset anywhere in the XUI range", and
built on those - including ruling out inputs it might be computed from. All of
that was wrong.

The cause was a bug in the scan that produced it. It walked back from each
store looking for `addi rS,r0,imm`, but broke early on any intervening store
*from* the same register:

```
81904334  stw r28,4(r3)     <- scan stopped here
81904338  stw r28,28(r3)    <- the store it was examining
```

`r28` is stored twice in a row, so the search for its constant terminated one
instruction short of the answer. The instruction it wanted was four words
further back.

So the input-varying experiments above were answering the wrong question:
nothing is computed, so of course neither the hardware-info word nor the device
moved it. Those measurements are still valid as facts, but their framing was
mistaken.

### What the question actually is

The context is born with rendering disabled, and something is supposed to
enable it. Nothing observed does. That is a different and more tractable
question than "what computes this value", and it makes the earlier candidate -
`8190F7A0`, the one function that stores a constant `0` to a `+0x1C` field -
worth another look: it takes its object from `81944A70`, the same allocator
this constructor uses, so the two plausibly operate on the same class.

## 8190F7A0 is a different class, and stays closed

The previous section reopened `8190F7A0` on the grounds that it takes its
object from `81944A70`, the same allocator the XUI context constructor uses,
so the two might operate on the same class. Reading what it initialises closes
it again:

```
819169D4  stw r30,28(r31)     ; +0x1C = 0
819169DC  stw r30,32(r31)     ; +0x20
819169E4  stw r30,4(r31)      ; +0x04
819169EC  stw r30,12(r31)     ; +0x0C
819169F0  stw r30,16(r31)     ; +0x10
819169F4  stw r30,20(r31)     ; +0x14
819169FC  stw r30,68(r31)     ; +0x44
        ... and stores at +0x48, +0x50
```

The XUI context is **44 bytes** - `addi r3,r0,44` before its allocation. An
object with fields at `+0x44`, `+0x48` and `+0x50` does not fit in 44 bytes, so
this is a larger, different class that happens to share a general-purpose
allocator.

"Same allocator, therefore possibly the same class" was too loose, and the
object size settles it in one line. The lead is closed for the second time, now
on evidence rather than on the weaker argument that dismissed it the first
time.

### Where that leaves it

The XUI context is 44 bytes, constructed with `[+0x04] = 1` and `[+0x1C] = 1`,
and nothing anywhere in xam's XUI range stores a constant `0` to `+0x1C` of an
object that size. So either the flag is cleared by a computed value somewhere,
or the Guide's context is built by the wrong constructor and a different path
would produce one with rendering enabled from the start.

Nothing here distinguishes those. What is now solid is the shape of the
question: a 44-byte object, born with two fields set to 1, one of which
disables the entire render path, and no observed code that turns it off.
