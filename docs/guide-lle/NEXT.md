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
> - The composite draw loop runs continuously to draw #2700+ (the log samples
>   it, so ~13 lines means thousands of draws, not thirteen).
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
> **Solved since that list was written**
>
> - **Controls have visuals.** `GetVisual` returns `S_OK` with real handles
>   (`btnJoinLive`, `btnB`, scene nodes) where it returned `80300017` with null
>   for the entire history of this project. The visual registry at `81D6CF50`
>   goes from empty to **281 entries**.
> - The cause was three things together: `huduiskin.xex` is a **resource-only
>   XEX** that Xenia rejected outright; xam's **skin loader `81795548`** has no
>   callers and never ran; and the bootstrap built a **second XUI context** over
>   the live one, freeing it under the device context that still pointed at it.
> - Flags for it: `--lle_xam_skin_init=true --guide_reuse_xui_ctx=true`. Both
>   default off.
>
> **The one open question now**
>
> The Guide builds a correct scene tree with visuals attached and never reaches
> the screen, because the device it draws through has **no front buffer**:
> `[device+0x3F74]` is null and the draw emitter `819F5D18` faults reading it.
> The device is reached as `[[81D6C978]+0x08]+0x0C` - context, then a 140-byte
> **wrapper**, then the device. `[dc+0x1CC]` is the *wrapper*, not the device;
> that naming caused several wrong readings.
>
> Two ways in, both tested and both blocked:
>
> - **mode 2** (default creator `8178F748`, passes `2`): scenes, visuals and
>   composite draws all work, but `819F4D28` skips the front-buffer setup for
>   mode 2 by design. Driving the skipped routine `81A0FE48` directly crashes
>   inside it - it depends on device state mode 2 never establishes.
> - **mode 1** (`--guide_create_primary_device`, creator `8178E9F0`, passes
>   `1`): the front buffer **is** allocated, but device init then stalls in
>   `InsertAsyncCommandBufferCall` waiting for async calls that never retire.
>   Cause unknown. It is **not** the stubbed system command buffer - handing the
>   guest a real one via `guide_syscmdbuf_buffer_kb` changes nothing and the
>   guest writes zero words into it.
>
> **Required flags for any Guide run** - without these you are measuring
> nothing: `--break_on_debugbreak=false` (supplied by `press.ps1` now) and
> `--guide_auto_press_seconds=N`. `press.ps1` injects no input despite its
> name; the Guide is opened by that cvar alone.
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
> | `XENIA_PRESENT_RT=1` | bind RT0 on the present's device. Was **harmful** because it aimed at `[dc+0x1CC]`, the 140-byte wrapper, so `+0x32A0` landed 12KB out of bounds and the "junk `0x60`" it released was unrelated memory. Now retargeted to `[wrapper+0x0C]` |
> | `XENIA_CRASH_PEEK_DEREF="31,1C8,8"` | follow the pointer at `r<reg>+<off>` and dump what it points at. Distinguishes "the field holds the wrong address" from "the address is right and its contents are wrong" - a distinction that produced one withdrawn conclusion when reasoned about instead of measured |
> | `XENIA_XUICTX_WATCH=1` | poll the XUI context pointer and `[ctx+0x0C]`, logging changes **and** the pointer going null. The first version skipped null iterations, so a freed context looked like silence - which hid a use-after-free for a full round of analysis |
>
> The reusable tools are `tools/press.ps1` (run harness; **builds first**, and
> detects the modal crash dialog), `tools/sym.ps1` (symbolize an exe RVA via
> the PDB, no debugger needed), `tools/xuiz_extract.py` (unpack a XUIZ
> resource container) and `tools/pe360.py` (correct addressing for the
> firmware PEs, `.pdata` function bounds, xrefs that assume neither
> adjacency nor matching registers). `ppcdis.py` is **deprecated** - it
> trusts `PointerToRawData` and prints every `.text` address 0x7200 high.
>
> New cvars this session, all default off: `lle_xam_skin_init`,
> `guide_reuse_xui_ctx`, `guide_force_front_buffer`.
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

### Result: the skin is a resource-only XEX, so `LoadUserModule` cannot load it

Ran with `--guide_skin_path=SYS:\huduiskin.xex`. The file is found -
`HostPathDevice::ResolvePath(\huduiskin.xex)` - and then the load fails on
every key:

    XEX load failed with code 3, trying with devkit encryption key...
    XEX load failed with code 3, trying with xex1 retail encryption key...
    XEX load failed with code 3, trying with xex1 devkit encryption key...
    XEX load failed with code 3

No `Guide: skin override` line follows, because that only logs when
`LoadUserModule` returns non-null. Visuals stayed `80300017` and the draw loop
kept running to #3000, exactly as before - so **this run changed nothing**, and
`guide_skin_path` as it stands cannot be the mechanism.

Code 3 is not a decryption failure. In `XexModule::ReadImage` it is the last
return: *"Not a patch and image doesn't have proper PE header"* - decryption
and decompression are fine, the payload simply is not a PE. Comparing the XEX2
optional headers says why:

| file | flags | optional header ids |
|---|---|---|
| `huduiskin.xex` | `DLL_MODULE` | `0002 0003 0080 0400` |
| `hud.xex` | `DLL_MODULE` | `0002 0003 0080 `**`0100 0101 0102 0103 0180 0183 0200`**` 0400` |
| `dash.xex` | `TITLE_MODULE` | 16 ids |

`huduiskin.xex` carries **RESOURCE_INFO** (`0002`) and **FILE_FORMAT_INFO**
(`0003`) and nothing else of substance - no entry point, no image base address,
no import libraries. It is a **resource-only XEX**: a data package in a XEX
wrapper, with no executable image at all. `LoadUserModule` is the wrong tool
for it by construction, and the `call_entry` question is moot since there is no
entry point to call.

So the shape of the remaining work is clearer than it has been:

* the visual names the scenes ask for (`graphic_metapane`, `legend_A`,
  `btn_oneline-icon`, ...) are skin data;
* that data is in `huduiskin.xex`, as resources;
* nothing currently reads it, and `XuiVisualRegister` is never called by
  anyone, so the registry stays empty and every `AttachVisual` misses.

Two things worth knowing before the next attempt:

1. Xenia *does* decrypt and decompress the file successfully - it only rejects
   it afterwards for lacking a PE header. So the resource bytes are reachable;
   the loader throws them away rather than failing to produce them.
2. This project already has a parser for the container format these resources
   use (`tools/xuiz_extract.py`, which handled hud's 34 scenes and xam's seven
   containers). If the skin's resources are XUIZ, the same parser should read
   them once they can be got at.

The open question is no longer "where is the skin" but "**what call turns skin
resources into registered visuals**" - i.e. what the console invokes that
eventually reaches `8193D4B8`, the single caller of `XuiVisualRegister`, whose
own callers are `817923A0`, `81795548` and `8193D740`. Those three are the
concrete next targets, and none of them currently executes.

---

## `81795548` is xam's skin loader, and it is the only road to `XuiVisualRegister`

Found by dumping the string constants each of the three registration roots
materialises, rather than by reading them:

    81795548:  "\SystemRoot\huduiskin.xex"   L"skin.xur"   L"skin"   L"skin://"   "Device"
    817923A0:  (no strings)
    8193D4B8:  ":DrawText -- TRUNCATION + ELLIPSIS : '%S'"
    8193D740:  (no strings)

`81795548` opens **`\SystemRoot\huduiskin.xex`**, goes after a `skin` section
and a `skin.xur` inside it, and works in the `skin://` namespace. It is xam's
skin loader, it sits above `8193D4B8` - the single caller of
`XuiVisualRegister` - and the demand-JIT trace says **it never runs**. That is
the whole explanation for the empty visual registry.

Its shape makes it easy to drive:

* it takes **no arguments** - the prologue reads no parameter register, it
  calls a helper, asserts the result, then initialises a global structure at
  `81D43C50` (the same global `817923A0` later reads `[+0x80]` from);
* it has **no callers anywhere in xam** and no data references, so on hardware
  something outside the module drives it.

That is exactly the shape of `817BBD70`, the heap-creation routine this project
already discovered had no callers and now drives from the bootstrap under
`lle_xam_heap_init`. Added the same treatment: **`lle_xam_skin_init`** calls
`81795548` right after xam's DllMain, on the same system-process thread.

`\SystemRoot` resolves correctly in our runs, so the path should be reachable:

    System title: registering SystemRoot -> '\Device\Harddisk0\Partition1'
    (exe '\Device\Harddisk0\Partition1\dash.xex')

which is `dashroot`, where `huduiskin.xex` lives.

### Supporting facts established along the way

**Neither hud nor dash loads the skin.** hud imports 77 XUI functions -
including `XuiControlGetVisual` (ordinal `395`) and `XuiControlAttachVisual`
(`38A`) - and not one skin or visual-registration entry point. dash imports 802
functions with no XUI skin entry either. So the skin is not the app's job under
this firmware; xam does it internally, which is consistent with the loader
having no importable callers.

**Correction to an earlier identification.** This file recorded
`XuiControlAttachVisual = 81959160`, from the `Visual='%ls' ... not found`
string xref. The import table gives ordinal `38A` = `XuiControlAttachVisual`,
and our own bootstrap resolves `38A -> 81935C70`. `81935C70` is a 0x5c wrapper
whose only `bl` targets are `819339D8` and `81959160`. So **the export is
`81935C70` and `81959160` is its implementation** - the earlier identification
was of the right code, labelled one level too high. `XuiVisualCreateInstance`
= `8193B6B0` and `XuiVisualRegister` = `8193D238` are unaffected.

**Reading Xenia's import dumps is the practical way to name xam functions.**
The `F <thunk> <target> <ordinal> (<n>) <name>` lines in the log give ordinal
and name for every import a module resolves; combined with our
`xam ordinal N -> addr` logging that maps names to addresses without needing an
export table, which the converted PE does not usefully carry.

Result of running with `lle_xam_skin_init=true` is pending at the time of
writing. If the loader runs but fails, the string constants above say exactly
which file and section to check next.

### Driving the skin loader works, and lands on a concrete Xenia gap

`lle_xam_skin_init=true` and the loader runs, does exactly what its strings
said it would, and xam itself narrates the failure:

    LLE xam: calling skin loader 81795548
    DemandFunction: enter 81795548
    ...
    HostPathDevice::ResolvePath(\huduiskin.xex)      (twice)
    XEX load failed with code 3, trying with devkit encryption key...
    XEX load failed with code 3, trying with xex1 retail encryption key...
    XEX load failed with code 3, trying with xex1 devkit encryption key...
    XEX load failed with code 3
    (DbgPrint) WRN[XAM]: Failed to load huduiskin.xex.

Three things worth separating out.

**1. The identification is confirmed.** `81795548` is the skin loader. It was
never called, calling it makes it run, and it goes straight for
`huduiskin.xex`. No inference left in that chain.

**2. XAM traces do fire.** `(DbgPrint) WRN[XAM]: Failed to load
huduiskin.xex.` is the first XAM trace this investigation has ever produced,
which retires the "the traces never fire" note recorded earlier. They fire when
the code path that emits them actually executes - the silence was never about
the trace level.

**3. The blocker is a Xenia limitation, and a precise one.** Code 3 is
`XexModule::ReadImage`'s last line - *"Not a patch and image doesn't have
proper PE header"*. Comparing optional headers **with their values** shows why
that gate is wrong for this file:

| optional header | `hud.xex` | `huduiskin.xex` |
|---|---|---|
| `00010100` ENTRY_POINT | `913F9D00` | **absent** |
| `00010201` IMAGE_BASE_ADDRESS | `913E0000` | **absent** |
| `000103FF` IMPORT_LIBRARIES | present | **absent** |
| `000002FF` RESOURCE_INFO | present | present |
| `000003FF` FILE_FORMAT_INFO | present | present |

`huduiskin.xex` is a **resource-only XEX**: no entry point, no imports, and -
importantly - **no image base at all**. Xenia derives `base_address_` from
`XEX_HEADER_IMAGE_BASE_ADDRESS` and falls back to the security info, so for
this file there is no meaningful address to map it at, and it is then rejected
for not looking like an executable. Both halves of that are reasonable for an
executable loader and wrong for a resource container.

So the remaining work is a genuine feature gap in Xenia rather than anything
Guide-specific: **the loader cannot load a resource-only XEX**. What xam wants
from it is not code but the `skin` section - the loader's other constants are
`L"skin"`, `L"skin.xur"` and `L"skin://"` - which it would then fetch through
the ordinary section lookup.

A fix has to decide three things, none of them yet decided:

* where to put the data, since the file names no base address (an ordinary
  heap allocation would do - nothing executes from it);
* how to signal "resource-only" rather than "corrupt" - the absence of
  `XEX_HEADER_ENTRY_POINT` is the cleanest test available in the header;
* that `XModule::GetSection` can then serve the `skin` section out of it.

Note `ReadImage` already decrypts and decompresses this file successfully -
only the final `is_valid_executable()` check rejects it - so the resource bytes
are produced and then thrown away. Worth confirming where they are written
before relying on that, because `base_address_` is garbage for this file and
`ReadImage` calls `LookupHeap(base_address_)->Reset()` early.

**Also fixed along the way:** the crash after the failed load. With the load
failing, `81795548` dereferences the null result and faults at `8177B2F0`
(`fault_addr ...30`, unwinding through `81795940`, inside the loader's `0x424`
body). That is downstream of the failure, not a separate defect - it will go
away when the load succeeds.

### Correction: `huduiskin.xex` does have a load address, and a resource table

I wrote above that it has "no image base at all". That was wrong, and it came
from reading `load_address` at offset `0x114` in `xex2_security_info` when it
is at **`0x110`** (`0x114` is `section_digest`). With the right offset:

    huduiskin.xex: image_size=0x38000 load_address=90F90000 page_descriptors=4
    hud.xex:       image_size=0x4A000 load_address=913E0000 page_descriptors=7

`90F90000` is a perfectly sane address, sitting just below hud's `913E0000`.
So the file **can** be mapped, and Xenia's `base_address_` fallback to the
security info already produces the right answer for it. The absent
`XEX_HEADER_IMAGE_BASE_ADDRESS` optional header is normal for this kind of
file, not a defect.

Its `RESOURCE_INFO` table confirms the rest:

    'skin'  address=90F90000  size=0x1284B  (75851 bytes)
    'xam'   address=90FA2880  size=0x25297  (152215 bytes)

Both fall inside `90F90000 .. 90FC8000`, exactly where the image maps. The
`skin` resource is what `81795548` is after (`L"skin"`, `L"skin.xur"`,
`L"skin://"`), and the `xam` resource is almost certainly the `xam://`
namespace the scenes reference - `InfoUpsellLive.xur` asks for
`xam://livelogo_upsell.png`.

So the fix is much smaller than "decide where to put the data": the address is
already known, `UserModule::GetSection` already serves sections straight out of
`RESOURCE_INFO` without needing a PE, and only two things stand in the way.

**Implemented:**

* `XexModule::is_resource_only()` - has `RESOURCE_INFO`, has no
  `XEX_HEADER_ENTRY_POINT`. That is the cleanest signal in the header for
  "container, not executable", and it cannot be confused with a corrupt image.
* `ReadImage` accepts such a file instead of returning 3. The PE check can
  never pass for one by design.
* `LoadContinue` skips `ReadPEHeaders()` for it. Everything below that which
  matters - the page-descriptor walk and memory protection - runs off the
  security info, and imports/exports are simply absent.

This is a general Xenia gap rather than anything Guide-specific: any
resource-only XEX hits it. If it holds up it belongs in the upstream patch set
alongside the module-dedupe fix.

### Second gap found by the same file: `CalculateHash` on a module with no code

With `ReadImage` and `LoadContinue` fixed, the load got further and then took a
host fault:

    HOST FAULT: pc=...+25C63A fault_addr=190F8F000
    frames: ... exe+25C63A exe+261AD2 exe+25E50F ...

Symbolised: `XXH3_64bits_update` <- `UserModule::CalculateHash` <-
`UserModule::Dump`. The arithmetic is worth writing down because the fault
address names the bug exactly.

`CalculateHash` locates the first and last **`XEX_SECTION_CODE`** page via a
lambda that returns `UINT32_MAX` when it finds none, then does:

    start_address = base_address + (find_code_section_page(true) * page_size)

A resource-only XEX has **no code sections at all**, so that is
`base + (UINT32_MAX * 0x1000)`, which wraps to `base - 0x1000`. With
`base = 90F90000` that is `90F8F000` - precisely the `fault_addr` above, one
page below the image, unmapped. The hash then walks from there and dies.

Guarded: if there is no code section, log and return. There is nothing to hash
in a module that contains no code. Like the `ReadImage` change this is a
general robustness fix - any code-less module reaches it - not something
specific to the Guide.

Two Xenia defects from one file, both of the same shape: code that is correct
for an executable and wrong for a resource container, with no guard for the
container case.

---

## MILESTONE: the skin loads and `XuiVisualRegister` runs

With `is_resource_only()` accepted in `ReadImage`, `ReadPEHeaders` skipped for
such modules, and `CalculateHash` guarded against code-less images:

    LLE xam: calling skin loader 81795548
    HostPathDevice::ResolvePath(\huduiskin.xex)
    Module \Device\Harddisk0\Partition1\huduiskin.xex:
      XEX_HEADER_BOUNDING_PATH: \Device\Flash\huduiskin.xex
    XexGetModuleSection: module='huduiskin' section='skin' -> 00000000 size=75851
    DemandFunction: enter 8193D238                        <-- XuiVisualRegister
    XexGetModuleSection: module='huduiskin' section='xam' -> 00000000 size=152215

Every part of that is a first for this project:

* `huduiskin.xex` **loads** - a resource-only XEX that Xenia previously
  rejected outright;
* both of its resources resolve with `X_STATUS_SUCCESS` at exactly the sizes
  the resource table declares (`0x1284B` = 75851, `0x25297` = 152215);
* **`XuiVisualRegister` (`8193D238`) executes**, having never run once in the
  entire history of this investigation.

The path then continues into the shared resources it needs -
`XexGetModuleSection: module='xam' section='xam' -> 00000000 size=65728` and
`section='shrdres' -> 00000000 size=237951` - so the skin is being consumed,
not merely opened.

### What still fails, and what it is not

The `LLE xam init` thread then dies:

    GUEST CRASH: access violation at guest PC 8177B2F0 fault_addr ...00000030
    r3=0  unwind (back chain): 81795940

The crash site is `stw r5,48(r29)` with `r29 = r3 = 0` - the callee is handed a
**null object** and has no null check on it (it asserts on `r5` only). The
caller loads that pointer from `[r31+0x28]`, and `r31` throughout `81795548`
is the skin context global at `81D43C50`. So a field of the skin context was
never initialised.

**This is not a regression from the fix.** The identical crash - same guest PC
`8177B2F0`, same `fault_addr ...30`, same `81795940` in the unwind - occurs in
the run *before* the loader change, when the skin failed to load entirely. So
whatever leaves `[81D43C50+0x28]` null happens on both paths and is a separate
missing initialisation, not a consequence of the skin now succeeding.

Two candidates sit immediately before it in the log and are worth checking
first, both pre-existing gaps rather than anything new:

    ResolvePath(media:) failed - device not found
    undefined extern call to 81D1027C DrvSetSysReqCallback

`DrvSetSysReqCallback` is declared in `xboxkrnl_table.inc` as a `kFunction`
with no implementation. The run has eight other undefined externs of the same
kind (`NicGetLinkState`, `DumpGetRawDumpInfo`, `HalRegisterHdDvdRomNotification`
and friends), so this class of gap is not unusual here - but this one fires on
the skin path immediately before the null is used.

### Where this leaves the render question

The registry is no longer empty *by construction* - the function that fills it
now runs. Whether it actually registered the visuals the scenes ask for
(`graphic_metapane`, `legend_A`, `btn_oneline-icon`, ...) is **not yet known**,
because the init thread dies before the Guide is opened at 30s, so no scene
walk and no `GetVisual` results were produced in this run. That is the next
measurement, and it needs the crash above cleared first.

### The crash wedges the bootstrap, so the wait is now bounded

Worth spelling out because the symptom is misleading. After the skin loader
faults, the run does not simply lose that thread:

    LLE xam: waiting for init thread          <- and never "init complete"
    Guide button: auto-press firing after 30s
    Guide button: pressed (user 0), handler=00000000 buf=00000000 out_sz=00000000

The bootstrap blocks forever on `xam_boot->Wait(0, 0, 0, nullptr)`, so the
Guide never receives a handler and the auto-press fires against zeros. A second
thread meanwhile spins in `KeWaitForMultipleObjects` on an unresolvable object,
millions of iterations. A run in that state looks like "the Guide does nothing"
when what actually happened is one guest thread died during init.

The faulting thread apparently does not terminate cleanly enough for `Wait` to
return, so the wait is now **bounded to 15s when `lle_xam_skin_init` is on**,
logging when it expires. The default path keeps the original unbounded wait, so
the established baseline is untouched.

This is a workaround for measurement, not a fix. The real defect is that
`[81D43C50+0x28]` is never populated. Searching xam for writes to it finds
none: 49 functions materialise `81D43C50`, four of them **read** `+0x28`
(`81790758`, `81790FD0`, `81795548` at `81795928`, `81795970`), and not one
writes it. Every reader uses it the same way - `lwz r3,40(r31)` followed by a
call taking a function pointer in `r5` and `1` in `r4` - so it is a
callback/notification manager created outside the module, in the same class of
"something on hardware drives this" gap as the skin loader itself and the heap
creator before it.

Caveat on the search: an absolute-address scan for `81D43C78` finds nothing,
but that is a limitation of the scan rather than evidence - `81795548` builds
`r31` roughly a hundred instructions before using it, well outside the
64-instruction window that scan allows. The per-function base-register scan is
the one to trust here.

### Measured: skin init is not yet a net win - it registers, then costs the scenes

With the bounded wait, the bootstrap completes and gets further into the Guide
than it ever has:

    LLE xam: init thread still running after 15s ... continuing anyway
    LLE xam: init complete
    Guide button: pressed (user 0), handler=913E69C0 buf=301B9000 out_sz=301BA000
    Guide button: dispatching open to 913E69C0
    Guide button: handler returned 00000000
    Guide button: obj=401587E0 vtable=913E1CB4
    Guide button: XuiInit returned 00000001, ctx now 40877DC0
    Guide button: xam CreateDevice returned 00000000, device now 407CB880
    Guide button: queued XUI bootstrap for the title thread (hud 913E0000, obj 401587E0)

A real handler instead of zeros, a live Guide object and vtable, `XuiInit` and
the device creator both returning, and the XUI bootstrap queued.

**And then nothing.** Zero `GuideScene` lines, zero `Guide composite draw`
lines, zero `visual ->` lines in the entire 18,947-line run. The queued
bootstrap never executes on the title thread, and the log ends in the familiar
storm - `GetNativeObject: refusing unmapped dispatch header` and
`KeWaitForMultipleObjects ... will not resolve` across eight or more dash
threads, counters in the hundreds of millions.

So, stated plainly: **enabling `lle_xam_skin_init` currently makes things
worse, not better.** The baseline without it loads four scenes, walks the tree
and runs the draw loop to #3000 - with null visuals. With it, the visual
registry gets populated but the scenes never get created at all. That is a
regression on the axis that matters, and the flag should stay off by default
until the underlying crash is fixed rather than timed out.

The likely mechanism is the same null. `[81D43C50+0x28]` is a
callback/notification manager; xam's init dies partway through creating its
notification plumbing, and the objects the title threads then wait on
(`KeWaitForMultipleObjects ... at 81D424A8, dispatch type 9`) never become
resolvable. Timing out the wait lets our bootstrap proceed but does not repair
the guest state it left behind.

**Next step is therefore the null itself, not another workaround**: find what
creates the object at `[81D43C50+0x28]`. It is read identically by four
functions and written by none, which puts it in the same category as the skin
loader (`81795548`) and the heap creator (`817BBD70`) - routines with no
callers inside xam that something outside the module is expected to drive. Two
of those three have already been found and driven from the bootstrap; this is
the third of the same kind.

---

## Ready to upstream: upstream-module-loading-fix.patch

`docs/guide-lle/upstream-module-loading-fix.patch`, cut against
`origin/canary_experimental` and verified to apply to a pristine checkout of it
(59 insertions, 2 deletions, four files). Third of these, after
`upstream-monitor-fix.patch` and `upstream-jit-bounds-fix.patch`, and the same
rationale: general Xenia defects this branch happened to expose.

**1. `LoadUserModule` deduplicates by path only.** `Matches(path)` is passed
the full path, and `XModule::Matches` compares its argument against the
existing module's basename, its name, and its path - so a second path to an
already-loaded module matches none of the three. Both copies take their image
base from the xex header, so the second `AllocFixed` lands on the first,
zeroing the range the first copy is executing from, writing the same bytes
back, and re-applying section protections. Measured here as xam `.text`
"going zero and coming back identical", with intermittent translation failures
and crashes for whatever happened to be JIT'd inside the window. Fix passes
the basename to `Matches` as well and logs both paths when it fires.

**2. Resource-only XEX images are rejected.** `ReadImage` ends with "not a
patch and image doesn't have proper PE header" - correct for an executable,
wrong for a resource container. `huduiskin.xex` (the 360 dashboard's HUD skin)
carries `RESOURCE_INFO` with a `skin` and an `xam` resource, no entry point and
no import libraries, and cannot pass that check by construction. Adds
`is_resource_only()` - has `RESOURCE_INFO`, lacks `ENTRY_POINT` - accepts such
images, and skips `ReadPEHeaders` for them in `LoadContinue`. Their load
address comes from the security info and is already correct, and
`UserModule::GetSection` already serves sections from `RESOURCE_INFO` without
touching a PE.

**3. `CalculateHash` walks off the front of a code-less module.** It locates
the first and last `XEX_SECTION_CODE` page with a lambda returning
`UINT32_MAX` when there is none, then computes
`base + (find_code_section_page(true) * page_size)`. With no code sections that
is `base + (UINT32_MAX * 0x1000)`, which wraps to `base - 0x1000` - one page
below the image, unmapped - and the hash faults there. Observed at `90F8F000`
for an image based at `90F90000`. Fix returns early when there is no code
section.

(1) and (3) are unambiguous bugs. (2) is a missing capability rather than a
bug, but it is the reason the third one is reachable at all.

Not included, deliberately: the `lle_xam_*` import-rebinding in
`xex_module.cc`, the `DllMain` guest-thread guard in `kernel_state.cc`, and
everything under `lle_xam_skin_init` - all Guide-specific or still
experimental. The patch was built by applying only these edits to a clean
worktree rather than by diffing this branch, since those files carry unrelated
changes.

### Refuted: the skin loader is not simply being called too early

The obvious benign explanation for the null at `[81D43C50+0x28]` was ordering -
that our bootstrap drives `81795548` before whatever populates the field. It
does not hold.

`81D43C78` is now in the `XamTextWatch` poll set. On a **default-path** run
(skin init off, 22,943 lines, scenes loading and the draw loop running to
#3000 as usual) the watcher produces **no change line for it at all**. Combined
with the crash itself showing `r3=0` at the point the loader reads it, the
field is null from load through the entire run. Nothing populates it at any
point, early or late.

That leaves "a routine we never invoke creates it", the same category as the
skin loader and the heap creator. Enumerating candidates does not narrow it
usefully, though: xam has **4,477 functions with no `bl` caller** anywhere in
its `.text` - most reached through vtables, dispatch tables or exports - and 46
of those contain a store to `+0x28` of some register. That is not a shortlist,
it is a haystack, and picking from it would be guesswork dressed up as
analysis.

What is established, and worth not re-deriving:

* nothing in xam's `.text` stores to `[81D43C50+0x28]` through the context
  register - checked across all 49 functions that materialise `81D43C50`, and
  the offset appears in load lists and in no store list;
* `r31` in `81795548` is assigned once at `8179556C` and never reassigned, so
  the faulting load genuinely reads that address;
* `81790758` is reference-counted (`lwarx`/`stwcx` on `[ctx+0x24]`, init on
  first reference) but **asserts** `[ctx+0x28] != 0` and calls through it, so
  it consumes the manager rather than creating it;
* `81790FD0` and `81795970` are reached from xam's init region (`817512D8` and
  `81750FA8`; `DllMain` is `817519D8`), and `81750FA8` does execute in our
  runs - so on hardware the manager exists before those run.

**Suggested next step is a runtime one, not more disassembly.** The cheap
question still unanswered is whether the registrations that *did* happen before
the crash actually landed - i.e. whether the XUI registry at `81D6D508` holds
entries after a skin run. If it does, the skin path is fundamentally working
and only the crash stands between it and a scene walk; if it does not, the
registrations are failing for a separate reason and the crash is a red herring.
That is one probe and one run, and it decides where the remaining effort
belongs.

---

## Confirmed: the skin registers 281 visuals. The crash is in the tail, after them.

The question from the last section - do the registrations actually land - is
answered, and the answer is yes. Two runs, same probe, same build, differing
only in `lle_xam_skin_init`:

    skin init OFF: visual registry @81D6CF50: 00000000 x12   (and 8193D238 never entered)
    skin init ON : visual registry @81D6CF50: 4089FD40 4089E740 00000119 00000119
                                              4089DE20 00000008 00000007 00000000
                                              00000004 00000000 00000000 00000000

Three live heap pointers and **`0x119` = 281** twice, which reads as
count-and-capacity. So the skin path really does fill the collection that
`XuiVisualCreateInstance` searches, with 281 entries, from an empty start.

**First, a correction to the plan I wrote last tick.** I proposed checking "the
XUI registry at `81D6D508`". That is the wrong table - it is the **class**
registry (48 slots, 38 non-null), which this file already records as
populated, and it says nothing about visuals. The visual registry is
`81D6CF50`, named by `XuiVisualRegister`'s own prologue: it builds `81D6CE5C`
as the lock and `81D6CF50` as the object it passes in `r3` to its insert
helper. Checking the first would have produced a confident, meaningless
answer.

**What this changes.** The ordering is now established:

1. the skin loads;
2. 281 visuals get registered;
3. *then* the loader's tail loads `[81D43C50+0x28]`, gets null, and faults
   storing at `+0x30` - about `0x2C` from the end of its `0x424` body.

So the crash is not preventing registration; it happens after it. The only
thing it costs is the loader returning cleanly, and with it xam's init
completing - which is what currently costs us the scenes.

**Stand-in applied.** Since the callee only stores the callback at `+0x30` and
`+0x34`, and nothing else in xam creates *or* reads that manager, a zeroed
`SystemHeapAlloc(0x100)` block is written to `81D43C78` before the loader runs.
That is explicitly a stand-in to let the tail complete, not a reconstruction of
the real object; it is gated behind `lle_xam_skin_init` with the rest.

If that lets init finish, this would be the first run in which the scenes are
created *and* the visual registry is populated at the same time - which is the
configuration the whole visual investigation has been trying to reach.

### The stand-in works: skin loader returns, init completes, 281 visuals live

    LLE xam: skin callback manager stand-in at 30058000
    LLE xam: skin loader returned 00000000        <- returns S_OK, does not fault
    LLE xam: init complete                        <- no 15s timeout; init finished normally
    LLE xam: visual registry @81D6CF50: 4089FD40 4089E740 00000119 00000119 ...

Three firsts together: `81795548` **completes and returns success**, xam's init
thread **finishes on its own** rather than being abandoned after a timeout, and
the visual registry is populated at the same time. The bounded wait is no
longer doing any work in this configuration - it simply is not needed.

Downstream of that, the Guide bootstrap now runs to the end of its sequence:

    Guide button: handler returned 00000000
    Guide button: obj=401587E0 vtable=913E1CB4
    Guide button: XuiInit returned 00000001, ctx now 40877DC0
    Guide button: xam CreateDevice returned 00000000, device now 407CB880
    Guide button: queued XUI bootstrap for the title thread

### The failure has moved into XUI itself

One crash, at log line 21,835 of 23,810, on a **dash** thread rather than ours:

    GUEST CRASH: access violation at guest PC 81901EAC fault_addr 006E0065
    unwind: 818FA4B0 81902630 8195BB20 8195C094 81966A40 81940390 8194A070
            8194A374 8194A4BC 8194A898 8193C1E0 913EBA0C

`913EBA0C` is inside hud, and everything below it is xam's XUI. So hud is
driving XUI for real, deep into the visual machinery, which is only possible
now that the registry has entries.

The instruction is an **indirect call**:

    819090A0  lwz   r3,460(r31)     ; [dc+0x1CC] - the device-context field
    819090A4  mtspr 9,r11           ; CTR = r11
    819090AC  bctr                  ; <- faults

`r11` is `006E0065`, which is not a pointer at all - it is two UTF-16 code
units, `'e'` and `'n'`. Something is reading a function pointer out of text
data, or a slot that should hold a vtable holds a string. `[dc+0x1CC]` is the
same device-context field this file already tracks in the present path.

**Do not read this as the stand-in causing it.** The stand-in is written once,
at `81D43C78`, and the only code that touches it stores a callback at `+0x30`
and `+0x34`; `30058000` bears no relation to `006E0065`. The more likely
reading is simply that XUI now gets far enough to instantiate visuals and hits
a separate defect - but that is a hypothesis, and this file has a poor record
with confident causal stories in this area, so it is written down as one.

Still **no pixels**: zero `GuideScene` lines and zero composite draws in this
run. The scene walk our bootstrap performs did not run; hud's own path did, and
crashed first. What changed is the shape of the problem - from "controls have
no visuals" to "XUI dispatches through a bad pointer while building them".

### The XUI crash decodes to a device-context mismatch already recorded here

The faulting sequence at `81901EAC` (function `81901E40`, len `0xD0`, no
callers - reached indirectly) is a virtual call:

    lwz   r11,456(r31)   ; r11 = [dc+0x1C8]        - the device's vtable
    lwz   r11,12(r11)    ; r11 = vtable[3]
    cmplwi r11,0 ; bne   ; null-checked, and it passes
    lwz   r3,460(r31)    ; r3  = [dc+0x1CC]        - the device itself
    mtspr 9,r11 ; bctr   ; vtable[3](device, ...)  <- faults

So it is `device->vtable[3](device, ...)`, with the vtable taken from
`[dc+0x1C8]` and the `this` pointer from `[dc+0x1CC]`. `vtable[3]` holds
`006E0065` - two UTF-16 code units, `'e'` and `'n'` - which is text, not code,
yet passes the null check.

The registers name the objects. The crash report already dumps **all 32 GPRs**
in four lines; `press.ps1` only shows the first six crash lines, which is why
`r31` was not visible at first - read the log directly for the rest.

    r24-r31: 00000006 00000000 407FCF50 407FCEF0 407FCF54 7042F1F0 407FCEF0 40879860

giving:

| what | value |
|---|---|
| `r31` - the device context | `40879860` |
| `[dc+0x1CC]` - the device it calls | `40877E00` |
| `XUI ctx` (logged at bootstrap) | `40877DC0` |
| device the bootstrap created | `407CB880` |

`[dc+0x1CC]` is `40877E00`, which is **`XUI ctx + 0x40`** - inside the XUI
context, not a device object - and it is **not** the device the bootstrap
created (`407CB880`). Reading a vtable through it and calling slot 3 lands on
whatever bytes happen to be there, which is why the target is text.

That is the same discrepancy this file already lists as an open question near
the top: *"bind targets: `[dc+0x1CC]` versus the wrapper's device"*. It has
been latent all along; the skin work simply made XUI travel far enough to
dereference it. So the two open threads - the visual registry and the device
mismatch - turn out to be the same road, and the second one is now the blocker.

**Refinement from `XENIA_CRASH_PEEK="31,1C0,8"`** - the fields, read at the
fault:

    peek r31+1C0 = 40879A20: 00000000 3F800000 40877DC0 40877E00
                             40879B10 40879B70 4087A040 4087A0A0

    [dc+0x1C0] = 00000000
    [dc+0x1C4] = 3F800000   (1.0f)
    [dc+0x1C8] = 40877DC0   <- the XUI context itself, not a vtable
    [dc+0x1CC] = 40877E00   <- XUI ctx + 0x40

So the earlier reading of `[dc+0x1C8]` as "the device's vtable" was wrong.
It holds the **XUI context** (`40877DC0`, the value the bootstrap logs as
`XUI ctx`), and the crashing code fetches `[XUIctx + 0x0C]` and calls it. The
`this` it passes, `[dc+0x1CC]`, is that context plus `0x40`. Both fields point
into the XUI context; neither points at the device the bootstrap created
(`407CB880`).

Also worth recording, because it removes a false lead: `XuiInit returned
00000001` is **not** a failure. The bootstrap's own comment states XuiInit
returns 1 when XUI was already initialised, and `XUI ctx before` is already
`40877DC0` - so xam had initialised XUI itself before we called it. The context
is real; the question is why `[ctx+0x0C]` holds text.

### It is corruption, not misconfiguration: the XUI context is overwritten

Dumping the context right after `XuiInit` shows a perfectly well-formed object:

    Guide button: XUI ctx @40877DC0: 8163E200 00000001 40877E00 8178DBD8
                                     00000000 00000000 00000000 00000001 ...

    [ctx+0x00] = 8163E200   a .rdata pointer - its vtable
    [ctx+0x04] = 00000001
    [ctx+0x08] = 40877E00   the same value the dc later passes as `this`
    [ctx+0x0C] = 8178DBD8   a real pointer into xam's .text

So `[ctx+0x0C]` holds a **valid function pointer** at init. By the time the
device context dereferences it, the same slot reads `006E0065` - two UTF-16
code units, `'e'` and `'n'`. The object is correct when created and wrong when
used.

That reframes the problem, and in a useful direction:

* it is **not** that `[dc+0x1C8]` points at the wrong object - it points at the
  genuine XUI context, and `[ctx+0x08]` matching `[dc+0x1CC]` confirms the two
  structures agree with each other;
* it is **not** an initialisation failure - `XuiInit` returning 1 means
  "already initialised", and the context proves it was;
* something **writes text over the live XUI context** between init and use.

`006E0065` reading as `'e'`,`'n'` is suggestive of a locale or language string
(`"en-..."`), which would fit a string table or resource load writing to the
wrong address - and the skin brings two new resources into play, `skin`
(75851 bytes) and `xam` (152215 bytes), plus the `xam`/`shrdres` sections the
loader pulls afterwards. That is a hypothesis, not a finding.

A second sample of the context is now taken at the point the XUI bootstrap is
queued, which brackets the window: still-good there means the corruption
happens inside the queued bootstrap on the title thread; already-bad means it
happens between `XuiInit` and the queue, in the device-creation path.

### Refuted: the XUI context is not corrupted. And a methodology mistake.

`XuiCtxWatch` polled `[ctx+0x0C]` every 250us from the queue point to the end
of the run and produced **no change line at all**. The slot holds `8178DBD8`
throughout. So the previous section's conclusion - "something writes text over
the live XUI context" - is wrong, and is withdrawn.

The mistake underneath it is worth naming, because it is easy to repeat here:
**I combined measurements taken in different runs.** The peek that gave
`[r31+0x1C8] = 40877DC0` came from one run; the context dumps and the watcher
came from others. The XUI context happens to land at `40877DC0` in every run,
which made the numbers look like one coherent picture, but the object whose
`+0x1C8` was peeked is heap-allocated and there is no guarantee it is the same
object - or has the same contents - across runs. Two facts that are each true
of a different run can contradict each other without either being wrong.

What actually follows from the watcher result: at the moment of the fault,
`[r31+0x1C8]` was **not** `40877DC0`, because `[40877DC0+0x0C]` was
demonstrably `8178DBD8` for the whole run. So `r31` at the crash points at
something else.

Note also that `r31` in `81901E40` is simply the function's **first argument**
(`or r31,r3,r3` in the prologue) - calling it "the dc" was an assumption
carried over from the `[dc+0x1CC]` naming in this file, not something the code
establishes. The function is:

    81901E40(a, b, c, d):
      r11 = [a+0x1C8] ; assert [r11+0x0C] != 0      <- twi, passes
      r11 = [a+0x1C8] ; r11 = [r11+0x0C]
      if (!r11) return 80004005
      [a+0x1CC] ->r3 ; call r11(a_device, b, c, &local)

Both dereferences are of `[a+0x1C8]`, and the assert at `81909078` passing
means `[r11+0x0C]` was non-zero - consistent with the garbage value rather
than with a null.

**Correct procedure from here, and it is now running:** take the peek and the
watch *in the same run*, so the two numbers describe the same objects. Any
future claim about these structures should come from a single run's log.

## DIAGNOSED: use-after-free of the XUI context

`XENIA_CRASH_PEEK_DEREF="31,1C8,8"` settles it in one line:

    GUEST CRASH: deref r31+1C8 -> 40877DC0:
        00580075 00690053 00630065 006E0065 0000FEED FEEDFEED FEEDFEED FEEDFEED

Read as UTF-16 halfwords that is `X u i S c e n e` - the wide string
**`"XuiScene"`** - followed by `FEEDFEED`, the freed-heap fill pattern.

So the XUI context at `40877DC0` is **freed**, and its block has been reused to
hold a class-name string. `[r31+0x1C8]` is a **dangling pointer**. The value
that ends up in CTR, `006E0065`, is nothing more exotic than the `"ne"` of
`"XuiScene"`.

Everything now fits without contradiction:

* the context is genuinely well-formed at `XuiInit` and still well-formed when
  the XUI bootstrap is queued - both dumps were correct;
* it is freed somewhere after that;
* the device context keeps its stale `[+0x1C8]`/`[+0x1CC]` pointers and calls
  through them;
* `[dc+0x1CC] = 40877E00` = `40877DC0 + 0x40` is stale for the same reason -
  it pointed into the same freed block.

**And it explains the false negative that cost a tick.** `XuiCtxWatch` reads
the context pointer from the global at `81D6C978` each iteration and skips when
it is zero. If the context is destroyed and the global cleared, the watcher
goes quiet rather than reporting a change - so "no change line" meant "the
global went to zero", not "nothing happened". A watcher that treats a
disappearing pointer as nothing to report will lie to you in exactly this
situation; it should log the pointer going null too. Recorded because the same
watcher is used elsewhere in this file.

### What to look at next

Who frees it. Two concrete threads:

1. `XuiInit` returned **1** = "already initialised", meaning xam had initialised
   XUI before our bootstrap called it. If the count is a refcount and our extra
   `XuiInit` did not take a reference, then a matching uninit elsewhere can drop
   the last one while the device context is still holding pointers. The
   bootstrap calls `XuiInit` at `81953760` - whether there is a paired uninit
   being reached is unknown.
2. Whatever runs inside the queued XUI bootstrap on the title thread, since the
   context is intact when it is queued and dead by the time it is used.

The cheap instrument is the one already written, with the null case fixed:
watch `[81D6C978]` itself and log when it changes **or goes to zero**, which
brackets the free against the surrounding log lines.

### Who owns `81D6C978`, and why our `XuiInit` takes no reference

Two static results that shape the fix.

**`XuiInit` does not refcount.** Our bootstrap calls it with null params, which
takes the branch at `8195A9D4`:

    lis  r30,0x81d7
    lwz  r11,-12124(r30)      ; [81D6D0A4] - the "XUI initialised" flag
    cmpwi r11,0 ; beq +0x4C   ; zero -> do the real init
    ...                        ; non-zero -> trace and return 1

When XUI is already up it traces and returns 1. **No counter is incremented**,
so our extra call takes no ownership of the context - it only observes that
someone else initialised it. Anything that later tears XUI down is free to do
so while the device context still holds pointers into it, which is exactly the
shape of the observed use-after-free.

**The init flag is never cleared.** `81D6D0A4` has three accesses in all of
xam: two reads and a single write, and that write is inside `XuiInit` itself.
Nothing sets it back to zero, so "XUI was uninitialised" is not what happened.

**But `81D6C978` has three writers**, all in the device-context region:

    818FAD98   (write at 818FADD0)
    818FF140   (write at 818FF278)
    818FF2C8   (write at 818FF3F4)

plus eleven readers. A global with several writers in the DC code, distinct
from the init flag that is written once and never cleared, reads much more like
a **current device context** pointer than the XUI global context - which would
also explain why the bootstrap's label for it ("XUI ctx") has been misleading
me. If that is right, the freed object is a device context being swapped or
destroyed while `[dc+0x1C8]` still refers to it, and the three writers above
are where to look.

Stated as the reading it is, not as established fact - the watcher run that
brackets the pointer change will say which.

**Watcher fixed.** It now logs the pointer transition itself, including going
to zero (`(CLEARED)`), rather than skipping null iterations. The previous
version's silence was the bug that hid the free.

### The free is ours: the bootstrap builds a second XUI context

With the watcher fixed, the transition is caught and attributed in one step:

    21528  XuiCtxWatch: polling [xui_ctx+0C]
    21561  XuiCtxWatch: ctx pointer 40877DC0 -> 408BCA60
    21562  GuideBootstrap: render host -> 80300005, XUI ctx 408BCA60, provider 81D22A54
    21563  GuideBootstrap: XuiRenderCreateDC -> 00000000 dc=407D2FC0
    21846  GUEST CRASH ...

The pointer is **replaced, not cleared**, and the replacement happens inside
**our own bootstrap**, at the call to `8178DC58` - xam's render-host init. That
routine builds a XUI context and stores it at `81D6C978`; when one is already
there it builds a second and frees the first.

So the whole chain is self-inflicted:

1. xam brings XUI up on its own and a device context `40879860` is created
   against context `40877DC0` - hence `[dc+0x1C8] = 40877DC0`;
2. our bootstrap calls the render-host init anyway, which builds `408BCA60`
   and frees `40877DC0`;
3. the heap reuses that block for the wide string `"XuiScene"`;
4. something still holding the old device context calls
   `[[dc+0x1C8]+0x0C]`, gets `006E0065` - the `"ne"` of `"XuiScene"` - and
   faults.

Note the render-host call returns **`80300005`**, an error, and swaps the
context anyway. So this was never doing what it looked like it was doing.

Two things worth separating: this is **not** caused by the skin work. The
second context has presumably been built on every Guide open for as long as
that call has been in the bootstrap. It only became fatal once the visual
registry was populated and XUI travelled far enough to dereference the stale
pointer - the same pattern as the `[dc+0x1CC]` mismatch recorded near the top
of this file, which is very likely this same bug seen from the other end.

**Fix under test:** `guide_reuse_xui_ctx` skips the render-host init when
`81D6C978` already holds a context, logging what it reused. Default **off**, so
the established baseline is untouched until this is shown to be better.

---

## CONTROLS HAVE VISUALS

`--lle_xam_skin_init=true --guide_reuse_xui_ctx=true`:

    GuideBootstrap: render host skipped, reusing live XUI ctx 40877DC0
    GuideScene: depth 2 node 000100B1 visual -> 00000000: 000100E5  (bootstrap scene)
    GuideScene:     child [+C] 000100B7 "btnJoinLive" visual -> 00000000: 000100EB
    GuideScene:     child [+10] 000100BD "btnB"       visual -> 00000000: 000100FA
    GuideScene:   000100B1 GetVisual -> 00000000: 000100E5
    GuideScene: depth 3 node 000100E2 visual -> 00000000: 00010129

Every one of those was `80300017: 00000000` in every previous run in this
project's history. They now return **`S_OK` with real visual handles**.

Run totals:

| | before | now |
|---|---|---|
| `GetVisual` | always `80300017`, null | `00000000` with handles |
| guest crashes / host faults | 1+ | **0** |
| composite draws | to #3000, drawing nothing | to #2700, with visuals attached |
| `SwapDraws` | - | **156,109 draws over 5,400 swaps** |
| log lines | ~23,000 | **372,545** |

The 16x jump in log volume is itself a signal: the run is doing far more work
per frame than it ever did with empty visuals.

One node still fails - `0001012D visual -> 8030000A` - which is a different
error from the old one (`8030000A` is the "handle does not resolve" code from
`XuiSendMessage`, not "no visual"). Two of three `GetVisual` calls succeed.

### What the fix actually was

Three things had to be true at once, and each was found by measurement rather
than inference:

1. **the skin has to load** - `huduiskin.xex` is a resource-only XEX that
   Xenia rejected outright (`ReadImage` requires a PE);
2. **the skin loader has to be driven** - `81795548` has no callers inside xam
   and never ran, so the visual registry stayed empty by construction;
3. **the bootstrap must stop replacing the XUI context** - calling the
   render-host init a second time freed the context the live device context
   still pointed at, and the resulting use-after-free killed the run just as
   XUI started using the visuals.

(3) is the one that had been latent longest. It was harmless while the registry
was empty, because XUI never got far enough to dereference the stale pointer.

**Not yet claimed: pixels.** Visuals resolving and 156k GPU draws are strong
indirect evidence, but this file has been wrong before about things that looked
conclusive in a log. A window capture is being taken to settle it by looking.

### But the screen is unchanged: the Guide still does not render

Captured the emulator window twice, 75s in, same build, differing only in
`--lle_xam_skin_init` / `--guide_reuse_xui_ctx`:

* **with both flags**: the Xbox 360 "sign in or out" profile screen - avatars,
  Create Profile / Download Profile, the `A Select` / `B Back` legend;
* **without them**: **the same screen**, same layout, same text, same avatars.

The two images differ only in incidental ways (a different frame, an artefact
at one edge). So what is on screen is **dash.xex's own UI**, which was already
rendering, and the Guide overlay is not being presented in either case.

This is worth stating bluntly because the log made it look like a win. The A/B
legend at the bottom of that screen even matches the skin's own visual names
(`legend_A`, `legend_B`), which is exactly the kind of coincidence that invites
a wrong conclusion - those glyphs are dash's, drawn the same way before any of
this work.

**What is genuinely new, and holds up:**

* the visual registry goes from empty to 281 entries;
* `GetVisual` returns `S_OK` with real handles instead of `80300017` with null;
* the run is crash-free where it previously died in a use-after-free;
* far more GPU work per frame (156k draws over 5.4k swaps).

**What is still missing:** the Guide's output does not reach the screen. That
is the present/composite path this file already documents at length -
`[dc+0x11C]`, `[dc+0x134]`, the render-target binding, and the
`[dc+0x1CC]`-versus-wrapper-device question. The scene tree is now built *and*
has visuals attached; nothing is compositing it onto the front buffer.

So the stop condition - pixels of the Guide on screen - is **not met**. The
visual half of the problem is solved; the presentation half is not.

### Null-render patched, with visuals attached: still no Guide, and it says why

The null-render flag was worth re-testing once the registry was populated -
every earlier test of it ran against an empty registry, so there was nothing to
draw even if presentation had worked. Tested now, with
`--lle_xam_skin_init --guide_reuse_xui_ctx --guide_patch_null_render`:

    Guide: patched 818FDF14 917E0134 -> 60000000 (null-render copy removed)
    GuideFrame 0:   dc=00000000 [134]=FFFFFFFF gpu_draws +0  (total 12222)
    GuideFrame 500: dc=00000000 [134]=FFFFFFFF gpu_draws +14072
    SwapDraws: swap #2600 cumulative draws 74891

(The startup cvar dump prints `guide_patch_null_render = false` because it
shows config-file values, not command-line overrides; the patch line confirms
it applied.)

Screen: **identical to the baseline again** - dash's sign-in UI, no Guide.
Draw rate is `74891 / 2600` = **28.8 per swap**, the dashboard's documented
constant of ~29. So patching the flag adds **no draws**; the Guide is still
contributing nothing.

And it introduces a crash that is more informative than the flag itself:

    GUEST CRASH at 819DE94C  r3=0  fault_addr ...24
    unwind: 819DEB30 8191B024 818FDE60 818F8374 818FAEB8 913EAB4C

`818FDE60` is immediately adjacent to the `818FDF14` we patched, and the chain
runs back into hud. With `[dc+0x134]` forced to zero, `XuiRenderBegin` stops
skipping its device call, proceeds - and dereferences a **null device**.

**So the flag is a symptom, not the cause.** `[dc+0x134] = 1` was suppressing a
render path that has no device to render into; clearing it does not create one,
it just lets the path reach the null and fault. That redirects the remaining
work away from `[xui_ctx+0x1C]` - which this file already called characterised
and exhausted - and onto the device: `dc=00000000` in every `GuideFrame` line
is the plainest statement of the problem in this whole log.

Which is the same `[dc+0x1CC]`-versus-wrapper-device question from the top of
the file, now reached from a third direction.

### Correction: `dc=00000000` was a diagnostic reading the wrong field

Last section called `GuideFrame ... dc=00000000` "the plainest statement of the
problem". That was wrong. `GuideFrame` reads `[guide_obj + 12]`, i.e.
`[obj+0x0C]` - not the device context the draw path uses. A single run shows
three distinct values:

    GuideBootstrap: XuiRenderCreateDC -> 00000000 dc=408BE3A0   (the DC we create)
    Guide composite draw #1 ... draw dc=407D4C80 [11C]=00000000 [134]=00000001 [1CC]=40877E00
    GuideFrame 0: dc=00000000                                    ([guide_obj+0x0C])

So the DC our bootstrap creates (`408BE3A0`) is **not** the one the composite
draw runs against (`407D4C80`), and `[guide_obj+0x0C]` is neither - it is
simply not a DC field. There is no "null device" in the sense claimed; there
are several device contexts and we have been reading a field that never held
one.

What is true, from that same run: the DC that actually draws has
**`[134] = 1`** - null-render set - and `[1CC] = 40877E00`, which is the XUI
context plus `0x40` rather than the device the bootstrap created
(`407CB880`).

That makes the next question precise and cheap: does
`guide_patch_null_render` - which nops the copy at `818FDF14` - actually reach
`407D4C80`, or is that DC constructed by a path the patch does not cover? If
the patch leaves `[134] = 1` on the drawing DC, then the earlier "patching it
exposes a null device" result was measuring a different DC's behaviour
entirely, and the flag has still never been properly tested.

### Located: the present path's device has no surface bound

One run, `--lle_xam_skin_init --guide_reuse_xui_ctx --guide_patch_null_render`:

    Guide: patched 818FDF14 917E0134 -> 60000000 (null-render copy removed)
    GetVisual -> 00000000 (x2)          <- visuals still resolve
    composite draws: 0                  <- none happen at all
    GUEST CRASH at 819DE94C  fault_addr ...24
    r24-r31: ... r31 = 40870D00
    SwapDraws: 74891 / 2600 swaps = 28.8 per swap (the dashboard constant)

So the flag is a real gate, and the two states are:

* **unpatched** - `[dc+0x134] = 1`, composite draws run (to #2700) and emit
  nothing;
* **patched** - the present path runs instead, and faults immediately, so no
  composite draws happen either.

The faulting code is exactly the sequence this file already sketched:

    819E5B38  lwz r8,12960(r31)    ; [dev+0x32A0]
    819E5B40  cmplwi r8,0
    819E5B44  bne   +8             ; use it when non-null
    819E5B48  lwz r11,12976(r31)   ; else [dev+0x32B0]
    819E5B4C  lwz r9,36(r11)       ; <- faults, r11 = 0

**Both** `[dev+0x32A0]` and `[dev+0x32B0]` are zero. The device has no surface,
primary or fallback, so the present has nothing to present to.

And the device is named: **`r31 = 40870D00`**, while the device the bootstrap
creates and binds is **`407CB880`**. Two different objects. That is exactly the
question recorded at the top of this file - *"bind targets: `[dc+0x1CC]` versus
the wrapper's device"* - and `40870D00` is the same `dev=40870D00` that appears
in the older trace beside `[dc+1CC]=40883A70`.

So the presentation blocker is now located rather than described: **the render
target is bound on `407CB880`, and the present reads `40870D00`, which has no
surface.** Either the bind has to target the device the present actually uses,
or the present has to be pointed at the bound device.

That is a much smaller question than "why are there no pixels", and it is the
last one between a scene tree with attached visuals and something on screen.

### The DC's device field is not a device, so the surface question was malformed

Sampling the surface fields per frame rather than at bootstrap gives, for all
2700 composite draws:

    draw dc=407D4C80 [11C]=00000000 [134]=00000001 [1CC]=40877E00
                     dev[32A0]=00000060 dev[32B0]=00000000

Two things follow, and the second undoes the framing of the last two sections.

**`0x60` is not a surface.** This session's notes already record `0x60` as junk
in the RT0 slot - treating it as "bound" once tipped the device into
`D3DDevice_Release` and collapsed the draw loop from 14 log lines to 1. It is
the same value here, stable across every frame.

**And `40877E00` is not a device.** It is the XUI context (`40877DC0`) plus
`0x40`. Reading `+0x32A0` from it lands roughly 12KB past a 44-byte object, so
`0x60` is simply whatever happens to be in that memory - it is not a broken
surface pointer, it is not a surface field at all.

So "which device has a surface" was the wrong question. The device survey
answered it honestly - all three known device globals have both fields zero -
but the composite draw's `[dc+0x1CC]` was never one of those devices to begin
with. Three separate readings of these numbers have now been wrong in the same
way: treating `[dc+0x1CC]` as a device pointer because this file names it one.

What is solid, and worth keeping:

* the DC the Guide draws through is `407D4C80`, with `[134] = 1`;
* its `[+0x1CC]` points into the XUI context, not at a device;
* the present path (`819DE94C`) uses a *different* object, `40870D00`, whose
  `[32A0]`/`[32B0]` really are both zero;
* all three named device globals - `801E6FC4`, `801E6FC8`, `81D43684` - had
  both fields zero at Guide-button time.

The next question should be **what sets `[dc+0x1CC]` when a DC is built**,
since every path here depends on that field holding a real device and it
demonstrably does not. That is a construction-time question about
`XuiRenderCreateDC`, not a runtime one about surfaces.

### Correction: `[dc+0x1CC]` is `[xui_ctx+0x08]`, and that is a real object

The DC constructor is `818FDE98`, and it settles what these fields are:

    81905098  DC ctor(r3 = dc, r4 = xui_ctx)
      or   r30,r3        ; the dc
      or   r27,r4        ; the XUI context
      ...
      stw  r27,456(r30)  ; [dc+0x1C8] = the XUI context
      lwz  r3,8(r27)     ; r3 = [ctx+0x08]
      stw  r3,460(r30)   ; [dc+0x1CC] = [ctx+0x08]
      lwz  r11,0(r3)     ; its vtable
      lwz  r11,0(r11)    ; slot 0
      mtspr 9,r11 ; bctr ; and calls it

So `[dc+0x1CC]` is not "the device" by definition - it is whatever
`[xui_ctx+0x08]` holds, and the constructor **immediately calls vtable slot 0
on it**. An object whose slot 0 is called successfully is a real object with a
real vtable.

**That retracts the previous section's conclusion.** I argued `40877E00` could
not be a device because it is the XUI context plus `0x40` and therefore "past a
44-byte object". Adjacency proves nothing - heap allocations sit next to each
other routinely, and a 44-byte context followed by another block at `+0x40` is
exactly what a heap does. The reasoning was wrong even though the observation
was right.

Also worth recording from the context constructor `818FD0E8`: it allocates
**44 bytes**, sets `[+0x04] = 1` and `[+0x1C] = 1` from literals, and sets
`[+0x08] = 0` along with every other field. So `[ctx+0x08]` is **zero at
construction** and `40877E00` is written into it later by something else -
which is the thing to identify, since `[dc+0x1CC]` is a straight copy of it.

Probe added: the composite-draw line now logs `[dev+0]`, the object's vtable
pointer. That identifies the class and says whether `+0x32A0` is even a
meaningful offset for it, instead of assuming it is a D3D device because this
file calls the field "the device".

### Settled: `[dc+0x1CC]` is a ~140-byte object, not a D3D device

The composite draw logs `dev[0]=81640680`, and that vtable is referenced twice
in xam - `8191AD00` and `8191ADC0`, a constructor/destructor pair. The
constructor sizes the class:

    81921F00  ctor(r3 = this)
      lis  r11,0x8164 ; addi r11,r11,1664   ; 81640680 - the vtable
      stw  r30,4(r31)                       ; [this+0x04] = 0
      addi r3,r31,16 ; addi r5,r0,124       ; clear 124 bytes from +0x10
      stw  r11,0(r31)                       ; [this+0x00] = vtable

`0x10 + 124` = **140 bytes**. `+0x32A0` is 12,960 bytes in - nowhere near
inside it.

So both of my last two readings were half right, and neither was usable:

* the object **is** real, with a genuine vtable, constructed properly - so
  "it's just ctx+0x40, therefore stray memory" was wrong;
* but it is **not** a D3D device, and `+0x32A0`/`+0x32B0` are not fields of it
  - so `dev[32A0]=00000060` is an out-of-bounds read whose value means nothing,
  and reporting it as a surface was wrong too.

The `0x60` that this file records as "junk in the RT0 slot" is, at least here,
simply memory 12KB past a 140-byte object.

**What this leaves, stated carefully:**

* `[dc+0x1CC]` = `[xui_ctx+0x08]` = a 140-byte object of class `81640680`.
  Whatever the render path wants from `[dc+0x1CC]`, it is not surfaces.
* The **present** path (`819DE94C`) uses a different object entirely -
  `r31 = 40870D00` at the fault - and *that* one is read at `+0x32A0` and
  `+0x32B0`, both zero. That object is the plausible device, and it is the one
  with no surface.
* So the two are not the same thing and should stop being conflated: the DC's
  `[+0x1CC]` and the present's `r31` are different objects reached by different
  paths.

The useful next question is where the present's `r31` comes from - which is a
question about its caller, not about `[dc+0x1CC]` at all.

### The present's device, traced to its source

Walking the crash unwind rather than guessing. The chain, with each link
checked against the disassembly:

    hud 913EAB4C
      -> 818FAEB8
        -> 818F8374
          -> 818FDDE8        (contains 818FDE60, no bl callers - reached via vtable)
            -> 8191AFD0      (contains 8191B024, likewise)
              -> 819DEA70    (len 0x104)
                -> 819DE8F8  (len 0x174) - faults at 819DE94C

How the device travels:

    8191AFD0(r3, ...)      or   r31,r3
                           lwz  r3,12(r31)     ; r3 = [r31 + 0x0C]   <-- the device
                           bl   819DEA70
    819DEA70(r3, ...)      or   r29,r3         ; first argument
                           ...  or r3,r29 ; bl 819DE8F8
    819DE8F8(r3, ...)      or   r31,r3         ; first argument
                           lwz  r8,12960(r31)  ; [dev+0x32A0]
                           lwz  r11,12976(r31) ; else [dev+0x32B0]
                           lwz  r9,36(r11)     ; faults, both were zero

So the object the present tries to draw into is **`[X + 0x0C]`**, where `X` is
whatever `8191AFD0` is handed - and at the fault that was `40870D00`, with both
surface fields zero.

Two things worth noting about `+0x0C`:

* it is the same offset `GuideFrame` reads off the Guide object, where it is
  `0`. That may be coincidence - `+0x0C` is a common offset - but if `X` turns
  out to be the Guide object then the two are the same field and the `0` there
  is directly meaningful rather than a misread, which would partly rehabilitate
  a diagnostic this file has already dismissed once.
* neither `818FDDE8` nor `8191AFD0` has a single `bl` caller in xam, so both
  are reached through vtables. Identifying `X` statically means finding which
  vtable slot points at `8191AFD0` - which is tractable, since a vtable
  containing it can be found the same way `81640680` was.

That is the next concrete step, and it is a search rather than a guess.

### `X` identified: `[dc+0x1CC]` is the wrapper, and the device hangs off it

`8191AFD0` - the frame that loads the present's device as `[X+0x0C]` - appears
in `.rdata` at **`816406AC`**. That is `81640680 + 0x2C`, i.e. **slot 11 of the
same vtable** as the 140-byte object sitting in `[dc+0x1CC]`.

So `X` is that object, and the whole chain resolves:

    [xui_ctx+0x08]  ->  40877E00   140-byte object, vtable 81640680   (the wrapper)
    [wrapper+0x0C]  ->  40870D00   the actual device
    [device+0x32A0] / [+0x32B0]    the surfaces - both zero

Which is exactly what the note at the top of this file meant by *"bind targets:
`[dc+0x1CC]` versus the wrapper's device"*. That phrasing has been in here the
whole time; what was missing was that `[dc+0x1CC]` **is** the wrapper, not the
device, and that the device is one more dereference away at `+0x0C`.

It also explains, without contradiction, every confusing measurement of the
last several sections:

* `[dc+0x1CC]` is a real, properly constructed object - correct;
* it is not a D3D device and `+0x32A0` is not a field of it - also correct;
* `dev[32A0]=00000060` was an out-of-bounds read 12KB past a 140-byte object -
  meaningless, as recorded;
* the present's `r31 = 40870D00` is a *different* object because it is
  `[wrapper+0x0C]`, fetched by slot 11 of the wrapper's own vtable.

So the presentation problem, stated exactly: **the render target is bound on
the wrapper, and the present draws into the wrapper's device, which has no
surface.** The fix is one dereference: bind on `[wrapper+0x0C]`.

Probe added to the composite-draw line to confirm the chain at runtime -
`wrap[0]`, `wrap[0C]`, and the real device's two surface fields - so the
identification is measured and not just inferred from a vtable offset.

**Confirmed at runtime**, stable across the whole draw loop:

    draw dc=407D4C80 [1CC]=40877E00 wrap[0]=81640680 wrap[0C]=40870D00
                     realdev[32A0]=00000000 realdev[32B0]=00000000

`[wrapper+0x0C]` is `40870D00`, which is exactly the `r31` the present faults
on, and the real device's two surface fields are both zero. The chain is now
measured end to end rather than argued from a vtable offset:

    dc 407D4C80 --[+0x1CC]--> wrapper 40877E00 --[+0x0C]--> device 40870D00 --> no surface

### The remaining step, stated precisely

**Nothing binds a render target on `40870D00`.** That is the entire remaining
gap between a scene tree with 281 registered visuals attached and pixels.

What is *not* the problem, all now ruled out by measurement rather than
argument: the skin (loads, 281 visuals registered), the visual lookup
(`GetVisual` returns `S_OK` with handles), the XUI context (well-formed, and
no longer freed under `guide_reuse_xui_ctx`), the class registry (38 of 48
populated), and `[dc+0x11C]` (zero, the required state).

What remains open is narrow but not trivial, and this file's own warning
applies to it directly: the `create_primary_device`, `fake_front_buffer` and
`use_bound_device` scaffolding hands the guest state the system would normally
have built, and results measured against hand-assembled state are not facts
about xam. Binding a surface onto `40870D00` by hand would be more of the same
unless it is done the way the console does it.

So the question to answer first is **what would normally put a surface in
`[device+0x32A0]`** - i.e. which call the system makes that we are not making -
rather than writing a pointer there and seeing what happens. That is the same
class of question as the skin loader and the heap creator, both of which turned
out to be real routines nobody was driving.

### Found: the surface setter is wrapper vtable slot 21, and nothing calls it

Scanning xam for stores to the two surface fields:

* `+0x32A0` (RT0): **no `stw` anywhere**. It is written through a computed
  offset - an indexed `SetRenderTarget` - so a fixed-displacement scan cannot
  see it. Worth knowing before anyone repeats the search.
* `+0x32B0` (the fallback the present uses when RT0 is zero): **exactly one**,
  in `819F38C8` at `819F3A24`, guarded by an `lwarx`/`stwcx` update:

        stw r29,12976(r31)     ; [dev+0x32B0] = r29

`819F38C8` is therefore the routine that would give the present something to
draw into. It **never executes** - zero `DemandFunction` entries - and neither
does any of its nine caller functions, nor their callers. The entire
surface-binding subtree is dead in our environment.

One of those callers is the interesting one. `8191B338` has no `bl` callers at
all, and it appears in `.rdata` at **`816406D4`** - which is
`81640680 + 0x54`, **slot 21 of the wrapper's own vtable**, the same vtable
whose slot 11 (`8191AFD0`) is the frame that fetches the device for the
present.

So the wrapper class has a method that binds the surface, sitting right beside
the method that reads it, and **nothing invokes it**.

That is the third instance of the same pattern in this investigation, after the
skin loader (`81795548`) and the heap creator (`817BBD70`): a real routine with
no callers inside xam, which something outside the module is expected to drive.
Both of the earlier ones turned out to be genuinely missing calls rather than
dead code, and driving them was correct.

**Next step:** establish what invokes wrapper slot 21 on hardware, and with
what argument - `819F38C8` takes the surface in `r29`, so the caller supplies
it. Driving the slot blindly would hand it a surface we invented, which is
exactly the scaffolding this file warns produces numbers that are not facts
about xam. The argument matters as much as the call.

### RT0 binds after all - it only needed `guide_bind_title_rt`

Running with `--guide_bind_title_rt=true` alongside the skin and reuse-ctx
flags gives, stable across the whole draw loop:

    draw dc=407D4C80 [1CC]=40877E00 wrap[0]=81640680 wrap[0C]=40870D00
                     realdev[32A0]=40958CD0 realdev[32B0]=00000000

`[realdev+0x32A0]` holds a surface where it was `00000000` in every previous
run. Zero crashes, `GetVisual` still returning `S_OK`, 156k draws over 5.4k
swaps.

**Attribution, carefully:** this was **not** my retarget of the
`XENIA_PRESENT_RT` block - that code did not run at all (no `bound RT0` line).
The bind came from the pre-existing path at `~2382`, which registers the
surface as the device's default target (`[rdev+0x3F78] = surf`) and then calls
xam's real setter `819F31A8(rdev, 0, surf)`. It simply needed the cvar, which
no run in this session had been passing.

The retarget is still a correct fix for the block it touches - that block
genuinely did aim at `[dc+0x1CC]`, the 140-byte wrapper, and its `+0x32A0` is
12KB out of bounds - but it is not what produced this result and should not be
credited with it.

**Screen: still the dashboard's sign-in UI, no Guide.** Which is expected, and
now for a precisely known reason: `[dc+0x134]` is still `1`, so
`XuiRenderPresent` returns without presenting. The composite draws run and go
nowhere.

**The next combination is the interesting one.** Patching `[dc+0x134]` to zero
was tested earlier and crashed at `819DE94C` - reading
`[dev+0x32A0] ? ... : [dev+0x32B0]` when **both were zero**. That is exactly
the condition that has now changed. So the two flags that each failed alone
fail for reasons the other fixes: the null-render patch needed a surface, and
the surface needs the null-render gate open to be presented through. Testing
them together is the obvious next step and is running now.

### The draw emitter is reached for the first time

All four flags together - skin, reuse-ctx, bind-title-rt, patch-null-render:

    GUEST CRASH at 819F5EC4  fault_addr ...20
    lr=819F5D6C  r11=40958CD0  r14=00000000
    unwind: 819F7FB4 819FEC68 8191B438 818F930C 818FB020 913EABC4

`819F5EC4` is `819F5D18 + 0x1AC`, and `819F5D18` is the function this file
records as **"the draw emitter ... never reached, because `[dc+0x134] = 1`
makes XuiRenderBegin skip its device call"**. It is reached now. That gate has
been closed for the entire history of this investigation.

Two details confirm the pieces are connected rather than coincidental:

* `r11 = 40958CD0` - the emitter is holding **the surface we bound**, so the
  RT work and the emitter are on the same path;
* the earlier crash at `819DE94C` is gone. That one faulted because
  `[dev+0x32A0]` and `[+0x32B0]` were both zero; binding RT0 fixed exactly
  that, and the failure moved deeper rather than repeating.

The new fault is `lwz r11,32(r14)` with **`r14 = 0`** - a null where a pointer
is expected, `0x1AC` into the emitter. `r14` is callee-saved, so it was either
never loaded on this path or is expected to hold something the surrounding
setup normally provides.

**Screen: still the dashboard sign-in UI.** The emitter faults before emitting,
so there are zero composite draws in this configuration and the draw rate stays
at the dashboard's `74891 / 2600` = 28.8 per swap.

So the position is: each flag removes the blocker the previous one exposed, and
the failure has walked from "no visuals" to "no skin" to "no surface" to
"inside the draw emitter". That is real movement along a single path rather
than a set of separate problems - but it is still **not pixels**, and the
honest summary is that the Guide has never yet drawn a frame.

### The emitter's missing argument is `[device+0x3F74]`

Traced the null all the way back, one frame at a time:

    emitter 819F5D18   r14 = r8, the 6th argument       (819FCF50: or r14,r8,r8)
      <- 819F7F20      r8 = r31 = its own 4th arg (r6)  (819FF188: or r8,r31,r31)
        <- 819FEB78    r6 = [r31 + 0x3F74]              (81A05E24: lwz r6,16244(r31))
          <- 8191B418  = **wrapper vtable slot 24**

So the value the draw emitter faults on is **`[device+0x3F74]`**, and the whole
draw path turns out to be wrapper methods:

| wrapper vtable slot | function | role |
|---|---|---|
| 11 | `8191AFD0` | fetches `[wrapper+0x0C]`, the device, for the present |
| 21 | `8191B338` | binds a surface: `819F38C8` -> `[dev+0x32B0]` |
| 24 | `8191B418` | drives the draw emitter |

And the near-miss is worth stating precisely: the existing bind code writes
**`[rdev+0x3F78]`** - the device's default render target, which it sets so that
`819F4C00` does not immediately unbind RT0 again. The emitter reads
**`[rdev+0x3F74]`**, the adjacent word, which nothing in our bootstrap writes.
Four bytes apart, one filled, one empty.

That is not proof that `+0x3F74` should hold the same surface - it is a
different field and may want a different object entirely (the assert near the
call, `[r6+0x20] & 0x3F == 0x3D`, says it is a typed object, so whatever goes
there has to be of that type). Logging both is the cheap first step, and the
bind log now reports `[3F74]` beside `[3F78]`.

If `+0x3F74` is genuinely never populated, it joins the skin loader, the heap
creator and wrapper slot 21 as the fourth thing in this investigation that
hardware sets up and our bootstrap does not.

### The device setup is skipped by a gate, and the gate is identified

Measured, on the real device:

    SetRenderTarget(dev 40870D00, 0, 40958CD0 fc=13FC1678) -> 40870D00;
        RT0 now 40958CD0  [3F78]=40958CD0  [3F74]=00000000

`[3F78]` holds the surface because our bind writes it. **`[3F74]` is null** -
exactly the field the draw emitter faults on, confirmed at runtime rather than
predicted.

Both fields have the same two writers, and neither ever runs:

    +0x3F74: 81A0F1C8 @81A0F244   81A0FA80 @81A0FCD4     both never executed
    +0x3F78: 81A0F1C8 @81A0F258   81A0FA80 @81A0FD1C     likewise

Those same two functions are also among the callers of `819F38C8`, the surface
binder. So one pair of routines populates the device's render state and binds
its surfaces, and neither is reached.

**Walking up finds the exact boundary**, which is more useful than another dead
subtree:

    81A0FA80  never
      <- 81A0FE48   never
        <- 819F4D28 **RAN**
          <- 8178F748 RAN   (the device creator our bootstrap calls)
            <- 81751718 RAN <- 817519D8 RAN (xam DllMain)

`819F4D28` executes and does **not** reach `81A0FE48`. The call sits at
`819F4E8C`, and the branch that skips it is right there:

    819F4E54  bl   81A0F768        ; runtime address
    819F4E58  cmpwi r3,0
    819F4E5C  bne  +0x50           ; non-zero -> jump past the setup entirely
    ...
    819F4E8C  bl   81A0FE48        ; the device setup - skipped

`81A0F768` **ran** (one `DemandFunction` entry), so it returned non-zero and
the setup was skipped. It is not a trivial predicate either - it writes
`[dev+0x59C4]`, `[dev+0x59C8]` and `[dev+0x3FE0]` to `-1` and calls three
further routines, so it is doing device reset/initialisation work and reporting
a result.

**So the presentation blocker is now a single question:** why does
`81A0F768(device)` return non-zero? That return value is the difference between
a device with render state and the null `[3F74]` the emitter dies on.

Worth noting this is the first blocker in the chain that is *not* "a routine
nobody drives" - the routine runs, and reports failure. That makes it more
tractable than the previous four, because there is a live execution to inspect
rather than an absent one to arrange.

### Correction: `81A0F768` always returns 1, and the "setup vs teardown" reading is unverified

Two things from the last section are wrong and should not be built on.

**1. `81A0F768` does not "run and report failure".** It is `0x5c` bytes with
**no conditional branches**: it calls three routines, writes `-1` to
`[dev+0x59C4]`, `[dev+0x59C8]` and `[dev+0x3FE0]`, then ends

    addi r3,r0,1
    blr

It returns **1 unconditionally**. So the `bne` at `819F4E5C` always takes, and
`81A0FE48` is never reached from `819F4D28` **by design** - not because
something failed. Reading a non-zero return as an error was an assumption, and
the disassembly contradicts it.

**2. The other chain looks like teardown, not setup.** `8178F748` reaches
`819F4A00` only through:

    lwz  r3,0(r27)
    cmplwi r3,0
    beq  +0x3c            ; skip when null
    bl   819F4A00
    addi r3,r0,0
    stw  r3,0(r27)        ; and clear the pointer afterwards

Calling something and then nulling the pointer is the shape of a release path,
not an initialise path. `[r27]` is null in our run, so the call is skipped -
which would be *correct* behaviour if there is nothing to tear down.

**What this means for the previous section:** I inferred that `81A0F1C8` and
`81A0FA80` are "the device setup" because they are the only writers of
`[dev+0x3F74]`. That does not follow. Writing a field is equally consistent
with resetting it during teardown, and `81A0F768` writing `-1` into three
neighbouring fields points the same way. The direction of all four routines is
**unverified**.

So the honest state: `[dev+0x3F74]` is null, the emitter needs it, and the two
functions that write it never run - but whether they are supposed to run, and
whether they would write a useful value or a `-1`, is not established. The next
step is to determine what these routines actually are before arranging to call
any of them, because arranging a call to a teardown routine would be worse than
doing nothing.

### `[dev+0x3F74]` is the front buffer, and the direction question is settled

Reading the *values* the two writers store answers what disassembling their
call sites could not.

**`81A0F1C8` is teardown.** It reads `[dev+0x3F74]`, releases it when non-null,
and then stores `r29` - which is set by `addi r29,r0,0`, i.e. **zero**. It
nulls the field. Same shape for `+0x3F78` immediately after.

**`81A0FA80` is the setup**, and it names the field outright:

    bl    819E7310            ; allocate
    cmplwi r3,0
    bne   +0x1c               ; success -> store it
    lis   r11,0x8166 ; addi r3,r11,13264
    bl    81A052A8            ; "Couldn't allocate front buffer.\n"
    addi  r3,r0,0 ; b exit    ; failure -> return 0
    stw   r3,16244(r29)       ; [dev+0x3F74] = the allocation

So **`[dev+0x3F74]` is the device's front buffer**, allocated by `819E7310`
(len `0x218`), and the draw emitter faults because the device has none.

That is the first time this blocker has a name in domain terms rather than as
an offset. It also explains why this file already carries a
`guide_fake_front_buffer` cvar: an earlier session evidently reached the same
conclusion from the other direction and reached for a substitute. The scaffolding
warning applies - a cloned colour surface is not what `819E7310` produces - but
the instinct was right about *what* is missing.

**Still open, and I am not going to guess it:** why `81A0FA80` never runs. The
gate analysis says `819F4D28` always skips `81A0FE48`, because `81A0F768`
returns `1` unconditionally - and if that were the whole story the front buffer
would never be allocated on hardware either, which cannot be right. So either
`81A0FE48` is reached another way that the `bl` scan does not see (an indirect
call, as several routines here are), or the device we have is of a kind that
takes a different path entirely. Both are checkable; neither is established.

Ruled out this tick, so nobody repeats it: `81A0F1C8` is not a candidate to
drive. Calling it would null the front buffer pointer, which is the opposite of
what is needed.

### Why the front buffer is never allocated: a hardcoded mode argument

Correcting the gate identified two sections ago, and then finding the real one.

**The `81A0F768` branch is not the gate.** Its `bne` jumps to `819F4E9C`, past
the setup call at `819F4E8C` - but the setup region is *entered* from
elsewhere, so falling past it proves nothing. Mapping every branch that lands
in `819F4E6C..819F4E90` finds exactly one way in:

    819F4E38  bc -> 819F4E6C

and the condition immediately before it is:

    819F4E34  cmpwi cr6,r29,2
    819F4E38  bne  +0x34          ; r29 != 2  ->  take the front-buffer path

So the allocation runs **when `r29 != 2`**. `r29` is assigned once, at
`819F4D38`: `or r29,r4,r4` - it is the function's **second argument**. So
`819F4D28(device, mode)` allocates a front buffer for every mode except `2`.

**And our path passes 2, hardcoded:**

    8178F7C8  addi r4,r0,2
    8178F7CC  addi r3,r0,0
    8178F7D0  bl   819F4D28

`8178F748` - the device creator this bootstrap calls, reached from xam's
DllMain - invokes `819F4D28(0, 2, ...)` with the mode as a literal. Mode 2 is
exactly the case that skips the allocation. Nothing is failing; this call site
is simply not the one that builds a front buffer.

`819F4D28` has three callers. The other two - `8178E9F0` (a root with no
callers) and `8191B9E8` (whose only caller `818FF140` also never runs) - never
execute. One of those is presumably the path that passes a different mode.

So the chain from "no pixels" to a single cause is now complete:

    8178F748 calls 819F4D28 with mode 2
      -> front-buffer allocation (81A0FE48 -> 81A0FA80 -> 819E7310) skipped
        -> [dev+0x3F74] stays null
          -> wrapper slot 24 -> 819FEB78 reads it as r6
            -> 819F7F20 passes it as arg 6
              -> draw emitter 819F5D18 takes it in r14 and faults on r14+0x20

Every link measured or disassembled, none inferred.

**What not to do:** call `819F4D28` again with a different mode, or write a
front buffer into `[dev+0x3F74]` by hand. The mode-2 call is deliberate, and
whatever normally runs the mode-not-2 path presumably sets up more than this
one field. The question is which of `8178E9F0` / `8191B9E8` runs on hardware
and what drives it - the same shape as the four earlier gaps, and the same
reason to identify it rather than fake it.

### Mode 1 allocates, mode 2 does not - and both mode-1 callers are roots

`819F4D28` has three call sites, and the mode each passes settles the picture:

| caller | mode | runs? |
|---|---|---|
| `8178E9F0` @`8178EB64` | `addi r4,r0,1` | never |
| `8191B9E8` @`8191BAA0` | `addi r4,r0,1` | never |
| `8178F748` @`8178F7D0` | `addi r4,r0,2` | **RAN** (our bootstrap's device creator) |

So **mode 1 is the front-buffer path** and mode 2 is the one that skips it. The
only call site that executes under this bootstrap is the one that skips.

Tracing the two mode-1 callers upward ends immediately: **both are roots with
no callers anywhere in xam.**

* `8178E9F0` - a root, `0x4DC` bytes, no strings.
* `8191B9E8` - called only by `818FF140`, which is itself a root. `818FF140`
  is also one of the three writers of the XUI context global `81D6C978`, so it
  looks like the real context-and-device creation path.

That is the fifth instance of this investigation's recurring shape - a real
routine with no callers inside xam that something outside the module drives -
after the skin loader, the heap creator, wrapper slot 21 and the wrapper's
draw path.

**But this one is harder than the others, and the difference matters.**
`81795548` (the skin loader) took **no arguments**, which is why driving it was
safe and worked. `818FF140` takes **two** (`r3`, `r4`) and immediately builds a
36-byte descriptor on the stack. Driving it means reconstructing those
arguments correctly, and getting them wrong would hand xam a malformed
descriptor rather than simply doing nothing. The same caution applies to
`8178E9F0`.

So the state at the end of this line of work:

* the complete path from "no pixels" to a single cause is measured, with no
  inferred links;
* the cause is that our device is created in mode 2, which by design has no
  front buffer;
* the routines that would create one in mode 1 are two roots that nothing in
  xam calls;
* driving either requires argument reconstruction, which is the first step that
  cannot be done by reading alone.

### Correction: mode 1 vs mode 2 was already known, and already has a cvar

I spent several sections re-deriving something this branch already documents.
`kernel_flags.cc` carries `guide_create_primary_device`, whose description
states it plainly:

> Call xam's OTHER device creator, `8178E9F0`, instead of `8178F748`.
> `8178F748` passes 2 and `8178E9F0` passes 1 ...

and `emulator.cc` selects between them at `1545`. There is also a comment at
the call site recording what happened when mode 1 was tried:

    // Snapshot the ring before the creator: mode 1 reaches
    // VdInitializeRingBuffer and the title stops swapping.

So the mode argument, the two creators, and the consequence of switching were
all established before this session. The trace from the emitter's `r14` down to
`addi r4,r0,2` is still correct and the intermediate links (the wrapper, the
front buffer, `819E7310`) are new, but the conclusion at the end of it was not.

**Lesson for anyone resuming:** before tracing a chain to its root here, grep
`kernel_flags.cc` for the addresses involved. The cvar descriptions in that
file are unusually detailed and several of them contain findings that are not
repeated in this document.

### What is actually new, and is being tested

Every previous mode-1 attempt ran with an **empty visual registry** - the skin
had never loaded, so no control had a visual and nothing could have been drawn
regardless of how the device was created. That is no longer true.

So the combination worth testing is mode 1 *plus* the working visual path:

    --lle_xam_skin_init --guide_reuse_xui_ctx --guide_bind_title_rt
    --guide_create_primary_device

The known cost is that mode 1 stops the title swapping, so dash's own rendering
is expected to suffer. That is acceptable for a measurement - the question is
whether the Guide draws, not whether the dashboard survives.

### Mode 1 with visuals: the front buffer is allocated, but the bootstrap stalls

First run of `guide_create_primary_device` together with the working visual
path. Results, all from one run:

    device creator used: 8178E9F0        (mode 1, as intended)
    81A0FA80 ran: yes                    (the front-buffer allocator)
    819E7310 ran: yes                    (the allocation itself)
    "Couldn't allocate front buffer": 0  (so it succeeded)
    crashes: 0
    visual registry: 0x119 = 281 entries (still populated)

**The front buffer is allocated for the first time.** That is the field the
draw emitter faults on, and mode 1 fills it exactly as the disassembly said it
would.

**But the Guide bootstrap does not get far enough to use it:**

    Guide button: pressed ... handler=913E69C0
    Guide button: handler returned 00000000
    Guide button: XuiInit returned 00000001, ctx now 40877DC0
    (nothing further)

No `xam CreateDevice returned`, no `queued XUI bootstrap`, no `GuideScene`
lines, zero composite draws, and the emitter never runs. The Guide thread's
last activity is JIT-ing `819F3FC8`, inside the device-creation path - it is
stuck there rather than crashing.

So the two configurations each hold half of what is needed:

| | mode 2 (default) | mode 1 |
|---|---|---|
| scenes created | yes | **no** |
| visuals attached | yes (`S_OK` + handles) | n/a |
| composite draws | yes, to #2700 | **none** |
| front buffer | **null** | **allocated** |
| emitter | faults on the null | never reached |

Neither renders. The honest reading is that mode 1 sets up the device the way
the draw path expects but breaks whatever the Guide bootstrap relies on
afterwards - consistent with the older note that mode 1 reaches
`VdInitializeRingBuffer` and disturbs the title.

**Next question, and it is a narrow one:** what does the bootstrap do between
`XuiInit` and `CreateDevice` that never returns under mode 1. That is a stall
inside a known call, on a known thread, with a known last-JIT address - which
is a far more tractable thing to chase than any of the five "nobody drives it"
gaps that came before.

### The mode-1 stall is a GPU-progress wait

`819F3FC8`, the last function JIT-ed on the stalled Guide thread, is not where
it is stuck - it is a **delay loop**:

    addi r11,r0,4 ; mtspr 9,r11      ; CTR = 4
    or r31,r31,r31  x8               ; nops
    bdnz -0x20

Four iterations of eight nops. It returns immediately. It is simply the last
*newly compiled* function; after that the thread spins in code already
translated, which is why the log goes quiet without a crash.

Its caller `819F4488` is the loop that matters:

    r29 = [r31]
    lbz  r11,11068(r29)   ; assert a flag byte
    bl   819F3FC8         ; the delay
    lbz  r11,11069(r29)   ; test another bit -> exit path at +0xC4
    lwz  r11,11024(r29)   ; else read counters:
    lwz  r10,256(r13)     ;   per-thread block
    lwz  r9,8(r31)
    lwz  r8,0(r11)
    cmplw r9,r8           ; and compare progress against a target

Poll a counter, delay, poll again - a **wait for GPU progress**. If the ring
buffer stops advancing, this spins forever, which is exactly the observed
symptom: no crash, no further JIT activity, the thread simply never returns.

That matches the note already at the creator call site - *"mode 1 reaches
VdInitializeRingBuffer and the title stops swapping"*. Mode 1 re-initialises
the ring, the GPU stops making progress against the counters this loop watches,
and the loop never exits.

**And the branch already has a cvar for it:** `guide_restore_title_ring`, which
snapshots the title's ring before the creator and restores it afterwards, on
the theory that if the ring is merely repointed the title can carry on. It has
been `false` in every run this session. Testing mode 1 with it enabled is the
obvious next step and uses infrastructure that already exists rather than new
scaffolding.

### Ring restore does not help, and the reason looks structural

Mode 1 with `guide_restore_title_ring=true`. The ring instrumentation shows
exactly what mode 1 does:

    GuideRing: saved title ring    ptr=1F9CA000 size=00008000 wb=1F9D303C
    GuideRing: before creator      ptr=1F9CA000 size=00008000 wb=1F9D303C
    GuideRing: CHANGED after 1s    ptr 1F9CA000->1DE3C000 size 00008000->00001000
    GuideRing: restored title ring ptr 1DE3C000->1F9CA000 size 00001000->00008000

So mode 1 **repoints the ring buffer** - a different address and a much smaller
size (`0x8000` -> `0x1000`). That is its own ring, for the device it just
created. Restoring the title's ring gives the title back what it needs, and the
title indeed keeps drawing (`SwapDraws` still advancing, 74895).

But the Guide is no better off: front buffer allocated, `CreateDevice` still
never returns, no scenes, no draws, no crash - the thread is still sitting in
`819F4488`, the GPU-progress wait.

**The most consistent reading** - stated as a reading, since it is not directly
proven - is that the two are in conflict rather than merely interfering: mode 1
gives the Guide's device its own ring and then waits for progress on it, while
restoring the title's ring takes that ring away. Whichever ring is installed,
one of the two consumers is waiting on a ring the GPU is not reading.

That would make this structural rather than a matter of finding another call:
xam is asking for **two** command streams and Xenia's command processor follows
one. It also explains why every variation tried here trades the title's
rendering against the Guide's without ever satisfying both.

Worth noting the existing `guide_fake_ring` cvar, still `false`, which suggests
an earlier session reached a similar suspicion.

**If that reading is right**, the remaining work is an emulator feature - a
second ring the command processor can service - not a further xam call to
locate. That is a materially different kind of task from the five
"nobody drives it" gaps, and worth being explicit about before spending more
ticks looking for a call that may not exist.

### Retracted: Xenia does follow the new ring, so the "two rings" reading is wrong

Last section proposed that xam asks for two command streams and Xenia services
one, so whichever ring is installed one consumer starves. That is not what the
evidence shows, and it should not be carried forward.

`GuideRingState` reads the **command processor's own fields**:

    *ptr  = primary_buffer_ptr_;
    *size = primary_buffer_size_;
    *wb   = read_ptr_writeback_ptr_;

So `GuideRing: CHANGED after 1s ptr 1F9CA000->1DE3C000` is not a report about
guest memory - it is Xenia's CP saying **it switched to the Guide's ring**.
`VdInitializeRingBuffer` is `kImplemented` and forwards straight to
`graphics_system->InitializeRingBuffer`, so the switch is honoured.

And the decisive comparison was already in hand: the **first** mode-1 run had
`guide_restore_title_ring` **off**, so the CP stayed on the Guide's ring for
the whole run - and the Guide stalled in `819F4488` in exactly the same way.
A consumer starved of its ring and a consumer holding its ring both stall
identically, so ring ownership is not the mechanism.

What that leaves: the Guide's wait is not simply starved. Either it is waiting
on progress it must first cause itself and never gets that far, or it is
waiting on something other than ring progress. Both are open.

**A note on this area specifically.** Four of my last several readings here -
"the context is corrupted", "there is a null device", "`[dc+0x1CC]` is not a
device", "Xenia follows one ring" - were each plausible from the evidence in
front of me and each wrong. The pattern is that the GPU/device structures have
several similarly-shaped pointers and this file names some of them
optimistically. The rule that has actually worked is the boring one: read what
the code stores, in one run, before saying what a field is.

### The stall is `InsertAsyncCommandBufferCall` waiting for its calls to drain

Reading the loop rather than theorising about it. `819F4488` is a
**progress-with-timeout** wait, not a spin:

    cmplw r9,r8                  ; last-seen vs current progress counter
    beq   -> no progress this pass
    stw   r11,8(r31)             ; else record the new value and a timestamp
    ...
    lwz   r10,12(r31) ; subf r10,r10,r30    ; elapsed since last progress
    lwz   r11,[81D31D60]                    ; threshold = 0x1388 = 5000
    cmplw r10,r11 ; bnlt -> timeout path

And the timeout path names itself. The strings it reaches for are:

    8165DC80: "A deadlock has occurred in InsertAsyncCommandBufferCall,
               because 64 Async Command Buffer Call objects have been inserted,"
    8165DD00: a "+++..." separator

So the Guide's device is inside **`InsertAsyncCommandBufferCall`**, waiting for
previously inserted async command-buffer calls to complete, and it gives up
after 5 seconds without progress. The `64` in the message is the queue depth
that triggers the wait in the first place.

The message does **not** appear in our logs (`0` occurrences against 28
`DbgPrint` lines overall), because the print is gated on `[r31+4] == 19` -
`lwz r11,4(r31) ; cmpwi r11,19 ; bne +0x34` - so a different type takes a
different path. Absence of the message is therefore not evidence the timeout
did not fire.

This is a much better-formed statement of the mode-1 stall than "the thread is
stuck": the Guide's device queues async command-buffer calls and nothing
retires them. Whether that is because Xenia never signals their completion, or
because the Guide never submits the work that would retire them, is the open
question - and both are checkable against `[r29+0x2B10]`, the progress counter
this loop actually watches.

**Operational note:** grepping the 20MB `xenia.log` without a tight pattern and
`head` produced a 20MB tool result. Always constrain both.

## Both roads end at the same place: the system command buffer is a stub

`VdGetSystemCommandBuffer` in `xboxkrnl_video.cc` does not return a command
buffer. It zeroes the structure and writes two constants:

    p0_ptr.Zero(0x94);
    store(p0_ptr, 0xBEEF0000);
    store(p1_ptr, 0xBEEF0001);

The comment above it already states the concern - *"The Guide's drawing is
expected to reach the GPU through the system command buffer, and this stub
hands back two magic constants instead of one."*

And it is genuinely on the path: **8 calls logged in the mode-1 run, two of
them inside `[XAM CREATEDEVICE]` scope**. So xam asks for the system command
buffer while creating the Guide's device, and gets `0xBEEF0000`.

That ties the two dead ends together:

| configuration | what happens |
|---|---|
| mode 2 (default) | device has no front buffer *by design*; scenes, visuals and composite draws all work, but the emitter faults on the null `[dev+0x3F74]` |
| mode 1 | device is set up properly and the front buffer **is** allocated; but device init then waits in `InsertAsyncCommandBufferCall` for async command-buffer calls that never retire |

The link between the stub and the non-retiring calls is **strongly supported
but not directly proven**: the calls are inserted against a command buffer that
is two magic constants, and nothing in Xenia processes them, so nothing can
signal their completion. Proving it would mean watching the progress counter
at `[r29+0x2B10]` across the wait.

**What this means for the goal.** Every blocker before this one was a missing
call inside xam - the skin loader, the heap creator, the surface binder, the
mode-1 device creator - and each was found and driven. This one is not: it is
an unimplemented emulator facility that the Guide's rendering path depends on.
No amount of further xam reversing produces it.

So the remaining work to get pixels is, most likely, **implementing the system
command buffer in Xenia's GPU** so that async command-buffer calls can be
submitted and retired. That is a substantially larger and different piece of
work than anything in this session, and it should be a deliberate decision
rather than something drifted into.

Everything up to that boundary now works and is reproducible:
skin loads, 281 visuals register, controls resolve visuals with `S_OK`,
scenes build, the composite draw loop runs, and the device can be given a real
front buffer. That is the state to build on.

### Retracted: the stubbed system command buffer is not the blocker

The previous section concluded that getting pixels most likely requires
implementing the system command buffer in Xenia's GPU. **That is not
supported, and testing it cost one run.**

`guide_syscmdbuf_buffer_kb=64` allocates a real buffer and hands its address
and size to the guest in the descriptor, instead of the `0xBEEF0000` constants.
With mode 1 and that flag:

    CreateDevice returned:  0      (still never returns - identical stall)
    queued XUI bootstrap:   0
    GuideScene lines:       0
    composite draws:        0
    crashes:                0
    SysCmdBufAcq #1: guest has written 0 non-zero words into the buffer we handed it

So the guest is given a real command buffer and **never writes a single word
into it**. Handing over the facility changes nothing, which means the stub was
not what the stall depends on.

Two further details worth keeping:

* the `SysCmdBufCaller` unwind is all `92xxxxxx` - **dash.xex**, not xam. The
  system command buffer is being used by the title, not by the Guide path.
* `GUIDE DRAW` scoped calls: **0**. The Guide's drawing never asks for a system
  command buffer at all, which undercuts the assumption in that function's own
  comment that the Guide's drawing is expected to reach the GPU through it.

**So the mode-1 stall cause is once again unknown**, and the honest position is
weaker than the last section claimed: `InsertAsyncCommandBufferCall` waits for
async calls to retire, and *why* they do not retire is not established. The
stub was a plausible culprit sitting right next to the evidence, and it is not
the culprit.

Recording this deliberately: the previous section came close to recommending a
large emulator feature on the strength of a code comment plus correlation. One
cvar that already existed refuted it in a single run. Test the cheap
falsification before proposing the expensive fix.

### Driving the skipped setup directly does not work either

`guide_force_front_buffer` calls `81A0FE48(device, 0)` on the device the
emitter reads, with the argument its own call site would have used. The routine
runs and **crashes inside itself**:

    GUEST CRASH at 81A04648  fault_addr ...4C  r3=0
    unwind: 81A0FF7C            (= 81A0FE48 + 0x134, inside its 0x2CC body)

So the call is reached and executes about a third of the way through before
dereferencing a null. The "forced front-buffer" log line never prints, because
the crash happens first.

That is a clean negative and it settles something worth settling: **the
front-buffer setup is not a self-contained step that can be lifted out of the
mode-1 path.** It depends on device state that `819F4D28` establishes earlier
when its mode argument is not 2 - the same suspicion recorded when this was
first proposed, now confirmed rather than assumed.

So the difference between the two creators is not one skipped call and one null
field. Mode 1 and mode 2 build genuinely different device configurations, and
the front buffer is downstream of that difference rather than being the
difference itself.

Where that leaves the two paths, with everything now tested rather than
inferred:

* **mode 2** - scenes, visuals, composite draws all work; no front buffer; the
  emitter faults on the null; the setup cannot be driven in isolation.
* **mode 1** - front buffer allocated; device init then stalls in
  `InsertAsyncCommandBufferCall` waiting for calls that never retire; cause
  unknown, and *not* the stubbed system command buffer.

Both are behind cvars that default off, so the established baseline is
unaffected either way.

### Regression check: the default path is unaffected by this session

Run with only `--guide_auto_press_seconds=45`, i.e. every new cvar off:

    GuideScene lines:   32          (the tree is walked)
    composite draws:    to #2400
    crashes / faults:   0
    visual registry:    empty       (expected - the skin flag is off)
    SwapDraws:          150295 draws over 5200 swaps

So the module-dedupe fix, resource-only XEX support, the `CalculateHash` guard,
the corrected `.text` scan bounds, the retargeted RT bind and the new probes
have not disturbed the established baseline. The visual registry being empty
here is the correct result, not a regression - it is populated only under
`lle_xam_skin_init`.

Worth doing explicitly because several of this session's changes touch code the
default path runs through (`LoadUserModule`, `ReadImage`, `CalculateHash`), not
just code behind flags.

### Mode-1 stall: two more explanations eliminated

**The wait's structures are properly initialised.** `[dev+0x2B10]`, the pointer
the stalled loop dereferences to read GPU progress, is written by exactly two
functions - `81A0F858` and `81A0FE48` - which are the *same* mode-1 setup
routines that allocate the front buffer. So under mode 1 the counter pointer is
valid; the counter simply never advances. The stall is not "waiting on an
uninitialised field".

That also explains cleanly why forcing `81A0FE48` in mode 2 crashed partway
through: it initialises several device fields, not just `[dev+0x3F74]`, and
mode 2's device is not in the state it expects.

**Graphics notifications are being delivered.** The obvious candidate for what
retires async command-buffer calls is the GPU notification callback, and it is
running: **2 actual `VdCallGraphicsNotificationRoutines` calls** in the mode-1
run (distinct from the 5 import-table lines, which are not calls). Front buffer
allocated, notifications delivered, and `CreateDevice` still never returns.

So neither "the structures are not set up" nor "no notifications arrive"
accounts for it. What remains untested is whether **enough** notifications
arrive - two is a small number, and if the title's swapping is disturbed under
mode 1 the callback may simply fire far less often than the wait needs. That
would be measurable by comparing the notification count against a mode-2 run,
which is one run rather than a new line of reverse engineering.

Running tally of eliminated explanations for this stall: the stubbed system
command buffer, ring ownership, uninitialised wait structures, and absent
notifications. None of them.

### Notification frequency is not the difference either

Counting `VdCallGraphicsNotificationRoutines` calls (the log samples at
`n <= 3 || n % 300 == 0`, so a handful of lines means a handful of calls):

| run | notification calls |
|---|---|
| mode 1 - stalls in `InsertAsyncCommandBufferCall` | **2** |
| mode 2 - scenes, visuals, composite draws to #2700 | **1** |

The **working** configuration receives *fewer* notifications than the stalling
one. So "not enough notifications arrive to retire the queued calls" is wrong,
and there is a second, more useful consequence: the graphics notification
callback is not what drives mode 2's composite draw loop either. Both
configurations are essentially notification-free.

That closes the cheap discriminators for this stall. Eliminated so far, each by
measurement rather than argument:

1. the stubbed system command buffer (a real buffer changes nothing; the guest
   writes zero words into it, and the Guide never requests one);
2. ring ownership (the CP does switch to the Guide's ring; starved and
   not-starved stall identically);
3. uninitialised wait structures (`[dev+0x2B10]` is set up by the same mode-1
   routines that allocate the front buffer);
4. absent notifications (they are delivered);
5. insufficient notifications (the working path gets fewer).

**Honest state of this line of work:** the mode-1 stall cause is unknown, and
the inexpensive experiments are exhausted. Further progress needs either
instrumenting the wait itself - logging `[[dev+0x2B10]]` across the loop to see
what the counter actually does - or accepting that mode 2 plus a front buffer
obtained some other way is the more promising route. Both are real work rather
than another flag combination.

### The wrapper's device is not the device mode 1 creates

`ProgressWatch` resolves the chain the draw path uses and finds the counter
pointer empty:

    ProgressWatch: ctx=40877DC0 wrap=40877E00 dev=40870D00 counter_ptr=00000000

`[dev+0x2B10]` is **null**, so no progress values were observed - there was
nothing to read. Taken with what is already established, that is more useful
than a counter trace would have been.

`[dev+0x2B10]` is written by exactly two functions, `81A0F858` and `81A0FE48`,
both part of the mode-1 setup. Under mode 1 those routines **do** run - the
front buffer is allocated, which is `81A0FA80` downstream of `81A0FE48`. Yet
the device the wrapper points at still has a null counter pointer.

The only consistent conclusion: **mode 1 sets up a different device object than
the one `[wrapper+0x0C]` refers to.** The draw path and the mode-1 device
creator are working on two separate devices.

That retro-explains several things that were confusing on their own:

* forcing `81A0FE48` on the wrapper's device crashed partway - it was being run
  against a device that had not been through the rest of mode 1's setup;
* mode 1 allocating a front buffer did not help the draw path - the buffer went
  to mode 1's device, not to the one the emitter reads;
* mode 2's device never gets `[+0x2B10]` either, which is consistent with it
  simply never being set up as a render device at all.

**Caveat, stated plainly:** this watcher followed the *draw path's* device. It
did **not** observe the device the stalled thread is working on, because that
object is not reachable from the context chain at that point. So this says
nothing about why mode 1's own wait never progresses - it says the two devices
are distinct, which is a different and previously unnoticed fact.

The question that now matters more: **which device should the Guide be drawing
through** - and if it is mode 1's, how the wrapper is meant to come to point at
it.

### How the wrapper gets its device, and a note on descending further

`[wrapper+0x0C]` is not set at construction - `8191AD00` stores `r30 = 0` into
it. Three functions in the wrapper's class write that field and have run:

* `8191AD00` - the constructor, writing zero;
* `8191AD70` - a small helper, called from `8191ADC0` and `8191BAC8`;
* `8191BAC8` - **the setter**: `8191BAC8(wrapper, device, x)` keeps `r4` in
  `r30` and stores it to `[wrapper+0x0C]`.

The setter's only caller is `818FCE38`, which **does run** (indirectly - it has
no `bl` callers), and it takes the device from its own object:

    or  r3,r29,r29     ; the wrapper
    lwz r4,8(r31)      ; the device, from [X+0x08]
    bl  8191BAC8       ; wrapper->SetDevice

So the wrapper is handed whatever `[X+0x08]` holds when `818FCE38` runs, and in
our runs that is `40870D00` - not the device mode 1 creates.

**A note on method, since this is the fifth consecutive link in one chain.**
Each tick of this descent costs a run or a disassembly and yields one more
"...and that comes from `[Y+0x08]`". The chain has not converged and there is no
evidence it is about to. The useful reframing for whoever picks this up:

* the *mechanism* is now well mapped - context -> wrapper -> device, with the
  setter and its caller both identified;
* what is missing is not another link but an **ordering**: mode 1 builds a
  device that never enters this chain, and nothing observed so far installs it
  into `[X+0x08]` before `818FCE38` reads it;
* so the question worth attacking is *when* `818FCE38` runs relative to the
  device creator, not *what* it reads. That is answerable by timestamping both
  in one run, rather than by tracing another pointer.

## The ordering, measured: the wrapper is wired up 16,000 lines before mode 1 runs

The reframed question - *when* does `818FCE38` run relative to the device
creator - is answerable from `DemandFunction: enter` lines already in the log,
with no new run at all:

| function | role | first entry (log line) |
|---|---|---|
| `819F4D28` | device creator core | 4074 |
| `818FCE38` | reads `[X+0x08]`, calls the setter | 5010 |
| `8191BAC8` | `wrapper->SetDevice` | **5026** |
| `8178E9F0` | the mode-1 creator | **21509** |
| `81A0FE48` | front-buffer setup | 21514 |
| `81A0FA80` | front-buffer allocation | 21572 |

**The wrapper receives its device at line 5026, during early boot. Mode 1 does
not run until line 21509 - at the Guide button press, roughly 45 seconds and
16,000 log lines later.**

So mode 1 is not competing with the draw path for a device; it is arriving long
after the draw path has already been wired to a different one, and nothing
re-installs it. That single fact explains the whole cluster of observations:

* mode 1 allocates a front buffer that the emitter never sees - different
  device, and the wrapper was bound before it existed;
* `[wrapper+0x0C]`'s device has a null `[+0x2B10]` - it was created by the
  early path (`819F4D28` at 4074), which for mode 2 never sets those fields;
* forcing `81A0FE48` on the wrapper's device crashed - that device was built by
  a different creator run entirely.

**Two ways forward, and both are now concrete rather than exploratory:**

1. **Run the device creator during boot**, before `818FCE38` at line 5010, so
   the wrapper is handed a mode-1 device in the first place. This branch
   already has `guide_bootstrap_before_device` in the scaffolding list, which
   suggests the ordering was suspected before.
2. **Re-install the device afterwards** by calling the setter directly -
   `8191BAC8(wrapper, device, x)` - which is identified and whose arguments are
   known. The obstacle is that mode 1 stalls before returning its device, so
   there is currently no pointer to install.

Option 1 is the more faithful of the two: it puts the call where the ordering
says it belongs rather than patching around the consequence.

### The mode-1 stall is expected behaviour, and already has a lever

Several sections above investigate why the mode-1 creator never returns,
eliminating five explanations in the process. That work was not needed:
`guide_bootstrap_before_device`'s own cvar description states it outright.

> Queue the Guide bootstrap onto the title thread BEFORE calling xam's device
> creator, instead of after. Only matters with `guide_create_primary_device`:
> **the mode-1 creator never returns**, so in the normal order the queue call
> is never reached and the bootstrap never runs at all. Reversing it lets the
> mode-1 device come up - **it does bind render targets, which mode 2 never
> does** - while the bootstrap proceeds on the title thread.

So "mode 1 stalls" is known, expected, and worked around by reordering rather
than fixed. The eliminations recorded above are still worth keeping - they rule
out five wrong explanations for anyone who assumes the stall is a bug - but the
question they were answering was already settled here.

**This is the second time in this session that `kernel_flags.cc` contained the
answer.** The first was mode 1 versus mode 2 itself. The lesson recorded
earlier bears repeating in stronger terms: *grep `kernel_flags.cc` for the
addresses and the behaviour before starting any investigation.* Its
descriptions are effectively an undocumented second findings file, and they are
not summarised anywhere else.

Testing the combination that description implies but which no run in this
session has used:

    --lle_xam_skin_init --guide_reuse_xui_ctx --guide_bind_title_rt
    --guide_create_primary_device --guide_bootstrap_before_device

which should give the bootstrap (scenes, visuals) *and* a mode-1 device that
binds render targets - the two halves that have never been available together.

### Both halves in one run - but not wired to each other

`--lle_xam_skin_init --guide_reuse_xui_ctx --guide_bind_title_rt
--guide_create_primary_device --guide_bootstrap_before_device`:

    GuideScene lines:      38          (scenes built)
    GetVisual -> S_OK:     2           (visuals attached)
    front buffer alloc:    yes         (81A0FA80 ran)
    crashes:               0
    composite draws:       2           (vs #2700 under mode 2)

So for the first time the bootstrap **and** the mode-1 device both come up in
the same run - the reordering does what its cvar says. But the two draws that
do happen show the halves are not connected:

    draw dc=407D5FD0 [134]=00000001 [1CC]=40877E00
                     wrap[0C]=40870D00 realdev[32A0]=40958CD0

* `wrap[0C]` is still **40870D00** - the wrapper is on the device it was given
  at line 5026, not on the one mode 1 just built. Reordering the *bootstrap*
  does not reorder the **wrapper's** binding, which happens during early boot
  regardless.
* `[134]` is still `1`, so the present is still gated off.
* the draw loop stops after 2 iterations, because the Guide thread goes into
  the mode-1 creator and never returns - expected per that cvar, but it means
  the loop that would draw is not running.

Screen unchanged: dash's sign-in UI.

Also captured, and worth keeping: the title device carries a
**surface-shaped field at `+0x3210` = `40958CD0`, `1280x720`** - the same
surface our RT bind installs. So the title's real front-buffer-sized surface is
identifiable on the device, which is the thing a correct binding would need.

**State:** the two halves exist simultaneously and are still bound to different
devices. Connecting them means getting `[wrapper+0x0C]` to point at the mode-1
device, and the wrapper is wired ~16,000 log lines before that device exists.
The setter `8191BAC8(wrapper, device, x)` is identified, so the missing piece
is a device pointer to pass it - which mode 1 does not return, because it never
returns at all.

### Mode 1's device is never published, which bounds the remaining work

Watching both device globals across a full mode-1 run:

    ProgressWatch: xam dev global 00000000 -> 40870D00 ([2B10]=00000000 [3F74]=00000000)
    ProgressWatch: ctx=40877DC0 wrap=40877E00 dev=40870D00 counter_ptr=00000000

`81D43684` receives `40870D00` - the **same** device the wrapper already has,
with both `[+0x2B10]` and `[+0x3F74]` null - and then never changes again.
`801E6FC8` never changes at all. So the device mode 1 builds, the one that gets
a front buffer and a progress counter, is **never published to either global**.

That settles the question this watcher was added to answer, and it bounds what
is left:

* the setter `8191BAC8(wrapper, device, x)` is identified and its arguments are
  known;
* the wrapper's device and the mode-1 device are both identified as *concepts*;
* but there is **no pointer to the mode-1 device available anywhere** outside
  the thread that is stalled inside its creator.

So connecting the halves is not a matter of calling one more function with
values already in hand. It needs the pointer extracted from the creator itself
- hooking `81A0FE48`'s entry to capture `r3`, or recognising the device by its
signature (`[+0x3F74]` and `[+0x2B10]` both non-null) with a memory scan. Both
are real instrumentation work rather than another flag.

**Worth stating plainly at this point:** every remaining route to pixels now
requires building something rather than discovering something. The discovery
phase of this line has ended - the mechanism is mapped end to end, from the
skin through the visual registry to the wrapper, the device, the front buffer
and the draw emitter, with each link measured. What is left is engineering
against that map.

### The signature scan does not find the mode-1 device, and why

Three iterations, each informative:

1. **Two plausible pointers at `+0x3F74` and `+0x2B10`** matched 8 unrelated
   objects at once, uniformly spaced `0xC0` apart - a structure array, not
   devices.
2. **Adding a vtable-in-`.rdata` test at `+0`** cut that to `0 candidates
   (1 passed the vtable+fields test)`. The two-counter reporting is what made
   this readable: a bare `0` would have said "no such device", when in fact one
   object passed everything and was rejected by an over-strict `[32A0]` filter.
3. **Dropping that filter** surfaced the single survivor:

        candidate 407FA7C0  vt=8163E814  [3F74]=3F800000  [2B10]=42820000  [32A0]=407FDD60

   and it is a **false positive**. `3F800000` is `1.0f` and `42820000` is
   `65.0f` - float bit patterns, not pointers. Its vtable is `8163E814`, not
   the device class.

The flaw is mine: a range test of `0x10000000..0x50000000` for "looks like a
guest pointer" also accepts most single-precision floats, which are extremely
common in a graphics object. Any future scan here needs either an alignment
test, a vtable equality test against the known device class, or both.

**So the mode-1 device is not findable this way**, at least not in
`0x40000000..0x41000000` with these fields. Combined with the earlier result
that it is never published to either device global, the pointer simply is not
reachable from outside the creator.

That leaves one route, and it is the more invasive one that was set aside
earlier: **hook `81A0FE48`'s entry and capture `r3`**. That routine runs, on
the mode-1 device, and its first argument *is* the device. Everything needed to
use the pointer afterwards is already identified - the setter `8191BAC8`, the
wrapper, and the arguments - so the hook is the only missing piece.

Recorded as the concrete next step rather than attempted here, because
instrumenting a guest function entry is a different class of change from
anything else in this session and deserves to start from a clean context.

## The wrapper's SetDevice: 8191BAC8(wrapper, device, params)

Two findings, both from reading rather than running.

**1. It is never called after boot.** A run with `guide_trace_devsetup=true`
captured `DevSetup #1: device=407CB880 arg2=709DF190 lr=819F4E90` but **zero**
`SetDevice #` lines. The wrapper is bound around log line 5026, during early
boot; the breakpoints install at the Guide press (~line 21000). The hook was
installed far too late to see it. This is the same install-timing mistake as
the device scan that was moved from `i == 30000` to `i == 4000` - worth
remembering as a class: *a breakpoint proves nothing about calls that happen
before it is installed.* To observe the boot-time binding the hook would have
to be installed at xam load.

**2. The third argument is a pointer, not a flag.** Disassembly of the tail:

```
81922d00  cmplwi cr6,r30,0     ; device
81922d04  bneq +8
81922d08  twi                  ; assert device != 0
81922d0c  lwz r11,12(r31)      ; current [wrapper+0x0C]
81922d10  cmplw r30,r11
81922d14  beq  +0x18           ; no-op if unchanged
81922d18  bl   819F4998        ; addref new device
81922d24  bl   8191AD70        ; release old
81922d28  stw  r30,12(r31)     ; [wrapper+0x0C] = device
81922d2c  addi r3,r31,16       ; dest = wrapper+0x10
81922d30  addi r5,r0,124       ; size = 124
81922d34  or   r4,r28,r28      ; src = third argument
81922d38  bl   memcpy
```

(ppcdis addresses; subtract 0x7200 for runtime - see the deprecation note.)

So `r5` is a **readable 124-byte block** memcpy'd into `wrapper+0x10`, which is
exactly the region the constructor clears. Passing `0` would memcpy from null.

That matters because it is the *second time* this argument-guessing mistake
would have been made: the earlier hand-call of `81A0FE48(real_dev, 0)` crashed
because its second argument is a stack pointer, not 0. Reading the callee
before calling it cost two disassembly commands and avoided a third crash.

`guide_rebind_wrapper_device` therefore passes the wrapper's **own** `+0x10` as
the source, making the memcpy a self-copy that leaves those fields untouched
while still swapping the device pointer.

### Correction: the front buffer belongs to the TITLE, not to either Guide device

The rebind worked mechanically on the first try:

```
Guide: rebound wrapper 40877E00 from device 40870D00 to 407CB880 -> 00000000;
       [wrapper+0C] now 407CB880 ([3F74]=00000000)
```

`[wrapper+0x0C]` changed, so `8191BAC8` accepts a hand-made call and the
124-byte self-copy is safe. But the run refuted the premise the rebind was
built on. Field `[3F74]` across the whole log:

```
7  [3F74]=00000000
2  [3F74]=A240A380   <- line 22201/22219, device 40952400
```

Device `40952400` is `VdGlobalDevice [801E6FC4]` - **dash's own device**. It is
the only one with a front buffer. Mode 1's device `407CB880` reads
`[3F74]=00000000` at every sample including `GuidePreDraw`, because mode 1's
init stalls and therefore never allocates one.

So "mode 1 allocates a front buffer, mode 2 doesn't, join them" was wrong in
its central claim. Both Guide-side devices are frontbuffer-less; the display
belongs to the title. Rebinding to `407CB880` predictably crashed at
`8191B07C` (`fault_addr 0`, `r3=407CB880`) - dereferencing a device that never
finished initialising.

This also fits how the Guide is supposed to work: it composites **over** the
running title rather than owning a display. That makes the title device the
natural target, and it removes the need for `guide_create_primary_device`
entirely - along with the mode-1 init stall that has blocked this path for
many ticks.

`guide_rebind_wrapper_device` now prefers `VdGlobalDevice` when it has a
non-null front buffer, falling back to the captured mode-1 device otherwise.

**Not yet established:** that the title device *accepts* the Guide's draws.
It is owned by dash, has its own render targets, and `[dc+0x134]=1` still
gates the present. A clean rebind is necessary, not sufficient.

### Third time lost to not reading kernel_flags.cc first

Having retargeted the rebind at the title device, I dropped
`--guide_create_primary_device` on the reasoning that mode 1's device was no
longer needed. The run crashed at `818FB17C` (`lr=913EA9F0`, dash's own code)
at log line 21696, before any `GuideScene`, `composite draw` or `DevSetup`.

The flag's own description says exactly why, and I had not read it:

> "Only matters with guide_create_primary_device: the mode-1 creator never
> returns, so in the normal order the queue call is never reached and the
> bootstrap never runs at all."

`guide_bootstrap_before_device` **reorders the bootstrap ahead of the device
creator**. With no creator being called, that ordering is meaningless and the
bootstrap runs against a state that is not ready. The two flags are a pair;
dropping one and keeping the other is not a valid configuration.

The rule, now stated for the third time in this file: **grep
`kernel_flags.cc` for a flag before adding, removing or reasoning about it.**
The cvar descriptions in that file are a second findings document, and they
have now pre-answered three questions that each cost a tick to rediscover
(mode 1 vs 2; the mode-1 stall being expected; this flag pairing).

Correct configuration for a title-device rebind: **neither** mode-1 flag.
Mode 2 alone already produces scenes, visuals and composite draws, and the
rebind target is read from `VdGlobalDevice` directly, so it has no dependency
on the `DevSetup` capture at all.

### Rebinding to the title device: front buffer reaches the draw path, then faults

First configuration in the project where the draw path sees a real front
buffer:

```
Guide: rebound wrapper 40877E00 from device 40870D00 to 40952400 -> 00000000;
       [wrapper+0C] now 40952400 ([3F74]=A240A380)
GuidePreDraw: device 40952400 RT0=00000000 RT1=00000000 depth=00000000
              [3F74]=A240A380
```

`GuidePreDraw` had read `[3F74]=00000000` in every previous run. So the
null-front-buffer blocker is genuinely cleared - and the next one is reached.

Crash: guest PC `819E567C`, `fault_addr 0000000100010007`. The instruction is

```
819ec6b8  lwzx r28,r27,r30     ; r28 = [device + 0x3308]   (r27=0x3308)
...
819ec868  cmplwi cr6,r28,0     ; null-checked...
819ec86c  beq   +0xe8
819ec870  lwz   r11,11036(r30) ; [device+0x2B1C]
819ec87c  stw   r11,8(r28)     ; ...but stored through regardless
```

with `r28 = 0000FFFF`, so `0xFFFF + 8 = 0x10007` - exactly the fault address.

`[title_dev+0x3308]` is a **0000FFFF sentinel**. xam's draw code indexes a
per-device table that only a xam-created device has populated. It null-checks
the slot but does not validate it, so dash's sentinel sails through.

Conclusion: **device identity matters, the front buffer does not travel with
it.** The Guide's own device `40870D00` has xam's tables but no buffer; the
title's `40952400` has the buffer but not the tables. Swapping the whole
device trades one missing half for the other.

`guide_borrow_front_buffer` therefore copies only `[+0x3F74]` from the title
device into the Guide's own device, leaving device identity alone. That keeps
xam's table state intact, which is what `0x3308` needs.

**Caveat to check in the result:** a front buffer is a surface object, and the
Guide device's `[32A0]/[32B0]` (size/format, both `00000000`) may also need to
agree with it. Lending one field may simply move the fault rather than clear
it.

### guide_borrow_front_buffer works; clear_null_render is still the wrong lever

Lending just `[+0x3F74]` to the Guide's own device is stable:

```
Guide: lent front buffer A240A380 from title device 40952400
       to guide device 40870D00; [3F74] now A240A380
GuidePreDraw: device 40870D00 ... [3F74]=A240A380
```

Composite draws ran to **#2100+ with no crash**. For the first time xam's
device tables and a real front buffer are on the same object. Committed as
`b783e6a`.

Remaining on every line: `[134]=00000001` and `RT0/RT1/depth=00000000`.

I then paired the borrow with `guide_clear_null_render`, whose description
promises exactly both halves (DC built non-null -> `XuiRenderBegin` runs
`vtable[20]`, "where any render-target setup would happen"). It crashed at
`819DE94C`, `r3=0`, deref `+0x24`, unwinding through `818FDE60` - the DC
initialiser - before the borrow even ran.

That flag was **already documented in this file, at line 370, as
non-functional**:

> The pre-existing `guide_clear_null_render` never worked because it clears
> `[ctx+0x1C]` at bootstrap while the per-frame context is constructed later.

So its cvar description states an intent the implementation does not achieve.
`kernel_flags.cc` is a good index of *what a lever is for*, but NEXT.md is
the authority on *whether it works*. Check both - reading only the cvar text
cost this run.

The working lever is `guide_patch_null_render`: it nops the store at
`818FDF14` in xam's image at load time, so the constructor's zero in
`[dc+0x134]` survives, `XuiRenderBegin` does not skip `vtable[20]`, and the
draw emitter `819F5D18` can build a DRAW_INDX packet. That is the pairing now
under test.

### Both null-render flags crash identically - the fault is vtable[20], not the flag

Pairing the borrow with `guide_patch_null_render` produced a crash **byte-for
-byte identical** to the `guide_clear_null_render` run: same PC `819DE94C`,
same `r3=0`, same `fault_addr 0x100000024`, same unwind
`819DEB30 / 8191B024 / 818FDE60 / 818F8374`.

Two conclusions follow.

1. The crash is not a property of either flag. It is what happens **whenever
   `[dc+0x134]` is genuinely 0**, because `XuiRenderBegin` then stops skipping
   `dc->vtable[20]` and that path dereferences null at `+0x24`. Both flags
   work; reaching the gated code is the problem.

2. **The line-370 note is wrong under `guide_reuse_xui_ctx`.** It says
   `clear_null_render` can never work because it clears `[ctx+0x1C]` at
   bootstrap while the per-frame context is built later. With
   `guide_reuse_xui_ctx` on, the per-frame context *is* the bootstrap context,
   so the clear does apply - which is exactly why it crashed identically.
   The note should be read as "never worked *before* ctx reuse existed".

The faulting function is `819DE8F8` (len 0x174), called from only two sites,
both in `819DEA70`.

**Ordering is the actual defect.** `vtable[20]` runs during DC *construction*;
`guide_borrow_front_buffer` lent the buffer from the composite-draw path, long
after. `GuidePreDraw` still read `[3F74]=00000000` at the moment of the crash
- the signature of a fix that arrives too late rather than one that is wrong.

The lend now also runs from the `XuiCtxWatch` poller (250us, started as soon
as the context global is non-null), walking
`[81D6C978] -> ctx -> [+0x08] wrapper -> [+0x0C] device` and writing
`[dev+0x3F74]` the first time that device exists. The draw-path lend stays as
a fallback; whichever runs first wins, and the second sees a non-null field
and skips.

### The early lend never ran: XuiCtxWatch is gated behind an env var

The `XuiCtxWatch`-hosted lend produced **zero** `lent front buffer` lines and
an identical crash. Cause: that thread is spawned inside

```
if (std::getenv("XENIA_XUICTX_WATCH")) {
```

and is further nested under the `guide_xam_ui_startup` path, so it never
started. The code was correct and simply never executed.

Worth generalising, because this is the second time a change has been judged
by a run in which it never ran (the first was the device signature scan
triggering at `i == 30000`, past the harness window): **before concluding
anything from a run, confirm the new code produced at least one line of
output.** A silent absence looks exactly like a fix that did not work.

`guide_borrow_front_buffer` now spawns its own `GuideLendFB` thread next to
`GuideAutoPress`, gated only by its own cvar, polling
`[81D6C978] -> ctx -> [+0x08] -> [+0x0C]` every 250us and logging either the
lend or an explicit give-up. No flag should depend on an unrelated environment
variable to take effect.

### GuideLendFB crashed the host: guest-pointer walks need guarding

The dedicated thread did run this time (`GuideLendFB: watching for the guide
device`), and immediately took the **host process** down:

```
ABORT: UNHANDLED EXCEPTION dialog during boot after ~1s
Title Info: Title not started yet.
Faulting thread name: GuideLendFB
```

The thread starts before the title does and walks pointers *read out of guest
memory*, so unmapped and garbage addresses are the normal case, not the
exceptional one. `Memory::TranslateVirtual` does not validate: it returns a
host pointer into reserved-but-uncommitted space, and the load faults the
emulator rather than returning garbage.

This is a different failure mode from a guest crash and needs saying
explicitly, because the harness reports both as "ABORT: UNHANDLED EXCEPTION":
**a fault whose `Faulting thread name` is one of our own watcher threads is a
host bug in the watcher, not a finding about the guest.** The give-up branch
added last tick did its job here - the absence of both the lend line and the
give-up line placed the failure inside the loop.

Reads in any polling thread now go through a guarded helper: reject null,
sub-page and misaligned addresses, require `LookupHeap` to find a heap,
require `QueryProtect` to report the page committed, and only then translate.
The store is likewise skipped if translation yields null.

### Guarded reads fixed the host crash; the chain itself is wrong

With the guard in place there is no host abort and `GuideLendFB` runs for the
whole session - but it logged **neither** the lend nor the give-up, so its
condition never became true. Meanwhile `GuidePreDraw` in the same run showed
the device present the entire time:

```
GuidePreDraw: device 40870D00 ... [3F74]=00000000
```

So the device exists and has a null front buffer - exactly the case the lend
is supposed to catch - and the poller could not see it. The two disagree
because they walk **different chains**:

* `GuidePreDraw`: `guide_draw_this_+12` -> dc -> `[dc+0x1CC]` wrapper ->
  `[+0x0C]` device.
* `GuideLendFB`: `[81D6C978]` ctx -> `[+0x08]` wrapper -> `[+0x0C]` device.

The second is from the note at line 2346 of this file. One of its links is
null in practice. Rather than guess which - the mistake this file has now
recorded several times - the poller prints `ctx`, `wrap`, `gdev`,
`gdev[3F74]`, `tdev` and `tdev[3F74]` once per second for the first 40s.

Note the DC-based chain is not simply a drop-in replacement: `guide_draw_this_`
is only set at draw time, which is the "too late" problem the early lend
exists to avoid. If the ctx chain is broken, the fix is to find an
*early-available* route to the wrapper, not to fall back to the draw-time one.

### The ctx chain was right; my own guard was the null

Diagnostics settled it in one run:

```
GuideLendFB[0]:     ctx=00000000 wrap=00000000 gdev=00000000 tdev=00000000
GuideLendFB[4000]:  ctx=40877DC0 wrap=40877E00 gdev=40870D00
                    gdev[3F74]=00000000 tdev=00000000 tdev[3F74]=00000000
```

`ctx -> [+0x08] -> [+0x0C]` resolves to `40877E00` / `40870D00`, matching the
composite-draw log (`[1CC]=40877E00 wrap[0C]=40870D00`) exactly. The note at
line 2346 is correct and the poller finds the device within a second.

The null was `tdev` - the read of the **constant** `0x801E6FC4`
(VdGlobalDevice), which the draw path reads unguarded every frame and gets
`40952400`. The guarded helper added last tick rejected it: `LookupHeap`
succeeds, but `QueryProtect` declines for that page, so the helper returned 0
and the lend condition could never be satisfied.

So the fix for the host crash introduced a false negative that looked exactly
like a missing object. Both failure modes - the unguarded version crashing the
host, and the guarded version silently seeing nothing - produced runs with no
lend line. Only the per-link dump distinguished them.

Guard now allows module/kernel image addresses (`0x80000000-0x90000000`)
through when `QueryProtect` declines, since `LookupHeap` has already
established the address is backed and xam's image is mapped once loaded.
Pointers read out of guest memory still get the full check.

## REFUTED: the front buffer is not what vtable[20] is missing

The early lend now works. It fires at iteration 245, well before DC
construction, and the draw path sees it:

```
Guide: lent front buffer 98409940 to guide device 40870D00 EARLY
       (GuideLendFB, iteration 245); [3F74] now 98409940
GuidePreDraw: device 40870D00 RT0=00000000 RT1=00000000 depth=00000000
              [3F74]=98409940
```

And the crash is **byte-identical** to every run without it - same guest PC
`819DE94C`, same `r3=0`, same `fault_addr 0x100000024`, same unwind, down to
the same host address `A09BCD77`.

So the ordering hypothesis is dead. `vtable[20]` does not fault for want of a
front buffer, early or late. Three ticks of work on lend timing (draw path ->
XuiCtxWatch -> dedicated thread -> guarded reads -> relaxed guard) produced a
working mechanism that answers the question **no**.

Where the null actually comes from, read rather than guessed:

```
819e5d28  or r3,r29,r29      ; caller 819DEA70 passes r29...
819e5d2c  bl 819DE8F8        ; ...into the faulting function
819e5b08  or r31,r3,r3       ; which keeps it in r31
                             ; and derefs [r31+0x24] -> fault, r3=0
```

`r29` is already null **in the caller**, `819DEA70`. The next question is what
`819DEA70` failed to construct - not anything about front buffers.

### Second bug found by the same run: the lend raced the title

`tdev=FFCAE000` on the first poll - `VdGlobalDevice` holds garbage until dash
publishes its device - so the lend copied `98409940` rather than dash's
`A240A380`. It lent a plausible-looking value read from a junk pointer.

That did not affect the refutation (the crash is identical either way) but it
would have quietly poisoned any later result. Early pollers must validate what
they read, not just that the read succeeded: `tdev` is now required to fall in
the guest heap range `0x40000000-0x50000000` before it is trusted.

### The vtable[20] null is [wrapper+0x0C], traced to its source

Chased the null up three frames by reading, not guessing:

```
8191AFD0:  or   r31,r3,r3      ; r31 = first arg (a wrapper)
           lwz  r3,12(r31)     ; r3 = [wrapper+0x0C]  <- the device slot
           bl   819DEA70
819DEA70:  or   r29,r3,r3      ; r29 = first arg, i.e. that device
           or   r3,r29,r29
           bl   819DE8F8
819DE8F8:  or   r31,r3,r3
           ...  [r31+0x24]     ; faults, r3=0
```

So the fault is a **null `[wrapper+0x0C]`** at DC-construction time. That very
likely explains why the null-render flag exists at all: xam skips
`vtable[20]` exactly when the device is not yet bound, and forcing the flag to
zero removes a guard rather than enabling a feature.

The complication is that `GuideLendFB` reads `[40877E00+0x0C] = 40870D00` -
non-null - one second into boot, and the composite-draw log agrees
(`[1CC]=40877E00 wrap[0C]=40870D00`). So either a **different** wrapper
reaches `8191AFD0`, or that slot is cleared again before DC construction.

Those two have different fixes, so a `cpu::Breakpoint` at `8191AFD0` now logs
the wrapper identity and its `[+0x0C]` rather than assuming. Unlike the
`8191BAC8` hook that saw nothing, this call happens **at the Guide press**,
after the traces install, so it should fire.

Also confirmed this run: validating `tdev` did not change which buffer is
lent (`98409940` again, iteration 241). `VdGlobalDevice` legitimately points
at a different device that early - it is not garbage, so the range check
passes. Dash's `40952400`/`A240A380` appears later. Whether the Guide should
get the early buffer or dash's is now an open question, but it is moot while
`vtable[20]` faults for an unrelated reason.

## CORRECTION: [wrapper+0x0C] is NOT null at the faulting call

The trace fired at exactly the frame the crash unwinds through:

```
WrapRender #1: wrapper=40877E00 [+0C]=40870D00 lr=818FDE60
```

Same wrapper the composite-draw log and `GuideLendFB` both report, and its
device slot is **populated**. So the previous tick's conclusion - "the fault
is a null `[wrapper+0x0C]` at DC-construction time" - is wrong as stated. The
disassembly was right about *where* r3 comes from (`lwz r3,12(r31)`), but the
inference that the slot must therefore be null did not survive measurement.

Worse for that theory: **this run did not crash at all.** No `GUEST CRASH`
lines, where the identical configuration one tick earlier crashed at
`819DE94C` every time.

The only delta is the two `cpu::Breakpoint`s installed by
`guide_trace_devsetup`. Breakpoints change JIT codegen and timing, so the
honest reading is one of:

* the crash is a **race** that the breakpoints' overhead hides, or
* the crash was always intermittent and earlier runs were unlucky.

Do not record "the trace fixes the crash" as a finding until a repeat run
says so. A measurement that changes the thing being measured is exactly the
case where one sample proves nothing.

### But no drawing happens either

```
GUEST CRASH     0
GuideScene     38
composite draw  0     <- borrow-only produced 2100+
DRAW_INDX       0
```

So with `guide_patch_null_render` on, the run survives and builds scenes, but
the composite-draw path is never entered. Not crashing is not the same as
rendering: clearing `[dc+0x134]` moves execution onto a branch that produces
no draws at all, which is a worse outcome for pixels than the crashing path
that at least reached #2100.

### The crash is a race; the breakpoints hide it. And the flag kills the draws.

Repeat run confirms the previous tick was not a fluke: **2 of 2** runs with
`guide_trace_devsetup` on show no crash, identical `WrapRender #1:
wrapper=40877E00 [+0C]=40870D00 lr=818FDE60`, and scenes built. The same
configuration **without** the trace crashed at `819DE94C` every time.

So the crash is timing-dependent - a race that breakpoint overhead hides -
not a deterministic null. Two supporting facts:

* The crash predates `GuideLendFB` entirely (it first appeared with the
  draw-path borrow), so our own guest-memory writes are not the cause.
* `[wrapper+0x0C]` is populated whenever we are slow enough to look.

### The real problem is not the crash

```
                     borrow only    + patch_null_render
GUEST CRASH               0                 0  (with trace)
GuideScene               32                38
composite draw         2100+                0
```

`guide_patch_null_render` **eliminates the composite draws**. The draw hook
reports itself installed in both cases ("draw hook installed on title
thread"), so the difference is either a null `render_obj` passed to
`SetGuideDrawHook` - which would make the `guide_draw_fn_ && guide_draw_this_`
test silently false - or a notification that stops firing.

This is the cleanest statement of the remaining problem so far:

* `[dc+134]=1`: draws happen (2100+), present is a no-op.
* `[dc+134]=0`: present would be real, but **no draws happen at all**.

Which suggests the composite-draw path being driven only runs in null-render
mode, and that clearing the flag is not "opening a gate in front of finished
geometry" - it selects a different, currently empty, code path.

The bootstrap site logged "installed" without its arguments, which cannot
distinguish those two cases. It now prints `fn` and `self`.

## Why patch_null_render shows 0 draws: the draw call HANGS

Not a suppressed code path - a hang. Chased by elimination, not guesswork:

* `draw hook args fn=913EAB28 self=401587F0` - both non-null, so the
  `guide_draw_fn_ && guide_draw_this_` test passes. The null-`render_obj`
  theory is dead.
* `VdSwap fetch_ptr` logs at line 22227, two lines *after* the hook is armed
  at 22225 - so a swap really does run with both globals set.
* There is no early `return` between the block at 2020 and the log at 2958;
  every `return` in that range is inside a lambda.
* The log at **2958 prints the draw's return value**, and the draw is executed
  at **2645**. A line that never appears therefore means the call never
  returned.
* `WrapRender #1` fires at log line 22431 - *inside* that call - and the log
  tail is the title thread spinning on `KeWaitForMultipleObjects` that will
  not resolve.

So `guide_patch_null_render` does not remove the draws. It hangs
`819F5D18`'s caller inside `vtable[20]`, and the title thread never swaps
again.

Combined with the earlier result, `vtable[20]` is broken in both directions:

* without the trace: **crashes** at `819DE94C` (race, fast path)
* with the trace:    **hangs** in the same call (breakpoint overhead changes
  which side of the race wins)

That is one defect with two faces, not two problems.

### Consequence: the flag pairing to aim for

* `[dc+134]=1` -> `XuiRenderBegin` skips `vtable[20]`; 2100+ draws happen;
  `XuiRenderPresent` returns S_OK without presenting.
* `[dc+134]=0` -> `vtable[20]` runs and crashes or hangs; nothing draws.

Neither state renders. What is wanted is the **combination**: Begin must see
non-zero (skip the broken setup, keep the draws) while Present sees zero (do a
real present). Both existing flags change the *field*, so they necessarily
change both call sites together.

The precise lever is therefore inside `XuiRenderPresent` (`dc->vtable[21]`,
runtime `818F9290`): patch **its** test of `[dc+0x134]`, leaving the field -
and hence `XuiRenderBegin` - alone. That is the next thing to try, and it is
a one-instruction image patch of the same kind `guide_patch_null_render`
already does at `818FDF14`.

### guide_patch_present_gate: split Begin from Present

`XuiRenderPresent` (runtime `818F9290`) reads the flag and branches:

```
818F92DC  lwz   r11,0x134(r31)   ; null-render flag
818F92E0  cmpwi cr6,r11,0
818F92E4  bne   cr6,+0x38        ; non-zero -> skip the real present
818F92E8  lwz   r3,0x1CC(r31)    ; else: wrapper
          lwz   r11,0x60(r11)    ; vtable[24]
          bctr                   ; the actual present
```

(Also confirms the corrected gate order above: `[dc+0x11C] != 0` returns
`0x8000FFFF` early, and our logs show `[11C]=00000000`, so that one is open.)

`guide_patch_present_gate` nops **only** `818F92E4`, leaving `[dc+0x134]`
untouched. That is the whole point: both existing flags clear the *field*,
which necessarily changes `XuiRenderBegin` as well, and Begin then runs the
`vtable[20]` path that crashes or hangs. Keeping the field set preserves the
2100+ draws while letting Present through.

Use it INSTEAD of `guide_patch_null_render` / `guide_clear_null_render`, never
alongside them - together they would reintroduce the hang this exists to
avoid.

Prediction to check against the result, recorded before the run so it cannot
be adjusted afterwards: draws should stay at 2100+, no crash, and the present
should reach `vtable[24]`. That is still not the same as pixels - the Guide
device's `RT0/RT1/depth` are all `00000000`, so what gets presented may be an
empty surface.

## The present gate is open: Present now reaches the real path

`guide_patch_present_gate` applied cleanly and did what it was designed to:

```
Guide: patched 818F92E4 409A0038 -> 60000000 (present gate removed;
       [dc+134] left intact for XuiRenderBegin)
```

The unwind changed completely, which is the proof it took effect:

```
before: 819DEB30 8191B024 818FDE60 818F8374   (vtable[20], during DC ctor)
after:  819F7FB4 819FEC68 8191B438 818F930C   (818F930C = inside
                                               XuiRenderPresent, past the gate)
```

So splitting Begin from Present was the right call: Begin still skips the
`vtable[20]` path that crashes/hangs, and Present now runs through to the real
presentation code.

### The stale lent buffer is now a real bug

New fault: `819F5EC4`, `fault_addr 0x98409960` = **`98409940 + 0x20`** - the
front buffer `GuideLendFB` lent (`r8` and `r14` both hold `98409940`).

`VdGlobalDevice` points at one device ~60ms into boot (front buffer
`98409940`) and at dash's `40952400` (`A240A380`) later. The one-shot lend
captured the early one and never revisited it. That was harmless while the
present was gated off and nothing followed the pointer - **a stale pointer
only becomes a bug once something dereferences it.** Opening the gate made it
one.

`GuideLendFB` now *syncs* rather than snapshots: every poll, if
`[gdev+0x3F74]` differs from the title's current `[tdev+0x3F74]`, it rewrites
it (logging the first 6 changes). By the time the present runs, the field
holds whatever dash currently owns.

### Syncing blindly wrote garbage - validate the value, not just the source

The sync reached dash's real buffer and then drifted off it:

```
lent 98409940 (was 00000000)   iteration 230
lent A240A380 (was 98409940)   iteration 239   <- correct
lent 01C001C0 (was A240A380)   iteration 485   <- not a pointer
lent 03C003C0 / 06000600 / 08C00900
```

`0x01C0`=448, `0x03C0`=960, `0x0600`=1536 - packed width/height pairs.
`VdGlobalDevice` does **not** always point at a device, so `[+0x3F74]` is not
always a front buffer, and copying it unconditionally propagates whatever
happens to sit at that offset.

Both fixes so far were about *where* the value came from (range-check `tdev`,
require a committed page). Neither checks **what was read**. A pointer-shaped
guard on the source says nothing about the payload. Real buffers seen are
`98409940` and `A240A380`; the filter is now `fb >= 0x80000000`.

Crash moved with each fix, which is the useful signal:

* stale `98409940` -> fault at `98409940+0x20` (`819F5EC4`)
* synced/garbage   -> fault at null`+0x18` (`819F5F60`), with `r8`/`r14`
                      holding `A240A380`

Same caller chain throughout (`819F7FB4 / 819FEC68 / 8191B438 / 818F930C`),
so the present path is being entered consistently and failing progressively
later.

## The present path now reaches the draw emitter - and faults on a null RT

Value validation (`fb >= 0x80000000`) did **not** change the crash: identical
PC `819F5F60`, identical `fault_addr 0x18`, identical `r8/r14 = A240A380`.
The garbage lends (`85008600`, `8B008BC0`, ... - still pair-shaped, so the
filter is too weak) are a separate defect that is **not** causing this fault:
at crash time the buffer in use is dash's correct `A240A380`.

Worth noting as a method point: two consecutive fixes were aimed at the lent
buffer, and the second one demonstrably fixed nothing. The register dump said
so immediately - `r14` already held the right value. Checking whether a fix
moved the crash is cheaper than assuming it did.

The faulting function is **`819F5D18` - the draw emitter itself** (len
0x2208, single caller `819F7F20`):

```
819FD148  bne   cr6,+0xc
819FD14C  lwz   r11,12976(r31)    ; [dev+0x32B0] = depth surface
819FD150  b     +0x10
819FD154  addi  r11,r17,3240      ; else indexed RT slot
819FD15C  lwzx  r11,r11,r31
819FD160  lhz   r11,0x18(r11)     ; <- faults, r11 = 0
```

So the emitter loads a render target / depth surface off the device and reads
a halfword at `+0x18` from it. `GuidePreDraw` has reported
`RT0=00000000 RT1=00000000 depth=00000000` in every single run. Previously
nothing got far enough to touch them; now the present path does.

This is the predicted next blocker, arriving exactly where predicted -
recorded last tick as "what gets presented may be an empty surface".

`guide_bind_title_rt` already exists for this and its description names the
right mechanism: the title's RT sits at `[VdGlobalDevice+0x3AC4]`, and
binding it "means the Guide draws into the title's back buffer, which is what
an overlay should do". Testing that now, alone rather than together with
`guide_bind_depth_copy`, so that if the fault moves it is clear which slot
mattered.

## RT bound -> emitter runs -> now it needs somewhere to write

`guide_bind_title_rt` worked:

```
Guide: RT bind sees dev 40870D00 RT0 00000000 (plausible=false) surf 40958CD0
GuidePreDraw: device 40870D00 RT0=40958CD0 RT1=00000000 depth=00000000
              [3F74]=A240A380
```

RT0 is bound to the title's surface and the front buffer is dash's real
`A240A380`. The fault moved off `819F5D18` entirely.

New fault `81A01638`, `fault_addr 0x00000004`, in function `81A015B8`
(len 0x90, **278 callers** - the PM4 word-emit helper):

```
81A08828  lwz  r11,0(r30)    ; r11 = write cursor
81A0882C  addi r11,r11,4     ; reserve a word
81A08834  stw  r11,0(r30)
81A08838  stw  r10,0(r11)    ; <- faults; r11 = 4, so the cursor was 0
```

So the emitter is now genuinely *emitting* - it just has no buffer. Checking
`kernel_flags.cc` first (the rule this file records three times) found the
flag written for precisely this, and its description predicts the observed
fault to the byte:

> "on a mode-2 device the cursor is 0 so the first word stores to guest
> address 4 ... this binds the one field standing between the emitter and
> somewhere real to write"

Cursor field is `[dev+0x2B4C]`. Testing `guide_bind_cmdbuf_kb=64` with the
now-working stack: borrow + present gate + title RT.

Note this is a different flag from `guide_syscmdbuf_buffer_kb`, which was
tried much earlier and produced "identical stall, guest wrote 0 words" - that
was before the emitter was ever reached, so it was measuring a path that was
not executing.

### guide_bind_cmdbuf_kb applies but the cursor is cleared before emission

The flag did take effect - the `= 0` line in the harness output is the config
*file* dump, not the runtime value, and the third argument below is
`csize/4` in words, i.e. 64KB:

```
Guide: cmdbuf init 81A01358(dev 40870D00, 301D5000, 16384) -> 301D4FFC;
       base=301D5000 cursor=301D4FFC limit=301E4FFC
```

Yet the crash is byte-identical (`81A01638`, `fault_addr 0x4`), so
`[dev+0x2B4C]` is 0 again by the time packets are emitted. `kernel_flags.cc`
explains it exactly:

> `guide_patch_cmdbuf_reset`: "Nop the store at 81A01464, which zeroes the
> command-buffer cursor [dev+0x2B4C]. xam calls 81A013B8 (its frame end/flush)
> from 81A06080 during the Guide's draw, and that clears the cursor set by the
> begin - which is why a command buffer prepared before the draw is always
> gone by the time packets are emitted. Needs guide_second_context_kb."

So `guide_bind_cmdbuf_kb` is the wrong shape of fix: preparing a buffer
*before* the draw cannot survive a flush that happens *during* it.
`guide_second_context_kb` is the intended mechanism - it resets the cursor
each frame, calls xam's own begin (`81A01358`) through the lifecycle that owns
it, runs the draw, then submits what was emitted, and its description says to
pair it with `guide_bind_title_rt`.

Now running the documented combination: second context 64KB +
patch_cmdbuf_reset + bind_title_rt + present gate + borrow. Also useful as
context for `GuideBootstrap`'s dump - the title device has a live cursor
(`+2B4C=FF9C9000`) while the xam device's is `00000000`.

## MILESTONE: first crash-free run with every prerequisite satisfied

Configuration: `lle_xam_skin_init` + `guide_reuse_xui_ctx` +
`guide_borrow_front_buffer` + `guide_patch_present_gate` +
`guide_bind_title_rt` + `guide_second_context_kb=64` +
`guide_patch_cmdbuf_reset`.

```
GuidePreDraw: device 40870D00 RT0=40958CD0 RT1=00000000 depth=00000000
              [3F74]=A240A380
Guide composite draw #600 -> 00000000; [11C]=00000000 [134]=00000001
              [1CC]=40877E00 wrap[0C]=40870D00 realdev[32A0]=40958CD0
GUEST CRASH   0
```

Everything that has blocked this path in turn is now simultaneously true:
draws run (600+), the title's RT is bound, dash's real front buffer is in
place, `[dc+134]` stays set so `XuiRenderBegin` skips the crashing/hanging
`vtable[20]`, the present gate is patched open, and the command buffer
survives the mid-draw flush. Nothing crashes.

### But the Guide emits nothing

```
GuideCtx2 #600: submitted 1 words from 40875804, GPU draws +0;
                buf: 0000200E 00000000 00000000 ...
DRAW_INDX  0
```

**One word per frame.** `0000200E` is not a type-3 PM4 header (those are
`0xC0......`), GPU draws increase by 0, and no DRAW_INDX packet is ever
built. The emitter is entered, has a valid cursor and a real buffer, and
writes one word.

So the whole crash chain was necessary but not sufficient: the plumbing is
now complete and carries no geometry. This is emphatically **not** the stop
condition - nothing is on screen.

Scene state in the same run, for the next investigation:

```
GuideScene                38 lines
000100B1 GetVisual -> 00000000: 000100E5   (resolves)
000100E2 GetVisual -> 00000000: 00010129   (resolves)
0001012D GetVisual -> 8030000A: 00000000   (fails)
```

Visuals do resolve for some nodes, so the earlier `80300017`-for-everything
problem is fixed by the skin init - but at least one node still returns
`8030000A` with a null visual. Whether a scene with partly-null visuals emits
no geometry at all is the next question, and it is a *content* question
rather than a device/plumbing one - a different class of problem from
everything solved so far.

### The emitter runs; the content is empty

Demand-JIT confirms every stage of the draw path actually executes in the
crash-free configuration:

```
DemandFunction: enter 819F5D18   (draw emitter)        1
DemandFunction: enter 819F7F20   (its caller)          1
DemandFunction: enter 81A015B8   (PM4 word emit)       1
DemandFunction: enter 819F31A8   (SetRenderTarget)     1
```

So "1 word per frame" is not a path that fails to run - it is a path that
runs and has nothing to say. `0000200E` parses as a type-0 PM4 header
(bits 30-31 = 00) for register index `0x200E` with count 0, and no data word
follows it.

That makes this a **content** problem, and `guide_inject_label_text` exists
as the direct test for it:

> "if draws appear, the render path works and the content was empty; if not,
> the emitter is failing for another reason entirely"

Worth noting the premise of that flag has partly changed since it was
written: it says `XuiControlGetVisual` returns `80300017` with a null visual,
but with `lle_xam_skin_init` the label `000100E2` now resolves to a real
visual `00010129`. Only `0001012D` still fails, with `8030000A`. So the test
is now "the label has a visual but no text" rather than "no visual at all",
which is a strictly better starting point than when the flag was authored.

### The label test failed to inject, and the scene is empty by design

`guide_inject_label_text` did not prove anything about the emitter, because
the injection itself failed on every node tried:

```
GuideScene: 0001012D SetText -> 80300016
GuideScene: 000100B1 SetText -> 80300016
GuideScene: 000100E2 SetText -> 80300016
GuideCtx2 #600: submitted 1 words ... GPU draws +0
```

`XuiTextElementSetText` refuses with `80300016` even on `labelHeading`
(`000100E2`), which *does* have a real visual now. So the diagnostic is
inconclusive rather than negative - the content stayed empty for a different
reason than the flag assumed, and its "if not, the emitter is failing for
another reason entirely" branch does **not** apply.

Two flag descriptions then explain the empty payload without any guessing:

* `guide_register_all_classes`: xam has **39** per-class registrars but the
  registry ends up with only **16** entries. "GuideMain.xur,
  GuideMainServer.xur and MiniMediaPlayer.xur all fail with E_FAIL while 20
  leaf scenes load - the shape of a scene asking for a control class that is
  not registered." A control whose class is unregistered plausibly also
  rejects `SetText` with `80300016`.
* `guide_scene_override`: "hud always creates InfoUpsellLive.xur - the 'no
  Xbox Live' upsell page - which is a **nearly empty page even when it
  works**."

That second point reframes the milestone. The crash-free pipeline has been
faithfully rendering a scene that is almost empty by design - so "1 word per
frame" may be the correct output for the content being drawn, not evidence of
a broken emitter.

Now testing both together: all classes registered + `GuideMain.xur` loaded
directly, on top of the working stack.

## guide_register_all_classes fixes the scene load

```
GuideBootstrap: called 39 class registrars, returns: 80070057 80300005
                80300005 80300005 ... (80300005 = already registered)
GuideScene: override XuiSceneCreate("GuideMain.xur") -> 00000000,
            scene 00010135
```

**`GuideMain.xur` now loads with S_OK.** It previously failed with `E_FAIL`,
and the flag's diagnosis - "a scene asking for a control class that is not
registered" - is confirmed: register all 39 and the load succeeds. This also
retires the 16-of-39 registry gap as an open issue.

### But it still emits 1 word, for a reason one level deeper

```
GuideScene: override last child 0001039A
GuideScene: override child 0001039A "" visual -> 8030000A: 00000000
GuideCtx2 #600: submitted 1 words ... GPU draws +0
```

Two separate things are now clear, and they should not be conflated:

1. `guide_scene_override` **creates and reports** a scene; it does not attach
   or render it. The draw path still walks hud's own `InfoUpsellLive`, so
   loading `GuideMain.xur` cannot by itself change what is emitted. Nothing
   in the flag set (`guide_step_scene`, `guide_scene_off_thread`,
   `guide_create_scene`, `guide_system_root`) attaches a scene either.
2. Even inside `GuideMain.xur`, the child's visual fails: `8030000A` with an
   empty name - the same failure as `0001012D` in hud's own scene. So visual
   resolution is broken for a class of elements *regardless* of which scene
   is loaded.

(2) is the more fundamental of the two: attaching a scene whose elements have
no visuals would still emit nothing. `lle_xam_skin_init` fixed visuals for
*some* nodes (`000100B1`, `000100E2` resolve) but not these.

So the next question is what distinguishes a node that resolves from one that
returns `8030000A`, and that is a question about the visual registry rather
than about devices, buffers or gates - none of which are in the way any more.

### Resource requests: still none from the Guide

```
xam://          0 occurrences
.png            4  - all XamBuildResourceLocator, all lr=92181B34 (DASH)
strings.xus     5
```

The four `.png` locators are the **title's**, not the Guide's:
`section://30013000,shrdres#loadingRing.png`, `B-Button_32.png`, etc., all
requested from `92181B34` in dash's code range. So the earlier measured
absence still holds even with `lle_xam_skin_init` on and 281 visuals in the
registry: **the Guide never requests any imagery.**

### Unifying hypothesis for the two open items

A scene that is created but never *shown* would explain both symptoms at
once:

* `XuiSceneCreate("GuideMain.xur")` returns S_OK and the object exists, but
  nothing attaches it - `guide_scene_override` is a reporter, and no flag in
  the set attaches a scene.
* Element visuals return `8030000A` because per-class visual instantiation
  happens on show/attach, not on create - which would also explain why no
  `xam://` image is ever requested (nothing has asked to be rasterised yet)
  and why `XuiTextElementSetText` returns `80300016` on elements whose
  visuals do not exist yet.

This is a hypothesis, not a finding. It is attractive because it explains
four separate observations with one cause, which is exactly the kind of
reasoning that has been wrong twice in this file already (the "null
[wrapper+0x0C]" inference and the front-buffer ordering theory). It needs the
same treatment those got: find hud's scene show/navigate entry point, hook it,
and see whether it is ever called - rather than building attachment machinery
on the strength of the story.

Note `lle_xam_skin_init` *did* populate 281 visuals and `000100B1` /
`000100E2` resolve, so visual creation is not globally broken - which is
evidence against the strongest form of this hypothesis and should not be
explained away.

### The "empty content" story does not survive contact with hud's own scene

`GuideMain.xur`'s scene `00010135` has exactly one child, `0001039A`:

```
override child 0001039A "" visual -> 8030000A: 00000000
0001039A GetId -> 00000000: 00000000 ""      (empty name)
0001039A GetPosition -> all zeros
depth 2 child of 0001039A -> 00000000        (no children)
0001039A object 4015A190 head 00:816847DC 08:0001039A 0C:00010135 10:00000064
```

So the override-created scene really is unpopulated, consistent with "created
but never shown".

**But that cannot be the explanation for the single emitted word.** hud's own
`InfoUpsellLive` scene is populated and its elements DO have visuals:

```
000100B1 "scnInfoUpsellLive"  -> 000100E5
000100B7 "btnJoinLive"        -> 000100EB
000100BD "btnB"               -> 000100FA
000100E2 "labelHeading"       -> 00010129
```

That is a scene with named controls and real visuals, and drawing it still
produces `1 word, GPU draws +0`. So "the content is empty" is refuted for the
scene actually being drawn, and the unifying hypothesis from the previous
tick is weakened exactly where it was most attractive.

Rather than reason further about it, using the tool built for this question.
`guide_coverage_fn` exists precisely to answer it:

> "Reports the furthest instruction reached, which is how to find where
> 819F5D18 stops instead of building a draw packet - its draw construction
> sites are reachable but sit behind ~390 branch points, too many to read."

Running with `guide_coverage_fn=0x819F5D18`, `trace_function_coverage=true`
and a `trace_function_data_path`. This should say where in the emitter
execution stops, instead of another inference about why.

### guide_coverage_fn is unusable here; the draw emits ZERO words

Coverage instrumentation crashed the run at 14,634 lines (a healthy run is
~2M) in the skin-init path:

```
GUEST CRASH at 81812FB0, lr=81907408, unwind ... 8179576C  (skin loader)
```

`trace_function_coverage` makes the JIT emit a counter per guest instruction,
which changes codegen enough to break `lle_xam_skin_init`. So the tool built
for "where does 819F5D18 stop" cannot be used in the one configuration where
the emitter actually runs. Worth recording so it is not retried blind.

A scan of the emitter's whole range for the emitted word is also negative:

```
instructions with immediate 0x200E in 819F5D18..+0x2208:  0
```

`0x0000200E` is therefore not built by the draw. Since `guide_second_context_kb`
"resets the cursor, calls xam's own begin (81A01358), runs the Guide's draw,
then submits whatever was emitted", that single word is almost certainly
**begin's**, which means the Guide's draw emits **zero** words - not one.

That is a sharper statement than "1 word per frame" and it rules out the
reading that the emitter builds a malformed packet.

Next measurement rather than inference: `guide_trace_emitter` hooks
`819F5D18` and logs `r3..r7`. If a count or geometry-list argument arrives as
0, there is nothing for the emitter to build and the ~390 branch points never
need reading.

## The emitter is called ONCE, with no geometry

```
Emitter #1: r3=40870D00 r4=00000000 r5=00000000 r6=00000000 r7=00000000
            lr=819F7FB4
```

Two facts, both new:

1. **Only one call in the entire run**, against 600+ composite draws (the
   hook allows 8 and only #1 appeared). So the draw path reaches the emitter
   once and never again - "the emitter runs" from the DemandFunction check
   meant *once*, which is much weaker than it looked.
2. **No geometry.** Every argument but the device is zero.

Reading the call site rather than guessing which zeros matter:

```
819FF190  addi r7,r0,0     ; r7 hardcoded 0
819FF198  addi r6,r0,0     ; r6 hardcoded 0
819FF1A4  or   r5,r28,r28
819FF1A8  or   r4,r29,r29
819FF1AC  or   r3,r30,r30  ; device
819FF1B0  bl   819F5D18
```

So `r6`/`r7` being zero is normal - they are constants, not missing data.
Only `r4`/`r5` carry information, and in `819F7F20`:

```
819FF138  or r29,r4,r4     ; r29 = arg2
819FF13C  or r28,r5,r5     ; r28 = arg3
```

they are simply that function's own arguments, arriving as 0. `819F7F20` has
**8 callers**, so the next step is to determine which one is on the Guide's
path (`lr` at the emitter was `819F7FB4`, i.e. the call inside `819F7F20`
itself, so that does not disambiguate the caller of `819F7F20`).

The productive question is no longer "why does the emitter build nothing" -
it builds nothing because it is handed nothing, exactly once. It is "what
should be passing a geometry list, and why is it passing 0".

### Candidate caller: 8191B250

`819F7F20`'s 8 call sites include `8191B2BC` in `8191B250`, which is in the
same wrapper/render region as `8191AFD0` (the `vtable[20]` path). What it
passes:

```
819224A0  or r7,r27,r27
819224A8  or r6,r28,r28
819224B0  or r5,r29,r29
819224B4  or r4,r30,r30
819224BC  bl 819F7F20
```

So this site forwards `r30` -> arg2 and `r29` -> arg3, which become the
emitter's `r4`/`r5`. If those are 0 at this level, the emptiness originates
at or above `8191B250` rather than anywhere in the emitter.

`guide_trace_emitter` now also hooks `819F7F20` itself and logs `lr`, which
identifies which of the 8 callers is actually on the Guide's path - the
emitter's own `lr` (`819F7FB4`) could not, since it points inside
`819F7F20`.

## The only emitter call comes from Present, not from the Guide's draw

`guide_trace_emitter` on `819F7F20` identified the caller, and it is **not**
the `8191B2BC` candidate I picked by region - a reminder that a
plausible-looking call site is not evidence:

```
DrawFn #1: r3=40870D00 r4=00000000 r5=00000000 r6=A240A380 lr=819FEC68
```

`r6` is the front buffer, and `pe360` resolves the chain uniquely (each of
these functions has exactly one caller):

```
818F930C   XuiRenderPresent
  8191B438   in 8191B418
    819FEC68   in 819FEB78   (len 0x158, 1 caller)
      819F7FB4   in 819F7F20
        819F5D18   the emitter
```

So the single emitter call in the whole run is **Present compositing the
front buffer**. hud's scene draw never reaches the emitter at all. Every
earlier statement of the form "the emitter runs" was about this one
present-path call.

### This forces an uncomfortable synthesis

The working configuration deliberately keeps `[dc+0x134]` set so that
`XuiRenderBegin` skips `dc->vtable[20]`. That is what avoids the crash/hang
and yields 600+ draws. But `guide_clear_null_render`'s own description says
vtable[20] is "where any render-target setup would happen".

So the two states are:

* `[134]=1` - Begin skips vtable[20]; present works; **the scene draw emits
  nothing**, because the render setup it needs never ran.
* `[134]=0` - vtable[20] runs and either crashes at `819DE94C` (fast path) or
  hangs the draw call (with breakpoints installed).

The present-gate split was still the right call - it is what got Present
working independently, and it produced the crash-free pipeline. But it cannot
by itself produce pixels, because skipping vtable[20] is precisely what stops
the Guide's geometry being set up.

**The real remaining blocker is vtable[20]'s hang**, not the present gate,
not the device, not the buffers. That is where the next work goes.

### Localising the vtable[20] hang

The blocker is now `vtable[20]`, so the question is where inside it execution
stops. The crash's own unwind gives the frames below `8191AFD0`:

```
819DEB30 (in 819DEA70)   8191B024 (in 8191AFD0)   818FDE60   818F8374
```

`guide_trace_hang` traces `819DEA70` and `819DE8F8`. Whichever logs **last**
bounds the hang to a single function, which is enough to start reading it.

One methodological note: breakpoint overhead is what turns this race from a
crash into a hang, so running the trace is not distorting the phenomenon -
the traced regime *is* the hang regime. That is the opposite of
`guide_coverage_fn`, whose instrumentation destroyed the configuration it was
meant to measure.

Running with `guide_patch_null_render` (so `[dc+134]` is genuinely 0 and
vtable[20] executes) on top of the otherwise-working stack. Note this
configuration is expected to produce 0 composite draws - that is the known
cost of letting vtable[20] run, and is not a regression.

### The hang is not where the unwind says it should be

Both traces installed (`Hang trace installed at 2 sites`) and **neither
fired**, while `WrapRender #1` at `8191AFD0` did. There was also no composite
draw at all, so the hang precedes the draw entirely rather than occurring
inside it.

That is hard to reconcile with the code. `8191AFD0` runs straight from entry
to its only call:

```
8191B004  lwz r3,12(r31)     ; [wrapper+0x0C]
...       register moves only
8191B020  bl  819DEA70
```

There is nothing between entry and `8191B020` that can block - no loop, no
call except the prologue register-save helper. Yet `819DEA70` never logs,
even though the *crash* run's unwind contains `819DEB30`, a return address
inside it.

Two candidate explanations, and they need separating before any more of this
path is read:

1. Execution really does stop inside `8191AFD0` - which would mean the stall
   is in the prologue helper `81814960` or in the JIT, not in xam's logic.
2. The breakpoint mechanism itself is implicated: hooking these addresses
   perturbs or halts the thread, in which case the "hang" is partly an
   artifact and the earlier "breakpoints turn the crash into a hang"
   observation has a simpler explanation than a race.

(2) would be a significant correction - several ticks have treated the
crash/hang duality as evidence of a timing race in xam. Bracketing the inside
of the function (`8191B004`, `8191B020`) distinguishes them: if the `lwz`
fires and the `bl` does not, the hang sits in code that provably cannot hang,
which indicts the instrument rather than the guest.

## Tooling correction: what cpu::Breakpoint can and cannot measure

Four sites installed, **none fired** - including `8191B004`, only 0x34 bytes
past the entry at `8191AFD0` where `WrapRender` fires reliably every run.
That is not a statement about the guest.

Reading the implementation:

* `Breakpoint::Install()` -> `X64Backend::InstallBreakpoint(bp)` patches
  `0x0F0B` at each host address found for the guest address.
* The per-function overload uses
  `GuestFunction::MapGuestAddressToMachineCode(guest_address)`, and when that
  returns 0 it does `assert_always()` - which in a **Release** build is a
  no-op, so the breakpoint silently never installs.
* `Processor::OnFunctionDefined` re-installs breakpoints for functions
  compiled *after* the breakpoint was added.

So a mid-function guest address only works if the JIT emitted a distinct host
address for that exact instruction. `8191B004` evidently has no mapping, and
Release swallows the failure. **Mid-function breakpoints are unreliable and
their silence means nothing.**

`819DEA70` *is* a genuine function entry (`in function 819DEA70`, len 0x104,
9 callers), so its silence is more meaningful - but it is not conclusive
either, given the same silent-install failure mode exists.

### What this invalidates

The previous tick's reasoning - "neither hang site fired, and 8191AFD0 is
straight-line, therefore the hang is inside 8191AFD0" - does not hold. The
premise was an instrument artifact.

More importantly, it puts a caveat on several ticks of reasoning that treated
the crash-vs-hang duality as evidence of a **timing race in xam**. That
conclusion came from observing that installing breakpoints changed the
outcome. Breakpoints patch `0x0F0B` into generated code and re-install on
recompilation, so they change codegen as well as timing - "a race whose
outcome breakpoint overhead decides" is one explanation, but so is "the
patched code behaves differently". Neither has been separated from the other,
and the race story should not be treated as established.

Solid ground, unaffected by any of this: the emitter/DrawFn traces at
`819F5D18` and `819F7F20` are **function-entry** hooks that did fire, and
their argument dumps (`r4=r5=0`, one call per run, caller chain resolving to
`XuiRenderPresent`) stand.

## MAJOR CORRECTION: the "vtable[20] hang" was my own breakpoint

Demand-JIT (breakpoint-free, so unaffected by the instrument) shows all three
functions in the path DO execute, and the ordering shows when:

```
21454  DemandFunction: enter 8191AFD0     thread 01000028
21461  DemandFunction: enter 819DEA70     thread 01000028
21463  DemandFunction: enter 819DE8F8     thread 01000028
21465  Hang trace installed at 4 sites
22509  WrapRender #1 ... lr=818FDE60      thread F8000144  (title thread)
```

The whole chain runs cleanly on thread `01000028` **before** the traces exist.
Afterwards, on the **title** thread, `8191AFD0` is entered, `WrapRender`
fires, and nothing past it ever executes.

The correlation across every run is exact:

| runs | guide_trace_devsetup | composite draws |
|------|----------------------|-----------------|
| b3aucwe9o, bwrhbtdln, b0kzn8zxw, bw5e1pe5i | ON | **0** |
| bf1j59rg9, bt6cxdttc | OFF | **600+** |

So the breakpoint at `8191AFD0` wedges the title thread when it is hit. The
"hang" is an artifact of the instrument, and last tick's suspicion - that
breakpoints might be causing rather than revealing it - is confirmed.

### What this invalidates, precisely

* "vtable[20] hangs" - **withdrawn**. There is no evidence xam hangs there.
* "The crash and the hang are one defect with two faces, and breakpoint
  overhead decides which side of the race wins" - **withdrawn**. There is no
  race. There is a crash, and separately there is my breakpoint stopping the
  thread.
* "guide_trace_devsetup makes the crash disappear, so it is timing-dependent"
  - **withdrawn**: the crash disappears because execution never reaches the
  crashing code, having stopped at the breakpoint.

### What survives

* The genuine crash at `819DE94C` with `[dc+0x134]=0` and **no** traces.
  That is a real guest fault and is still the blocker.
* The crash-free 600+ draw pipeline with `[134]=1` (no traces involved).
* The emitter/DrawFn argument findings - those hooks fired and returned data,
  and the run still produced its normal draw counts.

### Rule going forward

**Never leave a `cpu::Breakpoint` installed on a function that runs inside the
title's frame.** Hooks are for one-shot identification on paths that are
already stalled, not for observing a working pipeline. Any run that has both
a trace flag and a draw-count expectation is measuring two different systems.

### Chasing the real crash without breakpoints

With the hang retired as an artifact, the blocker is the genuine fault at
`819DE94C` (`[dc+0x134]=0`, no traces). Tracing r3 back, using only
disassembly:

```
8191AFD0  lwz r3,12(r31)   ; [wrapper+0x0C] -> arg1 of 819DEA70
819DEA70  or  r29,r3,r3    ; r29 is callee-saved, set once at entry
          or  r3,r29,r29
          bl  819DE8F8     ; [r3+0x24] faults with r3=0
```

Nothing between reassigns `r29` (the intervening `bl 81753710` cannot - r29
is callee-saved), so `[wrapper+0x0C]` really is **0** at that call.

`WrapRender` reported it non-null, but that observation is from the
invocation where the thread then wedged, so it says nothing about later
constructions.

Since a `cpu::Breakpoint` on the title thread wedges it, this is watched from
the `GuideLendFB` poller instead - host-side, no patched code, no thread
suspension. It warns whenever `[wrapper+0x0C]` reads 0, and
`guide_repair_wrapper_device` (default off) writes the last known device back.

The warning is deliberately independent of the repair flag: **first establish
that the slot really does go null**, then try repairing it. Inverting that
order would make a working run indistinguishable from a wrong diagnosis.

## The 819DE94C crash is gone - and vtable[20] running changes nothing

Best configuration so far, with **no breakpoints anywhere**:

```
lle_xam_skin_init + guide_reuse_xui_ctx + guide_borrow_front_buffer
+ guide_bind_title_rt + guide_second_context_kb=64
+ guide_patch_cmdbuf_reset + guide_patch_null_render
```

```
Guide composite draw #600 -> 00000000; [11C]=00000000 [134]=00000000
    [1CC]=40877E00 wrap[0C]=40870D00 realdev[32A0]=40958CD0
GuideCtx2 #600: submitted 1 words ... GPU draws +0
GUEST CRASH   none
"is NULL at iteration"   none
```

Two results:

1. **The `819DE94C` crash no longer reproduces.** Every run that hit it
   predates `guide_bind_title_rt`, `guide_second_context_kb` and
   `guide_patch_cmdbuf_reset`; with those present, `[dc+0x134]=0` is
   survivable. So the crash was a *consequence* of the missing render target
   or command buffer, not an independent defect - and the whole "the crash is
   the blocker" framing of the last few ticks was chasing a symptom that the
   plumbing work had already removed.
2. **`[dc+0x134]` is now genuinely 0**, so `XuiRenderBegin` does **not** skip
   `dc->vtable[20]` - and the Guide still emits one word with 0 GPU draws.

### That refutes the synthesis from two ticks ago

The claim was: "`[134]=1` means Begin skips vtable[20], which is where
render-target setup happens, so skipping it is exactly why the scene emits
nothing." vtable[20] now runs, with a real RT bound and a real command
buffer, and the output is byte-identical to when it was skipped.

So vtable[20] is not the reason there is no geometry. Nor is the present
gate, the device, the front buffer, the render target, or the command buffer
- each has now been made correct and none changed the payload.

Also worth noting: the wrapper-slot warning never fired, so the
`[wrapper+0x0C]==0` inference drawn from the crash registers was never
confirmed, and the crash it explained no longer exists. `guide_repair_wrapper_device`
stays off and unused.

**Everything downstream of the scene is now demonstrably working and empty.**
The remaining question is entirely upstream: why hud's draw walks a populated
scene (`btnJoinLive`, `btnB`, `labelHeading`, all with visuals) and produces
no geometry.

### The null-render flag lives in the CONTEXT and is still set

From the bootstrap log in the best-so-far run:

```
GuideBootstrap: null-render flag [ctx+1C] = 00000001 (hw info word = 00000220)
```

`guide_patch_null_render` nops the *copy* at `818FDF14` so `[dc+0x134]` stays
0 - and that is all it does. **`[ctx+0x1C]` itself is still 1.** Any other
consumer that reads the context's flag directly still sees null-render mode
and skips its work, which fits the current state exactly: every downstream
stage is correct and the payload is empty.

That reframes what `guide_patch_null_render` achieves. It makes the *device
context* look non-null while the *XUI context* remains in null-render mode -
a half-measure, and one that explains why running vtable[20] changed nothing.

`guide_clear_null_render` clears the source, `[ctx+0x1C]`. It has been tried
twice before and crashed both times, but both attempts predate
`guide_bind_title_rt`, `guide_second_context_kb` and
`guide_patch_cmdbuf_reset` - and the `819DE94C` crash they were hitting is now
gone. Its "never worked because the per-frame context is built later" caveat
also does not apply under `guide_reuse_xui_ctx`, where the per-frame context
IS the bootstrap context (already noted above).

So the same flag is worth retrying in the current stack, in place of
`guide_patch_null_render` rather than alongside it.

Also visible: the title itself is rendering healthily throughout
(`GuideFrame 1500: gpu_draws +14181, total 54679`), so the GPU path is alive
and it is specifically the Guide contributing nothing.

## THE RESOURCE PROVIDER IS A STUB: two vtable slots return E_NOTIMPL

First: clearing `[ctx+0x1C]` at source works and changes nothing.

```
XUI ctx 40877DC0 [1C] 00000001 -> 00000000 (null-render flag cleared at source)
Guide composite draw #600 ... [134]=00000000
GuideCtx2 #600: submitted 1 words ... GPU draws +0
```

So null-render is now off at **both** levels (context and device context) and
the payload is unchanged. That refutes the previous tick's lead as well.

Second: hud drives the pipeline correctly. All four import thunks execute:

```
913FE994 XuiSceneNavigateFirst   entered
913FE874 XuiRenderBegin          entered
913FE854 XuiRenderEnd            entered
913FE6D4 XuiSceneCreate          entered
```

So the scene **is** navigated and the render bracket **does** run. The
"created but never shown" hypothesis is dead - it was wrong in both of its
forms.

### The actual cause

The bootstrap log records the provider's vtable, and two slots share one
address - the signature of a stub:

```
GuideBootstrap: provider 81D22A54 vtable 81608258
                [0]=817924E0 [1]=8178F600 [2]=8178F600
```

`8178F600` disassembles to three instructions:

```
81796800  lis r3,0x8000
81796804  ori r3,r3,0x4001     ; 0x80004001 = E_NOTIMPL
81796808  blr
```

**The XUI resource provider cannot load resources.** That single fact
explains the long-standing measured absence - "the resource provider is only
ever asked to open one thing, `strings.xus`; no `xam://` resource is ever
requested" - and it explains why a fully-correct render pipeline draws
nothing: the elements have visuals, but the visuals have no imagery, because
the provider returns E_NOTIMPL for whatever slots [1] and [2] are.

This also retro-explains several dead ends:
* `XuiTextElementSetText -> 80300016` (no font/resource to lay text out with)
* `GetVisual -> 8030000A` on some nodes
* 281 visuals in the registry yet nothing rasterisable

### Next

Identify what slots [1] and [2] are (`817924E0` in slot [0] is presumably the
real "open"), find the non-stub provider xam uses for its own UI, and register
that instead. This is the first explanation in many ticks that accounts for
the empty payload without being contradicted by an existing measurement.

## CORRECTION: that provider is the memory:// handler, not "the" provider

The previous section's headline - "THE RESOURCE PROVIDER IS A STUB" - is
**wrong** and is retracted.

Dumping the whole vtable rather than the three slots the bootstrap logged:

```
81608258  [0] 817924E0   [1] E_NOTIMPL  [2] E_NOTIMPL  [3] 8178F588
          [4] E_NOTIMPL  [5] E_NOTIMPL  [6] E_NOTIMPL
81608274  "memory://%.*ws"      <- UTF-16, immediately after the 7 slots
```

`81D22A54` is the **memory://** provider. Two implemented methods and five
`E_NOTIMPL` is a normal shape for a minimal scheme handler, not evidence of
breakage. Reading three slots and stopping produced a conclusion that one
more command overturned.

Direct evidence it is not the blocker: `section://301BA000,hud#strings.xus`
**loads successfully** (`-> 00000000, table 407D2F90`), so resource loading
through the section provider works.

What survives from that tick is only the negative measurement, which was
already known: no `xam://` resource is ever requested. That still needs
explaining - but the explanation is not "the provider is a stub".

### Standing, verified state

Everything below has been confirmed by measurement in the current stack and
none of it is the blocker:

* device, wrapper, front buffer, render target, command buffer
* present gate (`818F92E4` nopped) - Present reaches `vtable[24]`
* null-render flag cleared at **both** `[ctx+0x1C]` and `[dc+0x134]`
* `vtable[20]` runs; no crash; 600+ composite draws
* hud calls `XuiSceneCreate`, `XuiSceneNavigateFirst`, `XuiRenderBegin`,
  `XuiRenderEnd` - the scene is created, navigated and bracketed
* the scene is populated and its controls have visuals
* the title renders normally throughout (54k GPU draws)

And the Guide emits one word - xam's own begin - with 0 GPU draws.

### Scheme strings present in xam

```
memory://    81608274   (vtable 81608258 immediately before it)
section://   8160815C
xam://       81601668
file://      815FC6E0
hud://       ABSENT
game://      ABSENT
```

So `xam://` *is* a scheme xam knows about. But the memory:// layout - vtable
then string - does **not** repeat for it: `8160164C` decodes as the text
"smartglass", so `xam://` sits in an unrelated data region with no vtable
in front of it.

`img.xrefs(0x81601668)` returns **0** references from xam's `.text`.

Caveat, stated because it matters: `xrefs()` scans `.text` for `lis`/`addi`
pairs only. A pointer to the string held in an `.rdata` table would not be
found by it, so "0 xrefs" means "xam never builds this address inline", not
"nothing uses it". Do not upgrade this to "xam has no xam:// handler" without
a data-section scan.

What this does suggest: `xam://` URLs in hud's scenes are resolved by
something other than an inline xam reference - most likely
`XamBuildResourceLocator`, which the title itself uses (observed producing
`section://30013000,shrdres#B-Button_32.png` from dash). If the Guide's
scenes never call it, their imagery is never located, which matches "no
`xam://` resource is ever requested" without requiring any component to be
broken.

### Closing the xrefs caveat: the pointer exists, and it is an icon table

The caveat from the previous tick was correct - "0 xrefs from `.text`" was not
conclusive. A scan of every non-`.text` section finds exactly one pointer to
the `xam://` string:

```
.rdata 81601E2C -> 81601668
```

and its neighbourhood is a table of `{id, 0, id2, string_ptr}` records:

```
81601E1C  816016A4  "xam://ico_18x_kinect..."
81601E2C  81601668  "xam://ico_32x_MaxBin..."
81601E3C  816017AC  "xam://download_compl..."
```

So the `xam://` strings in xam are **its own icon resource names**, held in an
ID-to-URL table. This is not a scheme-handler registration and says nothing
about how hud's scenes resolve their imagery. The lead is closed as a
dead end rather than left dangling.

It is worth noting what the caveat bought: without it, "0 xrefs" would have
been recorded as "xam has no `xam://` handler", which is both false and the
kind of claim that would have shaped several later ticks.

(Also: the dump script must encode UTF-16 output carefully - printing a
decoded string straight to a cp1252 console raises `UnicodeEncodeError` and
truncates the run. Use `errors='replace'` on the *output* as well as the
decode.)

### Element rendering DOES run - and still emits nothing

Breakpoint-free, from demand-JIT after the press (line 21465):

* **48** distinct xam render-area functions execute after the Guide press.
* Among them a contiguous family that looks like the element/visual render
  methods: `819E3780 819E37D8 819E3EF8 819E3F50 819E3FB0 819E4010 819E4180
  819E4420 819E4988`, plus `819DCBF0 819DCC48 819DCCA0 819DD1E8 819DD460
  819DDBC0`.
* Also `819F5620 819F57B0 819F5D18 819F7F20 81A013B8 81A01648 81A01808`.

So the render bracket is not merely entered - a substantial amount of XUI
render code runs inside it. Element rendering executes and produces no PM4.

The resource side is the sharp part:

```
XamBuildResourceLocator #1..#4   all lr=92181B34 (DASH), all shrdres#*.png
"texture|.png|bitmap" after press: 0 occurrences
```

hud imports `XamBuildResourceLocator` too (thunk `913FE8C4`), and the Guide
**never calls it**. Only `strings.xus` is ever opened for the Guide.

So the picture is now specific: xam's element render methods run over a
populated scene whose controls have visuals, and emit nothing, while not a
single image is ever located or opened. A visual with no imagery has nothing
to rasterise, which is consistent with every measurement taken so far and
requires no component to be broken.

Next: find where the XUR loader turns an element's image reference into a
locator call, and why that never happens for hud's scenes - hud has the
import, so the call site exists.

## Why hud never loads any imagery: [guide_obj+0x08] != -1

hud imports `XamBuildResourceLocator` (thunk `913FE8C4`) and has **5** call
sites. Note hud's extracted PE has ImageBase `98000000` but loads at
`913E0000`, so file VA -> runtime is `va - 0x98000000 + 0x913E0000`; the
earlier "0 callers" result was that mapping being wrong, not an absence.

```
runtime call sites: 913EB550  913EB9AC  913EC774  913EC7C0  913ECB9C
hud functions entered near them: 913EB940  913EBAA8  913EC578  913ECA00 ...
```

Three of the five sites lie inside functions that **did execute**
(`913EB940`, `913EC578`, `913ECA00`), yet the locator is never called by hud -
only by dash (`lr=92181B34`). So those functions run and branch around it.

The guard, disassembled from hud's image:

```
913EB984  lwz   r3,0x08(r31)     ; [guide_obj+0x08]
913EB988  addis r11,r0,0x9140
913EB98C  cmpwi cr6,r3,-1
913EB990  addis r10,r0,0x913E
913EB994  bne   cr6,+0x20        ; <- skips the call unless [+0x08] == -1
913EB998  addi  r7,r0,0x80
913EB99C  lwz   r3,0x04(r31)     ; skin module
913EB9A0  addi  r4,r10,0x1B24    ; "hud" container string
913EB9A4  lwz   r5,0x168(r11)
913EB9A8  addi  r6,r1,0x60
913EB9AC  bl    XamBuildResourceLocator
```

`r31` is the **guide object**: `[+0x04]` is the skin module and `913E1B24` is
the "hud" container string - both exactly as the bootstrap logs them
(`guide object 401587E0, [guide+4] = skin module 301BA000`;
`hud container@913E1B24 = "hud"`). That identification is measured, not
assumed.

So `0xFFFFFFFF` is the "not loaded yet" sentinel, and hud only builds a
resource locator when the slot still holds it. `[guide+0x08]` is evidently
something else, so **every** hud resource load is skipped - which is precisely
the long-standing measured absence.

Next: read `[guide_obj+0x08]` at runtime, and if it is not `-1`, try setting
it to `-1` before the draw so hud takes the load path. Note our own bootstrap
already loads `strings.xus` by hand and stores it at `[guide+4E8]`; if it also
writes `[guide+0x08]`, that would be self-inflicted and is the first thing to
check.

### The flag already existed: guide_static_locator

`[guide+0x08]` is **0**, not `-1`, and there is already a cvar for it. Its
description independently states the mechanism my hud disassembly found, and
adds the consequence:

> "hud's scene creator tests [guide+8] against -1 and takes the dynamic path
> for anything else, passing [guide+8] itself as the module - and it is 0,
> which produces the locator `section://@0,hud#strings.xus`: a null package,
> so `XuiSceneCreate("InfoUpsellLive.xur")` finds nothing and **returns an
> empty scene**. The static path uses [guide+4] instead, which the bootstrap
> already sets to hud's module."

Two independent derivations agreeing - the `cmpwi r3,-1 / bne` guard read out
of hud's image, and this description - is as good as confirmation gets short
of running it.

An empty scene from a null package explains the whole present state: every
render stage works, element render methods execute, and there is nothing in
the scene to rasterise. It also explains the resource absence without any
component being broken, which is what the last several leads all failed to do.

Testing it on the full working stack. If the payload stays at one word, the
"empty scene" explanation is wrong too and the scene contents seen in
`GuideScene` (btnJoinLive, btnB, labelHeading with visuals) need re-examining
- those came from a scene that this description says should be empty, which is
a tension worth resolving either way.

## guide_static_locator applies, and exposes an instrumentation blind spot

The flag works and is correctly ordered:

```
21541  GuideBootstrap: [guide+8] = FFFFFFFF (static locator path)
21727  DemandFunction: enter 913FE6D4        (XuiSceneCreate)
```

So `[guide+8]` is `-1` **before** hud creates its scene, and the
`cmpwi r3,-1 / bne` guard should now fall through to the locator call.
Payload unchanged: 1 word, GPU draws +0.

But the premise this was chasing is now in doubt.

```
XamBuildResourceLocator HLE log:  4 calls, all lr=92181B34 (dash)
log limit:                        200, so 4 really is the total *HLE* count
DemandFunction: enter 913FE8C4:   1   <- hud's own locator thunk WAS entered
```

hud executed its `XamBuildResourceLocator` thunk, and no HLE line appeared for
it. The thunks are Xenia import stubs (`0100031B 0200031B mtctr bctr`, ordinal
`0x31B`=795) patched at load; dash's calls reach Xenia's HLE
(`xam_info.cc`) and log, hud's evidently resolve to the **real LLE xam** and
do not.

### The correction

"**The Guide never requests any imagery**" - repeated across many ticks and
used to justify several leads - is **not established**. It rests on an HLE-side
log that structurally cannot observe hud->LLE-xam calls. The absence measured
was an instrumentation blind spot, not a fact about the guest.

This is the second blind spot of exactly this shape, after `cpu::Breakpoint`
wedging the title thread. Both produced confident negative claims from an
instrument that was never able to see the thing being claimed absent.

Any measurement of hud's behaviour must therefore come from something that
observes the LLE path: demand-JIT (which does), or a hook inside xam's own
code (not on Xenia's HLE export).

## CONFIRMED: guide_static_locator works - hud takes the static path

The `GuideScene` diagnostic already resolves xam ordinals to their **real LLE
addresses**, which is the instrument the HLE log could not be:

```
xam ordinal 31B (XamBuildResourceLocator)        -> 8178E340
xam ordinal 31E (XamBuildDynamicResourceLocator) -> 8178E420
```

Demand-JIT on those addresses:

```
8178E340  enter=1     <- static locator RAN
8178E420  enter=0     <- dynamic locator never ran
```

So with `[guide+8] = -1`, hud builds its locator through
`XamBuildResourceLocator` exactly as intended, and never touches the dynamic
path that would have produced the null-package `section://@0,...`.

**"The Guide never requests any imagery" is refuted.** It requests through
real xam; the HLE log simply cannot see it. Every lead that rested on that
absence - including the retracted "resource provider is a stub" - was built on
nothing.

### Where that leaves things

The scene loads, hud locates resources through the correct path, the package
demonstrably works (`strings.xus` loads from `section://301BA000,hud#`), every
render stage is correct, element render methods execute - and the Guide still
emits one word.

So the next question is narrow: do the located resources actually **open and
decode**, and do the visuals end up with imagery? That must be measured on the
LLE side. Useful handles already in the log: the visual-class global
`[81D6CDDC] = 40887070`, the string `"XuiVisualCreateInstance(%S)"` at
`816462C4`, and the XUI registry with **38 non-null of 48** entries.

### Visuals ARE created; the gap is between visual and geometry

`"XuiVisualCreateInstance(%S)"` at `816462C4` has exactly one xref, at
`8193B6D0`, inside function **`8193B6B0`** (len 0x114, 3 callers:
`8193B7F4`, `819591E0`, `819592B8`).

```
DemandFunction: enter 8193B6B0   = 1     <- visual instances ARE created
```

So the chain is intact end to end: the scene loads, `guide_static_locator`
puts hud on the working locator path, resources are located through real xam,
visual instances are created, and the element render methods execute.

And the emitter is still only ever called from `XuiRenderPresent`. So xam's
element rendering runs and **early-outs per element** rather than failing.

Candidate reason, from data already in the log:

```
0001012D GetPosition -> 00000000 00000000 00000000 00000000
000100B1 GetPosition -> 00000000 00000000 00000000 00000000
000100E2 GetPosition -> 431C0000 42100000 00000000 00000000
                        =156.0f   =36.0f   =0.0f    =0.0f
```

`labelHeading` has a real position (156, 36) and the trailing two floats are
**0.0**. If those are width/height, every element is zero-sized and there is
nothing to rasterise - which would explain an intact pipeline emitting
nothing, and would be a layout problem rather than a resource one.

This is *not* established: `XuiElementGetPosition` may well return x/y/z/w
rather than x/y/w/h, in which case two trailing zeros are unremarkable. Check
the function's semantics in xam before building on it - the last several
dead ends all came from treating a plausible reading as a fact.

### Zero-size lead: dead (the caveat was right again)

`XuiElementGetPosition(handle, buf)` is called with a 32-byte buffer and
returns a position vector, so

```
000100E2 GetPosition -> 431C0000 42100000 00000000 00000000
                        x=156.0  y=36.0   z=0.0    pad
```

means x/y/z + padding, **not** x/y/width/height. Element sizes are simply not
being read by this diagnostic, and nothing here says elements are zero-sized.

Third consecutive lead where flagging "this is not established" before acting
prevented a wrong turn (`0 xrefs`, the memory:// provider, and now this). The
pattern is consistent enough to state as a rule: **a plausible reading of a
number is worth one command to verify and several ticks if assumed.**

### Standing state after the static-locator work

Confirmed working, all measured on the LLE side:

* `[guide+8] = -1` before scene creation; hud takes the **static** locator
  path (`8178E340` entered, `8178E420` never)
* scene loads; `XuiSceneCreate` / `NavigateFirst` / `RenderBegin` / `RenderEnd`
  all execute
* `XuiVisualCreateInstance` (`8193B6B0`) executes - visuals are instantiated
* 48 xam render-area functions execute after the press
* every device/buffer/gate/RT stage verified correct
* the title renders normally throughout

Unexplained: element rendering runs and never reaches the draw emitter, which
is called exactly once per run, from `XuiRenderPresent`.

Next candidate to measure (not assume): element **size/bounds** via whatever
xam API supplies them, and per-element visibility/alpha. Both are read with
the same `GuideScene` diagnostic pattern that already resolves handles, so
neither needs a breakpoint.

### Back-buffer size is sane: 852x480

```
xam ordinal 350 -> 818FAF48   (XuiRenderGetBackBufferSize)
xam ordinal 336 -> 81932110   (XuiElementSetBounds)
GetBackBufferSize(dc 408BE3A0) -> 00000000; w=852 h=480 (raw 354 1E0)
```

hud calls this (thunk `913FE8A4` entered) and lays out against it, so a 0x0
answer would have collapsed the layout. It returns **852x480**. Dead end -
but a cheap one, and it removes an entire class of explanation.

Caveat: this probes the **bootstrap's** DC `408BE3A0`, while the draw path
uses `407D4C80`. The two could differ; if this line is ever load-bearing,
re-probe with the draw's DC.

Also measured, both LLE-visible:

```
XuiElementSetBounds   913FE884  entered      <- hud does lay elements out
XuiElementSetOpacity  913FE684  NOT entered  <- never set, so default opaque
```

### Where the search stands

Everything nameable has been verified working: device, wrapper, front buffer,
render target, command buffer, present gate, both null-render flags, the
static locator path, scene creation/navigation, the render bracket, visual
instantiation, element bounds, and a sane back-buffer size. The title renders
normally. And xam's element rendering runs without ever reaching the draw
emitter, which fires exactly once per run, from `XuiRenderPresent`.

The remaining question lives **inside xam's element render path** - between
"element with bounds and a visual" and "a DRAW_INDX packet". The two obvious
instruments are both unavailable there: `cpu::Breakpoint` wedges the title
thread, and `guide_coverage_fn`'s per-instruction counters crash the
skin-init path. Any further progress needs an instrument that survives the
title thread - demand-JIT of specific candidate functions is the one tool
that has kept working.

## XuiRenderEnd is the draw dispatcher

Runtime ordinal resolution (Xenia's own module lookup - the `.edata` parse in
`pe360` returns nonsense and should not be trusted):

```
xam ordinal 34B -> 818FAE68   XuiRenderBegin
xam ordinal 34F -> 818FAEE0   XuiRenderEnd
xam ordinal 350 -> 818FAF48   XuiRenderGetBackBufferSize
xam ordinal 336 -> 81932110   XuiElementSetBounds
xam ordinal 3DF -> 81932190   XuiElementGetPosition
```

`818FAEE0` = **XuiRenderEnd**, and it is where drawing is dispatched:

```
818FAEF0  or     r31,r3,r3      ; r31 = arg1
818FAEF8  cmplwi cr6,r31,0
818FAEFC  bne    cr6,+0x14
818FAF08  ori    r3,r3,0x0057   ; null -> E_INVALIDARG, no draw
818FAF10  lwz    r11,0(r31)
818FAF18  lwz    r11,0x4C(r11)  ; arg1->vtable[19]
818FAF20  bctrl
```

So "the Guide draws nothing" reduces to: **XuiRenderEnd's dispatch does not
produce a draw.**

### Correction to my own inference

I matched slot 19 against the **wrapper** vtable `81640680`, got `8191B250`,
confirmed by demand-JIT that `8191B250` never runs, and concluded hud passes
null. That reasoning is unsound: `XuiRenderEnd` takes the **DC**, and the DC
has its own vtable, so its slot 19 is a different function entirely.

This is the second time the same mistake has been made - `8191AFD0` was
called "vtable[20]" from a flag description when it is slot **11** of the
wrapper's table. **Whenever a vtable slot is quoted, name which object's
vtable it belongs to.**

The DC's vtable is now printed (`[11]`, `[19]`, `[20]`) so the correct slot 19
is read rather than assumed. Note `[render_obj+12]` is `407D4C80` and non-null
in every run, so hud does have a DC - the "hud passes null" story was already
weak on the evidence.

## The entire XUI render path executes - nothing is skipped

DC vtable is `8163E4E8` (same for both DCs), and its slots match everything
already known independently:

```
DC 407D4C80 vtable 8163E4E8  slot18=818F82F0  slot19=818F8A90
                             slot20=818FDDE8  slot21=818F9290
```

`slot21 = 818F9290` is `XuiRenderPresent` exactly as recorded much earlier,
and `slot20 = 818FDDE8` is the one `XuiRenderBegin` skips when the null-render
flag is set. That cross-check confirms the table is the right one.

Demand-JIT on the whole path:

```
818FAE68  XuiRenderBegin   entered
818FDDE8  DC vtable[20]    entered     <- render setup RUNS
818FAEE0  XuiRenderEnd     entered
818F8A90  DC vtable[19]    entered     <- the DRAW method RUNS
818F9290  XuiRenderPresent entered
```

So the "XuiRenderEnd is handed null" story is **dead**: the dispatch happens
and the draw method runs. Every stage of the pipeline, from device creation to
present, now demonstrably executes.

That is worth stating plainly because it eliminates the entire class of
explanation this project has been working through for many ticks - nothing is
gated off, skipped, null, or unregistered. `818F8A90` runs and emits no
geometry.

### Next

Examine `818F8A90`'s callees to find where quads should be produced, and which
of those actually run. The emitter `819F5D18` is reached only from Present, so
either element drawing uses a different emit path, or it produces zero
primitives. Demand-JIT over `818F8A90`'s call graph distinguishes those two
without needing a breakpoint.

### The draw call graph runs four levels deep, entirely

```
818F8A90 (DC vtable[19], len 0xE8)
  -> 8180D76C            entered
  -> 818F83D8 (0x74)     entered
       -> 81916238 (0x74)      entered
            -> 81D0F50C        entered
            -> 819149D0 (0xD8) entered
            -> 81D0F51C        entered
                 -> 8180D760   entered
                 -> 81910188   entered
                 -> 819100E8   entered
                 -> 81914088   entered
```

Every node executes. Walking the call graph further has clearly hit
diminishing returns: nothing anywhere in this pipeline is skipped, gated, or
short-circuited.

### A hypothesis that has not yet been tested

`guide_second_context_kb` "resets the cursor, calls xam's own begin
(81A01358), runs the Guide's draw, then submits whatever was emitted" - and it
sees **1 word**. That measures only the buffer at `[dev+0x2B4C]` on the
**Guide's** device.

But `guide_bind_title_rt` deliberately binds the **title's** render target,
and the composite draw runs inside `VdSwap`. If xam's element rendering emits
into a different buffer - the title's ring, or an internal batch - then:

* our submit would legitimately see 1 word, and
* `GPU draws +0` would be measured around *our* submit, while any real draws
  would land in the title's own count (`GuideFrame: gpu_draws +14065` per 500
  frames), where they are indistinguishable from dash's.

So "the Guide emits nothing" may actually be "we are counting the wrong
buffer". This is the first explanation consistent with a fully-executing
render path, and it is **testable**: compare the title's per-frame GPU draw
delta with the Guide open versus closed, and dump the region around
`[dev+0x2B4C]` and the title's cursor after a composite draw.

### REFUTED: the geometry is not going into another buffer

The title's own GPU draw counter, before and after the Guide opens (same run,
existing log - no new instrumentation needed):

```
BEFORE press:  +14094  +14123  +14123  +14123   per 500 frames
AFTER  press:  +13862  +14123  +14123  +14152  +14123
```

Identical. The Guide contributes **zero** draws to the title's ring, just as it
contributes zero to its own second context. So "we are counting the wrong
buffer" is wrong: no geometry is produced anywhere.

Combined with the previous tick, the position is now tightly bounded:

* the full render path executes, four levels deep, nothing skipped
* no draws appear in the Guide's buffer **or** the title's
* therefore xam's element rendering produces **zero primitives**

That points at the elements themselves having nothing renderable - most
plausibly no imagery, since visual *instances* exist but no image resource has
been shown to open. `guide_static_locator` proved locators are *built*
(`8178E340` runs); it did not show anything is opened or decoded.

Next: determine whether an image resource is actually opened and decoded -
i.e. whether the section provider's open path runs for anything other than
`strings.xus`. That is the last untested link between "scene with visuals" and
"pixels".

### Re-testing label injection now that the locator works

The section provider's vtable is **not** adjacent to its format string - the
words before `section://` are all UTF-16 (`file://`, `media://`,
`%s.xzp#%s`), so the memory:// vtable-then-string adjacency was coincidence,
not a layout convention. Do not assume it for other schemes.

Rather than keep hunting providers statically, re-running
`guide_inject_label_text`. It was tried before and returned `80300016` on
every node - but that was **before** `guide_static_locator`, when hud was
building `section://@0,...` against a null package. Every prerequisite has
changed since:

* hud now takes the static locator path (`8178E340` runs)
* the null-render flag is cleared at both levels
* the full render path executes four levels deep
* RT, front buffer, command buffer and present gate are all correct

If `SetText` now succeeds and geometry appears, the render path works and the
content was the whole problem. If it succeeds and *still* nothing is emitted,
that separates "no content" from "content that cannot be rasterised" - which
is a distinction nothing measured so far can make.

## Fonts: xam does request the typeface (user-suggested line of inquiry)

`F:\FuzionFrenzy` is not a raw 360 disc - it is an **Xbox One/Series
backward-compatibility package**: `Emu.exe`, `D3D12Core.dll`,
`DX12DirectResolveShaders.sbin`, `DX12EdramResolveShaders.sbin`, `EmuMenu`
(a modern WinUI/WebView2 Guide replacement), and the firmware partitions.
`Flash/` holds `xam.xex`, `hud.xex`, `huduiskin.xex`, `ximecore.xex` and
**`xenonjklatin.xtt`**.

Those `.sbin` shaders belong to Microsoft's BC emulator (EDRAM/direct resolve)
and are not Guide assets; they cannot help Xenia, whose blocker is upstream of
shaders - no draw packets are produced at all.

The font angle is more interesting. Strings in xam's own image:

```
815FC6E0  "file://media:/XenonJKLatin.xtt"   (also SCLatin / CLatin variants)
81640222  "gXTTCache.head.next == &gXTTCache..."   (an XTT font cache)
8163F970  "XUIFONT::Init -- must specify a typeface."
```

Measured, LLE-visible:

```
81754C10  references the font path         entered
81913770  XUIFONT::Init                     entered
```

So LLE xam uses the **same `media:` convention** as Xenia's HLE
(`XamGetLanguageTypeface` returns `file://media:/XenonJKLatin.xtt`), it does
resolve that path, and `XUIFONT::Init` runs. `dashroot/` contains
`xenonjklatin.xtt` (plus SegoeXbox-Light, xenonclatin, xenonsclatin), so the
file is present where xam looks. hud does **not** import the typeface APIs -
only dash does - so font handling is internal to xam.

**Unresolved:** whether the `.xtt` open actually succeeds and whether
`XUIFONT::Init` takes its "must specify a typeface" error path. Two cautions:

* `.xtt` never appears in the log, but **data-file opens are not logged by
  name** - only module loads are. That absence proves nothing (the same trap
  as the HLE resource log and the breakpoint silence).
* `81913C8C`, the error site, shows `enter=0`, but it is a **mid-function**
  address and demand-JIT only records function entries. That zero is
  meaningless.

A font that fails to initialise would leave text elements with no glyphs and
therefore no primitives - which fits the symptom exactly. Needs a real
measurement of the open result, not an inference from silence.

## VERIFIED: no font file is ever opened

VFS-level logging at `VirtualFileSystem::ResolvePath` - the chokepoint every
guest file lookup passes through - prints any `.xtt` lookup and its result.
Result: **no font lookup line at all.**

Unlike the earlier false negatives, this one was checked before being
believed:

* the `"font lookup"` string is present in the built binary (so the code
  shipped)
* `xe::utf8::find_first_of(haystack, needle)` is **substring** search, not
  character-set search - the condition is correct
* `ResolvePath` is demonstrably exercised: 72 logged calls in the same run,
  including `\xam.xex` and `\dash.xex`

So the absence is real: **xam resolves the typeface path but never opens the
file.**

Measured alongside it:

```
81754C10  references "file://media:/XenonJKLatin.xtt"   entered
81913770  XUIFONT::Init                                  entered
```

`81754C10` is almost certainly xam's own `XamGetLanguageTypeface` (ordinal
0x58C) - it *returns* a path; it does not open anything. Whoever receives that
path is responsible for the open, and that open never happens.

A Guide with no font has no glyphs, therefore no primitives - which fits the
symptom exactly: a render path that executes completely and emits nothing.

Fonts present and real in `dashroot/`: `xenonjklatin.xtt` (1.7MB, the default
for non-Chinese locales), `xenonclatin.xtt` (1.2MB, TChinese),
`xenonsclatin.xtt` (1.4MB, SChinese), `SegoeXbox-Light.xtt` (24KB). No
language override in the config, so `XenonJKLatin.xtt` is the expected
request.

Next: find the callers of `81754C10` and determine which should perform the
open, then why it does not.

### The font path runs, but four of its branches never do

Chain, all confirmed entered:

```
81795548  skin loader (driven by lle_xam_skin_init)
  -> 8178DE50  (len 0x98)          entered
       -> 81754C10  typeface path provider   entered
       -> 81AA5AD8 817AC550 81754A60 81755430 81970BD0  all entered
```

So the font-loading function and every one of its immediate callees execute,
and still no `.xtt` reaches the VFS.

The divergence is one level down, inside `81970BD0` (len 0x1D4):

```
entered:      8196B1C8  81970748  819705F0  8196C738  81970398  8193F730
NOT entered:  81970E90  81970DA8  817F68C0  81970A78
```

Four branches never run. Their shapes:

```
817F68C0  len 0x88  -> 81801D00 81D104EC 81800D78 818007A0
81970E90  len 0xCC  -> 81D0F8EC
81970DA8  len 0xE4  -> 8180D764 81D0F30C 817F7778 8193F8B0 81C85D68
81970A78  len 0xE4  -> 8180D768 81D0F50C 819706C8 8196D098 ...
```

`817F68C0` calls into the `8180xxxx` range, which is where xam's lower-level
helpers live - the most plausible I/O path of the four.

This is the first genuine branch divergence found anywhere in the Guide's
render or resource path: everything else measured has simply run. Worth
pursuing carefully rather than quickly - identifying which of these four is
the file open, and what condition gates it, is the concrete next step.

## The font block is gated on a ';' in a string at [r31+0x10]

Inside `81970BD0` (reached from the skin loader via `8178DE50`), the four
never-executed branches are all downstream of one gate:

```
81970C3C  lwz   r3,0x10(r31)   ; a string pointer
81970C40  cmpli r3,0
81970C44  beq   +0x54          ; null -> skip the whole block
81970C48  addi  r4,r0,0x3B     ; 0x3B = ';'
81970C4C  bl    81813270       ; wide-string char search (lhz/cmp loop)
81970C50  cmpli r3,0
81970C54  beq   +0x44          ; NOT FOUND -> skip the whole block
81970C58  lis   r11,0x8165
81970C60  bl    81970E90       <<< never runs
81970C84  bl    81970DA8       <<< never runs
81970C90  bl    817F68C0       <<< never runs
```

`81813270` ran (demand-JIT), which proves execution got **past** the null
check - so `[r31+0x10]` is non-null and the skip happens at the second gate:
**the string contains no `;`**.

So the Guide's font loading is gated on a semicolon-delimited typeface list.
No semicolon, no font block, no `.xtt` open - which is exactly the verified
absence at the VFS, and would leave text elements with no glyphs and therefore
no primitives.

Caveat worth keeping: `81813270` is a generic helper with no `.pdata` entry,
so demand-JIT "entered" only proves it ran *somewhere*, not necessarily from
this call site. The inference that we passed the null check is strong but not
airtight; reading `[r31+0x10]` directly would settle it.

### Next

Read the actual string at `[r31+0x10]` on the skin-init path. Note the skin
loader runs on the bootstrap thread, **not** the title thread, so a
`cpu::Breakpoint` there may be safe - the wedge documented earlier was
specifically on the title thread (`F8000144`). That needs verifying before
relying on it.

## RETRACTED: the semicolon gate is an error path, not the font gate

The previous section concluded that font loading is gated on a semicolon in
`[r31+0x10]`, and that the absence of one skips the `.xtt` open. **That is
backwards.** The branch polarity was read from a disassembly of the wrong
image.

Two errors compounded:

1. **Wrong image.** The addresses quoted in the previous section do not decode
   as described in `work/xam.pe` (Flash xam, 17003) - `81970C3C` there is a
   `blr` epilogue. The run uses **`xam17489`**. Every address in that section
   only makes sense against `work/xam17489.pe`.

2. **Inverted polarity.** Decoded against the correct image:

```
81970C30: bl     0x81813270
81970C34: cmpli  cr0,r3,0x0
81970C38: bne    0x81970C58   (+32)    ; found -> error block
81970C3C: lwz    r3,16(r31)
81970C40: cmpli  cr6,r3,0x0
81970C44: beq    0x81970C98   (+84)    ; null -> skip error block
81970C48: addi   r4,r0,59              ; ';'
81970C4C: bl     0x81813270
81970C50: cmpli  cr0,r3,0x0
81970C54: beq    0x81970C98   (+68)    ; NOT found -> skip error block
81970C58: lis    r11,0x8165
81970C5C: addi   r3,r11,-29376         ; = 0x81648D40
81970C60: bl     0x81970E90            ; <-- the "never executed" branch
```

`0x81650000 - 29376 = 0x81648D40`, which is the string:

```
XuiRegisterTypeface: Semicolon character found in typeface descriptor string
```

So `81970E90`, `81970DA8`, `817F68C0` and `81970A78` are the **diagnostic /
error path for a malformed typeface descriptor**. A semicolon is *rejected*,
not required. Their never running is the **healthy** state and means the
descriptor is well-formed. `beq +68` is the normal path.

Consequence: the "first genuine branch divergence" was not a divergence. The
font block is not gated on a semicolon and nothing here explains the missing
`.xtt` open. That inference chain is dead and should not be pursued further.

Supporting string evidence in `work/xam17489.pe` (absent from `xam.pe`, another
confirmation of which image is live):

```
81648D40  XuiRegisterTypeface: Semicolon character found in typeface descriptor string
8163F970  XUIFONT::Init -- must specify a typeface.
8163FAC4  XUIFONT::Init -- Failed to copy typeface descriptor (0x%08X)
81648CD4  Could not look up '%S' typeface
81645B27   Failed to create typeface %ls (hr=0x%08X)
```

Nine typeface paths exist, none containing a semicolon, e.g.
`file://media:/XenonJKLatin.xtt` at `815FC9A1` plus `p1`/`p2` variants.

### Where to look instead

`XUIFONT::Init` was already measured as entered. The three messages above are
its own failure reports and none has been observed. The open question is
unchanged from before the semicolon detour: **who receives the path from
`81754C10` and why no open reaches the VFS.** Reading the descriptor string at
`[r31+0x10]` is still worth doing, but as a *well-formedness check*, not as the
suspected gate.

Method note: this is the second finding in this file invalidated by
disassembling the wrong xam build. Every address quoted from here on should
state which image it was decoded against.

## RETRACTED: `81A0FE48` can be lifted out - it was called with a null argument

*(All addresses below decoded against `work/xam17489.pe` with
`tools/ppc.py`, which resolves branch targets absolutely.)*

The section "Driving the skipped setup directly does not work either"
concluded that the front-buffer setup depends on device state `819F4D28`
establishes earlier, and therefore cannot be lifted out of the mode-1 path.
**That conclusion does not follow from the crash it was based on.** The crash
is a null *argument*, supplied by our own call.

`guide_force_front_buffer` calls `81A0FE48(device, 0)`, described in its cvar
help as "with the argument its own call site would have used". That is the
error: the real call site passes a pointer, not zero.

### The crash decodes exactly

Recorded crash: `GUEST CRASH at 81A04648  fault_addr ...4C  r3=0`, unwind
`81A0FF7C`. That unwind value is a **return address**, so the fault is inside
the call made at `81A0FF78`:

```
81A0FF70: addi   r4,r30,72          ; r4 = arg2 + 0x48
81A0FF74: mr     r3,r31             ; r3 = device
81A0FF78: bl     0x81A04570         ; faults inside, at 81A04648
81A0FF7C: cmpi   cr0,r3,0           ; <- the recorded unwind address
```

`r30` is `81A0FE48`'s second parameter. With our `0`, `r4 = 0x48`, and the
fault address ends in `4C` - `0x48 + 4`. That is the reported fault address,
digit for digit. Nothing about device state is implicated.

### What the argument actually is

`81A0FE48` has exactly one real caller, `819F4E8C`, inside `819F4D28`:

```
819F4E84: mr     r4,r28
819F4E88: mr     r3,r31
819F4E8C: bl     0x81A0FE48
```

and `r28` is written exactly once in `819F4D28`, in the prologue:

```
819F4D34: mr     r31,r3      ; arg1  device
819F4D38: mr     r29,r4      ; arg2  mode  (cmpi r29,1 at 819F4D58)
819F4D3C: mr     r30,r6      ; arg4
819F4D40: mr     r28,r7      ; arg5  <-- passed to 81A0FE48
819F4D44: mr     r27,r8      ; arg6
```

So the second argument to `81A0FE48` is **`819F4D28`'s fifth parameter
(`r7`)**. It is a live structure pointer - `819F4D74` dereferences it at
`+0x20` early in the function, and `81A0FE48` reads it at `+0x48`.

**Mode 2 already supplies it.** `8178F748` calls `819F4D28` with mode 2 and
the same six arguments; `819F4D28` simply skips the `81A0FE48` call on that
path. The pointer is therefore live and correct in every mode-2 run.

### The concrete fix

Capture `r7` on entry to `819F4D28` (or at the `8178F748` call site) and pass
it as the second argument instead of `0`:

```
81A0FE48(device, captured_r7)
```

`81A0FE48` is what allocates `[dev+0x2B10]` and performs the setup that fills
`[dev+0x3F74]` - the null front buffer the emitter `819F5D18` faults on. This
is the documented mode-2 blocker, and it now has a small, specific fix rather
than an unbounded one.

### Related: what `[dev+0x2B10]` is

Only two instructions in xam write it:

```
81A0FEF8: stw r3,0x2B10(r31)   ; in 81A0FE48 - the allocation
81A0F9D0: stw r30,0x2B10(r31)  ; in 81A0F858 - preceded by two frees
```

The allocation is 128 bytes from `81A0A290(0x80, 5, 2)`:

```
81A0FEF0: li     r3,128
81A0FEEC: li     r4,5
81A0FEF4: bl     0x81A0A290
81A0FEF8: stw    r3,11024(r31)
```

The wait loop `819F4488` polls the **first word of that block**:

```
819F44C0: lwz    r11,11024(r29)   ; r11 = [dev+0x2B10]  (a pointer)
819F44CC: lwz    r8,0(r11)        ; progress = [[dev+0x2B10]+0]
819F44D4: cmpl   cr6,r9,r8        ; vs last seen at [r31+8]
```

So the "async calls never retire" symptom is precisely: *nothing ever
increments the first word of that 128-byte block.* That is a much narrower
statement than "cause unknown", and it is the thing to instrument next on the
mode-1 path.

Note also that `81A0F858` writes `[dev+0x2B10]` **after freeing it** (frees
`[+0x2B14]` and `[+0x2B10]` via `81A0A3A0`, then stores `r30` into both). Its
`guide_device_begin` cvar help calls it "the single routine that writes
`[dev+0x2B10]`", which reads as setup; the surrounding code is a teardown
shape. Worth confirming `r30` is zero there before relying on that cvar.

## Correction to the section above: the crash decode was right, the fix was not

The preceding section correctly identified *why* `81A0FE48(dev, 0)` crashes,
and then drew a wrong conclusion about how to fix it. Measured, not inferred:

```
DevCreate #1: device=00000000 mode=1 r6=00000100 arg5(r7)=709DF190 r8=709DF170 lr=8178EB68
DevCreate #1: device=00000000 mode=2 r6=00000000 arg5(r7)=00000000 r8=81D43684 lr=8178F7D4
```

**Mode 2's `r7` is genuinely zero.** The claim "mode 2 already supplies it, the
pointer is live and correct in every mode-2 run" is false. `0` was what the
real call site would have passed on that path all along.

What survives from that section, and what does not:

| claim | status |
|---|---|
| The crash is a null-argument deref: `r4 = r30+0x48`, fault on `0x4C` | **holds** - matches the recorded fault address exactly |
| `81A0FE48`'s arg2 is `819F4D28`'s arg5 (`r7`), via `r28` | **holds** - one caller, one write to r28 |
| Mode 2 supplies that pointer, so it can just be passed through | **refuted** - mode 2's r7 is 0 |
| `81A0FE48` can be lifted out of the mode-1 path | **refuted again** - the original conclusion stands |

So the original "cannot be lifted out" conclusion is **reinstated**, but for a
sharper reason than "it depends on device state": it depends on a **caller-built
parameter block that only the mode-1 creator constructs**.

### What that block is

In mode 1 the pointer is `709DF190`, and `r8` is `709DF170` - 32 bytes below
it. Both are stack addresses inside `8178E9F0`'s frame, so the mode-1 creator
builds a structure on its own stack and passes interior pointers to it.
`r6=0x100` on that path and `0` on the other. In mode 2 the corresponding `r8`
is `81D43684`, xam's device global, not a stack address at all - the two
creators do not even pass the same *kind* of argument.

This also means the value is **not capturable for later reuse**: it dies when
`8178E9F0`'s frame returns. Any attempt to drive `81A0FE48` outside that frame
must *synthesize* the block, not borrow a pointer to it.

### Concrete next step

Disassemble `8178E9F0` and record what it stores into the stack block it
passes as `r7`, and what `81A0FE48` reads at `+0x48`. That gives the layout
needed to fabricate one. `81A0FE48` passes `r30+0x48` to `81A04570`, so at
minimum offset `+0x48` must be a valid sub-structure - `81A04570` faulted
reading `+0x4C` of it.

### Instrumentation added

* `tools/ppc.py` - PowerPC BE disassembler with absolute branch-target
  resolution. Written because two findings in this file were invalidated by
  hand-decoding errors. It prints the image name in its header; quote that.
* `DevCreate` breakpoint at `819F4D28` (under `guide_trace_devsetup`), logging
  `device / mode / r6 / arg5(r7) / r8 / lr`. Unlike the existing `DevSetup`
  breakpoint at `81A0FE48`, this one fires on **both** device paths, which is
  what made the mode-1 vs mode-2 comparison above possible in two runs.
* `guide_force_front_buffer` now refuses to call `81A0FE48` with a zero second
  argument and logs a warning instead of reproducing the known crash. With
  mode 2 that means it is now a no-op, which is the honest behaviour until the
  block above can be synthesized.

## The mode-1 parameter block: layout, from `8178E9F0`

*(Decoded against `work/xam17489.pe` with `tools/ppc.py`.)*

The block `81A0FE48` needs is built on `8178E9F0`'s stack and is **124 bytes**
at `r1+0x90`:

```
8178EA70: li     r5,124            ; 0x7C
8178EA74: li     r4,0
8178EA78: addi   r3,r1,144         ; r1+0x90
8178EA7C: bl     0x818019E8        ; memset(block, 0, 124)
...
8178EB3C: addi   r8,r1,112         ; r8 = r1+0x70
8178EB44: addi   r7,r1,144         ; r7 = r1+0x90  = the block
8178EB64: bl     0x819F4D28
```

Cross-check against the measured run: `r7=709DF190`, `r8=709DF170`, so
`r1=709DF100`, `r1+0x90 = 709DF190`. Exact match, both pointers.

The mode-2 site hardcodes `li r7,0` at `8178F7BC` - it has no block at all,
which is why the pass-through fix in the earlier section could not work.

### Field map (offsets relative to the block, i.e. `r1` offset minus 0x90)

The whole 124 bytes are zeroed first, so **only these fields are non-zero** -
which is what makes synthesizing one realistic.

| offset | value | source |
|---|---|---|
| `+0x00` | **width** | `640` on the fallback path (`8178EAE4`), else `[r31+4]`-ish from the display mode |
| `+0x04` | **height** | `480` on the fallback path (`8178EAEC`), else `lhz r10,6(r31)` |
| `+0x08` | `0x28280186` | `lis 0x2828 / ori 0x0186` at `8178EB08`/`8178EB18` |
| `+0x34` | `0x80000000` | `lis r8,0x8000` at `8178EB14` |
| `+0x3C` | `1` | `li r24,1` at `8178EB30` |
| `+0x40` | r29 / r9 | branch-dependent |
| `+0x44` | byte | `lbz r11,340(r1)` at `8178EB10` |
| `+0x4C` | **4096** | `li r10,4096` at `8178EB2C`, stored at `8178EB50` |
| `+0x54` | `0x00010000` | `lis r7,0x0001` at `8178EB20` |
| `+0x60`,`+0x64` | r30 | `8178EAF0`, `8178EAFC` |
| `+0x68`,`+0x6C` | width, height again | `8178EB00`, `8178EB04` |
| `+0x70` | `[r1+0x168]` | `lwz r9,360(r1)` |
| `+0x74` | `[r1+0x16C]` | `lwz r5,364(r1)` |

Two width/height pairs (`+0x00/+0x04` and `+0x68/+0x6C`) and a format-looking
constant at `+0x08` make this a **D3D presentation-parameters** structure.

**`+0x4C` is the field that faulted.** `81A0FE48` computes `r4 = block+0x48`
at `81A0FF70` and `81A04570` reads `[r4+4]` = `block+0x4C`. On the fallback
path that field is simply `4096`.

### Why this is now synthesizable

124 zero bytes plus roughly a dozen fields, most of them literal constants, and
the two that are not (`width`/`height`) have a documented 640x480 fallback the
guest itself uses. That is a much smaller thing to fabricate than "device state
mode 2 never establishes".

Note also `8191BAC8` memcpy's **124 bytes** from its third argument into
`wrapper+0x10` - the same size. Worth checking whether `wrapper+0x10` on a
mode-2 device already holds a populated copy of this structure, in which case
`81A0FE48(real_dev, wrapper+0x10)` needs no fabrication at all. That is the
cheapest experiment and should be run before writing any synthesis code.

### Next

1. Dump `wrapper+0x10` (124 bytes) on a mode-2 run and compare against the
   field map above. If `+0x00/+0x04` look like a resolution and `+0x4C` is
   4096, pass it straight to `81A0FE48`.
2. Only if that fails, fabricate a block from the table.

## Measured now: the draw loop has no DC and no root element in this tree

Before continuing with the front buffer, the current tree was measured against
the configuration this file calls verified. It does not reproduce.

Run: `press.ps1 -Boot 40 -After 90 -Extra '--guide_auto_press_seconds=45
--lle_xam_skin_init=true --guide_reuse_xui_ctx=true'`

```
GuideFrame 0:    dc=00000000 [134]=FFFFFFFF gpu_draws +0 (total 39)
GuideFrame 500:  dc=00000000 [134]=FFFFFFFF gpu_draws +0 (total 39)
...
GuideFrame 5500: dc=00000000 [134]=FFFFFFFF gpu_draws +0 (total 39)
```

The loop itself runs fine - 5500+ frames, which is the "runs continuously"
behaviour recorded earlier. What it does **not** have is anything to draw with.
`dc` is `[obj+0x0C]`, and the object dump from the same run reads:

```
Guide: obj 401EA600 head 00:913E1CB4 04:00000000 08:00000000 0C:00000000 10:00000001 ...
```

* `[obj+0x08] = 0` - **no root XUI element**. `XuiElementLayoutTree` is handed
  nothing.
* `[obj+0x0C] = 0` - **no device context**. `[dc+0x134]` reads as `FFFFFFFF`
  only because the emulator substitutes that sentinel when `dc` is null; it is
  not a real flag value.

`gpu_draws` stays at 39 for the entire run - the title's, not the Guide's.

This is **upstream of the front-buffer work**. `[dev+0x3F74]`, `81A0FE48` and
its parameter block are all downstream of having a DC at all; with `dc = 0` the
emitter is never reached, so none of that can be the active blocker in this
tree.

Two possibilities, not yet distinguished:

1. The tree has regressed since the "Verified working now" entries were
   written. The working copy has uncommitted edits to
   `xboxkrnl_video.cc` and `virtual_file_system.cc`.
2. Those entries depended on flags not recorded next to them. The header block
   names `lle_xam_skin_init` and `guide_reuse_xui_ctx`; both were passed here.

**Do not chase the front buffer further until `dc` is non-zero in a run.**
Establishing which of the two above applies - ideally by stashing the working
copy and re-running the same command - is the cheapest next step, and it is a
prerequisite for interpreting any front-buffer result.

### Red herring recorded so the next reader does not chase it

The module import dump marks `XuiSceneCreate`, `XuiRenderCreateDC` and other
XUI imports with `!!`. Per `user_module.cc:1121` that marker means
"not implemented", but the check is `GetModule(library)->
GetProcAddressByOrdinal(...)` evaluated when the dump is printed, which is
before LLE xam registers its exports. The same run logs
`Guide: HLE map empty (LLE xam owns the registration)`. The `!!` on XUI
imports is therefore expected and is not evidence of an unresolved import.

### Why `dc` is null: the title-thread bootstrap never runs

Follow-up to the section above, same runs.

`dc` is `[obj+0x0C]`, and it is filled by `RunGuideBootstrapOnTitleThread`
(`xboxkrnl_video.cc:770`). In the measured runs that function **never
executes**: zero `GuideBootstrap` lines in a 22,525-line log.

The dispatch chain is:

```
emulator.cc:2050   QueueGuideBootstrap(...)      -> guide_bs_pending_ = true
xboxkrnl_video.cc:2047  VdSwap_entry:
                     if (guide_bs_pending_.exchange(false))
                       RunGuideBootstrapOnTitleThread(bth)
```

The dispatch at `:2047` sits **outside** the sampled `SwapDraws` block, so it is
evaluated on every swap - it is not gated by any diagnostic cvar. Therefore the
failure is upstream: `guide_bs_pending_` is never set.

Consistent with that, `Guide button: queued XUI bootstrap` (`emulator.cc:2175`)
does not appear in the full-flag run either.

What *does* run is the `GuideFrame` loop in `emulator.cc:5379`. Per the
`guide_bootstrap_on_title_thread` cvar help, that is the **older fallback
sequence** that "predates the class registrars, the skin module and the scene
creator's out-pointer, so it gets an XUI init and nothing else". Its behaviour
matches exactly: XUI init returns `0`, `[obj+8]` and `[obj+0x0C]` stay null, and
6000 frames draw nothing.

So the observed state is the fallback path running while the real one does not,
even though every relevant cvar is set correctly:

```
guide_bootstrap_on_title_thread = true
guide_create_scene              = true
guide_scene_off_thread          = false
guide_init_only                 = false
```

**Not yet isolated:** why the queue call is not reached. Both call sites are in
`emulator.cc` (`:1753`, `:2050`), and the log line that follows the second one
is absent, so execution diverges before it. Note the baseline run
(`--guide_auto_press_seconds=45` alone) *did* log `queued XUI bootstrap` once
while the full-flag run did not - so one of `lle_xam_skin_init`,
`guide_reuse_xui_ctx`, `guide_trace_devsetup` or `guide_force_front_buffer`
suppresses it. `guide_trace_devsetup` is the prior suspect: this file already
records "guide_trace_devsetup makes the crash disappear, so it is
timing-dependent".

**Next probe:** bisect those four flags against the presence of the
`queued XUI bootstrap` line. That is four short runs and it isolates the
suppressor without reading any more disassembly.

Until the bootstrap runs, `dc` stays null, and no front-buffer work can be
evaluated.

## Root blocker found: the dashboard never presents

The bisect in the previous section led somewhere more fundamental. Measured by
screenshotting the emulator window directly (`PrintWindow` with
`PW_RENDERFULLCONTENT`, so occlusion cannot confound it) 35 s into a run:

**The dashboard renders a pure black frame.** The window title confirms the
title is loaded - `[FFFE07D1 v2.0.17489.0] Xbox 360 Dashboard <Direct3D 12 -
RTV/DSV - XAudio2>` - but nothing is drawn.

Alongside it, `SwapDraws` is **0** in every run measured, including a 90 s one.
That counter increments once per `VdSwap_entry` and logs every 200 swaps, so
zero lines means **fewer than 200 swaps in 90 seconds**. The dashboard is not
presenting.

### This is not caused by the uncommitted work

Tested by stashing `src/` back to `fb7fb20` and rebuilding:

| | working tree | clean HEAD |
|---|---|---|
| dashboard frame | black | black |
| `SwapDraws` | 0 | 0 |
| `queued XUI bootstrap` | 1 | 1 |
| `GuideBootstrap` | 0 | 0 |

Identical. The tree has been restored and rebuilt; this was a measurement, not
a change.

### Why this explains everything downstream

```
no VdSwap  ->  guide_bs_pending_ never consumed at xboxkrnl_video.cc:2047
           ->  RunGuideBootstrapOnTitleThread never runs
           ->  no scene creator, no XuiRenderCreateDC
           ->  [obj+8] = 0 and [obj+0x0C] = 0
           ->  the GuideFrame fallback loop draws nothing, forever
```

The bootstrap *is* queued (`queued XUI bootstrap` = 1 at HEAD and in the
working tree). It is never dispatched, because dispatch happens inside
`VdSwap_entry` and the title never gets there.

And more basically: **the Guide composites over the title's frame.** A title
that never presents cannot have anything composited over it. No amount of
front-buffer work can produce a visible Guide while this holds.

### Re-reading an earlier claim in this file

`SUMMARY.md` states "The dashboard is unaffected throughout (framebuffer
byte-identical to baseline)." That is consistent with what is measured here if
the baseline framebuffer was **also black** - "byte-identical" then means
identically empty, not identically correct. The claim was probably never
evidence that the dashboard was rendering.

Likewise, "the composite draw loop runs continuously to draw #2700+" describes
the `GuideFrame` loop in `emulator.cc`, which calls hud's draw entry directly
on a timer. It is not evidence of presentation either.

### What to do next

The question is no longer about the Guide. It is: **why does `dash.xex` not
present under this build?** Concrete starting points:

1. Whether `VdSwap_entry` is entered at all - add an unsampled first-call log
   rather than inferring from the 200-swap sampler.
2. The `GUEST CRASH` at `92193720` (`fault_addr 0000000100000000`, in
   `dash.xex`) that occurs at boot in every run. This file records it as
   survivable; that should be re-checked, because if it kills the render
   thread it is a complete explanation.
3. Whether the dashboard presents with the Guide machinery entirely off
   (no `--guide_*` flags at all) - separating "dash is broken here" from
   "the Guide flags break dash".

Item 3 is the cheapest and should come first.

### Controlled: it is dash.xex, not the Guide flags, and not the emulator

Item 3 from the list above, run:

| run | flags | frame | `SwapDraws` |
|---|---|---|---|
| dash, full Guide config | `guide_auto_press_seconds`, `lle_xam_skin_init`, `guide_reuse_xui_ctx` | black | 0 |
| dash, clean HEAD build | same | black | 0 |
| dash, **no guide flags at all** | `--break_on_debugbreak=false` only | black | 0 |
| **Sonic & All-Stars Racing Transformed (5345085D)** | none | **renders correctly** | logs normally |

The Sonic run is the control that was missing from all previous reasoning here:
the same binary, same GPU backend (`Direct3D 12 - RTV/DSV`), presenting a real
frame at 1280x720. So the emulator's presentation path is intact, and the Guide
cvars are not what stops the dashboard - **`dash.xex` alone does not present**.

That reframes the whole objective. Every entry in this file assumes the
dashboard is running and the Guide needs to composite over it. The Guide cannot
become visible over a title that draws nothing, so the Guide work is blocked on
a dashboard problem that is not a Guide problem.

Two ways forward, and they are genuinely different projects:

* **Fix dash.xex presentation.** Start with the boot-time `GUEST CRASH` at
  `92193720` and whether it kills the render thread, then whether
  `VdSwap_entry` is entered at all (unsampled log, not the 200-swap sampler).
* **Change the host title.** The Guide is designed to composite over *any*
  running title. Sonic demonstrably presents. Running the Guide over a title
  that actually draws would sidestep the dashboard entirely and test the
  Guide path against a live frame for the first time.

The second is cheap to try and does not depend on diagnosing dash at all.

## BREAKTHROUGH: run the Guide over a title that presents

Acting on the previous section's option 2. Launching the same build with
`5345085D.iso` (Sonic & All-Stars Racing Transformed) instead of `dash.xex`,
identical Guide flags:

| marker | over dash.xex | over Sonic |
|---|---|---|
| `SwapDraws` | 0 | 13 and climbing |
| `queued XUI bootstrap` | 1 | 1 |
| **`GuideBootstrap`** | **0** | **31** |
| **`GuideScene`** | **0** | **43** |
| scene creator | never ran | `913EB940 -> 00000000, scene=00010054` |
| DC | null | **`4089AAF0`** |

**The real bootstrap runs for the first time.** The scene creator returns
`S_OK` with a live scene handle, and the device context is non-null. Every
"verified working" claim in this file is reproducible again - just not over the
dashboard, which never presents.

### Two long-standing worries, both settled

```
GuideScene: GetBackBufferSize(dc 4089AAF0) -> 00000000; w=852 h=480 (raw 00000354 000001E0)
```

Returns `S_OK` with a **real 852x480**. The recorded fear that "our Guide device
is synthetic, so if it reports 0x0 the layout collapses" is refuted - the layout
has genuine dimensions.

```
GuideBootstrap: DC present gates: [11C]=00000000 [134]=00000001 [1CC]=40877E00
```

`[11C]=0` is the required state, but **`[134]=1`** - the null-render flag is
SET. Per this file, that makes `XuiRenderPresent` return `S_OK` without
presenting and `XuiRenderBegin` skip `vtable[20]`. That is precisely the
symptom recorded since the beginning: "a render path that executes completely
and emits nothing." `--guide_clear_null_render=true` clears it to `00000000`.

### Walking the crash chain forward

With the null-render flag cleared, the real draw path executes and the failure
moves - each time to a deeper, more specific site. Every step is a cvar that
already existed in this tree.

**1. `--guide_clear_null_render=true`** -> draw path now runs, faults at:

```
GUEST CRASH at 819DE94C  fault_addr 0x24  r3=0
unwind: 819DEB30 8191B024 818FDE60 818F8374 818FAEB8 913EAB4C
```

`913EAB4C` is hud's own draw, so this is inside the Guide's render path.
Decoded:

```
819DE938: lwz   r8,12960(r31)    ; [dev+0x32A0]  RT0
819DE944: bne   819DE94C         ; non-null -> use it
819DE948: lwz   r11,12976(r31)   ; else [dev+0x32B0]
819DE94C: lwz   r9,36(r11)       ; [r11+0x24]   <-- r11 = 0
```

Both render-target slots are null. Confirmed independently in the bootstrap
dump - **on both devices**:

```
xam device   408A2F00: [32A0]=00000000 [32B0]=00000000 [3F74]=00000000
title device 409B4780: [32A0]=00000000 [32B0]=00000000 [3F74]=A280A380
```

**2. `+ --guide_borrow_front_buffer=true`** -> front buffer lent
(`A280A380 -> guide device 40870D00`), `[134]=0`, same crash site.

**3. `+ --guide_bind_title_rt=true` (with `XENIA_PRESENT_RT=1`)** -> crash
moves to:

```
GUEST CRASH at 81A01638  fault_addr 0x4
```

`81A01638` is `0x80` into **`81A015B8`, the packet emitter**. Fault address `4`
matches this file's own prediction exactly: "on a mode-2 device the cursor is 0
so the first word stores to guest address 4."

**4. `+ --guide_bind_cmdbuf_kb=64`** -> the buffer is created:

```
Guide: cmdbuf init 81A01358(dev 40870D00, 30175000, 16384) -> 30174FFC;
       base=30175000 cursor=30174FFC limit=30184FFC
```

but the crash is **unchanged** - still `81A01638`, still guest address `4`. So
the emitter reads its cursor as 0 despite `40870D00` having a real one. The
cursor is being taken from a **different device object** than the one
`guide_bind_cmdbuf_kb` writes to.

### Where this leaves things

The Guide is not yet visible, but the pipeline is further along than anything
recorded in this file: bootstrap runs, scene builds with a handle, DC is live,
back-buffer size is real, null-render is off, front buffer and RT are bound,
and execution now reaches the **packet emitter** - the function that actually
builds `DRAW_INDX`.

**Immediate next step:** identify which device object `81A015B8` reads
`+0x2B4C` from at the crash, and point `guide_bind_cmdbuf_kb` at that one
instead of `40870D00`. The bootstrap dump already shows the *title* device has
a populated cursor (`+2B4C=E4FF7000 +2B50=0000061F +2B54=00001FFF`) while the
xam device's is all zero - so reading r31 at the fault would settle it in one
run.

**Standing correction to this file's premise:** the dashboard was never a valid
host for this work. Use a presenting title.

### The emitter crash resolved: `guide_bind_cmdbuf_kb` writes the wrong field

Traced the `81A01638` fault to its source. Registers at the crash:

```
PC 81A01638  fault_addr 0x4   r3=40870D00  r31=40870D00  r11=00000004  r30=7091F280
```

`81A015B8` (the emitter) does:

```
81A015C4: mr   r31,r3          ; device
81A015C8: mr   r30,r4          ; POINTER TO THE CALLER'S CURSOR
81A015D8: lwz  r11,0(r30)      ; r11 = caller's cursor
81A015DC: lwz  r10,11084(r31)  ; [dev+0x2B4C]
...
81A0162C: addi r11,r11,4
81A01638: stw  r10,0(r11)      ; <-- store through cursor+4
```

`r11 = 4` therefore `[r30] = 0`: it is the **caller's** cursor that is zero, not
the device field. `r30 = 7091F280` is a stack address, so the cursor is a local.

The caller, at `81A0A678`:

```
81A0A684: li   r4,2309
81A0A688: bl   0x81A042E0      ; RESERVE - returns the cursor in r3
81A0A68C: stw  r3,80(r1)       ; -> local at r1+0x50   (returned 0)
81A0A69C: addi r4,r1,80        ; &local
81A0A6A0: bl   0x81A015B8      ; emit with a null cursor
```

So the reserve `81A042E0(dev, 2309)` returns 0. Its body:

```
81A042F8: lwz    r11,52(r31)      ; limit  = [dev+0x34]
81A042FC: lwz    r10,48(r31)      ; cursor = [dev+0x30]
81A04300: cmpl   cr6,r10,r11
81A04304: ble    81A0430C         ; else assert (0FE00019)
81A0430C: lwz    r11,14848(r31)   ; [dev+0x3A00]
81A04310: cmpi   cr6,r11,0
81A04314: beq    81A0431C         ; must be zero, else assert
81A0431C: lwz    r11,48(r31)      ; cursor
81A04320: rlwinm r30,r29,2,0,29   ; bytes = count * 4
81A04324: lwz    r10,52(r31)      ; limit
81A04328: add    r11,r30,r11
81A0432C: cmpl   cr6,r11,r10
81A04330: ble    81A0439C         ; fits -> success path
81A04338: bl     81A03D40         ; else flush/grow -> fails, returns 0
```

**The command-buffer cursor and limit live at `[dev+0x30]` and `[dev+0x34]`.**
`guide_bind_cmdbuf_kb` calls `81A01358`, which writes `[dev+0x2B4C]` - a
different field that the reserve never reads. That is why binding a 64 KB
buffer changed nothing and the crash site was identical with and without it.

Note both are on the *same* device object (`r3 = r31 = 40870D00` at both the
reserve call and the emitter call), so this is not a wrong-device problem as
the previous entry guessed. It is a wrong-field problem.

**Concrete fix to try:** allocate a buffer and set

```
[dev+0x30] = base            (cursor)
[dev+0x34] = base + size     (limit)
[dev+0x3A00] = 0             (asserted zero by the reserve)
```

on the device the emitter uses (`40870D00`), rather than `[dev+0x2B4C]`. With
`cursor <= limit` and room for `2309 * 4 = 9236` bytes, `81A042E0` takes the
`ble 81A0439C` success path and returns a real cursor.

### The chain closes: it all returns to `81A0FE48`

The reserve's grow path `81A03D40`, taken whenever the request does not fit:

```
81A03D5C: bl     0x817F6C30       ; current-thread query
81A03D60: lwz    r11,11016(r31)   ; [dev+0x2B08] - owning thread
81A03D64: cmpl   cr6,r11,r3
81A03D68: beq    81A03D70         ; else assert - the device is thread-affine
81A03D70: lwz    r11,11024(r31)   ; [dev+0x2B10]   <-- the 128-byte block
81A03D74: cmpli  cr6,r11,0x0
81A03D78: beq    81A03D80
81A03D7C: lwz    r11,0(r11)       ; its first word - the progress counter
81A03D80: lwz    r11,14576(r31)   ; [dev+0x38F0]
```

`[dev+0x2B10]` is allocated **only** by `81A0FE48` (`81A0A290(0x80, 5, 2)` at
`81A0FEF4`, stored at `81A0FEF8`), and `81A0FE48` is called **only** from
`819F4E8C`, which `819F4D28` skips when its mode argument is 2.

So the complete causal chain, end to end:

```
mode 2 skips 81A0FE48
  -> [dev+0x2B10] is null, and [dev+0x30]/[dev+0x34] are never established
  -> 81A042E0 (reserve) finds no room: cursor 0, limit 0
  -> falls through to 81A03D40 (grow), which itself depends on [dev+0x2B10]
  -> grow provides no space
  -> reserve returns 0
  -> 81A015B8 (emitter) stores a packet word through cursor 0
  -> GUEST CRASH at 81A01638, guest address 4
```

Every symptom recorded in this file is one link in that chain, and they all
terminate at the same place: **`81A0FE48` does not run on the mode-2 device.**

That makes the earlier parameter-block analysis the root fix rather than a
detour. The requirement is unchanged from that section: synthesize the
124-byte block `8178E9F0` builds at `r1+0x90` (memset to zero, then ~13 fields,
`+0x00/+0x04` = width/height with a 640x480 fallback, `+0x08` = 0x28280186,
`+0x4C` = 4096, `+0x54` = 0x00010000, `+0x34` = 0x80000000, `+0x3C` = 1), pass
it as `81A0FE48`'s second argument, and call it on the Guide's device.

Two constraints now known that were not before:

* It must run **on the owning thread** - `81A03D40` asserts
  `[dev+0x2B08] == 817F6C30()`, so the whole sequence is thread-affine. The
  bootstrap already runs on the title thread, which is the right place.
* `[dev+0x3A00]` must be zero when the reserve runs (it asserts), and
  `[dev+0x30] <= [dev+0x34]`.

**Do not hand-write `[dev+0x30]`/`[dev+0x34]`.** They are outputs of whatever
`81A0FE48` sets up; writing them directly is the same one-field-at-a-time
approach that this file has already recorded as non-convergent, and the grow
path would still fault on the null `[dev+0x2B10]` the moment a second packet
needed space.

## `guide_force_front_buffer` is structurally dead: it runs after the draw

An unconditional one-shot probe was added immediately before the
`guide_force_front_buffer` block, reporting its three conditions
(`cvar`, `real_dev`, `[3F74]`). **The probe never logged.** Neither did the
block's own skip-warning. So the block is not failing a condition - the code is
never reached at all.

Cause, by line number in `xboxkrnl_video.cc`:

```
2237  if (guide_bind_cmdbuf_kb > 0)     <- setup, runs
2313  if (guide_bind_title_rt)          <- setup, runs ("block entered" logs)
...
2694  in_guide_draw_scope = true;
2695  Execute(..., guide_draw_fn_, ...)  <-- THE GUIDE DRAW
2697  in_guide_draw_scope = false;
...
2977  gdraws counter, diagnostics
3135  if (guide_force_front_buffer)     <- AFTER the draw
```

`guide_force_front_buffer` is meant to give the device the front buffer the
draw needs, but it is placed **after** the draw. The draw faults inside the
emitter, the guest thread dies, and execution never returns to line 3135. The
`gdraws` counter at 2977 never increments either, which is the same evidence
from the other direction.

So this cvar has never run in any configuration, and the crash previously
recorded against it (`81A04648`, the null-arg2 fault analysed earlier in this
file) must have come from a different invocation path, not from this block.

**Fix:** move the synthesis-and-call so it executes with the other setup cvars
before line 2694, not in the post-draw diagnostic region. The block now
contains a working synthesis of the 124-byte parameter block (zeroed, then
`+0x00`/`+0x04` = 640x480, `+0x08` = 0x28280186, `+0x34` = 0x80000000,
`+0x3C` = 1, `+0x4C` = 4096, `+0x54` = 0x00010000, `+0x68`/`+0x6C` = 640x480),
so once it is reachable it can be evaluated for the first time.

Note the ordering constraint discovered alongside this:
`guide_borrow_front_buffer` fills `[real_dev+0x3F74]`, and
`guide_force_front_buffer` is gated on that field being **null**. The two are
mutually exclusive by construction - enabling both silently disables the
second. Whichever survives the move should own the field exclusively.

## The setup runs on the wrong thread - xam says so itself

The `guide_force_front_buffer` work was moved to execute **before** the draw at
line 2694, alongside the other setup cvars, instead of in the unreachable
post-draw region. It now runs, and the gate reports exactly the state that was
wanted:

```
Guide: pre-draw front-buffer gate: dc=408AD390 wrap=40877E00 dev=40870D00
                                   [3F74]=00000000 [2B10]=00000000
```

`81A0FE48` is then entered for real - the JIT compiles it and its callees in
sequence, including `81A04570`, the function that faulted when the second
argument was null:

```
DemandFunction: enter 81A0FE48 / 81A0F5B8 / 81D10AAC / 81D10A9C / 81A0EFC8 / 81A04570
```

So the synthesized 124-byte block is accepted far enough to reach the routine
that previously crashed. What stops it is not the block:

```
(DbgPrint) WRN[D3D]: The current thread (0x16) is trying to use a D3D device
object that is owned by a different thread (0x6).
```

**The device is thread-affine and we are on the wrong thread.** Device
`40870D00` is owned by thread `0x6`; the notification path this code runs from
is thread `0x16`. The result line never prints because the call does not
complete.

This is the same constraint already visible in two places and not connected
until now:

* `RunGuideBootstrapOnTitleThread`'s own header comment - "doing it from the
  Guide's own thread makes the guest D3D runtime refuse with *trying to use a
  D3D device object that is owned by a different thread*".
* `81A03D40` (the command-buffer grow path) asserts
  `[dev+0x2B08] == 817F6C30()`, i.e. the caller must be the owning thread.
  `[dev+0x2B08]` is where that owner is recorded.

### What this means

Two possibilities, and they need separating before any more device work:

1. The Guide draw hook is not running on the thread that owns the Guide's
   device. If so, every device-touching operation in this file - the RT bind,
   the cursor bind, the front-buffer lend - has been issued from the wrong
   thread and may have been silently refused the same way. That would explain
   why so many of them "ran" and changed nothing.
2. The device is owned by a thread that no longer matches because it was
   created during a different phase.

**Cheapest next probe:** log `[dev+0x2B08]` next to `817F6C30()` at the top of
the draw hook, and compare with the thread the hook actually runs on. That
single line distinguishes the two and tells us which thread the setup has to
be marshalled onto.

Note the guest's own diagnostic named this in one line after weeks of
inference - the same lesson recorded earlier in this file about trap type 25:
**the guest is often the best instrument.**

### Confirmed by direct read: owner 6, caller 0x16

The probe recommended above, run:

```
Guide: pre-draw front-buffer gate: dc=408AD390 wrap=40877E00 dev=40870D00
       [3F74]=00000000 [2B10]=00000000 owner[2B08]=00000006
       817F6C30()=00000016 match=NO
```

`[dev+0x2B08]` is `6`; xam's current-thread query returns `0x16`. That matches
the D3D warning digit for digit, and it is read directly rather than inferred.

Where the mismatch comes from:

* The device the **draw** uses is `40870D00`, reached as
  `[[guide_draw_this_+12]+0x1CC]+0x0C`. All the Guide's device work - the RT
  bind, the cursor bind, the front-buffer lend, and now `81A0FE48` - is issued
  against it from the notification/draw path, which is xam thread `0x16`.
* `guide_create_xam_device` (`emulator.cc:1756`) runs on the **Guide button
  dispatch thread**, logged as `01000028`:

```
01000028 Guide button: xam CreateDevice returned 00000000, device now 408A2F00
```

Note that is a *different* device object (`408A2F00`, not `40870D00`), so there
are at least two, and the one on the draw path was created on the dispatch
thread rather than the title's render thread. xam records the creating thread
in `[dev+0x2B08]` and refuses device calls from anywhere else.

### Consequence for everything already recorded in this file

Every device-touching experiment in this project has been issued from thread
`0x16` against a device owned by thread `6`. xam's refusal is a `DbgPrint`
warning, not a failure code, so those calls returned without doing anything and
without a visible error. That is a strong candidate explanation for the long
run of changes that "ran" and changed nothing - and it means results recorded
against them should not be trusted until re-measured.

### Next

Two options, in order of principle:

1. **Create the Guide's device on the title's render thread.** The bootstrap
   already runs there (`RunGuideBootstrapOnTitleThread`, thread `F80000A4` /
   xam `0x16`), so moving the `8178F748` call into it should make the created
   device owned by the thread that later draws with it. This is the fix that
   matches how the device is meant to be used.
2. **Transfer ownership** by writing the current thread into `[dev+0x2B08]`
   before the setup call. Cheap to test and would confirm the diagnosis in one
   run, but it forges a field xam maintains, so treat it as an experiment
   rather than a fix.

Do 2 first as a one-run confirmation, then 1 as the actual change.

## The ownership check is real, and forging it lets 81A0FE48 run

The check that produces the "owned by a different thread" warning is at
`819F4438`, found by locating the format string (`8165DAC8`) and its single
load site (`819F4464`):

```
819F4444: lwz    r11,11016(r31)   ; [dev+0x2B08]
819F4448: cmpli  cr6,r11,0x0
819F444C: beq    819F4478         ; zero -> check skipped entirely
819F4450: rlwinm r31,r11,0,0,31
819F4454: bl     0x817F6C30       ; current thread
819F4458: cmpl   cr6,r31,r3
819F445C: beq    819F447C         ; match -> proceed
819F4460: bl     0x817F6C30       ; else warn
```

So `[dev+0x2B08]` *is* the field, confirming the previous section. Writing the
calling thread into it before the setup call (`00000006 -> 00000016`) gets past
the gate, and `81A0FE48` then executes far deeper than ever before.

### xam validates the synthesized block and names its faults

With the thread gate passed, xam's own D3D validator starts reporting on the
124-byte block we hand it:

```
WRN[D3D]: Titles are required to use a minimum frame buffer of 1280x720 when
          the console is using an HD display mode. The current frame buffer
          size request will not pass certification.
WRN[D3D]: Validate: The resource's type doesn't match the type expected by the
          API.  Expected type Surface, got type .
WRN[D3D]: Validate: Invalid 'Fence' field (bad time value)
WRN[D3D]: Validate: The previously set resource has to be 'unset' by having its
          fence field updated. ...
```

The first was **our own choice**: `8178E9F0`'s fallback branch writes 640x480
(`li r10,640` / `li r11,480`), and that was copied literally. But that branch is
the SD path; the emulator reports an HD mode. Setting the block to **1280x720**
removes the complaint entirely - confirmed in the following run.

The remaining three are about fields left zero by the synthesis. "Expected type
Surface, got type ." says a slot that must hold a surface object is empty; the
`Fence` messages are the same object's lifetime fields. The candidates are
exactly the fields the field map lists as caller-sourced rather than constant:
`+0x40`, `+0x44`, `+0x60`, `+0x64`, `+0x70`, `+0x74`. Note branch A of
`8178E9F0` copies **16 bytes from `[r8]`** into `+0x60..+0x6C`
(`8178EAB8`-`8178EADC`), which is the right shape for a surface descriptor.

`81A0FE48` still does not return, so the setup is not complete - but it is now
failing on *content* that xam describes in words, rather than on a null pointer
or a thread gate.

### Status of the two options from the previous section

* Option 2 (forge `[dev+0x2B08]`) - **done, and it works as a probe.** It is in
  the tree, clearly commented as an experiment. It should be replaced by
  option 1 once the block is right; forging a field xam maintains is not a fix.
* Option 1 (create the device on the title's render thread) - not yet done.

### Next

Fill `+0x60..+0x6C` with a real surface. The title's device has one
(`[title_dev+0x3F74] = A280A380` was lent successfully in an earlier run), and
`8178E9F0` sources those 16 bytes from a caller-provided descriptor, so the
shape to copy is available. Then re-read the validator output - it has been
more informative in two runs than weeks of inference, which is the same lesson
this file already records twice.

## Ground truth: the real parameter block, dumped

Rather than inferring the block field-by-field from validator complaints, the
`819F4D28` breakpoint now dumps 124 bytes at `arg5` when it is non-zero. One
`--guide_create_primary_device` run over Sonic gives the real thing:

```
DevCreateBlock @7113F190:
00:00000280 04:000001E0 08:28280186 0C..30:0
34:80000000 38:00000000 3C:00000001 40:24900106 44:00000000 48:00000000
4C:00001000 50:00000000 54:00010000 58:00000000 5C:00000000
60:00000000 64:00000000 68:00000280 6C:000001E0
70:00000780 74:00000438 78:00000000
```

Three corrections to the synthesized version:

| field | synthesized | real | note |
|---|---|---|---|
| `+0x00`/`+0x04` | 1280x720 | **640x480** | the earlier change was wrong |
| `+0x40` | 0 | **`24900106`** | was missed entirely |
| `+0x70`/`+0x74` | 0 | **1920x1080** | was missed entirely |

**640x480 was right all along.** The
"minimum frame buffer of 1280x720" validator line is a *certification
advisory*, not a rejection - the genuine mode-1 block is 640x480 and triggers
it too. Changing the synthesis to 1280x720 on the strength of that message was
a mis-read of a warning as an error.

Also settled: `+0x60`/`+0x64` are **zero** in the real block, so the earlier
theory that they hold a surface descriptor ("Expected type Surface") is wrong.
The synthesis now reproduces every non-zero word verbatim.

## `guide_bind_title_rt` is actively harmful

Single-variable test, everything else held constant:

| configuration | `WRN[D3D]` lines |
|---|---|
| with `--guide_bind_title_rt` (+`XENIA_PRESENT_RT`) | **5** |
| without it | **1** |

The three "Validate:" complaints - wrong resource type, invalid `Fence` field,
previously-set resource not unset - come **from that cvar**, not from the
parameter block. Dropping it leaves only the harmless 1280x720 advisory.

That matches the warning already in its own help text: it binds RT0 on an
object reached via `[dc+0x1CC]`, which is the 140-byte *wrapper*, so `+0x32A0`
lands ~12 KB past the end of it. The `[32A0]=00000060` seen in the pre-draw
diagnostic is that same out-of-bounds read, not a render target.

**Recommendation: leave `guide_bind_title_rt` off.** It has been part of the
"working" flag set in several earlier measurements, and it was corrupting them.

## Where it stops now

With the correct block and `guide_bind_title_rt` off, one crash remains:

```
GUEST CRASH at 819F5EC4  fault_addr 0x20
819F5EC4: lwz r11,32(r14)          ; [r14+0x20], r14 null
unwind: 819F7FB4 819FEC68 8191B438 818F930C 818FB020 913EABC4
```

`913EABC4` is hud's draw, so this is the draw path, not `81A0FE48`.

**This is a fault this file already predicted.** The `guide_bind_depth_copy`
cvar help describes it exactly: "the post-redirect fault is an unguarded read
of `[r14+20]`, the low 6 bits of which are compared against 0x3D by a caller -
the shape of a surface format field - and the device never gets a depth surface
because 819F38C8 is never called in any run."

The next instruction confirms the reading:

```
819F5EC8: rlwinm r3,r11,0,26,31    ; low 6 bits of [r14+0x20]
819F5ECC: bl     0x819F4FE0
```

So the device needs a **depth surface**. `guide_bind_depth_copy` exists to
supply one and has never been tried in a configuration that got this far.

### Next

Run with `--guide_bind_depth_copy=true`, correct block, `guide_bind_title_rt`
off. That is the one untested cvar sitting directly on the current fault.

### Correction to the table above, and a re-attribution

The "5 vs 1" figures in the previous section were wrong: `5` was a count of
*thread-ownership* warnings and `1` a count of `WRN[D3D]` lines - two different
metrics compared against each other. A clean A/B, everything else held
constant, counting both the same way in both runs:

| configuration | `WRN[D3D]` | "owned by a different thread" | crashes |
|---|---:|---:|---:|
| **with** `guide_bind_title_rt` (+`XENIA_PRESENT_RT`) | **11** | **5** | 1 |
| **without** | **1** | **0** | 1 |

The conclusion stands and is stronger than recorded - 11 versus 1, not 5 versus
1 - but one attribution changes:

**The thread-ownership warnings come from `guide_bind_title_rt`, not from the
`81A0FE48` setup call.** With that cvar off they disappear entirely (5 -> 0),
even though the ownership forge still runs. So the earlier reading that our
setup call was being refused for thread reasons was measuring that cvar's
calls, not ours.

That also means the forge of `[dev+0x2B08]` has **not** been shown to be
necessary. It should be re-tested on its own now that the noise source is
known: if `81A0FE48` behaves identically with the forge removed, remove it -
it writes a field xam maintains and was only ever justified as a probe.

Also note the pre-draw `guide_bind_title_rt` block at line 2313 does resolve
`rdev = [[dc+0x1CC]+0x0C]`, i.e. the real device rather than the wrapper, so
the "binds into the wrapper" explanation offered for it above applies to the
*other*, post-draw block and not to this one. Why the correct block still
produces ten extra validator complaints is not established.

### `guide_bind_depth_copy` cannot fire in this configuration

Tested with the correct block and `guide_bind_title_rt` off: no depth-copy log
line, crash unchanged at `819F5EC4`.

Its own condition explains it - it clones RT0 "if the device has a colour
surface on RT0 but nothing on the depth slot". With `guide_bind_title_rt` off
there is no colour surface on RT0, so it does nothing. The two cvars are
therefore coupled: the depth fix needs the RT bind, and the RT bind brings ten
validator complaints with it.

Resolving that coupling - a correct RT0 bind that does not upset the validator -
is the next real step, and it is a prerequisite for testing the depth surface
at all.
