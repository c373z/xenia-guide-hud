# What would have to happen next

The Guide's software runs to completion; nothing reaches the screen. This is
what is known about the remaining gap, and what is not.

## The gap in one paragraph

xam creates a D3D device (runtime address logged as `device now 40877380`), and
that device is never connected to the GPU. It has no ring buffer, requests no
system command buffer, issues no swap, and produces no command data - its state
is byte-identical across 200 frames of drawing. On hardware the connection is
made once, by the boot path, and system software shares the GPU the title is
already using. Xenia implements the kernel as host functions, so that boot never
runs.

## What is established, with evidence

- **xam never calls the connection functions.** `VdInitializeRingBuffer`,
  `VdInitializeEngines`, `VdSetGraphicsInterruptCallback` are imported and never
  called - verified three ways: no direct `bl`, no static pointer table
  reference, no load through the import slot (phases 123, 140).
- **The title's device cannot be substituted.** xam asserts the resulting index
  is out of range three times, then faults dereferencing a pointer derived from
  it (phase 120).
- **The system command buffer is not on the Guide's path.** Neither the draw nor
  xam's device creation requests it, and `VdSetSystemCommandBuffer` is never
  called by anything (phases 115, 122).
- **The device's command region stays static.** Sampled at +0x2B00 on draw 1 and
  draw 200, byte-identical (phase 121).

## What is not known

- The layout of xam's D3D device structure beyond the +0x2B00 region observed.
- What `VdSetSystemCommandBufferGpuIdentifierAddress` is meant to write. Xenia's
  body is empty with the comment `r3 = 0x2B10(d3d?) + 8`, and the field at
  device+0x2B10 is zero in every sample - but whether filling it is sufficient,
  necessary, or even correct is unverified.
- Whether a second guest D3D device can share the title's ring at all, or
  whether the console's kernel multiplexes at a level Xenia does not model.

Do not guess at these. Three conclusions in FINDINGS.md were retracted for being
plausible mechanisms asserted without a test.

## Two routes, honestly assessed

**Model the system's GPU sharing.** Correct, and large: it means understanding
how the console gives system software access to a GPU the title owns, then
building that in Xenia's GPU layer. The pieces that already exist are
`ExecuteIndirectBuffer` and `ExecutePacket` on the command processor, which can
run PM4 from an arbitrary guest address - so a submission path is implementable
once there is something to submit.

**Run the real kernel.** What Microsoft's own emulator does; `xboxkrnlcf.bin`
ships alongside xam.xex and hud.xex in the same Flash directory. This removes
the whole class of problem - system boot, device creation, thread ownership,
timers, dispatcher objects - and replaces it with the work of running a real
kernel binary, which is a different project.

## Reproducing the current state

Default configuration, dashboard running, press the Guide key (0x08 by default).
`TRACE.md` has the expected log. If scene creation stops returning
`scene=00010000`, compare against that trace before assuming a regression
elsewhere.


## Update: the GPU side, resolved as far as the guest can take it

PRESENT.md now carries the full path from the button press to a running draw.
Summary of where it ends:

- The Guide's frame runs. XuiRenderPresent executes for real, six frames deep
  into xam's presentation code, verified by a back-chain unwind whose outermost
  frame is hud's own draw.
- Every null dereference on that path is cleared: the null-render flag
  (guide_clear_null_render), the wrong device (guide_use_bound_device), and the
  missing front buffer (guide_fake_front_buffer). Zero guest crashes.
- The screen is still byte-identical to a no-press control.

The single remaining blocker is VdGetSystemCommandBuffer, which Xenia stubs.
The guest contract for it is written out at the end of PRESENT.md, derived from
the only caller in xam. That is a Xenia GPU feature, not a Guide problem.

Two cautions carried forward:

- Any breakpoint changes scheduling enough that the draw path is never taken.
  Use host-side memory polling instead; see guide_watch_front_buffer.
- The device redirect was originally racy and some run-to-run differences
  recorded in PRESENT.md are that race, not the cvars. It is now applied per
  frame to [wrapper+12].


## Final state of the GPU question

Measured with a draw counter inside the command processor, with a control that
reads zero for a buffer of zeros:

- Deep configuration, Present executing for real: the guest draw dispatches
  **0** GPU draws.
- Stable configuration: the dashboard holds exactly 29.0 draws per swap across
  the press and 3300 composite draws, so the Guide adds none.

The Guide's software runs completely and produces GPU state but no drawing
work. Do not spend time on compositing or presentation - there is nothing being
drawn to composite. The gap is upstream: the device bring-up that mode 1 starts
and never finishes, which stalls in xam's async command buffer wait because
Xenia stubs VdGetSystemCommandBuffer.

Regression test: if any change makes the Guide render, the dashboard's draw
rate stops being exactly 5800 per 200 swaps.

## Second update: what is solid, what is scaffolding

The GPU question is settled and the file has grown a lot. Read this before
PRESENT.md, which is chronological and contains several claims that later
sections retract.

### Established with nothing fabricated

Measured in the stable six-cvar configuration from CONFIG.md:

- The Guide's software runs end to end: button, handler, Guide object, xam
  device, XUI render context, device context, scene (`scene=00010000`), and a
  full XUI frame per swap.
- **It lays out real UI geometry every frame** - screen coordinates and
  resolution values decode straight out of the memory the draw writes.
- **It dispatches exactly zero GPU draws.** The dashboard's rate holds at
  29.0 draws per swap across the press and thousands of composite draws; one
  extra draw would break the constant.
- The draw emitter `819F5D18` is never reached, because `[dc+0x134] = 1` makes
  `XuiRenderBegin` skip its device call and `XuiRenderPresent` return without
  presenting. Zero draws is the correct consequence, not a separate fault.

### Scaffolding, not findings

The deep configuration adds `create_primary_device`, `bootstrap_before_device`,
`use_bound_device`, `clear_null_render`, `fake_front_buffer` and
`syscmdbuf_fields`. Three of those hand the guest state the system would have
built: a different device, a front buffer that is a clone of the colour
surface, and display-mode fields. Anything measured there - including "the
emitter is reached and emits nothing" - describes a system partly assembled by
hand. Do not treat those numbers as facts about xam.

### The open question, in its cleanest form

`[xui_ctx+0x1C]` is `1`, and the DC copies it to `[dc+0x134]`. That single
field disables the entire render path. Known about it:

- No constant `1` is stored to that offset anywhere in the XUI range, so the
  value is computed.
- It is already `1` when the context pointer is published, so it is written
  during construction, inside `8178DC58`'s call tree.
- A host-side memory watch cannot catch it - the write precedes the moment the
  object's address becomes discoverable.
- Five of the 22 XUI functions that store to a `+0x1C` field actually execute:
  `818F5288`, `818F5488`, `818FCE38`, `818FD0E8`, `81903580`.

### Tools

`refs.py` finds direct, data and register-formed references and is the one to
use - `callers.py` sees only direct calls and produced three wrong conclusions
in this investigation. `cfg.py` for reachability; window-reading disassembly
has misled repeatedly. `fnlookup.py`, `ppcdis.py` (ghidra addresses: runtime +
0x7200 for xam, no shift for dash/hud).

### Two fixes worth upstreaming independently

`KeDebugMonitorData` and `KeCertMonitorData` were written into the block they
point at, then memset away, so both cvars had no guest-visible effect. Four
lines, unrelated to the Guide, and any title probing those variables is
affected.

## Third update: the null-render flag is characterised and exhausted

The second update named `[xui_ctx+0x1C]` as the open question and listed five
candidate writers to check. That framing is superseded - do not start there.

What the field is, established statically and confirmed at runtime:

- The XUI context is a 44-byte object identified by vtable `8163E200`.
- `refs.py` finds that vtable referenced exactly twice: constructor `818FD0E8`
  and destructor `818F7E40`. There is no second constructor.
- The constructor sets `[+0x04] = 1` and `[+0x1C] = 1` from literals,
  unconditionally.
- `[+0x1C]` is copied verbatim into `[dc+0x134]` when a device context is
  built, and that is what makes `XuiRenderPresent` return without presenting
  and `XuiRenderBegin` skip its device call.
- Of the nine vtable slots, only slot 2 touches `+0x1C`, and it is a copy
  constructor propagating it. **No method of the class clears either field.**
- The destructor asserts `[+0x04] == 0`, so external code is expected to clear
  at least that field. Neither is ever cleared in this emulator.

Why it cannot be pushed further from here:

- The writer is not in the class and not in the XUI range. Across xam, 136
  sites store a literal zero to some `+0x1C`; the offset is too common to
  isolate without class information the binary does not carry.
- Runtime observation cannot help: the code that would clear it never executes,
  so there is nothing to watch. Host-side polling confirms the field holds `1`
  from first observation to the end of a session.

Things already ruled out by measurement, so nobody repeats them: the
hardware-info word does not influence it; the device does not either (the flag
is `1` even with a mode-1 device and a bound render target); `819441D0` and
`8190F7A0` are unrelated classes despite matching size and offsets.

## Ready to upstream: upstream-monitor-fix.patch

`docs/guide-lle/upstream-monitor-fix.patch` contains the two kernel fixes on
their own, cut against `origin/canary_experimental` and verified to apply
cleanly to it (12 insertions, 4 deletions, one file).

They are not part of the Guide work and do not depend on anything else on this
branch. `KeDebugMonitorData` and `KeCertMonitorData` were each written into the
block they point at rather than into the exported variable, and the `memset`
immediately afterwards erased even that - so both `kernel_debug_monitor` and
`kernel_cert_monitor` had no guest-visible effect at all, presumably since they
were written.

Verified: with the fix and the cvars on, `KeDebugMonitorCallback` is invoked
21,411 times in a session where it was previously unreachable, and
`KeCertMonitorCallback` once. Dashboard framebuffer unchanged (`2EF6B4B7`), no
guest crashes, and both cvars still default off.

Note the branch's own copy of that file also changes `XboxHardwareInfo` to read
from `xbox_hardware_info_flags`, which is Guide-specific and deliberately
excluded from the patch.

---

## Session: running the Guide over a real title (Plants vs. Zombies)

Everything below was measured against a real game booting from an ISO, not the
dashboard. Two pieces of infrastructure made that possible:

* `guide_system_root` mounts a host directory as the guest device `SYS:`.
  Without it `lle_xam` and `guide_hud_path` have to live on `GAME:`, which is
  only the dashboard folder when the dashboard is the title; launching a disc
  makes `GAME:` the disc and both loads fail with `C0000225`.
* `guide_auto_press_seconds` fires the Guide button directly. `SendKeys` needs
  window focus the emulator does not reliably hold under automation, so the
  press was being silently dropped and the load-time `lle_guide_draw` path was
  being measured instead of the button path. Those are different code paths
  with different object conventions (`obj` vs the button handler's `obj+16`).

### Address conventions (settled, and worth not re-deriving)

`.pdata` stores **runtime** addresses. `ppcdis.py` takes **file VAs**
(runtime + 0x7200), and branch targets it prints are already file VAs. Check any
candidate against `.pdata` before trusting a disassembly - a wrong space lands
mid-function and looks like real code.

### The XUI render chain, decoded

* `XuiRenderBegin(dc, colour)` - the second argument is `0xFF000000`, an opaque
  black **clear colour**, not a surface. It is forwarded unchanged through every
  layer; nothing upstream supplies a render target.
* DC vtable is at runtime `8163E4E8`: slot18 `818F82F0`, slot19 `818F8A90`,
  slot20 `818FDDE8`, slot21 `818F9290`.
* Slot 18 reads `[dc+0x11C]` (must be 0) then `[dc+0x134]`, and dispatches
  slot 20 (`vtable+0x50`) only when `[dc+0x134] == 0`.
* Slot 20 asserts `[dc+0x1CC]` (the device) is non-null, then calls the device's
  own vtable slot 11 with the colour - it is **Clear**, not SetRenderTarget.
* `[dc+0x134]` gets its value from exactly one place: `818FDF14`
  `stw r11,0x134(r30)`, copying `[ctx+0x1C]`. `81900E70` already initialises the
  field to 0. `guide_patch_null_render` nops that store at xam load time.
  The pre-existing `guide_clear_null_render` never worked because it clears
  `[ctx+0x1C]` at bootstrap while the per-frame context is constructed later.

### Render-target plumbing

* RT0 is `[dev+0x32A0]`, RT1 `+0x32A4`, depth `+0x32B0`, front buffer `+0x3F74`,
  and `[dev+0x3F78]` is the device's **default** target.
* Real setter: `819F31A8(device, index<=3, surface)`. It asserts on index > 3
  and validates bit 30 of the surface's word 0.
* `819F4C00` is **unbind-all**: it walks RT0..RT3 and calls
  `SetRenderTarget(dev, i, NULL)` for every slot that does not equal
  `[dev+0x3F78]`. This is why poking `[dev+0x32A0]` directly does not stick -
  register the surface at `+0x3F78` as well and bind through `819F31A8`.
* The title's live render target is at `[VdGlobalDevice+0x3AC4]`. Scanning the
  title device for pointers whose `[+0x24]` unpacks as a fetch constant
  (`width = (rotl(v,14) & 0x3FFF) + 1`, `height = (rotl(v,29) & 0x7FFF) + 1`,
  the emitter's own bit ops) yields exactly one candidate and it decodes to the
  title's real resolution. `guide_bind_title_rt` binds it.

### Do not compare devices field-by-field across modules

The title and xam each statically link their own D3D build. Their device object
layouts are **not** the same struct, so reading the title's device at xam's
offsets is meaningless. Two apparent findings were discarded for this reason.
Surface objects are different - those are GPU fetch constants, hardware format,
and do transfer.

### The two device creators, fully characterised

| creator | title keeps rendering | device usable |
|---|---|---|
| none | yes | - |
| mode 2 `8178F748` (`pPresentationParameters = NULL`) | **yes** | no ring buffer, open-ended uninitialised state |
| mode 1 `8178E9F0` (real presentation parameters) | **no** | properly brought up |

The press itself is harmless - with no device creation the title runs on
untouched. Mode 1 re-points the GPU ring from the title's 1MB buffer at
`1FAE2000` to its own 4KB one.

### Dead end: mode 1 plus ring restore

The ring is only re-pointed, not destroyed, so restoring the registers looked
promising. `GuideRingSave()` / `GuideRingRestore()` on the command processor do
this non-destructively (Xenia's own `InitializeRingBuffer` memsets the ring and
resets the read index, which would be worse than not restoring).

**It does not work.** The registers were handed back exactly as saved and the
title never swapped again. It is not merely starved of a ring - its render
thread blocks and does not recover, almost certainly waiting on GPU progress
(fence / read-pointer writeback) that never advances once its packets stopped
being consumed. Restoring a register does not retroactively advance that.

Test this kind of claim in isolation before building on it. The planned
restructuring (move the draw and restore into a single `VdSwap` call) would have
failed for the same reason, since the thread is already blocked before ordering
matters.

### Where the mode-2 route stands

Faults cleared in sequence, each revealing the next: null-render gate -> clear
with no render target -> emitter's null 6th argument (`+0x3F74`) -> RT0 read
null (`+0x32A0`) -> packet emission with no command buffer -> descriptor path.

`guide_syscmdbuf_buffer_kb` / `guide_syscmdbuf_fields` both take effect and moved
the fault twice; `819FE138`, which those cvars were written for, is in the crash
unwind. The buffer is plumbed but the guest has written 0 words into it.

Patching device fields one at a time was **not converging** - six deep when it
stopped, each one something mode-1 bring-up would have done. The remaining
question is whether the system command buffer can give the mode-2 device a real
place to emit packets, since that is the one route that both preserves the title
and has been advancing.

### Not established

No GPU draws have been observed from the Guide. An earlier claim of "3 draws"
in this session was **wrong**: the baseline is 3000 per 200 swaps only after
startup settles, the first two samples vary run to run, and both deviating
intervals occurred before the draw hook was installed. `GuideDrawGPU: dispatched
0` has been accurate throughout.

### Dead end: hand-invoking xam's device bring-up routines

`guide_device_init_fn` takes a runtime address and calls it as `f(device, 0)` on
the Guide's device, so candidates can be tried without a rebuild. Both writers
of the command-buffer pointer `[dev+0x2B10]` were tried:

* `81A0F858` - returns S_OK and changes nothing. `[2B10]`, `[2B4C]`, RT0 and
  `[3F74]` all stay zero. Its writes sit behind branches that an
  un-brought-up device never reaches; only its prologue (two asserts and a
  conditional unbind-all) actually runs.
* `81A0FE48` - crashes at `81A04648` (fault `+0x4C`), unwinding through
  `81A0FF7C` which is `81A0FE48+0x134`. It takes a real second argument and
  depends on state that does not exist yet.

The generalisation: bring-up is not a routine that can be invoked on a
half-built device, it is a sequence whose steps assume each other. Picking
individual functions out of it and calling them is guesswork, and two
independent attempts failed in two different ways (silent no-op, and crash).

Anything further here needs the **contract** of `VdGetSystemCommandBuffer` -
what descriptor the guest expects and what it does with it - established by
reading `819FE138`, rather than by calling routines and seeing what happens.
`guide_syscmdbuf_fields` already supplies the two fields that function compares
(`+0x30 = 0x500`, `+0x34 = 0x5BE`); the question is what it reads after that.

### Dead end: hand-binding the command-buffer cursor

`guide_bind_cmdbuf_kb` allocates a buffer and writes it into `[dev+0x2B4C]`.
The bind applies (logged), and the crash at `81A01638` is byte-identical
afterwards. The assert at the top of `81A015B8` traps only when the caller's
cursor and `[dev+0x2B4C]` **differ**; we get an access violation instead, so
both read 0 by the time emission runs - the bound value was overwritten.

There are 11 `stw rX,0x2B4C(rY)` sites in xam. The cursor is managed per
emission, not set once, and unlike RT0 there is no "default" field to register a
value against so that a reset leaves it alone. Hand-binding cannot hold it.

### Which configuration actually reaches the draw emitter

Worth being explicit, because it is the opposite of what the device-quality
argument suggests:

* **mode 1** (properly brought-up device, RT0 bound by xam itself, front buffer
  present): render host, `XuiRenderCreateDC`, all three registrars and
  `scene=00010000` all succeed, the composite draw returns S_OK - and the
  emitter is **never entered**. Zero draws, no faults, nothing to chase.
* **mode 2 + hand-bound RT0 and front buffer**: the emitter **is** entered.
  That is what produced the successive faults at `819F5EC4`, `819F5F60` and
  finally packet emission at `81A01638`.

So the compromised-looking configuration is the one that gets the Guide drawing,
and its only remaining blocker is a write cursor that cannot be hand-bound.

No GPU draw has been observed in any configuration.

### The command-buffer protocol, and why binding it still does not hold

`81A01358(device, buffer, wordCount)` is the real setup routine:

    lwz  r11,[dev+0x2B4C] ; asserts the cursor is currently 0
    stw  r30,[dev+0x2B48] ; base   = buffer
    stw  r30,[dev+0x2B4C] ; cursor = buffer - 4
    stw  r11,[dev+0x2B50] ; limit  = buffer + count*4 - 4
    stw  r10,[dev+0x2B58] ; 4
    return cursor

The pre-decrement to `buffer-4` is why emission does `+4` **before** each store.
`guide_bind_cmdbuf_kb` now calls this rather than writing `+2B4C`, and it works
exactly as decoded (base/cursor/limit all correct, correct return).

The crash at `81A01638` is nevertheless byte-identical: emission reads a cursor
of 0 on the same device, and the entry assert does not trap, so both sides read
0 by then. The cursor is reset between our init and the emission - which the
entry assert implies by design, since it requires 0 on entry. This is a paired
begin/end protocol and the begin belongs inside the device's per-frame setup.

**Generalisation, now supported by four independent attempts** (RT0, the front
buffer, the raw cursor write, and this): every field involved is owned by a
begin/end lifecycle, and a value injected from outside that lifecycle is
discarded by it. Two of the four only appeared to work - RT0 and `+3F74` hold
because nothing resets them before the draw, not because binding them is
correct. Nothing short of running the real per-frame setup will hold, and that
setup is what mode 2 is defined to skip.

`819F4770(device)` is the routine that allocates the command buffer (4096 bytes
to `[dev+0x6340]`, 3072 to `[dev+0x6348]`) and is one of the 7 callers of
`81A01358`. Calling it directly returns 1 and leaves `[2B10]`, `[2B4C]`, RT0 and
`[3F74]` all zero - its call to the begin routine is behind a branch it does not
take on a half-built device. Third bring-up routine to no-op this way, after
`81A0F858` and (crashing instead) `81A0FE48`.

---

## A different model: let xam host the Guide (from Aurora)

Aurora, a custom dashboard that successfully shows the real Guide, imports 292
xam functions including 86 XUI ones - and **not one render entry point**. No
`XuiInit` (0x340), no `XuiRenderCreateDC` (0x34C), no `XuiRenderBegin` /
`End` / `Present` (0x34B / 0x34F / 0x353). It imports only the scene, element,
class and message ordinals (0x343-0x35F). It builds UI trees and never drives a
render loop.

What it does import is **`XamInputSendXenonButtonPress`, ordinal 0x506**. It
synthesises the Guide button press through xam's own API and lets xam host and
render the Guide.

That is the opposite of this bootstrap, which loads hud itself, calls its XUI
entry points and drives its draw loop from `VdSwap`. It also matches the one
lesson that held all through the render-side work: going *through* xam's own
paths works, injecting state from outside them does not.

### What works today

* Real xam does export 0x506; it resolves to runtime **817C4CD8**.
* That export is a thin wrapper taking ONE argument:
  `or r4,r3,r3 ; lwz r3,[81D4F610] ; b 817C2090` - the button code goes in the
  first argument and the global at `81D4F610` is its context.
* That context is null under this bootstrap, so the worker faults at `+0x10`.
  **`817C2B68()`** takes no arguments, asserts the global is still 0, allocates
  a 36-byte context and stores it. Calling it first fixes that.
* With both: `xam input ctx init -> [81D4F610] = <ptr>` then
  `xam button API(0, 400) -> 00000001`. **Accepted, no crash, and the title
  keeps rendering** (swaps continue at the normal 3000 per 200).

`guide_xam_button_api` takes the button code (0x400 = Guide) and does both steps.

### What does not happen yet

Nothing consumes the press. The draw rate stays flat at exactly 3000 per 200
swaps, no Guide appears, and no extra draws are dispatched. On hardware the
press is queued and xam's system UI thread picks it up. Earlier work already
found that thread is a problem here - the callback registry at `[815F044C]`
resolves to a null object ("registration is a no-op") and the recorded UI
thread at `81D42520` does not match the caller.

So the next question is what consumes a queued Xenon button press, rather than
anything render-side. This line is worth more than the emitter work: it does not
crash, does not disturb the title, and is the architecture the Guide is actually
built around.

### The consumer side: a complete causal chain

`XamInputSendXenonButtonPress` is consumed. xam spawns a Guide thread **in
response to the press** (not from the context initialiser - an earlier note
here claiming an ordering problem was wrong and the wait added for it has been
removed). That thread then blocks, and the whole chain is now known:

1. The thread runs a short setup, executes the table loop in `81BF8550`
   **exactly once** (proved with a counting breakpoint, not inferred from the
   absence of log lines), and that function returns normally.
2. It then blocks in `KeWaitForSingleObject(81E05528, 3, 1, 0, NULL)` - an
   infinite wait - called from `81BF8EC8`, returning to `81BF8ECC`. Confirmed
   twice over: an all-thread probe reports `lr=81BF8ECC r3=81E05528`, and the
   disassembly forms exactly that object in r29 at that call.
3. Only four functions reference the event `81E05528`. The signaller is
   `KeSetEvent(81E05528, 1, 0)` at `81BF913C` inside **`81BF90E8`**, and it is
   conditional - a `beq` at `81BF9130` skips it unless a check passes.
4. `81BF90E8` has exactly one caller: `81C0A2C0`.
5. `81C0A2C0` is never called directly. Its address is formed in a register at
   `81BF97CC` and passed as `r4` to a **virtual method, vtable slot 7**, of the
   object at `[r11+0x44]`:

        lwz  r3,68(r11)      ; the registrar object
        addi r4,r11,-23872   ; = 81C0A2C0, the callback
        lwz  r10,0(r3)       ; its vtable
        lwz  r9,28(r10)      ; slot 7
        bctrl                ; r3->slot7(r3, callback)

So the Guide thread waits on an event that is only ever signalled by a callback,
and that callback only fires if the registration through that vtable slot
actually works. Earlier work already found the callback registry at
`[815F044C]` resolving to a null object - "registration is a no-op". These are
very likely the same failure.

**This is the most tractable target found so far.** It is a closed set of five
functions and one event, with no rendering involved, and it does not crash or
disturb the title. Next: identify the registrar object at `[r11+0x44]` and what
its vtable slot 7 does under this bootstrap.

### CORRECTION: the Guide thread is not stuck

The section above traced a chain ending in "the Guide thread waits forever
because `[obj+0x130]` is 0 and the `KeSetEvent` never happens". **That is
wrong.** Measured directly with a breakpoint on the `KeSetEvent` call site
`81BF913C`:

    GuidePump hit #1 thread 01000018 r3=81E05528 r4=00000001 lr=81BF9118

The event **is** signalled, on the Audio Worker thread. So the gate passes, the
counter is not zero, and the wake path works end to end.

The thread being observed inside `KeWaitForSingleObject` is therefore normal -
it is an **idle event-driven loop**, waiting for its next event, not deadlocked.
"Parked in a wait" and "blocked forever" are not the same thing, and every
inference built on the second reading was unfounded.

How the error was made, since it is worth not repeating: each step was deduced
from the one after it rather than measured.

* "the thread is blocked" - inferred from an absence of log lines
* "the KeSetEvent never ran" - inferred from the thread being blocked
* "`[obj+0x130]` is 0" - inferred from the KeSetEvent not running
* "the loop never ran" - inferred from the error path not being JITed, when a
  *successful* run would never touch the error path either
* "a kernel call fails and skips the loop" - the call actually returns 0

Two of those were also contradicted by direct measurement when finally taken
(`r3=0` at `81BF9630`, and the `KeSetEvent` hit above). `[obj+0x130]` itself was
never measured at any point.

### What is actually established on the consumer side

* `XamInputSendXenonButtonPress` (0x506, runtime `817C4CD8`) is real, takes one
  argument, needs its context global `[81D4F610]` which `817C2B68()` creates.
* The press is **accepted** (`-> 00000001`), causes xam to spawn a Guide thread,
  and does not crash or disturb the title.
* Registration happens, the callback `81C0A2C0` fires, and the event is
  signalled. The consumer machinery works.
* No Guide appears and no draws are dispatched regardless.

So the open question is NOT why the thread is stuck. It is what else opening the
Guide requires beyond the button press - or whether `0x400` is even the right
button code for this API.

### The Aurora route works, and lands on the same wall from the correct side

Running with the hand-rolled bootstrap **disabled entirely**
(`lle_guide_draw=false`, `guide_create_xam_device=false`, only
`guide_xam_button_api` and `guide_patch_null_render` on), xam drives the whole
Guide itself. It:

* spawns its Guide thread in response to the press,
* runs registration, fires the callback, signals the event,
* JITs its own render path on the **title's** thread - `818FF2C8` (the XUI
  render host), `818FD0E8` (the XUI context constructor), `8191Bxxx`,
* calls hud's draw entry, and crashes at `819DE94C` with the unwind
  `819DEB30 8191B024 818FDE60 818F8374 818FAEB8 913EAB4C`.

That unwind is **identical** to the one the hand-rolled bootstrap produced. Two
consequences worth keeping: the reconstruction was faithful - we were driving
xam's real path, not an artificial one - and the render blocker is genuine
rather than self-inflicted.

Measured at slot 20 on xam's own objects (breakpoint at `818FDDE8`):

    r3=408936D0  lr=818F8374  [dc+1CC]=40883A70  dev=40870D00
    RT0=00000000  [3F74]=00000000  [134]=00000000

`[134]=0` confirms `guide_patch_null_render` takes effect on xam's own device
context. But xam's default device `40870D00` has no render target and no front
buffer, because nothing in this environment performs the GPU bring-up that
would give it one - and the only creator that does (mode 1) repoints the ring
away from the title, which is measured and unrecoverable.

### The likely shape of the real problem

On hardware xam has a display path independent of the running title, and the
hardware compositor merges them. Xenia models a single GPU ring owned by
whoever brought it up. The Guide needs a second rendering context the emulator
has no concept of.

Every dead end in this document is a symptom of that one thing:

* mode 1 vs mode 2 - the only properly brought-up device steals the ring
* ring save/restore - the title blocks on GPU progress that never resumes
* the begin/end lifecycles - state injected from outside is discarded because
  the owning renderer never ran its frame
* an RT-less device on both xam's own path and ours

So this is probably not a missing field or a wrong argument. Making the Guide
render likely means giving Xenia a way to run xam's rendering as a second
context and composite the result - an emulator feature, not a bootstrap fix.

What is genuinely finished: the hosting problem. xam accepts the press, spawns
its thread, and drives its own Guide bring-up, with the title still rendering
and no crash until the render target is needed.

### Ruled out: doing xam's bring-up before the title owns the ring

If the mode-1 conflict were an ordering problem, running xam's bring-up first
would fix it - the title's own `VdInitializeRingBuffer` would then take the ring
back naturally, which is the order on hardware. Tested by pressing at 12s, the
earliest the handler can function:

    GuideRing: before creator ptr=1FAE2000 size=00100000   (already the title's)
    GuideRing: CHANGED after 1s ptr 1FAE2000->1D686000

The title's ring is already up. There is no window, and there cannot be one:
the Guide button handler needs `hud.xex` loaded and registered, which only
happens once the title is running, and the title initialises its ring almost
immediately on boot.

So the conflict is not ordering, not configuration, and not a missing field.

---

## Current consolidated state

The whole stack now composes on the **xam-driven** path (our hand-rolled
bootstrap off), with no crash and the title rendering throughout:

| cvar | what it does |
|---|---|
| `guide_xam_button_api=1024` | xam hosts the Guide via `XamInputSendXenonButtonPress` |
| `guide_static_locator` | patches hud `913EB994` so the resource locator is well-formed |
| `guide_patch_null_render` | nops `818FDF14` so `[dc+0x134]` stays 0 |
| `guide_bind_title_rt` | binds the title's RT as RT0 through xam's own setter |
| `guide_fake_front_buffer` | clones RT0 into `[dev+0x3F74]` |
| `guide_second_context_kb=256` | captures what xam writes and submits it into the title's frame |

Crash progression on that path: `819DE94C` (clear, no render target) ->
`81A01638` (packet emission) -> none.

### Verified working, end to end

* press accepted, xam spawns its Guide thread, registration and callback fire,
  the wake event is signalled (all measured with breakpoints, not inferred)
* xam runs its own render bring-up on the title's thread
* scene `scnInfoUpsellLive` loads from `InfoUpsellLive.xur` with ~57 objects
* tree: root `00010000` -> scene `00010008` -> `labelHeading` `00010039`
* `labelHeading` has real layout: position (156.0, 36.0)
* render target bound (`RT0 = 40AE2160`, the title's own surface)
* every setup call returns S_OK

### The one unexplained step

The draw emitter `819F5D18` is entered every frame and constructs **no**
DRAW_INDX. xam's command buffer holds a single non-PM4 word (`0000200E`) and
nothing else, across thousands of frames.

The most specific lead: `XuiControlGetVisual` on `labelHeading` returns
`80300017` with a null visual. That element is a label - a control that
rasterises text - so it should have one. A tree that lays out correctly and has
no visuals draws nothing, which matches every symptom including the oldest
observation in this project (real layout numbers, zero draws).

Unresolved: why the visual is absent. A label's visual is text, which needs a
font; `dashroot` has `SegoeXbox-Light.xtt` and xam carries a `skin` resource
section, and neither has been shown to load. hud never calls
`XuiTextElementSetText`, though a `.xur` can carry static text, so that is
suggestive rather than conclusive.

### Corrections made while getting here

Several conclusions in earlier sections were wrong and were overturned by
measurement. Recorded so they are not re-derived:

* "the Guide thread is stuck" - it is an idle event loop; the wake event **is**
  signalled (breakpoint on the `KeSetEvent` call site).
* "the scene is empty" - it has ~57 objects; the emptiness came from dumping
  memory at `XuiObjectFromHandle`'s result against an assumed layout.
* "the visual resource isn't loading" - `XuiSceneCreate` returns S_OK and the
  tree is populated.
* "InfoUpsellLive is something our bootstrap asked for" - it is hud's own
  choice, created from `913EB940` even with `guide_create_scene=false`.

The pattern in every case: an inference from an absence (no log lines, zeroed
memory, an error code that fit the theory) that a direct API call or breakpoint
then contradicted. Asking the guest through its own API has been reliable;
reading memory against an assumed layout has not.

### Why the second context captures nothing (probable)

`guide_second_context_kb` captures the buffer at `[dev+0x30]`, whose window
xam's own begin (`81A041F0`) sets up as cursor=ptr, base=ptr+4, limit=ptr+160.
It works mechanically - the begin runs, the buffer is ours to read, the submit
path executes - and it always finds the same single non-PM4 word `0000200E`
sitting at `[ptr]`, with the entire packet area (`ptr+4` onward) zero.

The telling detail: with a render target bound, `XuiRenderBegin`'s slot 20 runs
a **Clear**, and a clear is GPU work that must emit packets. Zero words appear.
So it is not only the Guide's draws that are missing - even the clear emits
nothing into this buffer.

That points at the capture target, not the Guide. A ~144-byte window is far too
small to be a frame's draw stream; it looks like a small auxiliary/system
buffer. The real draw stream goes to the **ring buffer**, which the mode-2
device does not have - mode 2 skips `VdInitializeRingBuffer` by design.

This also reconciles the two halves that never fit together:

* mode 2 - no ring, emitter reached, emits nothing
* mode 1 - has a ring, but the emitter is never reached at all

Both are consistent with "the emitter needs a ring to target". If so,
`guide_second_context` has been faithfully capturing and submitting the wrong
buffer from the start, which explains why it never crashes and never varies.

**Now verified.** Scanning both devices for pointers into physical memory (the
rings observed live there: title `1FAE2000`, xam mode-1 `1D686000`):

    xam device   ring-like pointers: none
    title device ring-like pointers: +2A1C=1E4E4000 +36B0=1F6DC000
                                     +3910=1F6DD000 +5594=1E87C000

xam's mode-2 device references no physical memory whatsoever, while the title's
references four buffers. It has nowhere to emit a draw stream, so no Clear and
no draw can produce packets regardless of what else is correct.

**But supplying them does not fix it.** `guide_fake_ring` allocates physical
memory and writes it into `[dev+0x3B64]` and `[dev+0x3DC4]` on the mode-2
device - the two slots a mode-1 device populates - and nothing changes: the
buffer still holds only `0000200E` and no draws appear.

So the difference between the devices is real, but it is not established as the
*cause*. Either those two slots are not the ring, or the ring is not what gates
the emitter. The earlier wording here claimed this closed the question; it does
not.

What is still solid: the mode-2 device references no physical memory, a Clear
emits nothing, and `guide_second_context` captures a buffer that never receives
draws. What is unresolved: why the emitter, which is entered every frame,
constructs nothing.

### Cross-title validation

The Guide behaves identically on Plants vs Zombies and Sonic & All-Stars Racing
Transformed: same scene (`scnInfoUpsellLive`), same elements (`labelHeading`),
same handles, same absence of draws. Findings here are about xam/hud, not about
any one game.

That check also caught a portability bug worth remembering: `guide_bind_title_rt`
had a hardcoded `[title_dev+0x3AC4]` from PvZ, and on Sonic that field holds
`0x0D`, so SetRenderTarget faulted dereferencing address 13. Titles link their
own D3D builds with their own device layouts. The scan now locates the render
target by decoding `[p+0x24]` as a fetch constant and preferring a match
against `VdQueryVideoMode`'s display mode, with a warned fallback for titles
that render at a lower internal resolution.

### Three-title validation

| title | render target found | display-mode match |
|---|---|---|
| Plants vs Zombies | `[dev+0x3AC4]` = 1280x720 | exact |
| Fable III | `[dev+0x3148]` = 1280x720 | exact |
| Sonic & All-Stars Racing | `[dev+0x565C]` = 1153x609 | fallback (warned) |

Three titles, three different offsets. Any hardcoded offset is wrong on most
games - the original `0x3AC4` crashed Sonic outright - so the scan that decodes
`[p+0x24]` as a fetch constant and prefers a display-mode match is the only
portable way to find it.

The Guide itself behaves identically on all three: same scene
(`scnInfoUpsellLive`), same element (`labelHeading`), same handles, no crash,
and zero draws. Nothing about the Guide's behaviour depends on the title.

---

## Where the draws actually stop (localised)

The draw emitter `819F5D18` has exactly **one** caller, `819F7F20`, which in
turn has **eight** callers. Measured with breakpoints (armed at the button
press - see the tooling note below):

* The Guide reaches the emitter only via `819FEB78`, and that call site passes
  a **hardcoded literal zero** mask:

        81a05e44  addi r8,r0,0
        81a05e48  addi r7,r0,0
        81a05e50  addi r5,r0,0
        81a05e58  addi r4,r0,0     <- the mask
        81a05e64  bl   819F7F20

  Confirmed live: `819F7F20` and `819F5D18` are both entered with `r4 = 0`.
  This is a no-op call **by design**, not a failure.

* The call sites that pass a real, computed mask - `8191B250`, `81A0CFA0`,
  `81793E10`, `81792928` - are **never entered**. Zero hits, full-length
  unperturbed run.

So nothing in the scene generates draw calls at all. The emitter is not
failing, the device is not blocking it, and the ring is irrelevant: the render
walk never asks for anything to be drawn.

### Red herrings this retires

* the missing ring buffer on the mode-2 device
* the command-buffer capture and `guide_second_context`
* the whole "second rendering context" line

All of these sit downstream of a draw stream that was never going to have
content. They were worth building - the crashes they fixed were real - but they
could not have produced pixels.

### What this points at

`XuiControlGetVisual` on `labelHeading` returns `80300017` with a null visual.
That was set aside earlier as possibly a type mismatch; combined with "no draw
path is ever entered", the simplest consistent explanation is that the element
tree has geometry but nothing drawable attached, so the walk finds nothing to
emit.

Open question, now narrow: why do the loaded elements have no visuals, when the
scene, its registered classes and its layout are all present and correct?

### Tooling note that cost three runs

Breakpoints must be armed **after** the target has been JIT-translated.
`InstallGuideStoreTraces` was moved to xam-load time, which silently made every
probe inert - two "no hits" results were tooling artifacts, not evidence. Adding
a second install site did not help either, because the vector is static and the
early call claimed it. It is now armed only at the button press.

### The scene's full contents, and the one solid symptom

Walking the scene object's own child pointers (not just `GetLastChild`, which
reports only the last one) gives the whole page:

    00010008  "scnInfoUpsellLive"   scene, real vtable 913E23B8
      0001000E  "btnJoinLive"    GetVisual -> 80300017, null
      00010014  "btnB"           GetVisual -> 80300017, null
      00010039  "labelHeading"   GetVisual -> 80300017, null

A coherent upsell page - heading, a join button, a back button - and **not one
element has a visual**. Two are buttons, which unambiguously need visuals, so
this is not a label-specific type mismatch.

That is the one solid symptom, and it matches the localisation above exactly:
nothing has anything drawable, so the render walk never enters a draw path, so
the emitter is only ever reached through the hardcoded-zero no-op call.

### Leads that looked strong and dissolved

Recorded so they are not re-opened:

* **the missing ring buffer** - a mode-2 device really does reference no
  physical memory, but supplying buffers in the slots a mode-1 device uses
  (`guide_fake_ring`) changes nothing.
* **XUI classes not registered** - the registry at `81D6D508` is empty at hud
  load and holds **27** classes after the scene loads (an earlier note said 16;
  that came from a dump window only 16 words long). xam has 39 per-class
  registrars, and calling all of them returns `80300005` - already registered -
  38 times. Registration is complete and `GuideMain.xur` still fails.
* **`80300017` is a label type mismatch** - it is not; two buttons return the
  same.
* **`XuiInit` called with null params** - null is a legitimate path. A non-null
  block with `[0] <= 0xC` is what errors (`8000FFFF`). The normal path then
  consults a global at `81D6D0A4`, next to the provider at `81D6D0AC`, so the
  visual source comes from XUI's own state rather than init parameters.

Each of these looked convincing when first spotted and failed on closer
reading or on measurement. The pattern is worth remembering: in this codebase,
a mechanism that *could* explain the symptom is not evidence that it *does*.

### Still unexplained

Elements load with correct structure, ids and layout; none has a visual. The
scene, its classes, its locator, the device, the render target and the command
buffer are all verified working. No current lead into the visual question is
better supported than the four above were.

### Scene loading works; visuals never attach (multi-scene evidence)

`guide_scene_override` loads a named scene from hud's package directly through
`XuiSceneCreate`, so scenes other than the one hud picks can be tested.

| scene | result |
|---|---|
| `InfoUpsellLive.xur` | S_OK |
| `GamesTabSignedOut.xur` | S_OK |
| `Controller_Full.xur` | S_OK |
| `ConsoleContract.xur` | S_OK (large tree) |
| `Diagnostics.xur` | `80300013` |
| `GuideMain.xur` | `80004005` (E_FAIL) |

Four of six load. Resource loading, the locator, the package, the class
registry and the scene loader are therefore all healthy - demonstrated across
independent scenes, not inferred from one.

But every element sampled across those working scenes has **no visual**:

    ConsoleContract    "labHeading"     -> 80300017, null
    GamesTabSignedOut  "btnRedeemCode"  -> 80300017, null
    Controller_Full    "battery"        -> 8030000A, null
    InfoUpsellLive     "labelHeading", "btnJoinLive", "btnB" -> 80300017, null

So "nothing gets a visual" is systemic, not a property of the near-empty upsell
page - which was the main reason to doubt it earlier.

This joins up with a measured absence: scenes reference their imagery
cross-module (`xam://livelogo_upsell.png` is in hud's package), and the XUI
resource provider is only ever asked to open **one** thing - `strings.xus`. No
`xam://` resource is ever requested. Package contents: 360 `.xus`, 35 `.xur`,
27 `.png`.

### hud falls back because GuideMain.xur cannot be created

hud always creates `InfoUpsellLive.xur`, and `GuideMain.xur` - the real Guide
scene, present in the same package - fails with `E_FAIL` through the identical
locator that loads four other scenes. So the upsell page is a fallback, not a
choice. `Diagnostics.xur` fails differently (`80300013`), which argues against a
single systemic cause for the two failures.

### Every scene tested, and what fails

`guide_scene_override` takes a comma-separated list, so all 35 scenes in hud's
package can be tried in one run. Of 25 tested, **20 load**:

    fail: GuideMain.xur        80004005 (E_FAIL)
          GuideMainServer.xur  80004005
          MiniMediaPlayer.xur  80004005
          Diagnostics.xur      80300013
          QuickLaunch.xur      8007013D (Win32: resource name not found)

Everything else loads, including every tab-content scene (`HomeTabSignedIn`,
`HomeTabSignedInLive`, `HomeTabSignedOut`, `GamesTab*`, `SettingsTab*`,
`Options*`, `Status`, `InfoMessage`, `SmartGlassInfo`, the controller and
headset widgets, `ConsoleContract`).

The pattern: the Guide's tab **contents** all load; the top-level **containers**
(`GuideMain`, `GuideMainServer`) and one composite widget fail with the same
`E_FAIL`. So hud's upsell page is a fallback for a container that will not
create, not a choice.

Class registration is NOT the cause - see the correction above. 27 classes are
registered and every registrar reports "already registered".

### Two instrumentation flaws worth not repeating

Both made an experiment look like a negative when it had not been tested:

* the registry dump read a fixed 16 words, so it could never show registration
  growing - and it had been quoted as evidence the registry was "healthy".
* the registrar loop scored a non-zero return as failure. These functions take
  no arguments and do not return an HRESULT, so that scored every successful
  call as a failure.
