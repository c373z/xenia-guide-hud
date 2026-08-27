# What would have to happen next

The Guide's software runs to completion; nothing reaches the screen. This is
what is known about the remaining gap, and what is not.

> ## READ THIS FIRST - current state
>
> This file is append-only and roughly 3000 lines. Several early sections were
> **later proved wrong** and corrected much further down, so reading a section
> in isolation will mislead you. What follows is the state as of the latest
> entry; where it conflicts with anything below, this wins.
>
> **Verified working now**
>
> - `GuideMain.xur`, `GuideMainServer.xur` and `MiniMediaPlayer.xur` all load
>   (`XuiSceneCreate -> 00000000`). They failed with `E_FAIL` for the whole
>   history of this project until the string table was supplied.
> - The composite draw loop runs continuously, ~13-16 draws per run and still
>   going when the harness kills the process.
> - `ObInsertObject` is implemented (it was an unimplemented export, which is
>   what made xam's notification listener fail and hud give up).
> - xam is loaded once, not twice. The double load corrupted its `.text` and
>   caused every kind of intermittent failure recorded in the middle of this
>   file.
> - The DC present gates are all satisfied in the patched configuration.
>
> **Known wrong, corrected later in this file**
>
> | Early claim | Reality |
> |---|---|
> | `[dc+11C] == 0` bails out of the present | `== 0` is the **required** state; non-zero returns `8000FFFF` |
> | The emulator freezes / deadlocks loading hud.xex | It crashed and showed a **modal dialog**; the harness could not see it |
> | Scene files differ structurally, explaining `E_FAIL` | Nothing in the files distinguishes them; the cause was a missing kernel export |
> | The XUI resource provider is null and never installed | `[81D6D0AC]` now holds `81D22A54` |
> | No element anywhere has a visual | True, but only established later on a scene that actually renders; the early measurement was on unattached scenes |
>
> **The two open questions**
>
> 1. **No control has a visual**, including on the scene under active
>    composite draw. Confirmed on real controls (`btnB` is a button) across
>    several scenes.
> 2. **The present reads a different device** from the one the render-target
>    bind targets: `[dc+0x1CC]` versus the wrapper's device. Binding on the
>    present's device stops the draw loop and is disabled behind
>    `XENIA_PRESENT_RT`.
>
> **Diagnostic switches added this session** (all env vars, all off by
> default, so the normal path is unaffected)
>
> | variable | what it does |
> |---|---|
> | `XENIA_CRASH_PEEK="29,4E8,8"` | on a guest crash, dump N words at `r<reg> + <hex offset>` - for when the interesting value is a field of an object a register points at |
> | `XENIA_EFAIL_TAG="lo-hi"` | rewrite every `ori rX,rX,0x4005` in a range so each group of E_FAIL sites returns a distinct HRESULT; narrows by quartering. This is what located the scene failure in hud |
> | `XENIA_EFAIL_TAG_HUD="lo-hi"` | same, applied to hud after its DllMain |
> | `XENIA_TAG26=1` | same trick for the eight sites building `80300026` |
> | `XENIA_XAM_TRACE=1` | set xam's per-thread trace gate `[r13+0x2B4]`. Necessary but **not** sufficient - no XUI output yet |
> | `XENIA_PRESENT_RT=1` | bind RT0 on the present's device. **Harmful** - stops the draw loop; kept only because the device mismatch it addresses is real |
>
> The reusable tools are `tools/press.ps1` (run harness; detects the modal
> crash dialog), `tools/sym.ps1` (symbolize an exe RVA via the PDB, no
> debugger needed) and `tools/xuiz_extract.py` (unpack a XUIZ resource
> container).
>
> **Method note that keeps paying off:** on this problem, plausible causal
> stories have been wrong far more often than right - module-0 locators, patch
> timing, wrong object instance, case sensitivity, section lookups, and the
> gate polarity above were each consistent with all evidence at the time. Every
> one fell to a single direct measurement. Measure the thing; do not reason
> about it.

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

## The XUIZ resource container (offline analysis)

`tools/xuiz_extract.py` parses the XUIZ container embedded in hud/xam and
dumps all 34 scenes to disk. Entirely offline, so it replaces the breakpoint
probes that were perturbing the very code paths they measured.

    python tools/xuiz_extract.py work/hud17489.pe 0x21000 work/xur

XUIZ header (big-endian) at the section base (`0x21000` in hud 17489):

| Offset | Field |
|--------|-------|
| `+0x00` | `'XUIZ'` |
| `+0x04` | version (3) |
| `+0x08` | total container size (`0x28e9d`) |
| `+0x10` | directory size (`0x35ef`) |
| `+0x14` | entry count (`0x1a1` = 417) |
| `+0x1E` | directory: repeated `[u8 namelen][name][u32 size][u32 offset]` |

Gotchas, each of which cost a wrong turn:

- The directory starts at `+0x1E`, not `+0x1D` or `+0x1F`.
- The declared count is 417 but only **416** entries are real; the last
  overruns into the next section and its "size" reads back as that section's
  `XUIS` magic. The parser stops when a size exceeds the container.
- **Neither directory field locates a payload.** The offsets are not
  section-relative, and the size field *lags its name by one record* -- it
  holds the size of the **next** member, matching its own block for only 5 of
  34 entries.
- Scenes are found by scanning for `XUIB` magic. Each block declares its own
  length at `+0x0E`, which is verified against the distance to the next magic
  (34/34). With extents pinned that way the mapping is a plain 1:1 in
  directory order, and every scene's self-declared name then agrees with its
  filename (`Status.xur` -> `sceneStatus`, `GuideMain.xur` ->
  `GuideMainScene`), which is the real confirmation.

XUR header: magic `+0x00`, version 8 `+0x04`, tool version `0x000e0000`
`+0x0C`, file size `+0x0E`, section count `+0x12`. The section table follows
at the first offset >= `0x14` holding four uppercase ASCII bytes (`0x20`-`0x23`,
variable), as `(u32 magic, u32 offset, u32 size)` triplets. Offsets chain
exactly: first section starts at `table + 12*count`, each section abuts the
next, and the last ends on the file size. `STRN` is `u32 size`, `u16 count`,
then NUL-terminated strings.

### WARNING: an earlier commit reported the opposite

Commit `ff4e9c9` aligned blocks using the directory size field, which yielded
a self-consistent but **wrong** off-by-one in which every scene was named as
its predecessor. It matched 32/33 size boundaries, so it looked verified. Its
conclusions about section composition are void. The tell was content:
`MiniMediaPlayer.xur` came out full of `OptionsScene` strings. Always
cross-check the mapping against each scene's self-declared name.

### What this rules out for the `GuideMain.xur` E_FAIL

With the corrected mapping, **no structural property of the scene files
separates the 3 failing scenes from the 31 that load**:

- **Section composition.** `GuideMain` and `GuideMainServer` carry the full
  10-section set -- but so do `InfoMessage.xur` and (bar `QUAT`)
  `QuickLaunch.xur`, both of which load. `MiniMediaPlayer`'s 5-section set is
  a subset of several working scenes'.
- **Scene class.** `GuideMain`/`GuideMainServer` declare `HUDScene`, which is
  also what the working `QuickLaunch.xur` declares.
- **No class is unique to the failing scenes** -- the set difference over all
  `Xui*` strings is empty.
- **`XuiSoundXAudio`** (the initially promising lead, since both Guide scenes
  reference package-local `.xma` blade sounds) also appears in
  `QuickLaunch.xur`, which loads.
- **External media references** don't separate either: 13 loading scenes also
  reference `.png`/`.xma`, and several working scenes reference resources that
  are *absent* from the package (`sharedres://ico_32x_Mail.png`) without
  failing.
- Missing string table (no scene has a per-scene en-US `.xus`; default-locale
  strings all live in the root `Strings.xus`, indexed by scene name).

So the `E_FAIL` is **not** attributable to scene content. The next place to
look is the caller and the locator, not the files.


## BLOCKER: the emulator now freezes loading hud.xex

Every Guide experiment is currently unrunnable. The Guide Loader thread logs

    i> 01000024 Guide: loading GAME:\hud.xex

and that is the **last line written to the log**, in every configuration
tried. The freeze is global, not confined to the loader: the continuous
`DemandFunction` JIT spam from thread `F800011C` stops on the same line.

Established by elimination -- all of these stop at 5400-5580 lines:

| Variation | Lines |
|---|---|
| auto-press at 14s / 18s / 60s | 5415 / 5453 / 5461 |
| **no press at all** | 5477 |
| `guide_hud_path` = `SYS:\hud.xex` (config value) | 5461 |
| `guide_hud_path` = `GAME:\hud.xex` (CONFIG.md value) | 5575 |
| `guide_hud_path` empty (skip the load) | 5553 |
| 70s run vs 200s run | 5412 vs 5461 |

What this rules out:

- **Not a timing/JIT-warmup issue.** A 200s run logs no more than a 70s run.
- **Not the press path.** Removing the press entirely changes nothing.
- **Not the `SYS:` mount.** `GAME:\hud.xex` freezes identically.
- **Not the flags added for the scene work.** A bare run with only the
  auto-press freezes the same way.
- **Not stray processes.** No `xenia*` process survives between runs.
- **Not config drift.** All CONFIG.md prerequisites verify:
  `xbox_hardware_info_flags = 544` (`0x220`), `guide_create_xam_device`,
  `lle_xam_heap0_alias` true, `guide_use_title_device` false.

Where it hangs, precisely: the press logs `hud=loaded`, so `LoadUserModule`
returned, but `guide_handler_` is still 0 and no `Guide: DllMain entry` line
is ever written. Execution is therefore stuck inside
`FinishLoadingUserModule(hud)` at emulator.cc:3472 -- consistent with a
deadlock against a loader lock held by a title thread.

Because the press body is gated on `guide_handler_ && guide_buf_ &&
guide_out_sz_` (emulator.cc:1125), none of it runs: no `GuideScene:`
diagnostics, no `guide_scene_override`. **This is why the load-order
experiment below could not be run.**

Possibly related, seen when hud loading is skipped or uses `GAME:`: a guest
null-deref that does *not* match any signature in CONFIG.md --

    GUEST CRASH: access violation at guest PC 8175D01C, fault_addr 0
    lr=8175D020 r3=401E29A0 r4=0 r5=815F3DFC
    unwind: 81751118 81779D54 8177AA9C 8177AE48

### The experiment that is queued behind this

Nothing in the scene *files* distinguishes the 3 failing scenes, so the next
hypothesis is that the failure is **positional, not scene-specific** --
resource exhaustion partway through the list would look identical. The test
is to load the failing scenes first:

    --guide_scene_override=GuideMain.xur,GuideMainServer.xur,MiniMediaPlayer.xur,Options.xur,Status.xur

If they load when placed first, the scene files are exonerated entirely.

### Isolated: the freeze is a JIT/loader deadlock, and it needs LLE xam

Further elimination narrows the freeze above to a lock-ordering deadlock
between lazy JIT translation and module loading.

**It requires the LLE xam.** With `--lle_xam=` (Xenia's HLE xam) hud loads
straight through -- `DllMain entry=913F9D00`, `DllMain returned`, `buffers
inner=30108000 ...` -- and the run reaches **35994** lines instead of ~5500.
Everything else was ruled out first: `--lle_xam_scope=` (empty) still freezes,
so it is not the scope machinery.

**It is a deadlock, not a spin or a hang in guest code.** Sampling the frozen
process: log length pinned at 5530 across 24s while CPU crept 6.7s -> 8.3s ->
9.8s (~13% of one core, background timers), with all 62 threads in `Wait`.

**The precise stall point.** Across the whole log there are 1275
`DemandFunction: enter` lines and 1274 `defined`. Exactly one is unmatched:

    i> F80000F4 DemandFunction: enter 8186E528      <- never "defined"
    i> 01000024 Guide: loading SYS:\hud.xex         <- last line in the log

`LLE xam: DllMain returned` (thread 01000020) is already logged by then, so
xam is fully up. The stuck function is not the culprit: runtime `8186E528`
(file `81875728`) is a ~24-instruction leaf that calls `818756E0`, does some
bit twiddling and returns -- nothing a translator could hang on. Thread
`F80000F4` is therefore blocked on a *lock* inside `DemandFunction`, not on
translating that function; it is simply whichever thread happened to demand a
translation while the Guide Loader held the loader lock inside
`FinishLoadingUserModule(hud)`.

So: Guide Loader holds the module-loader lock and wants something the JIT
path holds, while `DemandFunction` holds the JIT/code-cache lock and wants the
loader lock. Classic ABBA. It is timing-dependent, which fits it appearing
only now despite an unchanged binary (verified: `xenia_canary.exe` at 13:54 is
newer than the newest source at 13:53, and the tree is clean -- a rebuild
would produce the same binary, so rebuilding is *not* the fix).

Pinning down which two locks these are needs a native stack dump of the frozen
process, which is emulator-internals work rather than Guide work.

**Workaround for unrelated experiments:** anything that does not depend on the
real xam can run with `--lle_xam=`. The scene load-order experiment cannot --
it needs LLE xam by construction.

### Refined: the contended lock is the kernel object table

The `guide_probe_threads_seconds` probe could not observe the freeze as
written, for a structural reason worth remembering: it was armed *inside* the
Guide button handler, which is gated on a handler that a freeze during hud
load never publishes. A probe armed there can never fire on the very hang it
exists to diagnose. It is now armed from `CompleteLaunch`, before the title
module load, and split into two passes -- raw registers first, then symbol
resolution -- because `LookupFunction` takes the code cache lock, which is
plausibly one of the held locks.

Armed early, the probe still logged nothing. That was itself ambiguous: a
wedged *logger* would look identical to a probe that never ran, and a wedged
logger would also explain "the log just stops" while CPU keeps ticking. So the
probe now writes progress markers to `probe.txt` with `fprintf`/`fflush`,
bypassing `XELOGI` entirely.

Result -- `probe.txt` contains exactly:

    probe: awake

and not `probe: got object table`. So:

- **The logger is fine.** The emulator is genuinely wedged, not merely silent.
- **The probe blocks in `object_table()->GetObjectsByType<XThread>()`** -- the
  kernel object table lock.

That is the lock everything piles up behind. It is held across the hud
`LoadUserModule`/`FinishLoadingUserModule` (module and thread objects are
inserted there) while that load waits on something else, and the lazily-JITing
thread `F80000F4` is stuck behind the same wall.

Still unknown: what the loader thread is itself waiting on while holding that
lock. The probe cannot answer it, because the probe needs the same lock to
enumerate threads. Getting the loader thread's own stack needs either a native
debugger (none installed -- no `cdb`, `ntsd`, or `procdump`) or a probe that
samples thread handles captured *before* the freeze rather than enumerating
them during it.

### Correction: the lock is lost ~20s BEFORE the hud load

The probe now samples without touching the object table: a cacher thread keeps
a fresh list of `object_ref<XThread>` (and wedges on the object table lock when
the hang hits, which is fine), while the sampler only reads those cached
references. Holding the refs keeps the threads alive, so the handles stay
valid. It produces data under the freeze:

    probe: awake, snapshot gen=1 threads=4
    0100000C GPU Commands          rip=00007FF9D7441B14 lr=00000000 r3=00000000
    01000010 GPU Frame limiter     rip=00007FF9D7442114 lr=00000000 r3=00000000
    01000014 XMA Decoder           rip=00007FF9D7441B14 lr=00000000 r3=00000000
    01000018 Audio Worker          rip=00007FF9D7441B14 lr=00000000 r3=00000000

The `gen` counter is the important number, and it needed a control before it
could be read:

| Run | generations in 35s | threads cached |
|---|---|---|
| healthy (`--lle_xam=`) | **35** (one per second, as designed) | 28 |
| frozen (LLE xam) | **1** | 4 |

The cacher is therefore sound. In the frozen run only the *first* enumeration
ever completed, and the snapshot holds just the four early GPU/audio worker
threads -- so the object table lock stopped being available at roughly
**t = 1s**, about twenty seconds before the Guide Loader touches hud.xex.

**This corrects the previous section.** The lock is not "held across the hud
module load". It is taken very early under LLE xam and never released; the
emulator then runs and logs normally for ~20s because nothing needs it, and
the hud load is merely the *first operation that does*. That is why the freeze
looked like it lived in `FinishLoadingUserModule` -- it is the victim, not the
cause.

The four sampled threads are all parked in ntdll (`rip=00007FF9D744....`) with
`lr=0`/`r3=0`, i.e. host worker threads with no guest context, so they are not
the holder either.

Next: find what acquires the object table lock early under LLE xam and does
not release it. The cacher already brackets it to within one second of
`CompleteLaunch`, so logging lock acquire/release around the title and xam
module loads should name the holder directly.

## ROOT CAUSE: it was never a deadlock. Xenia crashes and shows a modal dialog

The "freeze" is a **host access violation plus a modal error dialog**. Every
lock-contention finding above is real but downstream of this.

Enumerating the process's top-level windows during the "hang" finds two
visible `#32770` (Win32 dialog) windows titled **"Unhandled Exception in
Xenia"**:

| Faulting thread | Exception address |
|---|---|
| `XThread????  (F80000E4)` | `VCRUNTIME140.dll+1E78B` |
| `XThread????  (F80000DC)` | `xenia_canary.exe+223EFD` |

Both are `0xC0000005` access violations. `F80000DC` is **the same thread the
probe identified as the global critical region owner**. So the chain is:

1. A guest thread takes the global critical region.
2. It hits an access violation at `xenia_canary.exe+223EFD`.
3. Xenia's unhandled-exception handler puts up a **modal** dialog. That
   dialog's message loop runs on the faulting thread, inside
   `USER32.dll` -> `win32u.dll` -- exactly the stack the probe sampled.
4. The thread never returns, so it **never releases the lock**.
5. The rest of the emulator keeps running and logging for ~20s, until the
   first operation that needs the lock -- the hud load -- blocks forever.
6. The harness kills the process, so the log just stops.

The timing corroborates this precisely: the harness now aborts at **~1s**, and
the probe's cacher reached only `gen=1` -- i.e. the lock was lost at ~1s. The
hud load at ~20s was never the cause; it was the first victim.

This supersedes the deadlock framing in the two sections above. There is no
ABBA lock-ordering bug to find.

### The harness was blind to this

`press.ps1` only watched for process exit, and a modal dialog is not an exit.
It now enumerates windows once a second and aborts with the dialog text.

Two bugs had to be fixed to make that work, both worth knowing:

- `$ok = Wait-Or-Die ...` **captures every string the function writes**, so the
  existing `ABORT:` messages were being swallowed into `$ok` (and made it a
  truthy array). They are `Write-Host` now.
- A PowerShell scriptblock used as a P/Invoke callback cannot emit to the
  pipeline; output must be accumulated in a `$script:`-scoped variable and
  printed after the enumeration returns.

The harness now lives in `tools/press.ps1` rather than a temp scratchpad.

### Next

Resolve `xenia_canary.exe+223EFD` to a symbol (a PDB is built -- `/Zi`) and fix
the access violation. It reproduces in roughly 2 runs out of 3, at ~1s, on a
guest thread, and only with LLE xam. Fixing it should restore every Guide
experiment, including the queued scene load-order test.

### The faulting function: `XObject::GetNativeObject`

`xenia_canary.exe+223EFD` symbolizes to
**`xe::kernel::XObject::GetNativeObject+0x6D`** (`src/xenia/kernel/xobject.cc:408`).

Symbolizing needs no debugger. `dbghelp` via P/Invoke reads the PDB that sits
next to the exe -- see `tools/sym.ps1`. Two gotchas: `SymLoadModuleEx` ignores
the requested base unless a non-zero image size is passed, and
`SYMOPT_DEFERRED_LOADS` makes a failed PDB load look like a missing symbol
(`err 2`) rather than an error.

Why a fault there wedges the whole emulator:

```cpp
if (!already_locked) {
  global_critical_region::mutex().lock();     // <-- raw lock, no RAII
}
...
auto header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);
if (as_type == X_OBJECT_TYPES::UndefinedObject) {
  type = header->type;                        // <-- +0x6D faults here
}
...
if (!already_locked) {
  global_critical_region::mutex().unlock();   // never reached
}
```

The function takes the global critical region with a bare `lock()` and relies
on falling through to a bare `unlock()`. It then dereferences `native_ptr`.
`assert_not_null(native_ptr)` above is compiled out in release, so a null or
unmapped guest pointer faults *while the lock is held*, and the lock is never
released. Xenia's handler then shows its modal dialog on that thread, and the
lock is gone for the rest of the process's life.

**Note for whoever fixes this: switching to a scoped lock is not sufficient.**
The build uses `/EHsc`, under which an access violation is an SEH exception
that does not run C++ destructors, so RAII would not release the lock either.
The fix has to be to *not fault*: validate `native_ptr` (null, and mapped in
the guest heap) and return an empty ref before dereferencing the header.

Still to find: which caller passes the bad pointer. It happens ~1s in, only
under LLE xam, on guest thread `F80000DC`, so real xam is calling a kernel
export with an object pointer Xenia does not accept. Logging the guest LR when
`native_ptr` is null or unmapped should name it in one run.

### The bad pointer comes from xam's `KeWaitForMultipleObjects`

Guarding the header read in `GetNativeObject` and logging the guest `lr` names
the caller immediately, and it is the same one every run:

    GetNativeObject: refusing dispatch header (as_type=255) from guest lr=8177AC78

`8177AC78` (file `81781E78`) is the instruction after
`bl 81D1713C` = runtime `81D0FF3C` = **`KeWaitForMultipleObjects`** (ordinal
175). The register setup matches its signature: `r3`=count, `r4`=object array
at `r31+360`, then wait type/reason/mode/alertable/timeout and a stack wait
block array. That export resolves each array entry with
`GetNativeObject(..., UndefinedObject)`, which is exactly the `as_type=255`
seen above.

This is the same retry loop a previous pass already documented in the
`KeWaitForMultipleObjects` shim ("xam's mode-1 device bring-up ... retrying a
3-object wait tens of thousands of times a second"). What is new is that one
of those entries is not merely an unsupported type - it is **unmapped**, and
reading its header faults.

Two unsafe dereferences fixed, both of which faulted *while the global
critical region was held*, which is what turned a bad pointer into a
whole-emulator wedge:

1. `XObject::GetNativeObject` read `header->type` without validating
   `native_ptr` (`assert_not_null` is compiled out in release).
2. The `KeWaitForMultipleObjects` shim's own diagnostic logging then read
   `hdr[0]` unconditionally. Fixing (1) simply moved the fault here -
   `KeWaitForMultipleObjects_entry+0x35F`.

Boot now gets roughly twice as far: **~10.6k-11.2k log lines, up from ~5.7k**.

### Honest status

- The Guide is **still blocked**. Crashes remain, they are just different
  ones: `VCRUNTIME140.dll+1E78B`, and `xe::Emulator::ExceptionCallback+0x528`
  - i.e. Xenia's own exception handler faulting while handling a primary
  exception, which will hide whatever the real fault was.
- An earlier single run that reached 53k lines was **luck, not a fix**. Crash
  occurrence is intermittent per run; only multi-run samples mean anything
  here. Always run 2-3 times before concluding.
- The `GetNativeObject` guard is **knowingly over-broad**: `QueryRangeAccess`
  reports `kNoAccess` for `81D424A8`, which the shim reads successfully. That
  object is dispatch type 9, which Xenia does not implement, so it resolves to
  nullptr regardless and the outcome is unchanged - but the test is not a
  correct readability check. Narrowing it to `guest_ptr != 0 && LookupHeap()`
  was tried and brings the access violation straight back.

### The crash reporter was destroying its own evidence

`Emulator::ExceptionCallback` faulted at emulator.cc:2470, inside the
"poor-man's backtrace" that scans the guest stack:

```cpp
for (uint32_t i = 0; i < 400 && shown < 24; ++i) {
  uint32_t v = xe::load_and_swap<uint32_t>(mm->TranslateVirtual(sp + i * 4));
```

It walks 1600 bytes past `sp` with no bounds check. Every address here comes
from a thread that has *already faulted*, so none of it can be assumed mapped;
when `sp` sat near the end of its stack region the scan ran off the end and
faulted inside the handler. The back-chain walk just above had the same
problem (`[cur]` and `[caller_sp - 8]` unvalidated). Both are guarded now.

That second exception replaced the report for the first, so every crash it hit
was being reported as a handler crash instead of the real fault. Runs now
produce one dialog instead of two.

`tools/sym.ps1` also gives exact `file:line` now -- `IMAGEHLP_LINEW64` is
`SizeOfStruct@0, Key@8, LineNumber@16, FileName@24`, and the earlier guesses
of 12/16 silently produced garbage.

### Remaining crashes, and a flag that helps

- `xe::cpu::backend::x64::TrapDebugBreak+0x39` is **not a Xenia bug**: it is
  the guest's own `tw`/`twi` assert trap, which becomes a fatal dialog only
  because `break_on_debugbreak = true` in the config.
- Running with **`--break_on_debugbreak=false`** removes that dialog. Sampled
  three runs: 8.6k, 11.5k, and one that crashed not at all at **15062 lines**
  - the furthest the dashboard has booted in this whole investigation. Passed
  as a command-line flag deliberately; the saved config is left alone, since
  editing it has broken plain launches before.
- The dominant remaining crash is **`VCRUNTIME140.dll+1E78B`** (host code,
  most likely `memcpy`). It is host-side, so `ExceptionCallback` returns false
  for it and no `GUEST CRASH` report is written at all. Attributing it needs a
  host stack walk (`RtlCaptureStackBackTrace`) added to the handler's
  pass-through path.

Progress overall: boot went from ~5.7k log lines (hard wedge) to 11-15k with
runs that sometimes complete cleanly.

### The host-side crash: an unsigned underflow in the JIT

Logging a backtrace on `ExceptionCallback`'s pass-through path (host faults
previously produced **no log line at all**) gave the chain behind
`VCRUNTIME140.dll+1E78B`:

    guest JIT -> x64::ResolveFunction -> Processor::ResolveFunction
              -> Processor::DemandFunction -> PPCTranslator::Translate
              -> PPCHIRBuilder::Emit -> memcpy   [fault_addr 15CFA138000]

`ppc_hir_builder.cc:96` computes

```cpp
instr_count_ = (function_->end_address() - function_->address()) / 4 + 1;
```

with **unsigned** operands. Upstream already suspected this - the line above it
is `assert_true(address <= end_address)` with a comment "chrispy: i've seen
this one happen, not sure why" - but that assert is compiled out in release.
An end address before the start underflows to a count near 2^30, the two
`memset`s below then run over gigabytes, and it faults in `memcpy` with a
fault address around 1.5 TB. Nothing in the log said where it came from.

Guarding it names the offending function immediately, and it is the same one
every run:

    PPCHIRBuilder: refusing to translate xam 818936B8:
      end address 818936B4 is before the start
    ResolveFunction: no function for guest 818936B8 - the guest call to it
      cannot be satisfied

So a real xam function is registered with `end_address = address - 4`.

Three separate release-only landmines were involved, all the same shape - an
`assert_*` that documents an invariant, compiled out, followed by code that
relies on it:

| Site | Compiled-out assert | Consequence in release |
|---|---|---|
| `XObject::GetNativeObject` | `assert_not_null(native_ptr)` | faults under the global lock, wedges the emulator |
| `PPCHIRBuilder::Emit` | `assert_true(address <= end_address)` | unsigned underflow, gigabyte memset |
| `x64::ResolveFunction` | `assert_not_null(fn)` | null deref with nothing logged |

### Where boot stands

With `--break_on_debugbreak=false` (guest asserts no longer fatal) runs now
reach 8k-15k log lines, and some complete with no crash at all, against ~5.7k
when hard-wedged. Crashes are intermittent, so **always sample 2-3 runs**.

Next: find why xam function `818936B8` is registered with an end address
before its start. That is function discovery - `.pdata` parsing or the
XexModule function table - not the translator. Fixing it should remove the
last reproducible crash on this path.

### Root of the JIT underflow: xam code that reads as zero

`PPCScanner::Scan` ends a function when it fetches a zero instruction:

```cpp
if (!code) {
  // Don't include the 0's.
  address -= 4;
  break;
}
```

When the zero is the **first** instruction, `address` underflows to
`start_address - 4`, and `set_end_address(address)` records an end before the
start - which is precisely the corrupt bound that later underflows
`PPCHIRBuilder`'s unsigned instruction count. Fixed at source: a function whose
first instruction is zero is not a function, so the scan now fails instead of
recording negative bounds.

With that fixed the failure names a different address each build
(`818936B8`, then `81747D70`), which is the more interesting result:

**Those functions are not zero in the file.** Checking the xam image offline
at the project's usual `runtime + 0x7200` mapping, both hold ordinary
prologues:

    81747D70 -> 8174EF70  mfspr r12,8 ; stw r12,-8(r1) ; ...
    818936B8 -> 8189A8B8  mfspr r12,8 ; bl 8181494C ; ...

So real xam code reads back as `0x00000000` from guest memory. Two readings,
not yet distinguished:

1. Those pages are never populated - the XEX load leaves parts of xam's
   `.text` zero.
2. A race - functions are declared and scanned before the pages they cover
   have been written.

(2) is worth checking first because it would also explain why the crashes are
intermittent between runs, and why the address that trips it moves around.

This matters well beyond the crash: if parts of xam are missing or late,
every downstream oddity in this investigation - the unmapped wait object, the
guest asserts, the retry loops - could be a symptom of xam executing against
an incompletely loaded image, rather than separate problems.

Current remaining crash is `x64::ResolveFunction` dereferencing the null it
gets back when translation legitimately refuses (it now logs the target first,
but still cannot satisfy the call).

### Answered: it is a race, not missing pages

Of the two readings above, (2) is correct. Two measurements settle it.

**Measurement 1 - how much of xam is actually zero.** A one-shot scan of
xam's `.text` range right after it loads (now logged as `xam .text
population`) reports the same thing every run:

    47 of 1520 mapped pages are entirely zero (3.1%);
    longest zero run 35 pages at 81D3D000; first 81D14000 last 81D5F000

Those zero pages are all at the top of the range, `81D14000-81D5F000` - the
kernel import thunk and globals area (`81D0FF3C` KeWaitForMultipleObjects,
`81D424A8`, `81D4F610`, `81D6D508`). Zeros there are ordinary uninitialised
data, not a load failure. The `.text` code region is fully populated.

**Measurement 2 - the failing address moves.** The address the scanner finds
zero is different every run:

    8186E528, 81747D70, 818936B8, 818ACC98, 818AE538, 8181AB70

**None of them is inside the zero range from measurement 1.** So each is
populated by the time xam has finished loading, and the zero read is
transient: functions are being demand-scanned on guest threads while the image
is still being written.

That also explains the intermittency that has dogged this whole
investigation - which crash fires, and whether a run survives at all, depends
on which function happens to be scanned inside the window.

So the chain in full: guest threads scan xam before it is fully populated ->
a function's first instruction reads as zero -> `PPCScanner` backs up and
records `end = start - 4` -> `PPCHIRBuilder`'s unsigned count underflows ->
a gigabyte-sized memset faults in `memcpy` -> and, before the fixes above,
that fault happened while the global critical region was held, leaving the
emulator wedged behind a modal dialog twenty seconds later.

The fixes so far make every link in that chain fail loudly instead of
silently, which is why boot now reaches 11-15k lines instead of ~5.7k. They do
not remove the race itself.

Next: find why guest threads execute xam code before the module load has
finished populating it. That is the actual defect; everything else here is a
symptom of it.

### Correction: not "scanned before written" either. Still unresolved.

The previous section concluded the zeros were a load race - functions scanned
before their pages were written. Direct measurement says that is wrong, and
the honest state is that the cause is **not yet known**.

Three facts, each measured rather than inferred:

1. **The code is present at load.** Dumping the exact failing addresses right
   after xam loads gives correct code, byte-for-byte matching the image on
   disk:

       8186E528: 7D8802A6 9181FFF8 9421FFA0 9081007C 7C681B78 ...
       818936B8: 7D8802A6 4BF7A091 9421FF40 A1630000 ...
       81747D70: 7D8802A6 9181FFF8 FBE1FFF0 9421FFA0 ...

   (This also independently confirms the `runtime + 0x7200` file mapping: the
   guest bytes match what `ppcdis` shows at the mapped file VA.)

2. **The code is still present later.** The same dump at probe time (30s) is
   identical. Whole-page zero counts *fall* over the run, 47 -> 45, and the
   zero pages stay confined to `81D14000-81D5F000` (globals and import
   thunks, progressively initialised). Nothing clobbers the code region.

3. **Yet `PPCScanner` reads 48 consecutive zero bytes at those very
   addresses** partway through the run, through the same
   `memory->TranslateVirtual` path.

So the value is correct before and after, and zero in between. The load-race
story cannot explain (1): the population is already correct before `dash.xex`
is even loaded, long before the scanner trips.

Note also `LLE xam: loaded at 30013000`, which is a system-heap address, not
the `0x817xxxxx` range the code executes from - worth understanding before
theorising further, since it may mean there are two copies of the image.

What would fit: something transiently zeroes or remaps those words mid-run -
a second write pass over the image, a page being unmapped and re-mapped, or a
protection change that makes reads return zero. None of that is established.

**Do not write another confident causal story here without measuring it.**
This particular question has now produced three plausible-and-wrong answers
(missing pages, load race, clobbering). The next step is a watchpoint rather
than another inference: record the value at one of these addresses on a tight
timer from a host thread and log the transition, which pins *when* it goes
zero relative to the surrounding log lines.

### Measured: xam .text goes zero and comes back, identically

A polling watcher (`XamTextWatch`, started right after xam loads, samples four
known-affected addresses every 0.5ms) pins the behaviour down. Sample run:

    5435  XamTextWatch: 8186E528 changed 7D8802A6 -> 00000000
    5436  XamTextWatch: 81747D70 changed 7D8802A6 -> 00000000
    5444  PPCScanner: 818AE538 begins with 0x00000000; not a function
    5555  XamTextWatch: 818936B8 changed 7D8802A6 -> 00000000
    5556  XamTextWatch: 818AE538 changed 7D8802A6 -> 00000000
    5863  XamTextWatch: 81747D70 changed 00000000 -> 7D8802A6
    5913  XamTextWatch: 8186E528 changed 00000000 -> 7D8802A6
    5914  XamTextWatch: 818936B8 changed 00000000 -> 7D8802A6

Facts, all measured:

- The memory really does read zero, stably (three consecutive re-reads through
  a verified-correct host pointer, `module=xam`), so it is not a torn or
  racy read.
- It **comes back to the identical original value** (`7D8802A6`, the `mfspr
  r12,8` these functions all start with).
- Four addresses spread across ~800KB flip within a few log lines of each
  other and restore together. That is region-wide, not per-function writes.
- The scanner trips inside exactly this window - which is the whole bug.

Because the contents return unchanged, the likely shape is that reads
transiently resolve to a different (zero) mapping rather than the image being
overwritten and rewritten - but that is inference, not measurement.

Ruled out by experiment, not reasoning:

- **hud.xex load** - happens at line ~16888, long after the window.
- **Title (`dash.xex`) load** - a dump taken immediately after it shows the
  addresses still correct.
- **`lle_xam_heap0_alias`** - the project's heap-aliasing hack. Running with
  `--lle_xam_heap0_alias=false` reproduces the transitions unchanged.
- The surrounding log during the window is nothing but `DemandFunction` and
  `Invalid instruction` lines, i.e. heavy JIT translation and no other event.

Next: find what remaps or rewrites that range. Since the window is bounded now,
a page-protection trap (make the range read-only and catch the writer) or
logging every `Memory`/heap operation that touches `81700000-81D60000` during
those lines would name it. Prefer that over another hypothesis - this question
has already produced three wrong ones.

### The contents change without anyone writing them

Two experiments narrow this a lot.

**1. No heap operation touches the range during the window.** Logging
`AllocFixed`, `Decommit`, `Release` and `Protect` for anything intersecting
`81700000-81D60000` (`XamRangeOp` in memory.cc) produces entries only at load
time - one `AllocFixed 815F0000 +8C0000` and a run of per-64K `Protect`
calls - and **nothing** during the zero window hundreds of lines later.

**2. No write reaches the pages.** Setting the host pages read-only over the
code range (`XENIA_XAM_RO=1`, opt-in, default off) and letting the fault
logging catch the writer produces **no fault in the guarded range at all**,
while `XamTextWatch` still records the usual 8 transitions in the same run.

So the bytes read as zero and later read as their original values again,
without any write through that mapping and without any heap bookkeeping
change. Note the first attempt at this guard covered `81700000-81D60000`,
which includes xam's data, and immediately trapped a legitimate guest write
to `81D45A58` - so the range was narrowed to `81740000-818C0000`, which is
code only.

Leading explanation, **not yet confirmed**: Xenia maps guest memory through
more than one host view, and the write goes through an alias that the
read-only guard does not cover, or the view itself is briefly remapped to
zero-filled pages. That would explain contents changing with no fault and no
heap call. It is consistent with everything measured, which is exactly the
property the three previous wrong answers also had - so treat it as the next
thing to test, not as the answer.

How to test it: protect *every* host view of those pages rather than the one
obtained from `TranslateVirtual`, or log Xenia's view mapping/unmapping calls
(`MapViews`/`UnmapViews` and the physical-heap mirrors) with the same range
filter already used by `XamRangeOp`.

## SOLVED: xam was being loaded twice, over itself

The zero window is a **second load of xam.xex over the live image**.

`XamRangeOp` logging showed it once I stopped looking only at the first few
entries: `AllocFixed address=815F0000 size=008C0000` appears **twice** - once
at load (line 476) and again ~120 lines before the first zero transition
(line 5479), identical address and size. Re-allocating commits fresh zero
pages (hence the zeros), resets page protection to `PAGE_READWRITE` (hence the
read-only guard silently not firing - `VirtualQuery` at the moment of change
reports `protect=4`), and the loader then writes the same image back (hence
contents returning to the identical `7D8802A6`).

**What triggers it: our own Guide bootstrap.** The sequence is
`ResolvePath(\xam.xex)` immediately followed by the `Guide Loader` thread
starting. Loading `hud.xex` makes Xenia resolve hud's import of `xam.xex`, and
`KernelState::LoadUserModule` de-duplicates by **path**:

```cpp
auto name = xe::utf8::find_name_from_guest_path(raw_name);
std::string path(raw_name);
if (name == raw_name) {
  path = xe::utf8::join_guest_paths(
      xe::utf8::find_base_guest_path(executable_module_->path()), name);
}
for (auto& existing_module : user_modules_) {
  if (existing_module->Matches(path)) return existing_module;   // misses
}
```

An import of bare `xam.xex` resolves against the *executable's* directory. Our
LLE xam was loaded from `SYS:\xam.xex`, so the paths differ, the match fails,
and a second copy is loaded on top of the first.

### Confirmation

Running with `--lle_xam=GAME:\xam.xex --guide_hud_path=GAME:\hud.xex` so the
paths agree, three runs:

| | before | after |
|---|---|---|
| `AllocFixed 815F0000` | 2 | **1** |
| `XamTextWatch` transitions | 8 | **0** |
| `PPCScanner` failures | 1+ | **0** |
| log lines | 5.7k-15k | **~18040** |
| `Guide: buffers` (handler published) | no | **yes** |

Both `Guide: DllMain returned` and `Guide: buffers` now appear, so
`guide_handler_` is finally set - which is what the Guide button path has been
gated on all along.

### This was config drift, and CONFIG.md was right

CONFIG.md documents `lle_xam = "GAME:\xam.xex"`. The saved config had drifted
to `SYS:\xam.xex`. Config drift breaking a working setup has happened before
in this project; it is worth checking the saved config against CONFIG.md
before believing any new "regression".

### But do not just change the config

`SYS:` exists for a reason: it makes LLE xam usable with a *real game*, whose
`GAME:` disc obviously does not contain `xam.xex`. Switching to `GAME:` fixes
the dashboard and silently reintroduces the double-load for every real title.

The actual fix is to make the de-duplication find the already-loaded xam
regardless of the path it was loaded from - match `xam.xex` by module name, or
register the LLE xam under the path a title's imports will resolve to. Until
then, `GAME:` is a dashboard-only workaround.

## The scene load-order question, finally answered

With the double-load fixed the Guide button path works end to end -
`Guide button: pressed (user 0), handler=913E69C0` instead of the
`handler=00000000` that blocked every attempt before - so the queued
experiment could finally run.

**The failure is scene-specific, not positional.** Putting the three failing
scenes *first* in the load order changes nothing:

    XuiSceneCreate("GuideMain.xur")        -> 80004005, scene 00000000
    XuiSceneCreate("GuideMainServer.xur")  -> 80004005, scene 00000000
    XuiSceneCreate("MiniMediaPlayer.xur")  -> 80004005, scene 00000000
    XuiSceneCreate("Options.xur")          -> 00000000, scene 00010206
    XuiSceneCreate("Status.xur")           -> 00000000, scene 00010238

So resource exhaustion partway through a list is ruled out.

### Complete runtime census, 27 scenes

| Result | Scenes |
|---|---|
| `80004005` E_FAIL | GuideMain, GuideMainServer, MiniMediaPlayer |
| `8007013D` resource not found | QuickLaunch |
| `00000000` S_OK | the other 23 |

`QuickLaunch` failing with a *different* code is new information: `8007013D`
is a missing resource, and QuickLaunch is one of the scenes whose strings
reference `sharedres://` items absent from hud's package. That is a separate
failure from the E_FAIL trio.

Also confirmed on this healthy build: elements of scenes that *do* load still
have no visual - `child "labelHeading" visual -> 80300017: 00000000`,
`child "txtMessage" visual -> 80300017: 00000000`. So the missing-visual
problem is independent of the scene-creation failure and survives all of the
fixes above.

### Still no discriminator for the E_FAIL trio

Re-running the string-table diff against the *confirmed* sets (the earlier
pass used a `startswith("Xui")` filter that would have missed lowercase class
names such as `xuiButtonImageCenteredMusic`) finds **no string present in all
three failing scenes and absent from all 23 loading ones**.

A "these are host scenes that embed child scenes" idea did not survive
checking: the test keyed on each scene's second string as its class name, but
that string is often a control name (`battery`, `btnAchievements`), so it
flagged 16 loading scenes too. Not evidence either way.

What is left, given the files themselves look ordinary: the difference is in
what `XuiSceneCreate` *does* with them. The remaining approach is the one that
was blocked before by the freeze - breakpoint the `E_FAIL` construction sites
inside `8193AFB8` now that runs are stable and reach 18-24k lines, which they
never did when that was last attempted.

### Breakpointing the scene loader is still not viable - now proven by control

Worth settling, since the healthy build made it look worth retrying. It is
not, and this time it is a controlled result rather than an impression - same
flags, one variable:

| Run | `guide_trace_stores` | Result |
|---|---|---|
| A | `8193AE40,8193B454` | `StoreTrace: installed 2 breakpoints`, **zero** `GuideScene:` lines - the press fires with a good handler and the path never reaches scene creation |
| B | *(none)* | `XuiSceneCreate("GuideMain.xur") -> 80004005` as expected |

Two breakpoints are enough to stop the code path being taken at all. Do not
spend another tick on this approach.

### Static read of the E_FAIL path

Addressing note: the `.pdata`/ppcdis file VA is `runtime + 0x7200`, and xam's
PE image base is **`0x815F0000`** (these are PE32, so `ImageBase` is at
optional-header offset 28, not 24 - reading it at the PE32+ offset yields
garbage). That base is corroborated twice over: it is exactly the
`AllocFixed address=815F0000` seen at load, and `.text` then runs
`81720000-81D13CE0`, ending precisely where the zero pages began.

The two E_FAIL construction sites near the loader are, confirmed by scanning
for `lis rX,0x8000` + `ori rX,rX,0x4005`: runtime **`8193AE40`** and
**`8193B454`** (file `81942040`, `81942654`). 685 such sites exist in xam
`.text` overall.

The first one is reached like this:

```
81942014  addi r4,r1,96          ; r4 = local at sp+96
81942018  or   r3,r31,r31
8194201c  bl   819516e0          ; runtime 8194A4E0
81942020  lwz  r11,104(r1)       ; read sp+96 +8
81942024  cmpwi r11,0
81942028  bc   -> 8194204c       ; non-zero -> r3 = 0, return S_OK
8194202c  or   r3,r27,r27
81942030  bl   819382f0          ; runtime 819310F0 (release/cleanup)
81942038  stw  r11,0(r3)         ; null the out-param
81942040  lis  r3,0x8000
81942044  ori  r3,r3,0x4005      ; return E_FAIL
```

So **E_FAIL means the local at `sp+104` came back zero** from
`819516e0`/`8194A4E0`, which is called with `r3` = an object and `r4` = a
local buffer at `sp+96` that was just built by `8193AD10`/`8193AD60`
(runtime `81933B10`/`81933B60`) from `sp+80`. That shape - build a string,
then look something up with it - suggests a resource lookup returning null.

Next: decode `8194A4E0` and what it puts at `+8` of that local. That is a
lookup that succeeds for 23 scenes and fails for exactly three, so whatever it
consults is the discriminator the file contents never revealed.

### `8194A4E0` is a handle table lookup, and old registry counts are invalid

Reading the bound the lookup checks:

    GuideScene: class index bound [81D6D4F8] = 1024 (00000400)

1024 is a **handle table capacity**, not a count of registered classes. So
`8194A4E0` is the generic handle-to-object resolve (scene handles look like
`00010116`, whose low 16 bits index well inside 1024), and the earlier reading
of that code as "class index versus registered class count" was wrong. E_FAIL
therefore means the object behind the handle does not exist - the failure is
**upstream** of this lookup, not in it.

**More important, and a caution about everything measured before the
double-load fix:** the XUI registry now reports

    GuideScene: XUI registry (38 non-null of 48)

against **27** in the earlier survey. Same code, same dump, different number -
because that survey ran while xam was being loaded twice over itself. Any
conclusion drawn from guest state before commit `32508a6` is suspect and
should be re-measured before being relied on, including the "class
registration is healthy, 27 registered, all registrars report already
registered" result that was used to close off that line of enquiry.

Next: the second E_FAIL site (`8193B454`) and, more usefully, the code that
was supposed to *create* the object whose handle fails to resolve - that is
where the three failing scenes must diverge from the 23 that work.

### The E_FAIL does not come from xam's XUI code at all

Tagging trick, since breakpoints stop the path being taken: every E_FAIL site
builds `0x80004005` with `lis rX,0x8000` + `ori rX,rX,0x4005`, so rewriting
just the `ori` immediate gives each site a distinct HRESULT without moving any
code. `XENIA_EFAIL_TAG=1` does this for all 18 sites in xam's XUI region
(`81920000-81980000`), tagging them `0x80004010 + index`.

Result: **18 of 18 tagged, and `XuiSceneCreate` still returns `80004005`.**
So none of xam's XUI-region E_FAIL sites produces it. The error is either from
one of the other 667 sites elsewhere in xam, built some other way (a constant
load rather than lis/ori), or - worth checking first - returned by **Xenia's
own HLE**, since LLE xam still calls HLE kernel exports.

Note the constant is built into `r29`/`r30`/`r31` as well as `r3`; an early
version of the patch only matched `ori r3,r3` and silently skipped 5 sites.

### Aurora as a differential (user's suggestion)

`D:\USB\Aurora\Aurora.xex`. Aurora boots fine (17k lines on HLE xam), and with
LLE xam it is *healthier than dash*:

| | dash.xex (SYS: paths) | Aurora |
|---|---|---|
| `AllocFixed 815F0000` | 2 (double load) | **1** |
| `LLE xam: init complete` | yes | yes |
| `Guide: buffers` (handler) | yes | yes |
| reaches `XuiSceneCreate` | **yes** | **no** |

The double-load is dash-specific, which fits the diagnosis exactly: dash's own
directory contains `xam.xex`, so hud's import resolves to a second copy;
Aurora's directory does not.

But the test is **inconclusive on the Guide question**, because Aurora does
not get far enough to attempt scene creation. The press fires with a good
handler (`handler=913E69C0`) and then the guest sits in xam's
`KeWaitForMultipleObjects` retry loop against unmapped objects
(`81D42450`, `81D424A8`) - 1647 rate-limited spin lines in a 95s run, pressing
at 50s. Under dash the same press reaches `XuiSceneCreate`.

So Aurora does not yet answer "bootstrap or xam/XUI". To make it answer, the
wait-object spin has to be dealt with first - those two globals are in the
`81D14000-81D5F000` region that is legitimately zero at load and filled in as
xam initialises, so under Aurora something that populates them never runs.

## The E_FAIL comes from hud.xex, not xam

The tagging tool (`TagEFailSites`) rewrites the immediate of every
`ori rX,rX,0x4005` in a range so each group of sites returns a distinct
HRESULT. No code moves, so unlike a breakpoint it does not stop the path being
taken. Ranges are given by env var; `XENIA_EFAIL_TAG` tags xam at xam load and
`XENIA_EFAIL_TAG_HUD` tags hud right after its DllMain returns.

Elimination, in order:

| Range tagged | Sites | `XuiSceneCreate("GuideMain.xur")` |
|---|---|---|
| xam XUI region only | 18 | `80004005` (untagged) |
| **all** of xam `.text` | 886 | `80004005` (untagged) |
| hud `.text` | 11 | **`80004011`** |

So it is hud's own code that produces the error. That reframes the whole
question: `XuiSceneCreate` is failing because **hud refuses**, not because xam
cannot load the scene - consistent with the three failing scenes being the
ones backed by hud-implemented classes, and with the user's suggestion that
the fault is likely on the bootstrap side rather than in xam/XUI.

Mistakes worth not repeating:

- The first sweep required `lis rX,0x8000` **adjacent** to the `ori`. That
  misses 201 of xam's 886 sites, because the compiler schedules instructions
  between the pair. Match the `ori` alone.
- The constant is built into `r29`/`r30`/`r31` as well as `r3`; matching
  `ori r3,r3` exactly skipped 5 of 18 sites silently.
- `E_FAIL` appears nowhere as a stored constant in xam's `.rdata`/`.data`, and
  there is no `li rX,0x4005` + `oris` form, so the `ori` sweep is complete.
- xam's `.text` is `81718E00-81D0CAE0` at runtime (the PE gives file VAs;
  runtime is `file - 0x7200`). hud loads at runtime base `913E0000` with
  `.text` `913E6000-913FF094`, derived from its entry RVA against the observed
  `DllMain entry=913F9D00` and cross-checked against two known hud addresses.
- Do not compute which site a group tag corresponds to by hand - two attempts
  at that arithmetic disagreed with the file. The tool now logs each tagged
  address with its group.

Next: read the exact hud site the tag identifies, and work out which
precondition hud is testing that holds for the 23 working scenes and fails for
GuideMain, GuideMainServer and MiniMediaPlayer.

## ROOT CAUSE of the scene E_FAIL: `XamNotifyCreateListener` returns 0

Narrowing the tag ranges pins it to a single instruction. hud's runtime base
is **`913DF200`** (not `913E0000` - derive it from the tool's logged addresses,
not by hand; two hand calculations were wrong). Group 1 held two sites,
`913EC9BC` and `913F2810`; tagging only `913EC9BC` left the result untagged,
so the source is **`913F2810`**:

```
913F27F4  addi r4,r0,10        ; max_version = 10
913F27F8  addi r3,r0,32        ; mask = 0x20
913F27FC  bl   -> 913FE6E4     ; XamNotifyCreateListener (ordinal 0x28A)
913F2800  stw  r3,1076(r31)    ; [r31+0x434] = returned handle
913F2804  cmplwi r3,0
913F2808  bc   -> +12          ; non-zero: carry on
913F280C  lis  r30,0x8000
913F2810  ori  r30,r30,0x4005  ; zero: E_FAIL   <-- the failure
```

So `XuiSceneCreate` does not fail inside XUI at all. **hud asks for a
notification listener, gets 0 back, and gives up.** The three failing scenes
are the ones whose hud-side setup takes this path.

Xenia's HLE `XamNotifyCreateListener` (xam_notify.cc:22) always constructs an
`XNotifyListener` and returns its handle, so it should not return 0 - which
suggests the call is reaching **real xam** rather than the HLE shim (hud's
imports are redirected under `lle_xam_scope="hud.xex"`), and guest xam's
implementation is failing for its own reason. Worth confirming which of the
two actually runs before fixing anything.

That is a small, concrete target compared with everything that preceded it,
and it is on the bootstrap/kernel side rather than in XUI - which matches what
the Aurora experiment was intended to distinguish.

### Confirmed: the failing call reaches real xam, not Xenia's shim

Instrumenting Xenia's HLE `XamNotifyCreateListener_entry` to log every call
(with mask, max_version, process type, returned handle and caller `lr`) gives
**zero calls** across a full run in which `GuideMain.xur` still fails with
`80004005`.

So the guess in the previous section is right, and it matters: hud's import is
redirected to the guest implementation under `lle_xam_scope="hud.xex"`, so
**real xam's `XamNotifyCreateListener` is what returns 0**. Fixing Xenia's HLE
shim would have changed nothing - it is never invoked on this path.

That also means the failure is guest-side behaviour inside real xam, reached
with `mask=0x20, max_version=10`, and the question becomes why xam's own
implementation refuses. Worth checking first whether it depends on
notification state that our bootstrap never sets up, since that would fit the
"three scenes that need hud-side setup" pattern.

## SOLVED: `ObInsertObject` is unimplemented in Xenia

The full chain, every link measured rather than inferred:

1. hud's scene setup calls **`XamNotifyCreateListener(mask=0x20,
   max_version=10)`** (xam ordinal `0x28A`, runtime `817680C0`).
2. That is **real xam**, not Xenia's HLE shim - instrumenting the shim logs
   **zero** calls on a run that still fails.
3. xam's implementation creates the listener with **`ObCreateObject`**, which
   **succeeds**: `factory=81D22460 tag=66746F4E ('Notf') size=48 -> status
   00000000 object 301C9018`. The pool allocator is fine too - no "system heap
   exhausted" anywhere.
4. xam then calls **`ObInsertObject`** (xboxkrnl ordinal `0x108`/264) to turn
   that object into a handle. The log says:

       !> undefined extern call to 81D0FDBC ObInsertObject

   exactly once, matching the single `ObCreateObject` call. `ObInsertObject`
   appears **only** in `xboxkrnl_table.inc:278`; there is no implementation
   anywhere under `src/xenia/kernel/`.
5. With no handle, xam cleans up via `ObDereferenceObject` and returns **0**.
6. hud tests that result at `913F2810`, sees zero, and returns **`E_FAIL`**.
7. `XuiSceneCreate("GuideMain.xur")` -> `80004005`.

So the Guide's scenes fail on a **single missing kernel export**. That is why
nothing in the scene files ever distinguished the three failing scenes: the
files were never the problem. The three that fail are simply the ones whose
hud-side setup needs a notification listener.

### Why this took so long to find

Every earlier probe was aimed at XUI, and the error is not raised there. The
tagging tool is what finally located it, by rewriting E_FAIL immediates so the
returned HRESULT names its own construction site - all 886 sites in xam came
back untagged, which is what forced attention onto hud.

### Next

Implement `ObInsertObject`. Xenia already has the pieces: `ObCreateObject`
builds the object with an `X_OBJECT_HEADER`, and the object table hands out
handles elsewhere. The export needs to take the object pointer, allocate a
handle for it, and return the handle. Once it exists, re-run the scene census
- `GuideMain`, `GuideMainServer` and `MiniMediaPlayer` should stop returning
E_FAIL, and the queued question of why no element gets a visual becomes the
next thing worth measuring.

### Implementing `ObInsertObject`: constraints and the precedent to follow

Signature, from the call site in xam (`8176EB10`): `r3` = object pointer,
`r4` = attributes (0), `r5` = desired access (0), `r6` = out handle pointer.
So `NTSTATUS ObInsertObject(PVOID Object, POBJECT_ATTRIBUTES, ACCESS_MASK,
PHANDLE)`.

Why it cannot simply reuse `ObOpenObjectByPointer`: that resolves through
`XObject::GetNativeObject`, which only understands objects carrying a
dispatcher header it recognises (Event, Mutant, Semaphore). xam's notification
listener is not one of those - the same lookup already logs
"unsupported dispatcher type" for it - so it returns null.

Nor can it use `XObject::SetNativePointer`, which is how objects normally
record their handle: that calls `StashHandle` into the object's dispatch
header and carries an explicit `FIXME: This assumes the object has a dispatch
header (some don't!)`. Writing there would corrupt the listener.

**There is already precedent for exactly this problem in the same file.**
`xobject.cc` keeps a `GuestTimerTable()` - a guest-address to handle map -
introduced because adopted guest timers "cannot be stashed in the object
itself". `ObInsertObject` should do the same: keep a side table keyed on the
guest object pointer, so repeated inserts return a stable handle and nothing
is written into guest memory.

Remaining design question: `ObjectTable::AddHandle` needs an `XObject*`, and
the natural candidate for this caller is `XNotifyListener` (the factory's pool
tag is `'Notf'`). Whether to construct a real listener - which would also make
Xenia's own notification broadcast deliver into it, alongside guest xam's own
handling - or an inert placeholder is a semantics choice worth making
deliberately rather than by accident.

Risk assessment: the export is currently an *undefined extern*, so it does
nothing and returns garbage today. Nothing can depend on its present
behaviour, and no title that never calls it can regress. That makes this a
low-risk addition.

## `ObInsertObject` implemented - hud now gets its handle

Two changes:

- `xobject.h`: `set_guest_object_no_stash()`. `SetNativePointer` is the normal
  way an XObject records which guest object it wraps, but it stashes the handle
  *into the object's dispatch header*, and it carries an upstream `FIXME`
  admitting not every object has one. xam's notification listener does not, so
  stashing would corrupt it.
- `xboxkrnl_ob.cc`: `ObInsertObject(Object, Attributes, DesiredAccess, Handle)`.
  Creates an `XObject` wrapper, records the guest pointer without touching
  guest memory, allocates a handle via `ObjectTable::AddHandle`, and keeps a
  guest-pointer -> handle side table so re-inserting the same object returns
  the same handle instead of accumulating wrappers. The side table follows the
  `GuestTimerTable` precedent already in `xobject.cc`.

It deliberately does **not** go through `XObject::GetNativeObject` the way
`ObOpenObjectByPointer` does: that reads a dispatcher type out of the object's
first bytes, so for a header-less object it would interpret arbitrary bytes as
a type and could quietly build an `XEvent` around something that is not an
event.

Verified working:

    ObInsertObject: guest object 301C9018 -> handle F8000308
    ObInsertObject: guest object 301CD018 -> handle F8000310
    ObInsertObject: guest object 401EC9B0 -> handle F8000318

`301C9018` is the same object `ObCreateObject` reported creating, and the
import table no longer marks `ObInsertObject` with `!!`, with zero
"undefined extern call" lines for it.

### The next blocker (progress, not regression)

hud no longer gives up early. It now runs *into* scene creation and faults:

    GUEST CRASH: access violation at guest PC 913FA204, fault_addr 0000000100000000
    lr=913E8D38
    unwind: 913E9520 913E9AEC 913E8858 913E87BC 8194A070 8194A5A0
            81935040 81937D9C 8193B2BC 8193B3A4

`fault_addr` is the membase, i.e. a guest null dereference. The unwind runs
from xam's XUI scene loader (`8193B3A4` down through `8194A070`) into hud, so
hud is being called back during scene construction and dereferences null.
`913FA204` is hud RVA `0x1B004` (base `913DF200`), file VA `9801B004`.

This is the expected shape of clearing one blocker and meeting the next.

### Next blocker: `XuiLookupStringTableByIndex` returns null, hud copies it anyway

The crash after the `ObInsertObject` fix is at hud `913FA204`:

```
913FA200  (loop entry)
913FA204  lhz  r10,0(r4)      <<< faults, r4 = 0
913FA210  addi r4,r4,2
913FA214  bc   -> loop
```

That is a UTF-16 string copy called with a null source. The caller
(`lr=913E8D38`) is:

```
913E8D20  addi r4,r0,40        ; string index 40
913E8D24  lwz  r3,1256(r29)    ; the string table object
913E8D28  bl   -> 913FE134     ; XuiLookupStringTableByIndex (ordinal 0x397)
913E8D2C  or   r4,r3,r3        ; result used as the source...
913E8D30  addi r3,r1,80        ; ...into a local buffer
913E8D34  bl   -> 913FA200     ; wide strcpy - no null check
```

so hud asks for string 40, gets null, and copies from it without checking.

The import is **bound to real xam**, not left as a stub: there is no
"undefined extern call to 913FE134" in the log. The `!!` beside it in the
import dump only means *Xenia* has no HLE implementation, which is irrelevant
while LLE xam provides one. So real xam genuinely found no string at index 40.

That points at hud's string table not being populated for this scene -
`Strings.xus` is in the package and 23 scenes load through the
`section://<module>,hud#strings.xus` locator, so the resource resolves; what
is missing is whatever binds a string table to the object at `[r29+0x4E8]`.

Note this is a real hud bug that hardware never hits, because on a console the
lookup always succeeds. It cannot be worked around by making the lookup return
an empty string without understanding what hud does with it next.

### Where hud's string table comes from - and why it stays null

Tracing the field hud dereferences (`[obj+0x4E8]`):

- **48 loads, 2 stores** in hud `.text`, and *both stores write zero* -
  `913EC608` is the constructor zeroing it, `913EC69C` the cleanup path. So
  nothing assigns the table by direct store.
- Exactly one place takes its address: `913EC798  addi r3,r31,1256` followed
  by `bl 913EA7D8`, a smart-pointer assign helper (release old, then store
  new).
- That helper's payload is `XuiLoadStringTableFromFile` (ordinal `0x342`),
  called as `(locator, &field)`, and the locator it is handed was built
  immediately before by **`XamBuildDynamicResourceLocator`** (ordinal `0x31E`)
  at `913EC790`, into the local at `sp+96`.

So the field is only ever non-null if `XuiLoadStringTableFromFile` succeeds,
and its return value is **not checked** by the helper. A failed load leaves the
field zero, which is exactly the state the crash exposes.

This lands next to a defect the project already knows about. `guide_static_locator`
exists because hud's *dynamic* locator builder yields `section://@0,...` with
a module of 0. hud imports **both** builders - `XamBuildDynamicResourceLocator`
(`913FE8B4`) and `XamBuildResourceLocator` (`913FE8C4`) - and the string-table
path calls the dynamic one. The existing patch at `913EB994` forces the static
builder at a *different* decision point, and the log confirms it applied
(`Guide: patched hud 913EB994 409A0020 -> 60000000`), so it does not cover
this call site.

**hud's runtime base is `913DF200`.** Confirmed twice over: the tagging tool's
logged addresses, and now that the `913EB994` patch matched its expected
opcode, which lands at RVA `0xC794` only under this base (under the earlier
`913E0000` guess that RVA holds `38610050` instead). Worth stating plainly
because a wrong base silently disassembles the wrong instructions.

Next: confirm what locator the dynamic builder actually produces for this
call, and whether forcing the static builder here too - or supplying the
module it wants - makes `XuiLoadStringTableFromFile` succeed.

### A second locator chooser exists - patching it did NOT fix the crash

hud picks between the static and dynamic locator builders in more than one
place, and they are textually identical:

```
913EC748  lwz   r3,8(r31)        ; [obj+8], the dynamic module
913EC754  cmpwi cr6,r3,-1
913EC75C  bc    4,26 -> dynamic  ; != -1 takes XamBuildDynamicResourceLocator
          (fallthrough          -> XamBuildResourceLocator, module [obj+4])
```

`913EC75C` holds the same `409A0020` as the already-patched `913EB994`, and it
is the chooser on the **string-table** path - the one feeding
`XuiLoadStringTableFromFile`. `guide_static_locator` now nops both sites, and
both report success:

    Guide: patched hud 913EB994 409A0020 -> 60000000
    Guide: patched hud 913EC75C 409A0020 -> 60000000

**The crash is unchanged.** Same PC `913FA204`, same `lr=913E8D38`, same
unwind. So forcing the static builder on this path is not sufficient - stated
plainly because the patch is being kept (it addresses the same documented
defect at a site that was simply missed) and a later reader should not assume
it helped.

Why it probably still fails: the static builder takes its module from
`[obj+4]`, and the bootstrap only sets that on the object it creates itself -
`GuideBootstrap: [guide+4] = skin module 301B3000`. The object in this path is
whatever `r31` holds when hud loads its string table, which need not be that
one.

Next: identify the object `r31` points to here and whether its `+4` carries a
module at all. `XuiLoadStringTableFromFile` (runtime `81937AF0`) fails early -
its first call, the resource resolve at runtime `8196BEC0`, returns a negative
status and it bails - so confirming what locator string reaches that call is
what settles it.

### Xenia's HLE locator builder is reached, but not on hud's string-table path

Xenia does implement `XamBuildResourceLocator` (xam_info.cc), and it matters
how: a module of **0** silently produces `file://media:/{container}.xzp#{res}`
instead of a `section://` locator - a path that does not exist here, which
would fail a load exactly the way hud's string table does.

Logging every locator it builds shows it *is* being called:

    XamBuildResourceLocator #1: module=30013000 -> "section://30013000,shrdres#loadingRing.png"
    XamBuildResourceLocator #2: module=30013000 -> "section://30013000,shrdres#B-Button_32.png"
    #3, #4 likewise, all shrdres

`30013000` is the LLE xam module handle, and all four are shared-resource
lookups. **None is hud's `strings.xus`.** So hud's own call on the
string-table path does not reach Xenia's HLE - its import is bound to real
xam - and the module-0 theory cannot be confirmed or refuted from the HLE
side.

That also explains why the `913EC75C` patch changed nothing observable here:
whichever builder hud selects, the call lands in guest xam, not in the code
being logged.

### Two more eliminations on the null string table

**The HLE locator route is closed.** Every call into Xenia's
`XamBuildResourceLocator` carries `lr=92181B34`, an address in the `0x92......`
range - that is **dash.xex**, the title. hud never reaches it. So its imports
are bound to real xam on this path, and nothing about hud's locator can be
observed or fixed from the HLE side. The module-0 behaviour in Xenia's builder
is real but irrelevant here.

**Patch timing was not the problem either.** The locator patches used to be
applied *after* hud's DllMain returned, so if hud loaded its string table
during initialisation they would have arrived too late. They now run before
DllMain - confirmed in the log:

    17329  Guide: patched hud 913EB994 ...
    17330  Guide: patched hud 913EC75C ...
    17331  Guide: DllMain entry=913F9D00

and the crash is **byte-identical**: same PC `913FA204`, same `lr=913E8D38`,
same registers. The reordering is kept because patching before the patched
code can run is plainly more correct, but it fixed nothing and should not be
recorded as having done so.

### Leading hypothesis now: wrong object, not a failed load

Worth stating because the last three attempts all assumed the string table
*load* fails. There is a simpler possibility that fits every observation: the
load succeeds onto one object, and the crashing code reads `+0x4E8` from a
**different instance**. The crash site takes its table from `[r29+0x4E8]`
where `r29` is whatever object is being constructed for the scene, while the
initialisation at `913EC7xx` populates `[r31+0x4E8]` on hud's own object.
Nothing so far establishes those are the same object.

That is testable without breakpoints: the two stores that zero the field are
at `913EC608` (constructor) and `913EC69C` (cleanup), so counting constructor
runs against string-table loads would show whether more objects exist than
tables.

### The table pointer really is null, and the object is the right class

`XENIA_CRASH_PEEK` (new, env-gated) dumps a register-relative window from the
guest-crash handler, for when the interesting value is a *field* of an object
a register points at rather than the register itself. Format
`"<reg>,<hexoffset>,<words>"`, e.g. `29,4E8,8`.

At the crash, `r29 = 401EAFB0` and:

    GUEST CRASH: peek r29+4E8 = 401EB498: 00000000 00000000 00000000 00000000
                                          000004C7 00000000 00000000 401EBA30

Two things follow:

- **`[r29+0x4E8]` is genuinely null.** So the alternative reading - table
  present, index 40 simply absent - is refuted. hud really does look up a
  string in a null table.
- **`r29` is an instance of the right class.** `+0x4F8` holds `0x4C7` (1223),
  which is exactly what the constructor at `913EC5E4`/`913EC610` writes
  (`addi r9,r0,1223 ; stw r9,1272(r31)`). So this is the same type as the
  object whose string table is loaded at `913EC798` - just, apparently, a
  different instance.

### Why the locator patches were always beside the point for this object

The bootstrap already forces the static path *by data*, not by patching:

```cpp
xe::store_and_swap<uint32_t>(... guide_bs_obj_ + 4, guide_bs_skin_module_);
xe::store_and_swap<uint32_t>(... guide_bs_obj_ + 8, 0xFFFFFFFFu);  // static
```

Setting `[obj+8] = -1` makes the `cmpwi r3,-1` chooser fall through to the
static builder, and `[obj+4]` supplies its module. That is the same effect the
`913EB994`/`913EC75C` nops produce - but it is applied to
**`guide_bs_obj_` only**. Any other instance of the class keeps the
constructor's zeros, takes the dynamic path, and gets `section://@0,...`.

So the open question is now sharp: is `401EAFB0` the bootstrap's own object or
a different one? A run logging `guide_bs_obj_` alongside the crash registers
answers it directly.

### Refuted: it is the same object

Logging the bootstrap's object alongside the crash registers settles it:

    GuideBootstrap: guide object 401EAFB0, [guide+4] = skin module 301B3000
    GUEST CRASH: r24-r31 ... 401EAFB0 401EBA30 401EBC28      (r29 = 401EAFB0)

`r29` **is** `guide_bs_obj_`. So the wrong-instance hypothesis from the
previous section is wrong, and with it the idea that some other instance keeps
the constructor's zeros. On this exact object the bootstrap set `[+4]` to
hud's hmodule and `[+8]` to `-1` (static path), both locator choosers are
nopped, and `[+0x4E8]` is still null.

That leaves only two possibilities, and they are now cleanly separated:

1. the string-table load at `913EC798` **never runs** for this object, or
2. it runs and **fails**, leaving the field the constructor's zero (the assign
   helper ignores the return value, so a failure is silent).

Distinguishing them does not need a breakpoint in the XUI path: the load site
is in hud's own init, not the hot scene loader, so a counter on
`XuiLoadStringTableFromFile` - or simply checking whether `[guide+0x4E8]` is
ever non-null at any point, by polling it the way `XamTextWatch` polls xam -
answers it. Polling is the safer of the two given how often breakpoints have
perturbed this path.

Note for whoever picks this up: three successive hypotheses here (module 0 via
the HLE builder, patch timing, wrong instance) each explained every
observation available at the time and were each refuted by one direct
measurement. Measure before building on one.

### The load runs, with the right inputs, and still fails

Four measurements this round, each cheap and each closing something off.

**The loader really is called.** Xenia JITs on first call, so a
`DemandFunction: enter` is proof of execution - no breakpoint needed. Both
`XuiLoadStringTableFromFile` (`81937AF0`) and its internal resource resolve
(`8196BEC0`) appear. So the earlier "maybe it never runs" branch is closed:
**it runs and fails.**

**Timing is fine.** Ordering in one run:

    17372  Guide: DllMain returned
    17389  DemandFunction: enter 913EC578      (hud init function)
    18849  GuideBootstrap: guide object 401EAFB0, [guide+4] = 301B3000
    19209  DemandFunction: enter 81937AF0      (the loader, first call)

The loader first runs at 19209, *after* the module was set at 18849. So it had
the right `[obj+4]`.

**The locator inputs are correct.** Read out of live guest memory rather than
computed from the file (hud's extracted image has relocations applied, so
file-offset arithmetic for its `.rdata` produced mid-string garbage):

    hud container@913E1B24 = "hud"; [91400160] = 913E16AC -> "strings.xus"

**The section itself resolves.** With `XexGetModuleSection` logging promoted
from `XELOGD`:

    XexGetModuleSection: module='hud' section='hud' -> 00000000 size=167581

Status 0, and `167581` is exactly the XUIZ container size measured offline
(`0x28E9D`). So hud's resource section is reachable and correctly sized.

But note: that is the **only** `hud` section lookup in the run. If both the
scene path and the string-table path needed it, there should be two. So the
string-table load appears to fail *before* it gets as far as asking for the
section - which points at xam's locator parsing or its container walk rather
than at anything Xenia provides.

Next: attribute that single lookup to a caller (log `lr` alongside it). If it
belongs to the scene path, the string-table load never reaches the section at
all, and the failure is upstream in xam's handling of the locator string.

### Correction, and two more theories killed

**Correction to the previous section.** Attributing the section lookups with a
caller `lr` shows both come from the same xam resolver:

    XexGetModuleSection: module='hud' section='hud' -> 00000000 size=167581 lr=8196ED90
    XexGetModuleSection: module='xam' section='xam' -> 00000000 size=65728  lr=8196ED90

So the earlier inference - that only one `hud` lookup means the string-table
load never reaches the section - was wrong. A single lookup is what a resolver
that **caches** the container looks like, and the caller is xam's resource
code either way. The section is reached and resolves cleanly.

**Case sensitivity is not the problem.** The XUIZ directory holds
`Strings.xus` with a capital S while hud asks for `strings.xus`, which looked
like a strong candidate - and a plausible mechanism existed (a case-insensitive
compare needing an upcase table that an uninitialised xam might not have).
Testing it directly with the scene override:

    ("Options.xur") -> 00000000, scene 00010042
    ("options.xur") -> 00000000, scene 00010074
    ("OPTIONS.XUR") -> 00000000, scene 000100A6

All three load. The lookup is case-insensitive, so the theory is dead.

### Where this stands

Everything checkable about the inputs now checks out: same object, module set
before the load, correct container and resource strings, section resolves at
the right size, lookup case-insensitive, loader definitely executed. And
`XuiLoadStringTableFromFile` still fails, leaving `[guide+0x4E8]` null.

The one thing still *assumed* rather than observed is the locator string
itself. It has been inferred as `section://301B3000,hud#strings.xus` from the
static builder's inputs, never read. That is the next thing to measure, and
the honest next step - given that on this problem every inferred step has
eventually turned out to be the wrong one.

## SOLVED: the E_FAIL trio loads

The locator string was the last thing still inferred rather than observed, and
the way to settle it was to stop reasoning and **call the loader directly**.
Invoking `XuiLoadStringTableFromFile` (ordinal `0x342`) from the bootstrap with
a locator built from the skin module:

    XuiLoadStringTableFromFile("section://301B3000,hud#strings.xus") -> 00000000, table 408B66C0
    XuiLoadStringTableFromFile("section://301B3000,hud#Strings.xus") -> 00000000, table 408BC790

All succeed. So the loader was never broken and the locator format was right -
hud's own call is what fails. Rather than keep chasing why, the bootstrap now
loads the table itself and stores it where hud expects it:

```cpp
// after [guide+4] = skin module
uint32_t lst = xam->GetProcAddressByOrdinal(0x342);   // XuiLoadStringTableFromFile
... build "section://<skin module>,hud#strings.xus" ...
processor->Execute(ts, lst, {locator, &out}, 2);
store_and_swap<uint32_t>(guide_bs_obj_ + 0x4E8, table);
```

    GuideBootstrap: string table "section://301B3000,hud#strings.xus" -> 00000000,
                    table 40899E90, [guide+4E8] now 40899E90

**Result - the three scenes that never loaded now load:**

| Scene | Before | After |
|---|---|---|
| `GuideMain.xur` | `80004005` | **`00000000`**, scene `00010042` |
| `GuideMainServer.xur` | `80004005` | **`00000000`**, scene `00010155` |
| `MiniMediaPlayer.xur` | `80004005` | **`00000000`**, scene `00010219` |
| `Options.xur`, `Status.xur` | ok | ok |
| `QuickLaunch.xur` | `8007013D` | `8007065B` |

The `913FA204` null-string crash is gone too - it was the same missing table.

`QuickLaunch` still fails, with a *different* code than before (`8007065B`
rather than `8007013D`), consistent with its references to `sharedres://`
items that are absent from hud's package. That is a separate problem from the
E_FAIL trio.

### Why this took so long

Every hypothesis about *why* hud's load fails - module 0, patch timing, wrong
instance, section not reached, case sensitivity - was consistent with the
evidence and wrong. The step that worked was not another explanation but a
direct call to the function under suspicion with known-good inputs, which
answered "is the loader or the caller at fault" in one run.

Note what is **not** fixed: hud's own string-table load still fails silently.
The bootstrap now papers over it by supplying the table. That is worth
revisiting if the Guide misbehaves later in ways that trace back to hud
believing it owns that table.

### First look at a GuideMain that actually loads

With the scene finally creating, its tree can be walked. Recursing
`XuiElementGetLastChild` (hud imports no sibling accessor, so this follows one
branch only):

    override XuiSceneCreate("GuideMain.xur") -> 00000000, scene 00010042
    override last child 0001014D
      depth 0 node 0001014D id "" visual -> 8030000A: 00000000

So the scene exists, has a child, and that child has an empty id, no children
of its own, and no visual.

Two cautions before this becomes the next theory:

- **One branch is not the tree.** `GetLastChild` follows a single path; the
  XUR analysis put ~57 objects in this scene. A single childless node says
  nothing about the other branches. Enumerating properly needs
  `XuiElementGetChildById` (ordinal `0x32A`, which hud does import) against ids
  taken from the scene's own `STRN` - `Blade_Center`, `txt_Games`, `Blade3`,
  `ringOfLight_Group` and so on are already extracted in `work/xur/`.
- **`8030000A` may not mean "no visual".** `XuiControlGetVisual` is a *control*
  accessor; asking it about a plain element or group could just as easily be
  reporting "not a control". Earlier sessions read this code as proof that
  nothing anywhere has a visual, which was always weaker evidence than it
  looked - and it was measured on the near-empty upsell page.

Next: query known ids from GuideMain's string table and check visuals on
elements that are actually controls, before concluding anything about why
nothing draws.

### Elements do exist, and they genuinely have no visual

Querying by id with `XuiElementGetChildById` (ordinal `0x32A`) against ids
taken from GuideMain's own `STRN`, on the now-loading scene:

| id | lookup | handle | `XuiControlGetVisual` |
|---|---|---|---|
| `btnB` | `00000000` | `00010091` | `80300017` |
| `imgHeadsetBattery` | `00000000` | `0001009E` | `8030000A` |
| `Header` | `00000000` | `000100B5` | `80300017` |
| `Blade_Center` | `80300017` | - | not found |
| `txt_Games`, `Label_Head`, `ringOfLight_Group`, `Tab1`, `Blade3` | `80300017` | - | not found |

Two things this settles, and one it opens:

- **The scene has real elements.** `btnB`, `imgHeadsetBattery` and `Header`
  resolve to live handles. Earlier readings of "the scene is empty" or "one
  childless node" were artefacts of walking a single `GetLastChild` branch.
- **The no-visual result is real this time.** `btnB` is a button - an actual
  control - so `80300017` from `XuiControlGetVisual` is not the "asked a
  non-control" case that made the earlier evidence weak. A genuine control in
  a properly loaded scene has no visual.
- **The blade and tab ids are absent.** `Blade_Center`, `Tab1`, `Blade3`,
  `txt_Games` all fail lookup, while the outer chrome resolves. Those are the
  parts backed by nested scenes (`Tab1` -> `GamesTabScene`, and so on), which
  suggests the child scenes are not instantiated - a separate question from
  the missing visuals, and the more likely reason the Guide would still be
  blank even with visuals working.

So the remaining chain to pixels is now two concrete questions rather than one
vague one: why controls get no visual, and why the nested tab scenes are not
built.

### The missing visuals are global, not a GuideMain problem

Running the same id probe against `Options.xur` and `Status.xur` - scenes that
have loaded correctly since long before any of this session's fixes:

| Scene | id | handle | `XuiControlGetVisual` |
|---|---|---|---|
| Options | `btnOnlineStatus` | `00010067` | `80300017` |
| Options | `btnA` | `0001004C` | `80300017` |
| Options | `backBtn` | `0001004F` | `80300017` |
| Options | `artPanel` | `00010046` | `8030000A` |
| Options | `graphic_metapane` | `00010043` | `80300017` |
| Status | `artPanel` | `00010075` | `8030000A` |
| Status | `txtMessage` | `0001007B` | `80300017` |

Every one resolves to a live handle and **none has a visual**. So this is not
something GuideMain does differently - no control in any scene gets a visual.
That makes it a systemic gap in the visual/skin plumbing rather than anything
scene-specific, and it is the single remaining reason nothing can draw.

Worth noting `guide_skin_path` is deliberately blank because hud carries its
own skin as a resource section; whether that skin is actually bound to the
render context is now the obvious thing to check.

### Caveat on the nested-scene claim

The previous section inferred that the tab scenes are never instantiated
because `Blade_Center`, `Tab1` and friends fail lookup. That inference is not
safe: `XuiElementGetChildById` may search only immediate children, in which
case those ids failing means "not a direct child of the scene", not "never
created". The ids that did resolve in every scene look like top-level chrome,
which is consistent with a non-recursive search. Treat the nested-scene
question as open.

### Correction, and the state of the render path

**The earlier visual measurements were on the wrong scenes.** Scenes built by
`guide_scene_override` are standalone: created, inspected, never attached to
anything that renders. Their controls have no reason to hold a visual, so
"no control anywhere has a visual" was not established by them.

Re-measuring on the scene that *is* being drawn (`00010000`, the bootstrap's
own, which takes 600+ composite draws in a run):

    depth 1 node 00010008 visual -> 80300017: 00000000  (bootstrap scene)
    depth 2 node 00010039 visual -> 80300017: 00000000  (bootstrap scene)

So the result does hold where it counts - a scene under active composite draw,
with real depth, whose elements still have no visual. The conclusion survives,
but it needed the right scene to mean anything.

**The old provider blocker is gone.** `[81D6D0AC]` now reads `81D22A54`, not
null. The note above about XuiInit never installing a resource provider
described a state that no longer exists, which fits scenes loading.

**Two independent things stand between here and pixels:**

1. No element has a visual, including on the drawn scene.
2. The DC present gates are closed: `[dc+11C]=00000000` (bail with
   E_UNEXPECTED) and `[dc+134]=00000001` (return S_OK having presented
   nothing). Only `[134]==0` reaches a real present. These runs did not set
   `guide_patch_null_render` / `guide_clear_null_render`, which exist
   precisely to clear that flag - so this half is expected here rather than
   new.

Neither alone explains a blank screen; both have to be cleared.

## CORRECTION: the DC present gates were documented backwards

The note reproduced through several sections above says `XuiRenderPresent`'s
tail call bails when `[dc+11C] == 0`. Reading the function (runtime
`818F9290`, file `81900490`) shows the opposite:

```
819004b0  lwz   r11,284(r31)      ; [dc+0x11C]
819004b4  cmpwi cr6,r11,0
819004b8  bc    12,26 -> 819004cc ; BO=12 BI=26: branch when cr6.EQ, i.e.
                                  ; r11 == 0  ->  CONTINUE
819004bc  (nop hint)
819004c0  lis   r3,0x8000
819004c4  ori   r3,r3,0xffff      ; r11 != 0  ->  return 8000FFFF
819004c8  b     -> 81900524

819004cc  lwz   r11,460(r31)      ; [dc+0x1CC]
819004d4  bc    4,26 -> 819004dc  ; must be NON-zero, else the twi assert
819004dc  lwz   r11,308(r31)      ; [dc+0x134]
819004e4  bc    4,26 -> 8190051c  ; non-zero skips the present
819004e8  lwz   r3,460(r31)       ; zero reaches the real present
```

So the gates are:

| field | required | observed with `guide_patch_null_render` |
|---|---|---|
| `[dc+11C]` | **zero** | `00000000` ok |
| `[dc+1CC]` | non-zero | `4088B7A0` ok |
| `[dc+134]` | **zero** | `00000000` ok |

**All three are already satisfied.** `[11C]` being zero was never a problem -
it was the required state, misread as the failure. Several sections above
treat clearing it as outstanding work; they are wrong.

What is actually left on this path is different: with the patch applied the
run produces **one** composite draw instead of the 600+ seen without it. So
the present is reachable and something stops the loop after a frame - which
matches CONFIG.md's warning that this configuration "stops the dashboard after
one frame", written when the cause was unknown.

Next: find why drawing stops after the first frame with the null-render patch
applied, rather than treating the gates as the obstacle.

### The patched configuration reaches new code and faults there

`guide_patch_null_render` does not "stop drawing" - the bootstrap completes
normally:

    GuideBootstrap: render host -> 00000000, XUI ctx 4088A0A0, provider 81D22A54
    GuideBootstrap: scene creator 913EB940 -> 00000000, scene=00010000
    GuideBootstrap: DC present gates: [11C]=00000000 [134]=00000000 [1CC]=4088A0E0
    GuideBootstrap: draw hook installed on title thread
    GUEST CRASH: access violation at guest PC 819DE94C, fault_addr 0000000100000024

The draw count is zero because the *first* draw now reaches the real present -
which the unpatched build never did - and faults immediately. Reaching that
code at all is new.

The fault site (runtime `819DE94C`, file `819E5B4C`):

```
819e5b38  lwz   r8,12960(r31)    ; [dev+0x32A0]
819e5b40  cmplwi r8,0
819e5b44  bc    -> 819e5b4c      ; non-zero: use r8
819e5b48  lwz   r11,12976(r31)   ; else fall back to [dev+0x32B0]
819e5b4c  lwz   r9,36(r11)       ; [r11+0x24]   <-- faults, r11 == 0
```

So the present path wants a surface-ish object at **`[dev+0x32A0]`**, falling
back to **`[dev+0x32B0]`**, and under the bootstrap both are null. The
`fault_addr` of guest `0x24` confirms the null base.

These are not the fields the existing render-target work touches -
`guide_bind_title_rt` writes `[dev+0x3F78]` and `guide_fake_front_buffer`
`[dev+0x3F74]`. `0x32A0`/`0x32B0` are a different pair and nothing in the
bootstrap sets them.

Next: find what normally populates `[dev+0x32A0]` / `[dev+0x32B0]` - most
likely a SetRenderTarget-style call on xam's device - and whether the
bootstrap can make that call rather than poking the fields directly.

### A chain of nulls in the present path, and what "one draw" is worth

Working forward from the fault the null-render patch exposes:

| configuration | outcome |
|---|---|
| `guide_patch_null_render` | crash at `819DE94C`, `fault_addr` guest `0x24` (`[dev+0x32A0]` and `[dev+0x32B0]` both null) |
| + `guide_bind_title_rt` | crash moves to `819F5EC4`, `fault_addr` guest `0x20` - past the first null, into `lwz r11,32(r14)` with `r14` null |
| deepest-reach set + `guide_bind_title_rt` | **no crash**; `Guide composite draw #1 -> 00000000` |

`[dev+0x32B0]` has exactly one writer in all of xam, at runtime `819F3A24`,
which sits inside the bind routine `819F31A8` that `guide_bind_title_rt`
already calls - which is why enabling that flag moves the fault along.
`[dev+0x32A0]` has **no** direct store anywhere in xam `.text` (26 loads, zero
stores), so it is written some other way entirely.

**The completed draw is not evidence of pixels.** The device diagnostic
immediately after it reads:

    Guide device 4088B7A0: [32A0]=00000060 [32B0]=00000000

`0x60` is not a pointer. The present survives only because `0x60 + 0x24`
lands on a low guest address that happens to be mapped, so it is reading
garbage rather than a surface. This is exactly the trap the older note warns
about - a `00000000` return from the composite draw says nothing about whether
anything was presented.

So the honest state is: the present path can now be driven end to end without
faulting, on invalid surface state, once. Getting real output needs
`[dev+0x32A0]` to hold an actual surface, and finding what writes it - since
nothing in xam stores to it directly - is the open question.

### `[dev+0x32A0]` is an array slot, and the bind routine does write it

The earlier "no direct store anywhere" result was an artefact of scanning only
literal-offset `stw`. The field is reached as an **indexed array**:

    addi  r11,rIdx,3240      ; 3240 * 4 = 0x32A0
    rlwinm r11,r11,2,0,29
    lwzx  r30,r11,r31        ; [dev + (idx + 3240)*4]

Searching for that index pattern finds 7 sites, all `lwzx` reads - hence "no
stores". But scanning the bind routine `819F31A8` for **indexed** stores finds
four, and the first is the one that matters:

    819F331C   stwx r22,r27,r31      ; r27 = (r23 + 3240) * 4

with `819F331C` sitting just past the read at `819F32A8` that releases the
previous surface in the same slot. So `819F31A8(dev, index, surface)` is
exactly the routine that populates `[dev + 0x32A0 + index*4]`, and
`guide_bind_title_rt` already calls it with index 0.

**So why does the present still see `[32A0] = 00000060`?** Two candidates,
both checkable:

- **Wrong device.** The present reads device `4088B7A0`. Under
  `guide_create_primary_device` two xam devices exist - which is why
  `guide_use_bound_device` exists at all - so the bind may be populating the
  other one.
- **Wrong index.** The store uses `r23`, not a literal 0; if the index the
  routine ends up with is not 0 the surface lands in a different slot, and
  `0x60` is what slot 0 happens to hold.

Next: log the device pointer and index `guide_bind_title_rt` actually passes,
and compare against the device the present reads. That distinguishes the two
without further static reading.

### The RT0 bind guard skips silently when the slot holds junk

`guide_bind_title_rt`'s bind is guarded by

```cpp
if (rdev && surf && !r2(rdev + 0x32A0u)) { ... bind ... }
else if (!rdev || !surf) { XELOGW("cannot bind title RT ..."); }
```

so it binds only when RT0 is **zero**. Under the bootstrap RT0 reads
`00000060` - not a pointer, but not zero either - so the first branch is false
while the second is also false, and the bind is skipped with **no log at
all**. Confirmed in a full run:

    SetRenderTarget logged: 0
    bound title RT logged:  0
    cannot bind logged:     0
    Guide device 4088B7A0: [32A0]=00000060 [32B0]=00000000

That silence is why this went unnoticed: every other failure mode in this code
logs something.

The guard now treats RT0 as bound only if it looks like a real surface -
mapped, and readable across the first 0x28 bytes - and warns when it replaces
junk. That is strictly better than testing non-zero, since the present
dereferences whatever is there as a surface.

**It did not change the outcome**, though: RT0 still reads `00000060` and
neither the new warning nor the bind logs appear. So the enclosing
`guide_bind_title_rt` block is not being reached at all, which is a separate
problem from the guard. A one-shot log at the top of that block is in flight
to establish whether it runs.

## The render target is bound to the wrong device

Measured directly, with both pointers side by side in one run:

    Guide: RT bind sees dev 40883A80 RT0 4088B3E0 (plausible=true) surf 40958CD0
    Guide device 4088B7A0: [32A0]=00000060 [32B0]=00000000

`guide_bind_title_rt` reaches its device through the render wrapper
(`[wrapper+12]`, with fallbacks) and finds **`40883A80`**, whose RT0 already
holds a valid surface - so it correctly does nothing. The present takes its
device from the draw DC instead, `dev = [dc+0x1CC]` = **`4088B7A0`**, and
*that* one has RT0 = `00000060`.

So the bind has never been binding anything relevant. Two devices, and the
flag has been pointed at the one that did not need it. This is what
`guide_use_bound_device` was written for, and it is set in these runs without
fixing the mismatch.

The surface the scan finds (`40958CD0`) is now cached and the draw path binds
it on `[dc+0x1CC]`, where the present's own device is in hand, via the same
`819F31A8` setter.

**That code does not fire yet.** The `Guide device ...` line immediately above
it prints, so execution reaches the condition, and the bind block that
populates the cache logs *earlier* in the same run (line 20721 vs 21102) - so
on the face of it the cached surface should be set. A log of the condition's
own inputs is in flight rather than another guess about why.

Two guard bugs found on the way, both worth keeping regardless of this:

- The original RT0 test bound only when the slot was **exactly zero**, so a
  junk value like `0x60` silently skipped the bind *and* the else-branch
  warning - no log at all. It now requires the slot to look like a mapped,
  readable surface.
- `guide_bind_title_rt` being enabled therefore did nothing in earlier runs,
  which means the earlier claim that enabling it "moved the crash" was wrong;
  the other deepest-reach flags did that.

### The present's device is being destroyed

The bind on `[dc+0x1CC]` does execute - and xam answers immediately:

    Guide: present-device bind check: flag=true cached surf 40958CD0 dev 4088B7A0
    (DbgPrint) WRN[D3D]: VALIDATE_DEVICE called on a device that's currently
                         finalizing in D3DDevice_Release

So `4088B7A0` is not merely "the other device" - it is a device **in the
middle of being released**. That explains what nothing else did:

- why its `[32A0]` holds `00000060` rather than a surface or a clean null -
  it is teardown state, not an unbound slot;
- why binding a render target on it has no useful effect;
- why the composite draw completes exactly **once** and then stops.

`guide_use_bound_device` is supposed to point the present at the live device
and is set in these runs, yet `[dc+0x1CC]` still resolves to the dying one.
The live device is `40883A80`: its RT0 (`4088B3E0`) is a valid surface, and it
is the one `guide_bind_title_rt` reaches through the render wrapper.

This reframes the remaining work. Binding harder on `4088B7A0` cannot help;
what matters is why the draw DC references a device that is being finalised,
and whether the DC should be built against `40883A80` instead - or built later,
after whatever tears the first device down has finished.

(The `bound RT0 ...` line never prints because the `Execute` of `819F31A8` is
still running when the run ends - the JIT is visibly still translating inside
it - not because the call was skipped.)

### The RT bind was destroying the device

A/B on the base configuration, one flag apart:

| configuration | composite draws | "currently finalizing" warnings |
|---|---|---|
| without `guide_bind_title_rt` | 14 | 0 |
| with `guide_bind_title_rt` | 1 | 1 |

So the device is not found dying - **binding is what kills it**, and the
mechanism is in the setter's own code, already disassembled further up:

```
819fa4b0  lwzx  r30,r27,r31   ; r30 = whatever RT0 currently holds
819fa4b8  bc    -> skip if zero
819fa4cc  bl    819e6148      ; else release r30
```

`819F31A8` releases the previous contents of the slot before storing the new
surface. RT0 holds `00000060`, which is not a refcounted surface, so that
release corrupts state and tips the device into `D3DDevice_Release`.

**This was self-inflicted.** Fixing the guard two sections above - so that a
junk RT0 no longer counts as "already bound" - is what allowed the bind to run
at all, and the bind then destroys the device. The guard fix is still right in
principle (silently skipping with no log was worse), but it turned a no-op
into an actively harmful call.

The fix is to zero the slot before calling the setter, so its release path is
skipped and only the store happens. That is now in place, with the clearing
logged.

Also worth noting: the base configuration produces **14** composite draws, not
the 600+ recorded earlier in this file. The earlier figure came from runs with
a different flag set; treat draw counts as comparable only within one
configuration.

### Resolved: binding RT0 on the present's device is harmful; path disabled

Sorting out a claim I got wrong twice in a row, so the reasoning is on record.

First I concluded the bind *destroys* the device, from the A/B where enabling
it produced a "currently finalizing" warning. That inference was unsound: the
warning is emitted **by our own call** into `VALIDATE_DEVICE`, so a run that
never calls in cannot produce it. Absence of the warning without the bind is
not evidence the device was healthy.

The measurement that actually settles it is the draw loop, not the warning:

| configuration | composite draws | last draw | log end |
|---|---|---|---|
| no bind | 16 | line 23295 | 23463 |
| bind | 1 | - | - |

Without the bind the loop is still running when the process is killed - only
168 lines separate the last draw from the end of the log. With it, drawing
stops after one. So calling `819F31A8` on the present's device really does
break the loop, and zeroing RT0 first (so the setter skips releasing the junk
it holds) does **not** avoid it.

The path is now behind `XENIA_PRESENT_RT`, off by default. With it off:

    draws=13 finalizing=0 crashes=0

which matches the behaviour before any of this was added. Kept rather than
deleted because the *reason* it breaks the loop is still unknown, and the
device mismatch it was written to address is real:
`guide_bind_title_rt` binds on `40883A80` while the present reads
`[dc+0x1CC]`, a different device.

Lesson worth keeping: a diagnostic that only fires when you poke something
cannot tell you whether poking it caused the problem. Prefer a signal that
exists in both arms of the comparison - here, whether the draw loop keeps
running.

### What `XuiControlGetVisual` actually does

Resolved to runtime `81935B60` (file `8193CD60`):

```
8193cd78  cmplwi r29,0            ; out pointer null -> 80070057
8193cd98  stw    r31,0(r29)       ; *out = 0
8193cda0  lwz    r4,-12836(r11)   ; r4 = [81D6CDDC]  - the visual class
8193cda4  bl     -> 81943378      ; find(control, class)
8193cda8  cmplwi r3,0
8193cdac  bc     -> continue if non-zero
8193cdb4  lis    r3,0x8030        ; null -> 803000xx, the code we keep seeing
```

So "no visual" means `81943378(control, [81D6CDDC])` found nothing. The
visual is a **child object of a particular class**, and the class is taken
from the global at `[81D6CDDC]`.

Two consequences worth separating:

- Visuals are **attached**, not implicit. hud imports exactly two visual APIs:
  `XuiControlAttachVisual` (`0x38A`) and `XuiControlGetVisual` (`0x395`). A
  control has a visual only because something called Attach for it.
- If `[81D6CDDC]` is itself null, the search cannot match anything and every
  control reports no visual regardless of what the scene contains. That would
  explain the result being uniform across every scene tested, which is
  otherwise a strange coincidence. A log of that global is in flight.

Correction to an earlier speculation: the unnamed `XUIB` block in hud's
package that looked like it might be a skin was an artefact of the wrong
container mapping. The corrected extractor finds exactly 34 blocks for 34
directory entries with no orphan, so there is no hidden skin there.

### The visual class global is fine; visuals are simply never attached

    GuideScene: visual class global [81D6CDDC] = 40881270

Non-null and a plausible heap pointer, so the "the class global is null, hence
nothing can ever match" theory is dead. `XuiControlGetVisual` has a valid class
to search for and finds no child of it - the controls genuinely have no visual
attached.

Since visuals are attached explicitly (`XuiControlAttachVisual`), something
has to do the attaching, and the likely trigger is that a scene must be
**navigated to**, not merely created. hud imports the whole navigation family:

    358  XuiSceneNavigateBack        35B  XuiScenePlayBackFromTransition
    359  XuiSceneNavigateFirst       35C  XuiScenePlayBackToTransition
    35A  XuiSceneNavigateForward     35D  XuiScenePlayFromTransition
    39E  XuiSceneInterruptTransitions
    3B9  XuiTabSceneGoto

Everything this project does creates scenes - `XuiSceneCreate` directly, or
hud's scene creator - and nothing navigates to one. That would explain why
controls are inert in *both* the ad-hoc override scenes and the bootstrap's
own scene, which is otherwise awkward: the bootstrap scene is being drawn, so
"never rendered" does not account for it, but "created but never made current"
does.

Next: resolve `XuiSceneNavigateFirst` (`0x359`) and read its prologue to
establish its argument count before calling it - guessing an arity on a guest
export risks a fault, and this path has punished guesswork repeatedly.

### Navigation needs a host, and our scenes have no parent

`XuiSceneNavigateFirst` resolves to runtime `8193C0D0`. Reading it before
calling it (worth doing - guessing an arity on a guest export risks a fault):

- Three arguments: `r3` host, `r4` scene, `r5` transition.
- The transition byte is validated as `< 4` or `0xFD`/`0xFE`/`0xFF`, else
  `E_INVALIDARG`.
- **`r3` is only dereferenced when the transition is `0xFD`** (checked at
  `81943334`), so `(0, scene, 0)` is safe to call.

Calling it that way on a freshly created scene:

    GuideScene: NavigateFirst(0, 00010042, 0) -> 8030000B

and the visuals are unchanged. The rejection path is explicit:

```
81943380  bl     81938df0     ; fetch the scene's parent into sp+80
81943388  cmplwi r11,0
8194338c  bc     -> proceed if non-null
81943390  cmplwi r28,0        ; otherwise fall back to arg1, the host
81943394  bc     -> use it if non-null
8194339c  lis    r3,0x8030
819433a0  ori    r3,r3,0xb    ; 8030000B: no parent and no host given
```

So navigation requires either a scene that already has a parent, or an
explicit host in `r3`. Scenes created here have neither: `XuiSceneCreate` is
called standalone, so they are detached from the navigation tree entirely.

That is consistent with the visuals result rather than a separate problem - a
scene that is not part of any navigation tree is never made current, and
nothing attaches visuals to controls in it.

Open: what object is the host. `r3` is passed to `81938D20(host, scene)` at
`819433AC`, so reading that call is the way to identify what it expects -
candidates are hud's guide object (`guide_bs_obj_`) and the bootstrap's own
scene (`00010000`), but neither should be guessed at.

### What the navigation host must be

`81938D20(host, scene)` - the call `NavigateFirst` makes with its host
argument - starts by converting the host:

```
81938d40  or   r3,r30          ; the host
81938d44  bl   81938240        ; resolve it
81938d48  cmplwi r3,0
81938d4c  bc   -> proceed if non-zero
81938d54  lis  r3,0x8030
81938d58  ori  r3,r3,0x16      ; 80300016 if it does not resolve
81938d60  or   r4,r31          ; else attach the scene to it
81938d64  bl   8196e590
```

So the host must be a value `81938240` can resolve - an XUI **handle**, not an
arbitrary object pointer. hud's guide object is a C++ object and would fail
that; the bootstrap's own scene (`00010000`) is a real handle and is the
obvious candidate, so it is now cached at creation and passed as the host.

Addressing note, since this cost a wrong disassembly: **`ppcdis` prints file
VAs, and so do the branch targets it prints.** A target like `-> 81938d20` is
already a file VA; adding the usual `+0x7200` lands mid-function and produces
convincing but meaningless output. Only runtime addresses - those logged by
the emulator - need the offset.

### Navigation works now - and does not attach visuals

With the bootstrap's scene as the host:

    GuideScene: NavigateFirst(host 00010000, 00010042, 0) -> 00000000

That is a real capability gained: scenes can now be navigated, where every
attempt before returned `8030000B`. The host had to be an XUI handle, and the
bootstrap's scene is one.

**But the visuals are unchanged** - `btnB`, `btnA`, `btnOnlineStatus` all still
report `80300017`. So the hypothesis that navigation attaches visuals is
**refuted**. Worth stating plainly: navigation was a reasonable guess that fit
the evidence, and it is wrong.

### Where visuals actually come from

`XuiControlAttachVisual` (file `8193CE70`) has exactly three internal callers
in xam:

    81770B88   (runtime 81769988)
    8196BDB8   (runtime 81964BB8)
    81982358   (runtime 8197B158)

The XUI-region one sits inside a dispatch switch - the surrounding code is a
run of `b -> 8196c68c` tails, the shape of a message handler jump table - and
is gated on a helper returning zero:

```
8196bda4  stw  r11,8(r30)      ; mark something on the object
8196bda8  bl   81938bc8
8196bdb0  bc   -> skip if non-zero
8196bdb4  lwz  r3,0(r31)
8196bdb8  bl   8193ce70        ; AttachVisual
```

So a control gets its visual **when it is sent a particular message**, not when
its scene is created, and not when the scene is navigated to. That points at
the message pump rather than at scene setup - hud imports `XuiSendMessage`
(`0x35F`), and this project already has a `guide_trace_pump` cvar from an
earlier pass at the same area.

Next: identify which message id reaches `8196BDA0`, and whether anything is
sending it. That is a jump-table index away, and it is measurable rather than
guessable.

### The message that attaches a visual is id 9

The dispatch at `8196BD00` is a compare chain on the message id, not a jump
table, and the branch to the `AttachVisual` path is explicit:

```
8196bd00  cmplwi r11,0x46   -> 8196c1ec / 8196c188
8196bd14  cmplwi r11,0x21   -> 8196bfc8 / 8196bf74
8196bd20  cmplwi r11,0x0c   -> 8196be94 / 8196be3c
8196bd2c  cmplwi r11,0x00   -> 8196be0c
8196bd34  cmplwi r11,0x09   -> 8196bda0     <-- AttachVisual path
8196bd3c  cmplwi r11,0x0a   -> 8196bd94
8196bd44  cmplwi r11,0x0b   -> 8196c68c
```

So a control attaches its visual on receiving **message `0x9`**, and the
handler at `8196BDA0` sets `[obj+8] = 1` before calling
`81938BC8`; only if that returns zero does it call `AttachVisual`.

This is directly testable rather than another inference: `XuiSendMessage`
(`0x35F`) is exported, so message 9 can be sent to a control that currently
reports no visual, and the visual re-queried immediately afterwards. If it
appears, the missing step is that nothing delivers message 9; if it does not,
the `81938BC8` gate is what fails and that becomes the target.

Read the export's prologue before calling it. That discipline is what made the
`NavigateFirst` call safe - it revealed the host argument is only dereferenced
for one transition value, so `(0, scene, 0)` could not fault.

### `XuiSendMessage` decoded - and `8030000A` does not mean "no visual"

`XuiSendMessage` (`0x35F`) resolves to runtime **`8194A4E0`**, which is the
same function analysed much earlier in this file as a "generic handle-to-object
lookup" reached from the scene loader. Both readings are half right: it
resolves the handle and then dispatches.

```
81951700  cmplwi r30,0        ; message struct null      -> 80070057
81951708  lwz    r11,4(r30)
8195170c  cmplwi r11,0xa      ; [msg+4] == 0xA           -> 80070057
   ... handle table lookup on r31 ...
81951774  lwz    r3,4(r11)    ; object out of the slot
81951780  cmplwi r3,0
81951790  ori    r3,r3,0xa    ; slot empty               -> 8030000A
8195179c  bl     81951150     ; else dispatch(object, msg)
```

and the dispatcher clears a result field at `[msg+8]` before running.

**This corrects an earlier conflation.** `8030000A` is *handle resolution
failure*, not "this control has no visual". The two codes seen in the element
survey mean different things:

| code | meaning |
|---|---|
| `80300017` | object resolved; no visual child of the searched class |
| `8030000A` | the handle did not resolve to an object at all |

So `imgHeadsetBattery` and `artPanel`, which returned `8030000A`, were not
reporting a missing visual - their handles did not resolve. Only the
`80300017` cases (`btnB`, `btnA`, `btnOnlineStatus`, `Header`, `txtMessage`)
are genuine missing-visual results. That does not change the headline - real
controls still lack visuals - but any future count of "how many elements lack
visuals" has to separate them.

Still needed to send message 9: the offset of the id within the message
struct. `[msg+4]` is a type-ish field that must not be `0xA` and `[msg+8]` is
an output slot, so `[msg+0]` is the obvious candidate but is not yet
confirmed - and constructing the struct wrongly is exactly the kind of guess
that has cost time here.

### Message 9 sends cleanly but is rejected: `80300026`

The id offset is confirmed from two independent places: `XuiSendMessage`
validates `[msg+4]` (rejecting `0xA`) and the dispatcher loads the id with
`lwz r11,4(r30)` at `8196BCFC`, immediately before the compare chain. So the
message struct is:

| offset | meaning |
|---|---|
| `+0` | size (see below) |
| `+4` | message id |
| `+8` | output flag, cleared by the dispatcher at `81951170` |

which is exactly XUI's documented `XUIMessage { dwSize, dwMessage, bHandled }`.

Sending message 9 to `btnB`, `btnA` and `btnOnlineStatus` with the rest zeroed:

    msg9 -> 80300026; visual now 80300017: 00000000

No fault - the struct layout is safe - but rejected with `80300026`, and no
visual attached. The obvious suspect is `dwSize` left at zero, so the probe
now tries `0x0C`, `0x10`, `0x18`, `0x20` and stops at the first that returns
`S_OK`.

Worth noting what this experiment is worth either way: if some size makes the
send succeed and a visual appears, the missing step is that nothing delivers
message 9. If the send succeeds and no visual appears, the gate at `81938BC8`
inside the handler is what fails. Both outcomes are informative, which is why
this is being driven by a call rather than by more reading.

### Tagging `80300026`: the adjacency trap again

Message 9 is rejected with `80300026` regardless of `dwSize` - `0x0C`, `0x10`,
`0x18` and `0x20` all give the same code - so the size field is not the gate.

Tagging the sites that build that constant, using the same immediate-rewrite
trick that located the scene `E_FAIL` in hud: patch each site's `ori` to a
distinct low byte and let the returned HRESULT name itself.

First attempt found six sites by requiring `lis rX,0x8030` **immediately
before** the `ori`. All six were tagged and the send still returned an
untagged `80300026` - the same failure mode as the earlier E_FAIL hunt, where
that adjacency requirement hid 201 of 886 sites.

Dropping the requirement finds **eight** in the XUI region: the six already
known plus `819362CC` and `81959304`. All eight are now tagged.

This is worth writing down as a rule rather than a one-off: on this compiler's
output, **never match a two-instruction constant build by adjacency**. Match
the low half alone (`ori rX,rX,imm`) and accept the false positives - the
scheduler routinely separates the pair.

### `80300026` is a failed lookup, and the tag trick found it immediately

With all eight sites tagged, message 9 returns **`80300037`** - tag index 7,
i.e. runtime **`81959304`**, one of the two sites the adjacency filter had
missed. Dropping that filter paid for itself in a single run.

The site:

```
819604dc  bl     81949c68       ; loop body
819604e8  bc     -> loop while non-zero
819604ec  lwz    r11,80(r1)     ; did the search find anything?
819604f4  bc     -> success if non-zero
819604f8  lwz    r11,20(r31)
81960500  ori    r11,r11,0x4    ; set a failure bit in [obj+0x14]
81960504  ori    r30,r30,0x26   ; return 80300026
81960508  stw    r11,20(r31)
```

So the message-9 handler **walks a list looking for something to attach,
finds nothing**, records the failure in `[obj+0x14]` bit 2, and returns
`80300026`. The message is delivered and handled correctly; there is simply
nothing to attach.

That is consistent with the visual templates not being present - which is what
a missing skin would look like, and this project deliberately runs with
`guide_skin_path` blank on the grounds that hud carries its own skin as a
resource section. Whether that skin is ever handed to XUI has never been
established; the container work earlier in this file found 34 scenes and no
skin among them.

Next: identify what the loop at `81960494`-`819604E8` is searching. That names
the collection that is empty, and therefore what has to be populated - rather
than assuming it is a skin.

### There is no skin package in xam either

Running the XUIZ extractor's directory parse over `xam17489.pe` finds **seven**
containers:

| offset | entries | contents |
|---|---|---|
| `00830000` | 10 | `strings.xus` and its locale variants |
| `00834D00` | 54 | `XamNuiTipsAndProgressScene.xur`, `XamVariables.xur`, `..\handles\*.xur`, `..\dash\SimpleCursorScene.xur` |
| `0085CF00` | 10 | more strings |
| `0085E000` | 30 | `gamercard.xur`, `star_*.png` |
| `00866F00` | 37 | `Blade_dark.png`, `Blade_grey.png`, `*.xma` |
| `00871900` | 151 | `ico_*.png` |
| `008ABA80` | 63 | `notify.xur`, notification art |

**None contains anything skin-shaped** - no `.xzp`, no `.xui`, no file with
"skin" in its name. Combined with the earlier result that hud's package holds
34 scenes and no skin, that means **neither module ships a skin file**, and the
"a skin is missing" reading of `80300026` does not survive.

So the visual a control attaches has to come from somewhere else - the scene's
own data (the decoded `.xur` sections include `CUST`, which is a candidate) or
the class registry. The failing search is
`819428B0(container, r27, &out, r26)` where the container comes from
`81946428`; naming that container is what identifies the empty collection.

Recording the negative because it is the kind that saves time later: two
sessions could easily be spent looking for a skin to load, and there is not
one to find.

### The visual-attach call chain, consolidated

All addresses below are **file VAs** as printed by `ppcdis` (runtime = file -
`0x7200`).

```
XuiSendMessage (ord 35F, runtime 8194A4E0 / file 819516E0)
  validates [msg+4] != 0xA, resolves the handle
  -> 8030000A if the handle does not resolve
  -> dispatch 81951150(object, msg)      ; clears [msg+8]
       -> handler, compare chain at 8196BD00 on [msg+4]
            id 9 -> 8196BDA0             ; the visual-attach case
                 sets [obj+8] = 1
                 calls 81938BC8; attaches only if it returns 0
                 -> AttachVisual 8193CE70
```

and the case that actually runs ends in a search that finds nothing:

```
81960494  bl 81949D58(x)            ; getter; calls 819490B0(x, &out) and
                                    ; derives a value from the result
81946428(src, &desc)                ; memcpy 40 bytes into desc, [desc+0] = 40
819428B0(container, r27, &out, r26) ; container = [desc+4]
   nothing found -> set [obj+0x14] |= 4, return 80300026
```

So message 9 is delivered and handled correctly; the handler looks something
up in a collection reached through a 40-byte descriptor and the collection
does not contain it.

**Next step is `r27`/`r26`, not the container.** Knowing *what* is being
searched for identifies the missing item directly, whereas naming the
collection only says where it should have been. Both are one disassembly away,
but the search key is the more useful of the two.

Descending one call per pass has been giving diminishing returns, so this is
recorded as a map rather than continued blind - anyone resuming can jump
straight to `819428B0`'s arguments.

### The search key is the control's class

The function containing the failing search starts at file `81960360`
(runtime `81959160`), and its key is built from the object itself:

```
8196036c  or   r31,r3            ; the control
8196038c  lwz  r3,0(r31)         ; [obj+0] - its class / vtable
81960390  bl   8193B370          ; derive the key from it
81960398  or   r27,r3,r3         ; <-- r27, the search key
```

So the message-9 handler asks: *what is registered for this control's class?*
and the answer is nothing - hence `80300026` and no visual.

That is a different shape of problem from the ones ruled out. The **classes**
themselves are registered (the XUI registry shows 38 non-null entries, and the
registrars all report "already registered"), so what is missing is a per-class
entry in whatever collection `819428B0` searches - the visual template for the
class, in XUI terms.

A string constant referenced at the top of the function turns out to be
`' %d'`, a formatting fragment, so it does not name the function - noted only
so nobody re-checks it.

Where this leaves the visual question: it is **not** a missing skin file
(neither module ships one), **not** an unregistered class, **not** a null
class global, and **not** an undelivered message. It is an empty per-class
registration that something during normal xam/hud startup would populate and
our bootstrap does not.

### Addressing caveat: `+0x7200` is a `.text` relationship, not an image-wide one

Reading the string operand of `819428B0` by computing
`runtime + 0x7200 -> file offset` yields `'ice (0x%08X)'`, sitting sixty-odd
bytes inside `'...tusChangedNotification: Device status stopped from firing
tw...'`. That is mid-string, i.e. the computed address is wrong.

This is the **second** time this has happened, both in `.rdata`: the same
approach produced `'rpOutputLocation'` and `'ontroller.xur'` for hud, and that
was resolved by reading the strings out of **guest memory** instead, which
gave the correct `"hud"` and `"strings.xus"`.

So the rule is narrower than this file has been treating it:

- `runtime + 0x7200 = file VA` is reliable for **`.text`** - it has been
  cross-checked repeatedly against live disassembly and against the successful
  `913EB994` patch.
- For **`.rdata`** it does not hold. Read string constants from guest memory
  at the runtime address rather than computing a file offset.

Practically: when a disassembled instruction pair builds a pointer to a string
constant, do not resolve it offline. Log it from the emulator.

### Confirmed: the failing call is `XuiVisualCreateInstance`

Reading the same operand from **guest memory** rather than computing a file
offset:

    string at runtime 816462C4 = "XuiVisualCreateInstance(%S)"

against `'ice (0x%08X)'` from the offline read. Two completely different
strings, so the `.rdata` caveat in the previous section is correct - and worth
having tested, because the offline result was a plausible-looking fragment
rather than obvious garbage, and a shared string suffix would have explained
it innocently.

**These trace strings name their functions.** That is a much better tool than
disassembling blind: xam's XUI functions announce themselves at entry with a
printf-style trace, so the identity of any function in this chain can be read
out of guest memory instead of inferred. Use it.

So the chain ends in:

    XuiVisualCreateInstance(name)

where `name` is the wide string derived from the control's class
(`[obj+0]` -> `8193B370`). It finds nothing and returns `80300026`, the
handler records the failure in `[obj+0x14]` bit 2, and the control has no
visual.

That reframes the remaining question precisely: **the visual class named after
the control's class is not registered.** Not a missing skin, not an
undelivered message, not an unregistered *control* class - a missing *visual*
class registration.

Next: log the actual name string passed to `XuiVisualCreateInstance`. It is a
`%S` argument in a trace the function itself emits, so it can be read straight
out of guest memory at the call - and it will say exactly which visual class
is missing.

### Two corrections to the previous section

**The trace strings identify functions, but do not yield runtime values.**
The previous section recommended reading xam's entry traces as a general tool.
Half of that is right and half is not:

- Reading the *format string* at a known address does name the function -
  that is how `819428B0` was identified as `XuiVisualCreateInstance`, and it
  works because the string is static data.
- But the traces **never fire**. A full run has 31 `DbgPrint` lines and not
  one from XUI, so xam's tracing is gated off at runtime. The `%S` argument -
  the name of the missing visual class - cannot be obtained this way without
  first finding and enabling xam's trace level.

**The key-derivation reconstruction was wrong.** The attempt to compute the
lookup key outside the handler assumed the function at `81960360` receives the
control object, so that `[obj+0]` is its class. Running it says otherwise:

    obj 408C3890  class 00010048  visual class name -> 00000000 ""

`00010048` is the control's own **handle**, not a class pointer, and
`81934170` returns 0 for it. So `r31` in that function is some other
structure, and the derivation `[obj+0] -> 8193B370` cannot be reproduced from
outside without knowing what that structure is.

Net: the chain up to `XuiVisualCreateInstance(name)` still stands - that part
was read from the code and confirmed by the tag - but **the identity of
`name` is not yet known**, and neither of the two shortcuts tried this pass
can produce it.

### xam's trace gate, and four imported variables with no value

The trace sink at `817FED60` bails when `[r13+0x2B4]` is zero:

```
817fed8c  lwz    r11,692(r13)
817fed90  cmplwi r11,0
817fed94  bc     -> bail
```

`r13` is per-thread (the log records `r13=3005B000` for the UI thread), so
tracing is enabled per thread. `XENIA_XAM_TRACE=1` now points that slot at a
zeroed 256-byte buffer rather than a bare `1`, in case anything downstream
dereferences it:

    xam trace gate [r13+2B4] (r13=3005B000) was 00000000, now 301C8000

**It did not produce any XUI trace output.** The gate is set on the thread the
diagnostics run on, and the XUI work that matters may run on another; or the
sink needs more than a non-null pointer. Either way, tracing is not enabled by
this alone - recorded so the next attempt starts from "the gate is necessary
but not sufficient" rather than repeating it.

**Separately, a real gap worth knowing about.** A full run reports four
imported *variables* that Xenia never provides a value for:

    StfsDeviceErrorEvent
    UsbdDriverLoadRequiredEvent
    XboxKrnlBaseVersion
    g_XuiAutomation

`g_XuiAutomation` is the interesting one - an XUI global that xam imports and
reads, left unpopulated. Whether it bears on the visual path is unknown and
should not be assumed; it is noted because an unpopulated global that guest
code branches on is exactly the class of problem that has produced several of
the failures in this file.

---

## Ready to upstream: upstream-jit-bounds-fix.patch

`docs/guide-lle/upstream-jit-bounds-fix.patch`, cut against
`origin/canary_experimental` @ `9090088` and verified to apply cleanly to it in
a scratch worktree (174 insertions, 0 deletions, four files). Same rationale as
`upstream-monitor-fix.patch`: these are general Xenia bugs that this branch
happened to expose, not Guide work, and they do not depend on anything else
here.

Two of them are one bug seen from both ends. `PPCScanner::Scan` backs up by one
instruction when it reads a zero opcode, but did not special-case the *first*
instruction being zero - that sets `end_address = start_address - 4`.
`PPCHIRBuilder::Emit` then computes `(end - start) / 4 + 1` in **unsigned**
arithmetic, which underflows to ~2^30, and the `memset`s sized from it run over
gigabytes and fault inside `memcpy`. The reported crash is therefore nowhere
near the cause, which is what made this expensive to find. The patch fixes the
scanner (don't produce the bad range) *and* the builder (don't trust the range;
`Emit` returning false is already a supported outcome). Either alone would have
stopped the crash; both are worth having, since the builder guard turns any
future source of corrupt bounds into a log line instead of a wild memset.

The `assert_true(address <= end_address)` already sitting above that
computation is compiled out in release, so it never fired.

The other two files add `ObInsertObject`, which was declared but unimplemented.
Guest code that creates an object and asks the kernel for a handle to it got
nothing back. The implementation deliberately does **not** go through
`XObject::GetNativeObject`: for a guest object with no dispatch header that
reads a type field out of arbitrary bytes. It keeps its own pointer->handle
table instead, and `xobject.h` gains `set_guest_object_no_stash` because
`SetNativePointer` writes the handle into a dispatch header that such objects
do not have (its own upstream `FIXME` says as much).

Verified here: `ObInsertObject: guest object 301C9018 -> handle F8000308`, and
with it xam's notification listener finally receives a handle. Before it,
`XuiSceneCreate` returned `E_FAIL` for every scene that needed one.

Caveat worth stating plainly for anyone upstreaming this: the two JIT fixes are
robustness fixes, and the path that triggered them here is the LLE Guide
bootstrap. I have not produced a stock-Xenia repro. The argument for them is
that the underflow is real and reachable from any corrupt bound, not that a
shipping title is known to hit it. `ObInsertObject` is a plain missing export
and needs no such caveat.

Excluded on purpose: the matching `ExceptionCallback` stack-walk bounds fix in
`emulator.cc`. It is correct and it is general, but that file is heavily
Guide-modified on this branch and the fix does not separate cleanly.

---

## The `+0x7200` rule was a bug in `ppcdis.py`, not a property of the image

This supersedes the "Addressing caveat" section above, and retires the whole
runtime-vs-file-VA distinction. **These converted firmware PEs are memory
images: the bytes for any VA live at file offset `VA - ImageBase`, in every
section.** There is no per-section fudge and no skew to apply.

`ppcdis.py` maps a VA through the section header's `PointerToRawData`. For
xam's `.text` that field is 0x7200 too low, so every address `ppcdis` prints
for `.text` is 0x7200 higher than the real one. The constant then got recorded
as an image-wide rule and applied to `.rdata`, where there is no such error -
which is precisely why `.rdata` reads came out as `'rpOutputLocation'`,
`'ontroller.xur'` and `'ice (0x%08X)'`. Those were not a mysterious section
quirk; they were the fudge being applied where nothing needed fixing.

How it was caught: `.text`'s raw data begins with exactly 0x7200 bytes of zero,
and `.text` raw pointer `0x128E00 + 0x7200 = 0x130000`, which is `.text`'s own
RVA. Checking `.rdata` gives the same identity - the string logged by the
emulator at runtime `816462C4` sits at file offset `0x562C4`, and
`0x562C4 = 816462C4 - 815F0000`.

Three independent confirmations of the corrected mapping:

* `.pdata` function begins now land on real prologues (`7D8802A6`,
  `mfspr r12,8`) and tile `.text` contiguously - each record's length reaches
  the next record's begin. Under the old mapping they landed mid-instruction.
* `.rdata` strings read correctly at the addresses the emulator logs.
* The `.pdata` record for `XuiVisualCreateInstance` begins exactly at the
  prologue found by other means.

Practical consequence for reading this file: every address recorded here as a
**runtime** address is correct and needs no adjustment. Every address recorded
as a **"file VA"** is 0x7200 too high; subtract 0x7200 and it becomes both the
true VA and the runtime address, which are the same thing.

`tools/pe360.py` replaces the ad-hoc scripts and does this correctly. It also
carries the `.pdata` bitfield layout (`PrologLen:8 | FunctionLen:22 | flags:2`,
read from the low bits up - reading it from the high bits gives every function
a 64-instruction prologue and wildly overlapping bounds, which is how the first
attempt failed) and an xref scanner that assumes neither adjacency nor matching
registers, because xam emits `lis r8,0x8164` ... `addi r11,r8,0x6938` with the
halves dozens of instructions apart. `ppcdis.py` is kept only because older
sections quote its output; it has a warning header now.

### What this immediately bought: the visual registration path, named

Searching xam for `Xui*` strings turns up 116 distinct names, three of which
settle the visual question:

| string | at | referenced from |
|---|---|---|
| `XuiVisualCreateInstance(%S)` | `816462C4` | `8193B6D0` |
| `XuiVisualRegister: visual %S already registered` | `81646374` | `8193D298` |
| `XuiControlAttachVisual: Visual='%ls' specified on hObj=0x%08x ID='%ls' not found` | `81647BA0` | `81959240` |

which gives, in runtime addresses:

* `XuiVisualCreateInstance` = **`8193B6B0`**
* `XuiVisualRegister` = **`8193D238`**
* `XuiControlAttachVisual` = **`81959160`**

The third is the function this file already identified by other means as the
one whose search fails, and `XuiVisualCreateInstance` is called from exactly
that function (`bl` at `819591E0` and `819592B8`). Two independent routes
agreeing on both identifications is the strongest confirmation the visual work
has had.

`XuiVisualRegister` has exactly one caller, and the chain above it is short:

```
8193D238  XuiVisualRegister
  <- 8193D4B8            (bl at 8193D6D4)
       <- 817923A0       (no callers - a root)
       <- 81795548       (no callers - a root)
       <- 8193D740 <- 8193F7C0 <- 8193F838        (root)
                                <- 81AA5A60 <- 81AA5AD8 <- 8178DE50
```

**Next, and it is a cheap decisive test rather than more reversing:** run and
check whether `8193D238` is ever entered. Xenia JITs on first call, so a
`DemandFunction: enter 8193D238` line is proof of execution and its absence is
proof of the opposite - no breakpoint needed, no perturbation of the path.
If it never runs, the empty per-class collection is explained outright, and the
three roots above name the small set of entry points that would populate it.

### Harness trap: `press.ps1` does not build

It runs `build\bin\Windows\Release\xenia_canary.exe` and nothing else. Several
ticks in this project use a patch -> build -> run -> revert cycle for one-off
experiments, and the revert restores the *source* while leaving the *binary*
built from the patched source. `press.ps1` then happily runs that binary
against a clean tree, and the result looks like a regression in code that was
never changed.

The staleness was real here - the exe was timestamped 22:44 against source
restored at 22:51 - and `press.ps1` now builds first (`-NoBuild` opts out).

**But it was not the cause of the crash that led me to it, and the correction
matters more than the trap.** A run died at boot with a host breakpoint
(`xenia_canary.exe+56A169` = `x64::TrapDebugBreak+0x39`) having reached neither
the bootstrap nor any scene, and I read that as a regression. It was not:
`git diff` against the last known-good commit was empty for every file under
`src/`, and after a clean rebuild the boot crash still happened - this time as
an access violation at `+56A574` = `x64::ResolveFunction+0x294`.

Both are **already documented in this file** as the expected state:
`TrapDebugBreak+0x39` is recorded as not-a-Xenia-bug, and `x64::ResolveFunction`
dereferencing the null it gets when translation refuses is recorded as "the
current remaining crash". It is also recorded as a **race**, which is the piece
that explains the confusion: the same build boots cleanly to `draws=13` on some
runs and dies at boot on others. Two consecutive bad runs are unremarkable and
are not evidence of anything having changed.

Lesson worth more than the harness fix: before calling a run a regression,
check this file for the failure signature. Both symbols were in here already.

**Check `xenia_canary.exe`'s mtime against `src/` before believing any result**,
and rebuild (`cmake --build build --config Release`) when in doubt. A run that
contradicts a previously verified state is far more likely to be a stale binary
than a real regression.

### Lead, not yet a conclusion: the XUI trace level is already 1

The XUI warning traces are gated on a verbosity global at **`0x81D28964`**,
read as `lwz r11,-30364(rN)` off `lis rN,0x81d3` and compared `>= 1` for the
first level and `>= 2` for the second. In the on-disk image that global is
already **1**, so the level-1 traces ought to emit by default - yet no XUI
trace has ever appeared in a log.

That makes the silence a property of the sink or of the runtime value, not of
the gate, which is the opposite of what "traces never fire" implied. Both
`XuiVisualRegister` and `XuiControlAttachVisual` route through the same
formatter at **`81970F60`**, called as
`f("Warning", format, ...)` - the first argument is the literal string
`'Warning'` at `8161FE68`, so these are warning-level messages, and the
formatter reads a pointer from `0x81D391F0` before doing anything else.

Worth chasing because the payoff is direct: the full text of the failing trace
is

> `XuiControlAttachVisual: Visual='%ls' specified on hObj=0x%08x ID='%ls' not found...trying class defaults`

so making it emit prints **the name of the missing visual and the control it
belongs to**, which is the one fact the visual investigation still lacks. Note
also the tail - `trying class defaults` - which says the failing lookup is not
fatal by design and there is a documented fallback path behind it.

### Ground truth at last: a zero page is real code, and we can now prove it

The corrected addressing (`file offset = VA - ImageBase`, above) makes a check
possible that was not reliable before: comparing what the emulator sees at a
guest address against what the firmware image actually contains there, with no
skew to argue about.

Run it on the address that kills the current boot and the answer is
unambiguous. `ResolveFunction` dies on guest `81747A00`, and the scanner's own
log says why:

    PPCScanner: 81747A00 begins with 0x00000000; not a function
      (module=xam heap=yes access=3) reread 00000000 00000000 00000000
      window[-16..+28]: 00000000 x12

On disk that same address is:

    81747A00: 7D8802A6 9181FFF8 9421FFA0 7C641B78 2B030040 40990008 ...

a textbook `mfspr r12,8` prologue. It has an exact `.pdata` record - begin
`81747A00`, length `0x84` - and **two static callers** (`81747D84`,
`817483E8`). Only 4 of the 128 words around it are zero on disk.

So the zero pages are **corruption of real code**, not padding, not alignment
filler, and not the guest calling a bogus pointer. Every one of the three
benign explanations is now ruled out for this instance, with ground truth
rather than inference.

Two further notes that narrow it:

* `81747A00` is nowhere near the range the earlier one-shot population scan
  reported (`81D14000`-`81D5F000`, longest run 35 pages). Either the zero set
  moves between runs - consistent with this file's "it is a race" conclusion -
  or there is more than one region affected. The scan reports a *count*; it has
  never reported *which* pages, which is why this was not visible before.
* `heap=yes access=3` in the scanner log says the page is mapped and readable.
  The memory is there; the contents are not. That rules out a mapping failure
  and points at the copy/decompress step or at something overwriting it after.

**The obvious next move, now cheap:** have the population scan diff xam's
loaded `.text` against `work/xam17489.pe` instead of merely counting zero
pages, and log the mismatching ranges. That turns "3.1% of pages are zero" into
an exact list of what is wrong and whether it moves run to run - which decides
the race question outright and probably names the loader step responsible.

### Two flags this investigation needs on every run

Recorded together because both cost a wasted run this session:

* `--break_on_debugbreak=false` - without it the guest's own `tw`/`twi` assert
  trap becomes a fatal modal dialog about one second into boot and nothing
  gets anywhere. `press.ps1` now passes it by default (`-BreakOnDebugBreak`
  opts back in); the saved config is still left alone deliberately.
* Building before running - see the harness note above.

With the first flag the boot no longer dies in `TrapDebugBreak`; it dies a
little later in `ResolveFunction` on the zero page above, which is the real
blocker and the thing worth fixing.

### The population scan was measuring the wrong 6 MB

`ReportXamTextPopulation` scanned `81770000-81D60000`. xam's `.text` is
`81720000-81D13CE0`, from the section table of the decrypted image. The scan
bounds are wrong at **both** ends:

* it started **320 KB inside** `.text`, so `81720000-81770000` was never
  looked at - and that is exactly where `81747A00` lives, the address the boot
  currently dies on;
* it ran **304 KB past** the end of `.text`, into the inter-section gap and
  into `.data`.

Every zero page the scan has ever reported was in that overrun. With ranges
now logged rather than just counted, they are:

    zero ranges (after xam load):   81D14000-81D1FFFF (12)  81D3D000-81D5FFFF (35)
    zero ranges (after title load): 81D14000-81D1FFFF (12)  81D3D000-81D5FFFF (35)

- `81D14000-81D1FFFF` is the padding between `.text` (ends `81D13CE0`) and
  `.data` (starts `81D20000`). No section covers it.
- `81D3D000-81D5FFFF` is inside `.data`.

Both are supposed to be zero. So the familiar "**47 of 1520 mapped pages of
xam `.text` are entirely zero (3.1%)**" figure, quoted repeatedly in this file,
never described `.text` at all - it described alignment padding and
zero-initialised data. The figure is retired.

The earlier reading of those pages as "ordinary uninitialised data, not a load
failure" was right about what they *are*. What did not follow, and was assumed,
is the sentence after it: "**the `.text` code region is fully populated**".
That was never tested for `81720000-81770000`, because the scan could not see
it. Bounds corrected and `81747A00` added to the probe list.

What this does **not** touch: the `XamTextWatch` result further up - xam `.text`
reading zero and coming back to the identical value, region-wide - sampled
specific addresses directly rather than going through this scan, so it stands
unaffected. The transient-zero phenomenon is still real and still unexplained;
only the page-census evidence around it is withdrawn.

### `LLE xam: loaded at 30013000` is not a second copy

Recorded because this file flags it as "worth understanding before theorising
further, since it may mean there are two copies of the image". It does not.
That log line prints `xam_module->hmodule_ptr()`, and `hmodule_ptr_` is
documented in `xmodule.h` as pointing to the `LDR_DATA_TABLE_ENTRY` - a
system-heap allocation describing the module, not the image. `30013000` is
that descriptor, which is also why resource lookups pass `module=30013000` as
the handle. There is one image, mapped where the section table says.

**And `81747A00` is not a new site.** It shares a page with `81747D70`
(`81747A00 & ~0xFFF == 81747D70 & ~0xFFF == 81747000`), which is one of the
four addresses `XamTextWatch` already caught going zero and coming back. So
the boot-killing read is the *same* transient phenomenon, not a second
independent one - the contribution here is only the disk-side ground truth
that the bytes involved are real code (prologue, own `.pdata` record, two real
callers, 954 KB of contiguous non-zero code around it), which rules out the
"it is padding / a bogus pointer" readings for good.

That the flips are **page-granular** is itself a clue worth keeping: a
region-wide transition that restores identical bytes, observed at page
granularity, looks far more like a mapping or protection operation than like
anything writing data.

### The boot crash, fully traced: a XAM feature lookup that misses

With the scan bounds fixed, `xam .text population` reports **0 of 1524 mapped
pages zero (0.0%)** at both checkpoints, and the probe at `81747A00` returns
bytes identical to the image on disk. So `.text` is fully and correctly
populated - the last remnant of the "missing pages" story is gone, and this
also independently confirms the corrected addressing (guest bytes at a guest
VA == image bytes at `VA - ImageBase`, no skew).

The crash itself now decodes end to end.

**`81747A00` is xam's feature lookup.** It searches a table of 13 32-byte
records at `815FA1E0` for one whose `+8` word matches the requested id, and
returns the record, or NULL after printing `'Unknown XAM feature %d\n'`. The
loop bound confirms the shape: it runs while the offset is `< 0x1A0`, and
`815FA1E0 + 0x1A0 = 815FA380`, exactly where that string starts.

| key | feature | key | feature |
|---|---|---|---|
| 1 | `PRELOADED_HUD` | 33 | `DEVKIT_HEAP` |
| 2 | `MESSENGER` | 34 | `PIX_STREAM` |
| 3 | `XMP` | 35 | `ETX_BOOST` |
| 4 | `COMMUNITY` | 36 | `XS_LOGS` |
| 5 | `XIME` | 38 | `TESTXEX` |
| 6 | `XSTUDIO` | 39 | `XAMUIAUTOMATION` |
| 7 | `WIRELESS_WAVEA` | | |

(Records 7-12 carry `1` in their first word, the retail/devkit split.)

**`81747D70` is its caller**, and the fault falls out of it exactly:

    81747D70(feature_id, ptr):
      or   r31,r4              ; save ptr
      bl   81747A00            ; -> r3 = record, or NULL
      cmplwi r3,0 ; bne +8 ; twi     <- the guest assert
      ... four more null-checks, each guarded the same way ...
      lwz  r6,16(r3)           <- 81747DDC: faults on r3 = 0, addr 0x10

which is precisely the reported crash: `guest PC 81747DDC`,
`fault_addr ...00000010`, `r3=0`, `lr=81747D88` - and `81747D84` is one of the
two static call sites of `81747A00` found earlier.

**The requested id is 6.** `81747A00` starts with `or r4,r3,r3` and never
rewrites `r4`, so the `r4=00000006` in the crash dump is the key that was
looked up. Key 6 is `XSTUDIO`, and it **is** in the table on disk.

So the lookup missed on a key that exists.

My first reading was that the table must therefore have read wrong, and that
since `815FA1E0` is in `.rdata` rather than `.text`, the transient-zero
phenomenon would need rescoping. **Measurement does not support that.** Probes
on the table now report, at both checkpoints:

    xam probe 815FA1E0 (after xam load):   00000000 815FA1CC 00000001 ...
    xam probe 815FA280 (after xam load):   00000000 815FA1C4 00000006 ...

byte-identical to the image on disk - rec0 carrying key 1 and rec5 key 6,
exactly as they should. The table is intact. It could still be corrupt at the
instant of the lookup, which these two checkpoints cannot see, but there is no
evidence for that and it should not be asserted.

**So the honest state is: the code is right, the table is right, the key is
present, and the lookup returned NULL anyway. The cause is not established.**
Two candidates worth testing, neither yet tested:

* A translation defect around the search loop. The loop body contains a `twi`
  (guarded, and never taken for these keys, but present), and this project has
  already found one real JIT bug. Whether Xenia's handling of a trap
  instruction inside a loop body preserves the loop's control flow is
  unverified.
* The same intermittency that produces everything else here. The very next run
  died somewhere else entirely - `ResolveFunction: no function for guest
  817B9C28` - so which address fails is not stable between runs, and this may
  be one more face of the transient-zero behaviour rather than anything
  specific to the feature table.

Recorded as a question rather than an answer, per the standing warning above
that this area has already produced three plausible-and-wrong causal stories.

Two practical notes:

* This code is dense with defensive `twi` asserts - five in `81747D70` alone.
  `--break_on_debugbreak=false` does not fix anything; it converts an
  informative assert into a null dereference several instructions later. When
  a crash looks like a null deref in xam, check whether an assert fired first.
* `0FE00019` is `twi 31,r0,25`, an unconditional trap. `ppcdis` renders it as
  `.long`, so these asserts are invisible in its output - worth knowing when
  reading any xam disassembly in this file.

### What XStudio is, and why xam is asking at all

Worth recording because it makes the failing path legible rather than
arbitrary. The requester is `817CE3C8`, reached via
`bl` at `817CE440` - which is exactly the `817CE444` in the crash's unwind
chain. It does:

    r3 = 6                       ; XSTUDIO
    bl 81747D20                  ; "is this feature enabled?"
    if (!enabled) -> report 'XSTUDIO: XStudio feature disabled', return 80004005
    ...
    r3 = '\Device\Flash\xstudio.xex' ; open it
    if (open failed) skip
    r3 = 6; r4 = 81D4FDB0
    bl 81747D70                  ; <- the call that crashes

So this is the **XStudio devkit feature** bringing itself up: it asks whether
the feature is enabled, and if so loads `\Device\Flash\xstudio.xex`. Two
things follow that are worth knowing before anyone "fixes" this:

* On a retail console this path should not run at all - the enabled check
  should say no and the function should return `80004005` down the
  `'XStudio feature disabled'` branch. That it proceeds means the enabled
  check is answering **yes** in our environment.
* It only reaches the crashing call if the open **succeeded**, since the code
  branches away when the open returns negative. So `\Device\Flash\xstudio.xex`
  is apparently opening successfully here.

Either of those is a more promising thing to investigate than the lookup
itself: the cleanest outcome is that XStudio should never have been enabled,
in which case the whole path - assert, null deref and all - simply does not
execute. `81D4FDB0`, the pointer passed in `r4`, is also the `81D4FDB0(+54)`
in the crash's stack code refs.

### The XStudio feature gate, decoded - and a contradiction worth keeping

`81747D20` is the enable check, and it is a plain bitmask test:

    81747D20(id):
      assert id <= 0x40 ; assert id != 0
      ld   r11, -1888(0x81D40000)     ; 64-bit mask at 81D3F8A0
      r10 = 1 << (id - 1)
      r11 = r10 & r11
      return (r11 != 0) ? 1 : 0

So feature `id` is enabled iff bit `id-1` of the 64-bit word at **`81D3F8A0`**
is set. XSTUDIO is id 6, i.e. bit 5.

Three measurements of that mask, all agreeing:

* **zero in the image on disk**;
* **zero at both probe checkpoints** (after xam load, after title load);
* **never changes during a run** - it is now in the `XamTextWatch` poll set and
  produced no change line at all, while four `.text` addresses in the same run
  flipped to zero and back.

Nothing writes it, either: the only two accesses to `81D3F8A0` anywhere in
xam's `.text` are the `ld` above and a second `ld` in `81748210`, both reads,
and no pointer to it is stored anywhere in the image.

**Which contradicts the crash.** For `817CE3C8` to reach its `bl` to
`81747D70` at `817CE440`, the check must have returned non-zero - the disabled
branch returns `80004005` well before that call. Yet the crash's back-chain
unwind is `817CE444 ...`, i.e. the return address immediately after exactly
that `bl`. Mask zero and that call happening cannot both be true, so one of
these is wrong and it is not yet clear which:

* the back-chain entry is stale stack data rather than a live frame (the
  "stack code refs" line in the same report is explicitly a heuristic scan, and
  `81747D70` has three other callers);
* or the mask is non-zero at the instant of the check, in a way that 0.5 ms
  polling and two checkpoints both miss.

Recorded unresolved rather than papered over. The useful consequence either
way: **XSTUDIO is not enabled**, so the "xam is trying to bring up a devkit
feature it should not" reading of the crash is *not* supported by the mask.

Related, decoded while here: `81748210` reads a second global at `81D3F8C0`
and, if it is non-zero, asserts that bit 32 - feature 33, `DEVKIT_HEAP` - is
set in the same mask. Another devkit gate keyed off the same word.

Also worth connecting: Xenia's own HLE has `XamXStudioRequest_entry` returning
`X_E_FAIL` as a `kStub`, and `xam_nui.cc` documents `XamNuiGetDeviceStatus`
calling `XamXStudioRequest(6, &var)` - the same id 6 and the same
`(id, pointer)` shape as `81747D70(6, 81D4FDB0)`. So `81747D70` is very likely
the real `XamXStudioRequest`, and under LLE the guest reaches the real
implementation instead of the stub that was written precisely because XStudio
is not available.

### What the host-level details say about the transient zeroing

From the same run, `VirtualQuery` at the moment each address flips reports
`protect=4` (PAGE_READWRITE), `state=1000` (MEM_COMMIT), `type=40000`
(MEM_MAPPED), `allocbase=0x180000000`, and a different `RegionSize` per
address. Those sizes are not arbitrary - adding each to its own page base gives
the same answer every time:

    18186E000 + 642000 = 181EB0000
    181893000 + 61D000 = 181EB0000
    181747000 + 769000 = 181EB0000
    1818AE000 + 602000 = 181EB0000

All four addresses lie in **one** mapped region that ends at host
`0x181EB0000`, i.e. guest `81EB0000`. The pages are committed and writable
throughout, so nothing is being decommitted or protection-flipped underneath
them - which argues against "the mapping was replaced" and for something
actually writing zeros over the range and then writing the original bytes back.

Note also that `.data` (`81D3F8A0`) sits outside that region and does not flip,
which is the first evidence that the phenomenon is bounded rather than
image-wide. Whether `.rdata` flips is now being measured - `815FA1E0` and
`815FA280` are in the watch set.

### `.edata` does not describe exports in this image

Tried to name xam functions from the export directory. `.edata` is listed at
`81E20000`, which under the (otherwise verified) memory-image mapping is file
offset `0x830000` - and that offset holds the XUIZ resource container with
`strings.xus`, not an `IMAGE_EXPORT_DIRECTORY`; parsed either endianness it
yields nonsense. So exports are not reachable from this file. Xenia gets them
from the `.xex` itself, which is why it can log `xam ordinal 35F -> 8194A4E0`;
that log line is the practical way to map ordinals to addresses.

The memory-image mapping remains verified for `.rdata`, `.pdata`, `.text` and
`.data`, each against runtime probes. `.edata`'s section entry simply does not
appear to describe what is at that address.

---

## ROOT CAUSE: xam is loaded twice, and the second load zeroes the first

This is the thing behind the transient-zero behaviour, the intermittent
translation failures, and most likely a good deal of the "impossible" guest
behaviour recorded throughout this file. It is measured, not inferred.

**Step 1 - the read-only guard says it is not a write.** Running with
`XENIA_XAM_RO=1` arms a host-level `PAGE_READONLY` guard over
`81740000-818C0000`. The guard armed, **no writer ever trapped**, and the four
watched addresses still went to zero and came back. At the moment of the flip
`VirtualQuery` reported `protect=4` - `PAGE_READWRITE`. The guard had been
undone. Per the note already in that code: if the protection is found reset,
the mapping was replaced rather than written through.

**Step 2 - the region geometry names the range.** Each flipping address
reports a different `RegionSize`, but added to its own page base they all give
the same end:

    18186E000 + 642000 = 181EB0000      181747000 + 769000 = 181EB0000
    181893000 + 61D000 = 181EB0000      1818AE000 + 602000 = 181EB0000

One region, ending at host `181EB0000` = guest `81EB0000`.

**Step 3 - `XamRangeOp` names the operation.** That instrumentation already
existed in `memory.cc` and had never been correlated against the flip. Doing so
lines up exactly:

| log line | event |
|---|---|
| 476 | `AllocFixed address=815F0000 size=008C0000` (first load) |
| 477-479 | xam's import warnings: `UsbdDriverLoadRequiredEvent`, `XboxKrnlBaseVersion`, `StfsDeviceErrorEvent` |
| **5565** | **`AllocFixed address=815F0000 size=008C0000` again** |
| 5733-6169 | the four watched addresses read zero |
| 6754-6762 | **the same three import warnings, again** |
| 6775+ | 204 `Protect` calls re-applying 64 KB section protections from `81700000` up |

`815F0000` is xam's ImageBase and `815F0000 + 8C0000 = 81EB0000` - precisely
the region from step 2. The same allocation, the same imports, the same
protections: **xam is loaded a second time, on top of itself, while the first
copy is executing.**

That accounts for every property of the phenomenon that made it so confusing:

* memory reads **zero** - `AllocFixed` zeroes the range;
* it comes back **byte-identical** - the same file is written back over it;
* the **read-only guard is defeated without trapping** - the range is
  re-allocated and re-protected, not written through;
* it is **region-wide** across ~800 KB and restores together;
* `.data` and `.rdata` outside the image were unaffected in the watch set;
* it happens **once, mid-boot**, which is why every symptom is intermittent -
  what breaks depends on which functions happen to be translated inside the
  window.

**Step 4 - the defect.** `KernelState::LoadUserModule` deduplicates with
`existing_module->Matches(path)`, passing the **full path**. `XModule::Matches`
compares the argument against the existing module's basename, its name, and
its path - so given `SYS:\xam.xex` already loaded, a request for
`\Device\Flash\xam.xex` matches none of the three and loads a second copy. Both
copies take their image base from the xex header, so the second lands on the
first.

**Fix applied:** also test `Matches(name)`, where `name` is the basename
`LoadUserModule` has already computed. That makes the dedupe name-based, which
is what the console does and what fixed image bases require, and it uses the
existing helper exactly as it was designed to be used. A `XELOGW` names the two
paths whenever it fires, so a genuine same-name-different-file case would be
visible rather than silent.

This supersedes the earlier "xam was being loaded twice (path-based dedupe
missed `SYS:` vs `GAME:`)" note, which recorded the problem and applied a
dashboard-only path workaround while leaving the general fix pending. The
workaround did not cover this path.

### Three flags, not two: the Guide never opens without `guide_auto_press_seconds`

Added to the list above after losing several runs to it. `press.ps1` does not
press anything - there is no key injection in it at all, despite the name. The
Guide open is driven entirely from inside the emulator by

    --guide_auto_press_seconds=N

which **defaults to 0**, i.e. no press ever fires. Without it a run boots
cleanly to 18-20k lines, loads xam and hud, runs both DllMains, allocates the
Guide buffers - and then simply sits there, with no `Guide button:` line, no
scene creation and no draws. That looks exactly like a regression in the Guide
path and is nothing of the kind.

So a meaningful Guide run needs all three:

    press.ps1 -Extra '--guide_auto_press_seconds=30' -Boot 35 -After 45

with `--break_on_debugbreak=false` now supplied by the harness automatically.
If a run shows no `Guide button:` line, check this flag before concluding
anything about the Guide code.

---

## The visual registry is empty because nothing ever registers: `huduiskin.xex`

Three results, in order, that close the loop on the visual question.

**1. `XuiVisualRegister` never executes.** With boot stable after the
double-load fix, the demand-JIT test finally ran cleanly. Xenia JITs on first
call, so a `DemandFunction: enter` line is proof of execution and its absence
is proof of the opposite:

    DemandFunction: enter 81959160     <- XuiControlAttachVisual   (runs)
    DemandFunction: enter 8193B6B0     <- XuiVisualCreateInstance  (runs)
    (nothing for 8193D238)             <- XuiVisualRegister        (never runs)

and nothing in its caller chain (`8193D4B8`, `8193D740`, `8193F7C0`,
`817923A0`, `81795548`, `81AA5A60`) runs either. So the collection
`XuiVisualCreateInstance` searches is not merely missing an entry for one
class - **it is empty, because nothing ever puts anything in it.**

**2. The scenes name the visuals they want, and they are skin names.**
Dumping `InfoUpsellLive.xur` (the scene actually on screen -
`scnInfoUpsellLive`) gives its control classes and, separately, the visual
names it references:

    classes:  XuiLabel  XuiImage  XuiButton  XuiNavButton  XuiBackButton  XuiFigure
    visuals:  graphic_metapane  RightPanelShader  btn_oneline-icon  artPanel
              legend_A  legend_B  shade  shine
    assets:   xam://livelogo_upsell.png  sharedres://ico_64x_xboxlive.png ...

Those visual names are what goes into `XuiControlAttachVisual`'s
`Visual='%ls' ... not found`.

**3. The skin exists, as its own module, and has never been loaded.**
`dashroot` contains **`huduiskin.xex`**. This corrects a conclusion recorded
earlier in this file: "neither module ships a skin" was established by scanning
the XUIZ containers *inside* `hud.xex` and `xam.xex`, and that much is true -
hud's container holds 34 `.xur` scenes and nothing skin-shaped. But the skin
was never inside either module. It is a **separate xex sitting next to them**,
and this project has been running with `guide_skin_path` deliberately blank on
the reasoning that "hud carries its own skin as a resource section" - which the
container scan itself had already refuted.

The plumbing is already there: `guide_skin_path` loads the named module and
sets `guide_skin_module_`. It has simply never been pointed at anything.

Next: run with `--guide_skin_path=SYS:\huduiskin.xex` (SYS: is `dashroot`) and
see whether `XuiVisualRegister` starts executing and controls acquire visuals.
**In flight at the time of writing - not yet a result.** Loading the module is
not automatically the same as XUI being told to use it as a skin, so if the
registrations still do not happen, the next question is what call turns a
loaded skin module into registered visuals.
