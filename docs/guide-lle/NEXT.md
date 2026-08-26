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
