# Running the Xbox 360 Guide (`hud.xex`) under Xenia's JIT — Phase 1 Research

Date: 2026-08-23
Sources: `F:\FuzionFrenzy` (Xbox BC-on-PC package), `xenia-canary` @ HEAD (cloned to `./xenia-canary`)

---

## 1. What we have

### The Guide binaries (`F:\FuzionFrenzy\Flash\`)

| File | Size | Role |
|---|---:|---|
| `xboxkrnlcf.bin` | 1,507,328 | Real PPC LLE kernel |
| `xam.xex` | 2,433,024 | BC-stripped xam |
| `hud.xex` | 122,880 | **The Guide** |
| `huduiskin.xex` | 81,920 | Guide UI skin / resources |
| `ximecore.xex` | 94,208 | On-screen keyboard |
| `xenonjklatin.xtt` | 1,740,800 | Guide font |

### Critical finding: **the XEXs are not encrypted**

```
hud.xex   encryption = none   compression = LZX   (Devkit, DLL module)
xam.xex   encryption = none   compression = LZX
```

No keyvault, no PIRS unlock, no AES session key. `xextool -b` decompresses them
directly. Basefiles extracted to `work/hud.pe` (335,872 B) and `work/xam.pe`
(6,356,992 B).

### Module identity

| Module | Load addr | Entry | Version | Built |
|---|---|---|---|---|
| `hud.xex` (`hud.dll`) | `0x913E0000` | `0x913FA418` | v2.0.**17003**.0 | 2020-11-16 |
| `xam.xex` | `0x815F0000` | — | v2.0.17003.0 | — |
| `xboxkrnlcf.bin` | `0x80040000` | RVA `0x799E0` | `xboxkrnl.exe@17003.0+1861.0` | — |

`xboxkrnlcf.bin` is a genuine PowerPC BE PE (machine `0x01F2`), 931 ordinal-only
exports, 10 sections including a 12 KB `.pdata` (PPC unwind data — usable for
automatic function boundary discovery).

`hud.xex` bounding path is `\Device\Flash\hud.xex`, allowed media = System Flash.
Static libs link `XBOXKRNL v2.0.17003.0` and `XNETY`.

---

## 2. The actual import surface

The XEX header import counts (472 / 43 / 912) double-count each import — there is
a pointer slot *and* a thunk per function. The real unique-function counts are:

| Consumer | Provider | Unique functions |
|---|---|---:|
| `hud.xex` | `xam.xex` | **236** |
| `hud.xex` | `xboxkrnl.exe` | **21** |
| `xam.xex` | `xboxkrnl.exe` | **445** |

Every single ordinal `hud.xex` imports is already **named** in Xenia's
`xam_table.inc` / `xboxkrnl_table.inc`. There are zero unknown ordinals. That is
a significant head start — no ordinal archaeology needed.

## 3. Xenia canary's current coverage

| Table | Ordinals known | Implemented | of which stubs |
|---|---:|---:|---:|
| `xboxkrnl` | 922 | 300 | — |
| `xam` | 1736 | 354 | — |

---

## 4. The two routes, measured

### Route A — keep Xenia's HLE xam, load only `hud.xex`

Requires satisfying 236 xam + 21 kernel functions.

**hud.xex → xam.xex (236)**

| Status in Xenia | Count |
|---|---:|
| Implemented | 52 |
| Stub | 14 |
| Sketchy | 5 |
| **Missing entirely** | **165** |

**hud.xex → xboxkrnl (21)** — 14 implemented, 1 stub, 1 sketchy, **5 missing**
(`HalPowerDownToBackgroundMode`, `ExExpansionCall`, `EtxProducerLog`,
`EtxProducerRegister`, `EtxProducerUnregister` — all trivially stubbable).

Breakdown of the 165 missing xam functions:

| Family | Count | Nature |
|---|---:|---|
| **XUI framework** (`Xui*`, `XUI*`) | **81** | Scene graph, timelines, animation, renderer, `.xur` resources, class registry |
| Xam Guide surface (`Xam*`) | 68 | `XamShow*UI`, `XamPlayTimer*`, `XamTask*`, `XamApp*`, `XamLoader*`, `XamHud*` |
| CRT / misc | 12 | `XMemCpy`, `XMemSet`, `MultiByteToWideChar`, `ResetEvent`, `SetLastError`, `GetLocalTime`, `FileTimeToLocalFileTime`, `SystemTimeToFileTime`, `XGetOverlappedResult`, `XamAllocSize` — all easy |
| `GamerCard*` | 4 | Startup/cleanup/control registration |

The headline is the 81 XUI entries. That is not 81 functions of work — XUI is the
360's entire retained-mode UI toolkit. Reimplementing it means a scene graph, a
timeline/animation engine, `.xur` binary resource parsing, focus/navigation
semantics, and a renderer, all behaving closely enough that the Guide's own
layouts drive correctly. This is the expensive path.

### Route B — LLE the real `xam.xex` on top of Xenia's kernel

Requires satisfying 445 kernel functions.

| Status in Xenia | Count |
|---|---:|
| Implemented | 188 |
| Stub | 36 |
| Sketchy | 6 |
| **Missing entirely** | **215** |

215 > 170, so on raw count Route B looks worse. It is not, and this is the key
result of the research. Break the 215 down by subsystem:

| Prefix | Count | Comment |
|---|---:|---|
| `Xe*` | 42 | GPU / Xenos |
| `X*` (misc) | 40 | assorted |
| `Hal*` | 13 | hardware abstraction |
| `Xex*` | 12 | module loader |
| `Nic*` | 10 | network interface |
| `Etx*` | 9 | event tracing — stub to `S_OK` |
| `Vd*` | 8 | video driver |
| `Mtpd*` | 8 | media transfer protocol |
| `Ke*` | 8 | core scheduler |
| `Io*` | 8 | I/O manager |
| `Drv*` | 8 | drivers |
| `Usbd*` | 5 | USB |
| `Rtl*` | 5 | runtime library |
| `Mm*` | 5 | memory manager |
| `Nt*` | 4 | native API |
| `Ani*`, `L*`, `Ob*`, `Dump*`, `Stfs*` | 16 | misc |

The overwhelming majority is peripheral hardware (`Nic`, `Vd`, `Mtpd`, `Drv`,
`Usbd`, `Ani`, `Dump`, `Stfs`, most of `Hal` and `Xe`) that `xam.xex` merely links
against and that the Guide code path is unlikely to ever call. Those can be
stubbed and left to assert if hit.

**The core kernel — the part that must actually work — is only 27 functions short:**

```
ExTerminateTitleProcess      MmDoubleMapMemory            MmUnmapMemory
KeCancelTimer                KeConnectInterrupt           KeInitializeInterrupt
KeSetPowerMode               KeSetPriorityClassThread     KeSetTimer
KeSetTimerEx                 MmPersistPhysicalMemoryAllocation
NtCreateDirectoryObject      NtDeleteFile                 NtSetSystemTime
ObGetWaitableObject          ObInsertObject               ObTranslateSymbolicLink
RtlAppendStringToString      RtlCaptureContext            RtlUnicodeToUtf8
RtlUnicodeToUtf8Size         RtlUnwind                    NtCancelIoFileEx
MmGetPoolPagesType           ExExpansionCall              KeCallAndWaitForDpcRoutine
MmResetLowestAvailablePages
```

Every one of these is a well-understood NT-derived primitive. `KeSetTimer`,
`RtlUnwind`, `ObInsertObject`, `RtlUnicodeToUtf8` are days of work, not months.

### Recommendation

**Route B.** Implementing ~27 core kernel primitives plus a large but mechanical
stub layer buys you the *real* XUI implementation, running as real PPC code under
the JIT, with exactly correct behaviour. Route A's 81 XUI entries are a from-
scratch reimplementation of a UI toolkit whose semantics are undocumented, and
any inaccuracy shows up as a Guide that lays out wrong or navigates wrong.

Route B also matches what the BC team themselves did: their stack loads the real
`xam.xex` and `hud.xex` against the real `xboxkrnlcf.bin`. We have all three.

---

## 5. Prior art found on E:

- `E:\re\dashproj\dash17559` — Ghidra project for a **17559** dash
- `E:\tmp\ppc_recomp*.cpp`, `ppc_recomp_shared.h` — XenonRecomp-style **static
  recompilation** output, image base `0x9217E000`. A previous attempt via static
  recomp rather than JIT.
- `E:\tmp\xamnames.json` — ordinal→name map, already built
- `E:\xeBuild\17489\17489-fs\` — **complete** retail dashboard set (`dash.xex`,
  `hud.xex`, `xam.xex`, `xshell.xex`, `xbdm.xex`, fonts, `xlaunch.fdf`)

Note: `E:\xeBuild\17559\` is launcher-only and contains no dashboard XEXs. **17489
is the newest complete dashboard set on disk**, not 17559.

## 6. Environment constraints

- **No compilers installed** — no `cl`, `gcc`, `clang`, `cmake`, `ninja`, or
  `dotnet`. Building Xenia requires installing a toolchain first.
- **The Bash sandbox has no network.** Only PowerShell can reach the internet.
- `xextool v6.6` recovered from `E:\WinDirBackup\...` → `./tools/xextool.exe`

## 7. Artifacts produced

| Path | Contents |
|---|---|
| `research/hud_xam_gap.tsv` | 236 rows: ordinal, name, Xenia status |
| `research/hud_krnl_gap.tsv` | 21 rows |
| `research/xam_krnl_gap.tsv` | 445 rows |
| `research/pe.ps1` | PPC PE header/export dumper |
| `research/xex.ps1` | XEX2 header / import-library dumper |
| `work/hud.pe`, `work/xam.pe` | Decompressed basefiles |
| `work/*.idc` | xextool IDA scripts with per-ordinal import addresses |

## 8. Next steps

1. Install a build toolchain (MSVC Build Tools + CMake) — currently blocking.
2. Build xenia-canary unmodified; confirm it runs.
3. Add a module-load path for `xboxkrnlcf.bin` + `xam.xex` from a `Flash\` root.
4. Implement the 27 core kernel functions; auto-generate asserting stubs for the
   remaining 188.
5. Boot `hud.xex` and iterate on whatever actually gets called.

---

# Phase 2 — Toolchain and first boot attempt

## Build environment (all captured in `build.cmd`)

Five prerequisites were missing. In the order they surfaced:

1. **MSVC** — the "Build Tools 2026" install (`18.9.12112.369`) had no `VC`
   directory at all. A **Build Tools 2022** instance (`17.14.37614.0`, MSVC
   14.44.35207) provided the actual compiler.
2. **Windows SDK not on PATH** — `vcvars64.bat` silently failed to locate it
   because `vswhere.exe` was not on PATH. Fixed by prepending
   `...\Microsoft Visual Studio\Installer` and pinning SDK `10.0.26100.0`.
3. **Python 3** — hard `find_package(Python3)` requirement. Installed 3.12.10.
4. **`version.h`** — generated by `xenia-build.py`, which the CMake preset path
   never invokes (and which itself dies on the same missing-`vswhere` issue).
   Written directly from `git rev-parse` into `build/version.h`.
5. **Vulkan SDK** — `glslangValidator` needed by `compile_shader_spirv.py`.
   Installed 1.4.357.0; `VULKAN_SDK` + `%VULKAN_SDK%\Bin` exported.

Result: `build/bin/Windows/Release/xenia_canary.exe`, branch
`canary_experimental` @ `9090088`. CMake 4.4.2, Ninja 1.13.2.

## First boot attempt

```
xenia_canary.exe --headless=true testroot\Flash\hud.xex
-> Loading module GAME:\hud.xex
-> Failed to load user module ...\hud.xex
-> Failed to launch target: C00000BB      (X_STATUS_NOT_SUPPORTED)
```

Cause is `emulator.cc:1522`:

```cpp
if (!module->is_executable()) {
  kernel_state_->UnloadUserModule(module, false);
  return X_STATUS_NOT_SUPPORTED;
}
```

**Both `hud.xex` and `xam.xex` are DLL modules, not titles.** Confirmed via
xextool:

| Module | PE name | Flags |
|---|---|---|
| `hud.xex` | `hud.dll` | Devkit, Compressed, Not-Encrypted, **DLL Module** |
| `xam.xex` | `xamc.dll` | Devkit, Compressed, Not-Encrypted, **DLL Module**, Title Exports |

(`xamc` = the BC/compatibility xam variant.)

This is correct emulator behaviour, not a bug — the Guide is never booted, it is
loaded *by* xam. Xenia has no path to load a DLL module standalone:
`load_module_map` is a JIT **symbol map** (`XexModule::ReadMap`), not a loader,
and `launch_module` only rewrites a path inside an already-mounted container.

## Conclusion of Phase 2

Everything that can be learned without modifying Xenia has been learned. The next
step requires a patch, and it is small and well-defined:

1. Add a host-side bootstrap that loads `xam.xex` as a `UserModule` (not a
   `KernelModule`), then loads `hud.xex` against it.
2. Suppress `LOAD_KERNEL_MODULE(xam::XamModule)` in `emulator.cc:317` when that
   mode is active, so `IsKernelModule("xam.xex")` returns false and
   `user_module.cc:1068` takes the guest-module branch.
3. Bypass the `is_executable()` gate for this bootstrap path.

Only after that does the 27-function core kernel work begin.

---

# Phase 3 — Dashboard boot and xam heap reverse engineering

## What runs now

`dash.xex` (17489, title `FFFE07D1`) and `hud.xex` (17003) both execute real
PowerPC under Xenia's JIT. The real `xam.xex` initializes, commits heaps,
spawns 8 worker threads, and creates named sync objects. Zero undefined
kernel externs remain.

Commits on branch `guide-lle`:

| Commit | Change |
|---|---|
| `6375208` | `allow_dll_module_launch` — boot DLL XEXs |
| `968d2d2` | `lle_xam` — bind guest imports to real xam |
| `cd1e74a` | DllMain on a guest bootstrap thread (3 defects) |
| `cafb02e` | `lle_show_guide` — call XamShowGuideUI |
| `f7b290b` | xam init before title start + 6 kernel exports |
| `4d329a0` | 13 more kernel exports |
| `8af1573` | Title workspace placed at image end |

## Ghidra environment

Ghidra 12.1.2 headless, JDK 21 (both on `E:\re`). Two obstacles:
`analyzeHeadless.bat` cannot handle the `&` in the project path (use a clean
path), and Ghidra 12 uses PyGhidra, not Jython — scripts must be Java.

Default analysis covered only ~19% of `.text`. Recovery sequence:
1. Parse `.pdata` (PPC unwind table) → +18,385 functions
2. Linear disassembly of all `.text` → **1,489,733 instructions**

Scripts live in the scratchpad `scripts/` dir. Reusable.

## xam heap architecture (17489)

Heap descriptor array: **base `0x81D4E1B0`, stride `0x198` bytes**, 10 entries.

Init function `0x817bcaf0` calls `0x817bb780(index, enable, size, r6, r7, r8)`
with hardcoded constants:

| Heap | enable | size | r6 | Committed |
|---:|---:|---:|---:|---|
| 0 | **0** | **0** | 0 | no — placeholder, named `UNINITIALIZED` |
| 1 | 1 | `0x1F0000` | 0 | yes → `40000000` |
| 2 | 1 | `0x200000` | 0 | yes → `401F0000` |
| 3 | 1 | `0x1B0000` | 0 | yes → `403F0000` |
| 4 | 1 | `0x330000` | 0 | yes → `405A0000` |
| 5 | 1 | `0x20000` | 0 | yes → `408D0000` |
| 6 | 1 | `0x20000` | **1** | no — different path (physical) |
| 7 | 1 | `0x20000` | 0 | yes → `408F0000` |
| 8 | 1 | `0x40000` | 0 | yes → `40910000` |
| 9 | 1 | `0x60000` | 0 | no — gated on `0x8174ef20` |

`0x817bb780` skips commit when `enable == 0` (`cmpwi r26,0` at `817bb818`).
Commit is `0x817b99f0` → `NtAllocateVirtualMemory` with alloc type
`0x40803000` / `0x60803000` (RESERVE|COMMIT|NOZERO|LARGE_PAGES) and
protect `4`. **All seven allocations succeed.**

## Heap id resolution — `0x817baf38`

```
(id & 0xFFFFFF00) == 0xFFFFFF00 -> per-app heap, only app 0xF2 handled
                                   (lookup 0x817863f8; app->[0x88]/[0x8c])
id < 0xA                        -> &heap[id]
id == 0xCCCC                    -> twi (assert) - uninitialized memory
else                            -> NULL
```

Allocator `0x817bc430`: resolves the heap, and on NULL asserts, writes
`*out = 0` and returns `0x8000FFFF`.

## The actual failure

xam's heap system is **working as designed**. The failures are:

- allocations against **heap 0**, the deliberately zero-sized placeholder
- one report of heap id **`0xCCCC`** — a field read before initialization

Both mean xam state that the real 17489 `xboxkrnl` would have set up is
absent under Xenia's HLE kernel. This is not a missing export and not a
memory shortage; it is kernel/xam initialization contract divergence.

Downstream symptoms, all one root cause:
- 91 × `Out of memory allocating N bytes from heap 0 (UNINITIALIZED)`
- empty in-process app table
- `XMsgInProcessCall() failed to find app id 0xFC (XLiveBase)` → fault at `0x81759E68`
- `XamShowGuideUI` → `0x0E` (`ERROR_OUTOFMEMORY`)

## Assessment

Nothing renders yet. Remaining: xam/kernel init contract, the entire video
path (`VdInitializeEngines` is never reached), ~200 unimplemented kernel
exports, input/audio/profile. This is multi-week reverse engineering, not a
short task.

---

# Phase 4 — Dashboard rendering investigation (HLE xam)

## The route change that mattered

Phase 1 chose Route B (LLE xam) because it is *faithful*. For the goal of
getting pixels on screen that was the wrong optimisation: **`dash.xex` runs
far better on Xenia's own HLE xam**, which is designed against Xenia's
kernel. This was never tested until late, because the LLE decision had
already been made.

## What the dashboard does now

`dash.xex` (17489, title `FFFE07D1`) under HLE xam:

- boots with **zero undefined externs**
- completes D3D init: `VdInitializeEngines`, `VdInitializeRingBuffer`,
  `VdEnableRingBufferRPtrWriteBack`, `VdSetGraphicsInterruptCallback`
- runs its main loop at ~60 fps (2364 iterations in 40 s)
- polls `XamInputGetKeystrokeEx`, `XamReadTile`, `XUsbcamGetState`,
  `XamNuiIsDeviceReady`, `XMsgInProcessCall(0xFC, 0x58003)` each frame
- **loads all of its embedded UI resources successfully**
- receives **2500+ graphics interrupts** (vblank source=0, CP source=1)
- creates 10 graphics pipelines from translated Xenos shaders
- uploads a 1280x720 RGBA texture per frame, plus 256x256 `k_8` font atlases
- **never calls `VdSwap`** — nothing is presented, window stays black

## The dashboard is Lua-scripted

A 12-byte-stride registration table at `0x9295c380`-`0x9295c4b8` in
dash.xex, entries `{func_ptr, 0xffffffff, name_ptr}`:

```
NavigateBack  NavigateHome  NavigateSearch  NavigateToURI
NavigateToXuiScene  PushBackURI  CreateDashRoot  CreateDashChannel
CreateDashSlot  CreateSlotContainer  SendVuiActivateMsg
LoadLuaProvider  WaitForPendingLoads  LoadComponent
ExecuteEpixCommand  GetCookie  SetCookie  DeleteCookie
GetLaunchParam  GetSessionId  GetCurrentUserIndex
IsInvokedByDash  IsStagingMode  GetDashLaunchURI  RebootToDash  Terminate
```

`LoadLuaProvider` + `ExecuteEpixCommand` + the string
`Provider=Epix;IsBootApp=true;` confirm the NXE UI is Lua/Epix driven.

## Embedded resources (36 total, all load OK)

`xextool -d` extracts them; `XexGetModuleSection` returns success for each:

| Section | Size | Section | Size |
|---|---:|---|---:|
| `dashcomm` | 994,473 | `consoles` | 973,226 |
| `controlp` | 487,943 | `network` | 353,998 |
| `hubapp` | 197,266 | `dashlua` | 149,634 |
| `epix` | 131,795 | `contui` | 107,358 |
| `luaxbox` | 77,654 | `dashuisk` | 75,760 |
| `hubui` | 10,004 | `dashmain` | 3,661 |
| `shrdres` (from xam) | 237,951 | | |

## Ruled out with evidence

| Hypothesis | Verdict |
|---|---|
| Missing kernel/xam exports | No — zero undefined externs |
| `VdRetrainEDRAM` return value | No — `VdInitializeEngines` fires either way |
| `VdRetrainEDRAM` out-params | No — zeroing them regressed GPU activity |
| Wrong video mode | No — reports valid 1280x720/60/widescreen |
| Missing `DASHUSER:` device | Real bug, fixed, but not the gate |
| Missing UI resources | No — all 36 load successfully |
| Vblank interrupts not firing | No — 2500+ dispatches confirmed |
| `VdSwap` guarded by a condition | No — both branches reach the call |

The remaining gap is inside dash's own Lua/Epix UI layer: everything Xenia
supplies is working, but the UI never produces draw content beyond the
initial 10 pipelines.

## The Xbox Guide button

- `X_INPUT_GAMEPAD_GUIDE = 0x0400`; bound to `0x08` (Backspace) in
  `winkey_binding_table.inc`; `guide_button` cvar defaults on.
- **`ImGuiDrawer::SetGuideButtonAction` has zero callers** — the host hook
  `onGuidePressFunction_` is permanently null, marked
  `// GUIDE BUTTON - More info needed`.
- Pressing it against the running dashboard produced 83,620 new log lines
  and **zero** guide/hud activity.

On hardware xam intercepts the button and loads `hud.xex`. Xenia's HLE xam
has no such behaviour, so nothing happens. Only a working LLE xam could
open the real Guide — which is what Phase 2/3 was building toward.

---

# Phase 5 — THE DASHBOARD RENDERS

## Working configuration

```
xenia_canary.exe \
  --logged_profile_slot_0_xuid=<XUID of an existing profile> \
  --async_shader_compilation=false \
  dashroot\dash.xex
```

Produces the full NXE dashboard: home/social/games/apps/settings nav, Play
Game / My Pins / Recent tiles, the Xbox Live panel, Friends / Activity Feed
/ Avatar Store blades, gamertag shown, and the A/X/Y button legend.

`dash.xex` 17489, title `FFFE07D1`, real PowerPC under Xenia's JIT with
translated Xenos shaders on D3D12.

## The two blockers, and why they were hard to see

**1. No profile signed in.** dash sat on `"Signing in %s..."` with
`loadingRing.png` forever. `ProfileManager: Found 1 Profiles` but
`logged_profile_slot_0_xuid = ""` — a profile existed, no slot was
occupied, `XamUserGetSigninInfo` returned nothing for all four slots, and
the Lua `WaitForPendingLoads` never resolved.

This looked like "dash renders nothing". It was actually rendering a
sign-in screen correctly. The 10 pipelines were the spinner and button
glyphs.

**2. Async shader compilation dropped every draw.** Once signed in, dash
issued real draws and Xenia discarded them:

```
Skipping draw - pipeline not ready: VS ... mod ..., PS ... mod ...   x53
```

`async_shader_compilation` defaults to true, which skips draws whose
pipeline is still compiling. Setting it false took skipped draws to 0.

Neither blocker was a missing export or a broken emulator subsystem. The
draws were being thrown away one layer below everything Phase 4
investigated.

## Measurement lesson

Pipeline count is a poor progress metric: pipelines are created only for
*new* shader combinations, so a fully-drawing UI reusing 10 cached
pipelines is indistinguishable from a UI drawing nothing. `Skipping draw`
was the signal that mattered, and it only appeared once sign-in completed.

Note also that the `VdSwap=0` counter used throughout Phase 4 was
measuring the wrong thing — the dashboard demonstrably presents frames.

## The Xbox Guide button — definitive

Tested against the live rendering dashboard: 15,797 new log lines after the
press, screenshot identical, no Guide overlay, no state change.

Chain:
1. Backspace -> `X_INPUT_GAMEPAD_GUIDE` (0x0400), forwarded by the winkey driver
2. `ImGuiDrawer` calls `onGuidePressFunction_`
3. **`SetGuideButtonAction` has zero callers** - the hook is always null
   (`// GUIDE BUTTON - More info needed`)
4. The button reaches the guest, but HLE xam has no Guide implementation

The Guide is unreachable under HLE xam by construction. On hardware xam
intercepts the button and loads `hud.xex`. That requires the **LLE xam**
path from Phases 2-3 (`--lle_xam`, guest bootstrap thread,
`XamShowGuideUI` ordinal 0x304, 236/236 imports bound), which is blocked on
xam's heap-id initialisation.

---

# Phase 6 — The Guide (hud.xex) message interface

## hud.xex runs under HLE xam

With `cd1e74a` (DllMain on a guest thread) and `XamRegisterSysApp`
implemented, `hud.xex` 17489 loads and runs with **zero undefined externs**:

```
Bootstrap: DllMain hud entry=913F9D00
XamRegisterSysApp(F8000004, 000000FF, 913E69C0, 00000000)
Bootstrap: DllMain hud returned
```

It registers as **system app 0xFF** with handler **`913E69C0`**, then idles
waiting for messages. That is correct overlay behaviour - the Guide is
dormant, not broken.

Note hud needed only *one* missing export at runtime, not the 165 the
static import analysis suggested. Import counts badly overstate real work.

## Ghidra loading: XEX basefiles are FLAT

Xenia's XEX loader maps the basefile with **offset == RVA**. Ghidra's PE
loader honours the section table's raw offsets instead, which for hud
shifts `.text` by 0xE00 and makes runtime addresses resolve to zeros.

Import XEX basefiles as raw binaries at the XEX load address:

```
analyzeHeadless <proj> <name> -import hud.pe \
  -loader BinaryLoader -loader-baseAddr 0x913E0000 \
  -processor "PowerPC:BE:32:default"
```

Ghidra addresses then match runtime log addresses 1:1.

## The Guide's message handler

`913E69C0`, signature `(r3 = message, r4 = buffer, r5 = size)`:

```
913e69cc  lis  r11,-0x8000        ; 0x80000000
913e69d4  ori  r27,r11,0x4        ; 0x80000004
913e69dc  subf. r11,r27,r3
913e69e0  beq  0x913e6a2c         ; message == 0x80000004
913e69e8  cmplwi cr6,r11,0x1
913e69f0  beq  cr6,0x913e6a0c     ; message == 0x80000005
                                  ; else -> default path 913e69f4

913e6a2c  lwz  r11,0x4(r29)       ; buffer->[0x04]
913e6a48  lwz  r30,0x8(r11)       ; sub-command at ->[0x08]
913e6a58  cmplwi cr6,r30,0x1      ; dispatch: 1, 2, 3..8, >=9
```

So the Guide accepts two messages, `0x80000004` (with a sub-command) and
`0x80000005`.

## What remains to open the Guide

1. Store the handler registered via `XamRegisterSysApp` (app 0xFF)
2. Implement `XamShowGuideUI` (ordinal 0x304) - **currently absent from
   Xenia's HLE xam**, though ~20 other `XamShow*UI` functions exist - to
   dispatch `0x80000004` with the right sub-command
3. Wire `SetGuideButtonAction` (zero callers today) to call it

Steps 2 and 3 need a guest thread context to execute hud's handler, which
the existing bootstrap thread pattern already provides.

---

# Phase 7 — The Guide message interface, fully decoded

## Loading the Guide as an overlay

`hud.xex` registers no graphics interrupt callback and has no rendering
context of its own - it is an overlay. Running it standalone can never show
the Guide. `--guide_hud_path` loads it after the title's main thread exists
so the dashboard's graphics are live underneath.

Verified working:

```
Guide: loading GAME:\hud.xex
Guide: DllMain entry=913F9D00
Guide: DllMain returned
Guide: dispatch msg=80000004 subcmd=1 -> 913E69C0
```

## What hud actually executes

Guide Loader thread, complete trace (requires
`--log_high_frequency_kernel_calls=true`, or `XMemSet`/`XMemCpy` are hidden):

```
RtlInitializeCriticalSection(914004C8)
XamRegisterSysApp(F800023C, 0xFF, 913E69C0, 0)
XamAllocEx(20000000, 0, 0x998)          <- Guide object
KeQueryPerformanceFrequency()
XMemSet(3010A064, 0, 0x480)
XMemSet(3010A4EC, 0, 0x008)
XMemSet(3010A510, 0, 0x47C)             <- constructor, matches 913ECA00
XMemCpy(3010A510, 30108000, 0x47C)      <- template copy
```

The Guide object **is constructed**. Execution then enters the message
dispatcher and stops there.

## Sub-command dispatch (handler 913E69C0)

```
913e6a9c  li r3,0x998        ; subcmd 1 -> allocates 0x998
913e6aa0  bl 0x913f9f38      ; allocator
913e6ab0  lwz r4,0xc(r29)    ; buffer[0x0C]
913e6ab4  bl 0x913eca00      ; Guide object constructor
913e6ae0  stw r3,0x690(r31)  ; store object
913e6af8  bl 0x913f99b8      ; message dispatcher
```

Sub-command sizes distinguish the paths: 1 -> 0x998, 2 -> 0x518,
0 and 3..8 -> 0x510. The observed 0x998 confirms sub-command 1 was taken,
so the message buffer layout reconstruction is correct.

## Message dispatcher (913F99B8)

Jump table at **`913E2FB0`** (15 byte offsets), target =
`0x913F99F8 + offset`. Accepts messages `0x80000002`-`0x80000010`:

| Message | Target | Message | Target |
|---|---|---|---|
| 80000002 | 913F9A8C | 80000008 | 913F9A58 |
| 80000003 | (default) | 80000009 | 913F9A38 |
| **80000004** | **913F9A08** | 8000000A | 913F99F8 |
| 80000005 | 913F9A28 | 8000000B-0F | (default) |
| 80000006 | 913F9A7C | 80000010 | 913F9A6C |
| 80000007 | 913F9A48 | | |

Each target performs a virtual call on the Guide object.

## Guide object vtable (913E1CB4)

| Slot | Address | | Slot | Address |
|---|---|---|---|---|
| +00 | 913ED990 | | +10 | 913EA5F0 |
| +04 | **913EA4C8** | | +14 | 913ECB30 |
| +08 | 913EA520 | | +18 | 913EACA8 |
| +0C | 913EA588 | | +1C | 913EACA8 |

Message `0x80000004` resolves to **vtable slot +0x04 = `913EA4C8`**:

```
913f9a08  lwz r11,0x0(r3)   ; vtable
913f9a10  lwz r11,0x4(r11)  ; +0x04
913f9a24  bctr              ; -> 913EA4C8
```

**`913EA4C8` is the Guide's open method, and that is where execution
currently stops.** It is the next thing to trace.

## Measurement lesson (third occurrence)

`kHighFrequency` on `XMemSet`/`XMemCpy` hid hud's progress and produced a
false "stuck after allocation" conclusion - the same class of error as the
`VdRetrainEDRAM` tag and an over-narrow output filter earlier. When a trace
goes silent, check what the filter suppresses before concluding a stall.

---

# Phase 8 — Why the Guide cannot render under HLE xam

## The message channel works completely

Sequencing matters: `0x80000004` **constructs** the Guide object and stores
it at `91400690`; every other message **loads** that object
(`913e69f4: lwz r3,0x690(r11)`) and dereferences null if it does not exist.
Creating first, then dispatching, makes every message succeed:

```
msg=0x8000000A  create=00000000  handler=00000000
msg=0x80000005  create=00000000  handler=00000000
msg=0x80000009  create=00000000  handler=00000000
msg=0x80000002  create=00000000  handler=00000000
```

Full round-trip into the Xbox Guide, all returning success. Nothing renders.

## The reason

```
hud.xex imports 83 XUI functions - every one marked !! (unimplemented)
hud.xex called 0 XUI functions at runtime
```

`XuiSendMessage`, `XuiElementGetChildById`, `XuiObjectSetProperty`,
`XUIElementPropVal_Construct/Destruct/SetString`,
`XuiElementPlayNamedFrames`, `XuiLookupStringTableByIndex`, `XuiSetTimer`,
`XuiObjectFromHandle`, `XuiGetOuter` ...

The Guide's entire UI is built on **XUI, which lives inside xam**. Xenia's
HLE xam implements none of it.

## Why the dashboard renders but the Guide cannot

| | XUI source | Result under HLE xam |
|---|---|---|
| `dash.xex` | **statically linked** (16.9 MB image) | renders |
| `hud.xex` | **imported from xam** (83 functions) | cannot render |

This is exactly the split Phase 1 measured: hud needs 81 (now 83) XUI
functions from xam, and reimplementing XUI means a scene graph, timeline
engine, `.xur` parsing, focus semantics and a renderer with undocumented
behaviour.

**The Phase 1 conclusion was correct.** The Guide requires the real xam -
the LLE path - which remains blocked on xam's heap-id initialisation
(Phase 3). HLE xam was the right choice for the dashboard and can never be
the right choice for the Guide.

---

# Phase 9 — Locating the LLE xam heap failure

## The stack walk that Phase 3 could not do

Phase 3 attempted this diagnostic and got garbage (`lr=00000001`, zeros)
because it used the PPC32 SysV convention `[frame + 4]`. Xenon MSVC saves
LR **8 bytes below the caller's stack pointer**:

```
mfspr r12,LR
stw   r12,-0x8(r1)     ; LR at S-8
stwu  r1,-0xN(r1)      ; frame at S-N, [S-N] = S
```

so for a frame `F`, its saved LR is at `[F] - 8`. With that correction the
walk produces exact call chains, terminating on the `0xBC` stack fill.

## The failing allocation's call chain

Hooking `_vsnprintf` on the `"...from heap..."` format string:

```
frame[5] lr=81751a24 -> func 817519d8 (104 bytes)   <- xam ENTRY POINT (DllMain)
frame[4] lr=81751898 -> func 81751718 (704 bytes)
frame[3] lr=8175fb30 -> func 8175fb00 (304 bytes)
frame[2] lr=817b5be4 -> func 817b5bb0 (88 bytes)
frame[1] lr=817b541c -> func 817b53b0 (120 bytes)
frame[0] lr=817b535c -> func 817b5230 (384 bytes)   <- allocation request
```

The failure happens during **xam's own DllMain initialisation**, five calls
deep, not in a later subsystem. This matches the observed behaviour: heaps
1-5, 7, 8 commit successfully first, then something in init requests heap 0
(the deliberately zero-sized placeholder).

`817b5230` calls `81d16aac`, `81d1677c` and `81d1675c` in xam's import
thunk region, setting up arguments including `0x800021` and `0x100000`.

## Why this matters

The Guide requires XUI, XUI lives in xam, so the Guide requires LLE xam
(Phase 8). LLE xam is blocked on this heap failure. This call chain is the
thread to pull to unblock it - the next step is identifying which kernel
imports `81d16aac`/`81d1677c`/`81d1675c` resolve to, and what state they
return that leads xam to select heap 0.

---

# Phase 10 — Root cause of the LLE xam heap failure

## Correction: xam's Ghidra addresses are shifted

xam's PE, like hud's, has raw offsets that differ from RVAs:

```
.text   VA=0x00130000  Raw=0x00128E00   <- shifted 0x7200
.rdata  VA=0x00000400  Raw=0x00000400   <- identical
```

Xenia maps the XEX flat (offset == RVA); Ghidra honours raw offsets. So for
xam **code**:

```
Ghidra address = runtime address + 0x7200
```

`.rdata` matches 1:1, which is why the heap *strings* resolved correctly in
Phase 3 and masked the problem. Structural findings from Phase 3 (heap table
stride 0x198, the ten entries, the id resolver, the 0xCCCC assert) remain
valid - they were derived from Ghidra addresses used consistently. Only the
runtime mapping was wrong.

Verification: runtime entry `817519D8` + 0x7200 = `81758BD8`, and the
outermost stack frame lands just past it.

## The call chain (runtime -> Ghidra)

```
81751a24 -> 81758bd8 (104)   xam DllMain
81751898 -> 81758918 (704)
8175fb30 -> 81766d00 (304)
817b5be4 -> 817bcdb0 (88)
817b541c -> 817bc5b0 (120)   composes the heap id
817b535c -> 817bc430 (384)   allocator - the Phase 3 function, confirmed
```

Preceded by `sprintf("XAM Pool %d")` and six `ExCreateThread` calls: this is
xam's worker thread pool construction.

## How the heap id is built

```
817bcdb0:  app_id validated <= 0xFF, packed into bits 16-23, | 0x20000000
817bc5b0:  bl 817badb0                  ; heap selector -> low byte
           heap_id = (arg3 << 8) | selector_result
           bl 817bc430                  ; allocator
```

## The selector - 817BADB0

```
bl 0x81783270          ; current app id -> r29
cmplwi r29,0x0         ; no current app?
beq -> fallback:
   bl 0x81d1653c       ; KeGetCurrentProcessType()
   cmpwi r3,0x2        ; X_PROCTYPE_SYSTEM -> assert
   test flags bit 3    ; set -> accept
   test flags bit 5    ; set -> accept
   twi r0,0x19         ; otherwise ASSERT
```

**xam selects a heap from the current app id.** Under our LLE bootstrap that
id is 0, none of the fallbacks apply, and the selector traps - so the heap
id degrades to 0, the deliberately zero-sized placeholder, and all 91
allocations fail.

This is not a missing export or a memory shortage. It is xam requiring
per-context state ("which xam app is currently running") that Xenia's HLE
kernel never establishes for the thread running xam's DllMain.

## Consequence

The Guide needs XUI; XUI lives in xam; so the Guide needs LLE xam
(Phase 8). LLE xam needs this app-id context. Satisfying it means
establishing xam's notion of a current app before its DllMain runs -
plausibly by setting the state `81783270` reads, or by running xam's init
on a thread whose process type and flags satisfy the fallback path.

---

# Phase 11 - The app-id getter, disassembled

Ghidra is no longer installed on this machine. `work/ppcdis.py` decodes the
subset of PPC needed here straight out of `work/xam17489.pe`, mapping a Ghidra
VA to a raw offset as `va - 0x815F0000 - 0x7200`. It self-checks: the getter's
first two instructions decode to `mfspr r12,LR` / `stw r12,-8(r1)`, the Xenon
MSVC prologue, which confirms the offset arithmetic independently.

## Correcting Phase 10

Phase 10 placed the `KeGetCurrentProcessType` fallback in the selector. That is
true but incomplete - **both** functions have one, and they are distinct:

```
81783270 (getter):
    r11 = [0x81D227F0]
    if (r11 == -1) return 0xFE            ; sentinel short-circuit
    r3 = 817822A8()                       ; per-thread app context
    if (r3 != 0) { r31 = [r3+24]; ... }   ; real per-thread app id
    else r31 = KeGetCurrentProcessType()==2 ? 0xFE : 0xEE

817BADB0 (selector):
    r29 = getter(); r28 = requested app id
    if (r29 == 0)  -> its own process-type fallback, else twi
    if (r29 == r28) -> ok
    if (r28 == 0)   -> ok
    else            -> twi (trap) -> heap id 0
```

The getter never returns 0 in practice, so the selector's fallback is mostly
dead code. The trap that matters is the `r29 != r28` mismatch.

## Why the Phase 10 fix could not finish the job

Stack-walking the survivors put frames 3+ at `0x92xxxxxx` - that is **dash.xex**
(`XEX_HEADER_IMAGE_BASE_ADDRESS: 92000000`), not xam. The remaining failures are
not xam initialising itself; they are dash calling into xam from title threads.
A title thread can never satisfy a SYSTEM process-type check, so running init in
the system process was structurally unable to fix them.

Note the `+0x7200` shift is xam-only. Applying it to dash frames is meaningless.

## The sentinel

`[0x81D227F0]` is zero at runtime - never initialised under our bootstrap. The
value `-1` makes the getter report `0xFE` (XamApp) unconditionally, which is
what a title thread needs in order to match the requested app id.

## Correcting Phase 11 - the selector trap patch does nothing

The first measurement of the trap patch reported OOM=0 and was wrong. Writing
to guest .text faults: Xenia maps code pages read-only (`old_protect=1`), and
the store silently killed the setup thread. Everything downstream - DllMain,
`init complete`, even the failure branch - never ran. xam never initialised, so
it never allocated, so zero allocations failed. A vacuous zero.

It went unnoticed because the sanity checks used were "does dash still render"
and "are there fault lines", and dash renders fine without LLE xam at all.
The check that mattered was whether `init complete` appears in the log.

Re-measured with the page unprotected around the store, so the write actually
lands and xam actually boots:

```
patch=true   OOM=6  init_complete=1   trap 0FE00019 -> 48000004
patch=false  OOM=6  init_complete=1
```

Identical. **The trap at 817BAE38 is not what fails the remaining six.** The
cvar now defaults to false.

Standing result: the app-id sentinel is the only verified fix, 11 -> 6.

### Where to look next

The selector has a *second* twi, at 817BADFC, terminating the table scan that
precedes the app-id comparison:

```
817badcc  addi r11,r11,-912      ; table at 0x8160FC70
817bade0  lwz r8,0(r11)
817bade4  cmplw r9,r8            ; match?
817bade8  beq -> 817bae00        ; found
817badec  addi r10,r10,4
817badf4  cmplwi r10,0x50        ; 20 entries
817badf8  blt -> 817bade0
817badfc  twi                    ; not found -> TRAP
```

Any OOM whose requested value is absent from that 20-entry table traps here,
before the app-id logic is ever reached. That is the more likely source of the
remaining six.

### Method note

Always pair an error-count metric with a liveness metric from the same run.
`OOM=0` and `init_complete=0` together mean nothing was tested.

## Both selector traps ruled out

Patching *both* twi sites (817BAE38 app-id mismatch, 817BADFC table-scan
failure) to `b +4`, with the writes verified to land and xam booting:

```
patch=true   OOM=6  init_complete=1  xamerr=5   both traps -> 48000004
patch=false  OOM=6  init_complete=1  xamerr=5
```

Identical. **Neither trap is on the path of the remaining six.** The
"selector traps -> heap id degrades to 0" model explains the failures the
sentinel fixed, but not these.

What the residual errors actually say:

```
Available system memory on heap 52428 (UNINITIALIZED) is dangerously low (0 bytes)
Out of memory allocating 64 bytes from heap 0 (UNINITIALIZED)
```

52428 is 0xCCCC - the uninitialised-memory fill pattern. A heap *descriptor*
is being read before anything wrote it, which points at heap creation never
having run for that heap, not at heap *selection* picking the wrong one.
That is a different subsystem from the selector, and the next thing to trace.

## PE section shifts (xam 17489, image base 815F0000)

The single 0x7200 shift applies only to .text. Correct values:

```
.rdata   VA=815F0400  raw=00000400  shift +0x0
.pdata   VA=816F5200  raw=00105200  shift +0x0
.text    VA=81720000  raw=00128E00  shift +0x7200
.data    VA=81D20000  raw=0071CC00  shift +0x13400
.edata   VA=81E20000  raw=0073A000  shift +0xF6000
```

## Guest .text is writable, with care

Xenia maps guest code read-only (`old_protect=1`). Unprotect to RW, store,
restore. Done before DllMain so the JIT cannot serve a cached translation.
This makes guest-code experiments cheap and is how both traps were ruled out.

## Phase 12 - heap creation is never attempted

Following the 0xCCCC clue instead of the selector:

- Heap descriptor array: **0x81D4E1B0**, 10 entries x 408 bytes (0x198),
  indexed by heap id. Resolver 817BAF38 walks it.
- The resolver explicitly compares the id against **0xCCCC** and returns null
  rather than trapping (817BAFB4). xam treats that value as a known
  "never initialised" sentinel by design.
- The query function returns early when descriptor field +4 is zero, which is
  exactly what emits "dangerously low (0 bytes)".

The decisive evidence is a message that is *absent*. xam carries
`"Unable to commit %d bytes for heap."` at 8160FC48 and it never fires.
Creation is not failing - it is never attempted. Several phases were spent on
heap *selection* when the descriptors were simply never built.

Call chain from that string:

```
817B9340  single-heap creator (contains the "Unable to commit" path)
817BBD70  calls it five times; operates on 0x81D4E1B0 directly; takes one arg
          -> NO callers anywhere inside xam
```

A root with no internal callers means something outside the module drives it
on hardware - precisely what our bootstrap does not reproduce.

### Calling it directly does not work (yet)

`lle_xam_heap_init` executes 817B4B70 (runtime) after DllMain with r3=0:

```
heap_init=true   OOM=0  init_complete=0  xamerr=0  unable_to_commit=0
heap_init=false  OOM=6  init_complete=1  xamerr=5  unable_to_commit=0
```

OOM=0 is vacuous again - `init_complete=0`. "calling heap init" logs, "heap
init returned" never does, and the init thread emits nothing further, so the
call faults or deadlocks. The -0x7200 mapping is not in doubt (the trap probe
read back the exact expected twi at the same shift).

Candidates: r3=0 is the wrong argument; the routine needs state DllMain has
not established; or it deadlocks on the lock taken at 817BBDC4/817BBDC8.

Two dead ends recorded so they are not retried:
- No function writes the descriptor array directly. A load/store classifier
  flagged 817BA314, but those stores target the caller's out-struct.
- The export directory does not parse as a standard IMAGE_EXPORT_DIRECTORY
  (yields 8 exports for a module with ~1500). Not needed - guest addresses can
  be executed directly.

## Correcting Phase 12 - the heaps ARE created

Phase 12 concluded heap creation is never attempted. That is wrong. Reading
the descriptors directly out of guest memory after DllMain:

```
heap[0] +0=0000CCCC +4=00000000    <- placeholder, by design
heap[1] +0=00000001 +4=40000000
heap[2] +0=00000002 +4=401F0000
heap[3] +0=00000003 +4=00000000  +8=403F0000
heap[4] +0=00000004 +4=405A0000
heap[5] +0=00000005 +4=408D0000
heap[6] +0=00000006 +4=8C000000
heap[7] +0=00000007 +4=408F0000
heap[8] +0=00000008 +4=40910000
heap[9] +0=0000CCCC +4=00000000
```

Heaps 1-8 are fully built with real base addresses. DllMain does create them.

The reasoning error: `"Unable to commit %d bytes for heap."` is absent from the
logs because creation *succeeded*, not because it never ran. An absent error
message was read as evidence of an absent call. It is evidence of neither -
only that nothing failed.

Lesson, and it is the same one as the vacuous OOM=0: prefer reading the state
directly over inferring it from which messages did or did not appear. Ten
lines of hex settled in one run what several phases of inference got backwards.

## So it is selection after all - but not a trap

The failing 64-byte allocation, from the stack walk (runtime 8198DDA0):

```
81994f8c  stw  r28,80(r1)
81994f90  addi r5,r1,80        ; out pointer
81994f94  addi r4,r0,64        ; size - the 64 bytes
81994f98  lis  r3,0x1810       ; request = 0x18100000
81994f9c  bl   817bc688
```

Walking `0x18100000` through the selector by hand:

- flags `0x18100000 & 0xF36F0000 = 0x10000000` = table entry [15]. Found, so
  the table-scan trap at 817BADFC never fires.
- app id `0x18100000 & 0xFF` = **0**, which hits the explicit
  "requested id is 0 -> accept" branch, so the mismatch trap at 817BAE38
  never fires either.

This is why patching both traps changed nothing: **neither was ever on this
path.** That negative result was correct and is now explained.

- 817BC688 checks bit 6 (clear, no trap), sets r5=5, tails into the composer.
- Composer 817BC5B0: `heap_id = (r5 << 8) | selector_result`.
- In the selector, r31 starts as the request and every `oris` path is skipped
  (817BAED8 sees `r31 & 0xF0000000 = 0x10000000`, non-zero, so it branches
  straight to 817BAF14). r28 is 0, so `r31 |= r29` is skipped too, and the
  current app id never enters the value.

The allocator prints r28 as the heap id and it is 0, with 817BAB20 resolving
the name to "UNINITIALIZED". 817BC518 compares r28 against 9, so by that point
it is a small array index, not the composed tag.

Open: where the composed tag is reduced to index 0. That reduction is the
remaining unknown, not heap creation and not the traps.

# Phase 13 - the Guide runs, XUI executes, and the show path is identified

With the heap fixed, hud.xex loads and its DllMain completes for the first
time. The Guide object is constructed in xam's heap:

```
Guide: object @91400690 = 401EA710      <- inside heap[1] (base 40000000)
```

## hud registers with real xam, not with Xenia

hud's registration function (PE 98007948) is:

```
cmplwi r4,0x1
bc 12,24 -> unregister      ; reason == 0
bc 4,26  -> skip, return 1  ; reason != 1
addi r4,r0,255              ; app id 0xFF
bl XamRegisterSysApp
```

We pass reason 1, so it registers - but Xenia's HLE XamRegisterSysApp is
called **zero** times. Under LLE xam the import binds to the real guest
export, so the handler goes into xam's own table and never reaches Xenia's
`sys_app_handlers_` map. The "hud did not register app 0xFF" warning was a
false negative from reading the wrong table.

The handler address is recoverable from hud's own code (98007960:
`lis r11,0x913e` / `addi r5,r11,27072`) = **module base + 0x69C0**.

## The message contract

hud's outer handler splits on `msg - 0x80000004`; the inner dispatcher
(PE 9801A7B8) is a jump table on `msg - 0x80000002`, valid while `<= 0xE`:

```
80000002  02  vtable+0x24
80000004  04  CREATE      (constructs the object, stores it at 91400690)
80000005  05  DESTROY     (unregisters classes, fills object with FEEDFEED)
80000006  06  vtable+0x20
80000007  07  vtable+0x18
80000008  08  REGISTER / SHOW      <-- the one that matters
80000009  09  vtable+0xc
8000000A  0A  vtable+0x10
8000000B..0F  default (invalid)
80000010  10  vtable+0x1c
```

0x80000005 is NOT show - it tears the Guide down. The 124
"Class '...' is not registered" warnings it emits are teardown of classes
that were never registered.

## Where the show path stops

```
ERR[XAM]: XuiGamerCardElement::RegisterClasses: CBaseScene::Register() failed.
          HRESULT: 0x80300006
ERR[XAM]: GamerCardRegisterControls: XuiGamerCardElement::RegisterClasses() failed.
==== CRASH DUMP ==== PC: 0x818FAF98  read at 0x0000000100000105
```

The crash is a consequence, not the cause:

```
81902188  lwz r11,0(r31)     ; vtable pointer
81902198  lwz r11,260(r11)   ; vtable[0x104]  <- faults
8190219c  mtctr r11
819021a0  bctrl
```

`r11 = 0xFFFFFFFF`, and `0xFFFFFFFF + 260 = 0x100000103`, matching the
faulting address. CBaseScene::Register() failed, left the scene object's
vtable pointer invalid, and the caller made a virtual call on it anyway.

**The real blocker is `CBaseScene::Register()` returning 0x80300006.**

hud carries UTF-16 references to XUI resource files it expects to exist -
GuideMain.xur, GuideMainServer.xur, HUDScene, HomeTabScene, GamesTabScene,
ConsoleContract.xur, Controller_*.xur - plus XUIS package magic. These live
in a skin package (dashroot has huduiskin.xex), which nothing currently
loads, and the XUI runtime has no render device bound. Either would explain
class registration failing.

## Method note

A message sweep initially reported `pipelines=0` for all seven candidates.
That was not a result: PowerShell parses `0x80000002` as int32, overflowing
to -2147483646, which a uint32 cvar rejects, so Xenia exited before logging.
The `lines=0` column caught it. Without a liveness number in the table the
run would have read as "none of these messages render".

# Phase 14 - why CBaseScene::Register fails

0x80300006 appears at exactly **one** site in xam .text (ghidra 81956584,
runtime 8194F384), inside the function at ghidra 81953EE0 (runtime
8194CCE0). The path that reaches it:

```
819563a0  lwz  r3,8(r30)     ; parent-class name from the class descriptor
819563a8  cmplwi r3,0
819563ac  beq  -> skip       ; no parent, fine
819563b0  bl   81950d60      ; look the parent up
819563b4  or.  r27,r3,r3
819563b8  beq  -> 81956580   ; null -> cleanup -> return 0x80300006
```

81950D60 is a lock-protected registry lookup: acquire the critical section
at 0x81D6D030, search the table at **0x81D6D508**, release, return the hit.
It returns null, so **the parent class is not registered**.

## The registry is only ever populated through the export

Five functions touch 0x81D6D508. The internal Register (81953EE0) both
looks up the parent and inserts the child, and it has exactly one caller:
ghidra 81958948 / runtime **81951748**, which itself has no internal callers
at all - it is the exported XuiRegisterClass.

So nothing inside xam registers the built-in XUI classes. They are
registered from outside the module, exactly like the heap-init routine in
Phase 12 (817BBD70, also callerless). Our bootstrap drives neither.

xam does contain the class names - XuiElement, XuiControl, XuiVisual,
XuiCanvas, XuiScene, XuiButton, XuiLabel, XuiNavButton, XuiList,
XuiGamerCard and ~30 more, all UTF-16 in .rdata - so the classes exist;
they are simply never registered.

## Consequence

hud's RegisterClasses walks its own scene classes, each deriving from an
XUI base. The first parent lookup fails, Register returns 0x80300006, the
scene object is left with vtable pointer 0xFFFFFFFF, and the caller
virtual-calls through it (0xFFFFFFFF + 260 = 0x100000103 = the faulting
address in the crash dump).

Fixing the crash means registering the XUI base classes first, not
patching the faulting instruction.

# Phase 15 - the XUI class registry, and a mapping error

CBaseScene::Register builds a class descriptor on the stack and passes it to
819565B8 (runtime 8194F3B8) - the real registration entry. Phase 14 called
that function "Unregister", which was wrong and is why the first caller
search found only one wrapper. Against the correct entry there are **34**
class-register wrappers.

Descriptor layout (from CBaseScene, 8180EE78):

```
stw r9,100(r1)   ; 0x8161C220 -> "BaseScene"   (class name)
stw r8,104(r1)   ; 0x815FB498 -> "XuiScene"    (parent name)
```

Register reads the parent with `lwz r3,8(r30)`, i.e. the field at +104, and
fails with 0x80300006 when that name is not already in the registry.

## Strings are UTF-16BE

Earlier phases decoded these as UTF-16LE, which is why class-name hits kept
landing on odd addresses (8161C221, 815FB499). Xenon is big-endian: the
strings start one byte earlier. Correct decode: 8161C220 = "BaseScene",
815FB498 = "XuiScene".

## The mapping was not trustworthy

Extracting "first string-looking operand" per wrapper produced a plausible
table, but the descriptor layout is not uniform. For 81AAE520:

```
stw r9,100(r1)   ; 0x81D34F30 -> .data, not a string
stw r8,104(r1)   ; 0x815F3548 -> "XuiElement"
```

Because +104 is the *parent* field, this wrapper registers a class derived
from XuiElement, with its own name held in .data - it does **not** register
XuiElement. The table read it as "registers XuiElement".

## What the ordered call actually showed

Driving the supposed base-class registrars in dependency order:

```
81AA7320 -> 80300006
8174FF38 -> 80300006
81750350 -> 80300006
817503E8 -> 00000000
8199BE08 -> 00000000
8176B2C8 -> 00000000
```

The three that return 0x80300006 are hitting the same missing-parent path,
so the classes they need are still absent. The three that return 0 do not
prove success - the group registrars appear to ignore child failures, since
CBaseScene::Register still fails afterwards.

`lle_xam_xui_init` is kept as a probe but defaults to false: it does not fix
anything, and its address list is built on the unreliable mapping.

## Next

Decode the descriptor struct exactly (all fields, not just two stores) for
each of the 34 wrappers, so class vs parent is read from the right offsets.
Only then is a dependency order meaningful. The open question is what
registers the true roots - XuiVisual/XuiElement/XuiControl - since no
wrapper in the 819565B8 set appears to.

# Phase 16 - a tooling bug, and what the registry actually says

## Direct read of the registry

```
XUI crit     @81D6D030: 01000000 00000000 00000000 00000000 FFFFFFFF ...
XUI registry @81D6D508: 00000000 00000000 00000000 00000000 00000000 ...
```

The critical section is initialised (normal RTL_CRITICAL_SECTION shape); the
registry is entirely zero. Note the careful reading: all-zero is equally
consistent with a validly-initialised **empty** container. What is certain is
that **no class has ever been registered**, which explains uniformly why every
parent lookup fails - including for classes expected to be roots.

## The function-start detector was wrong

Prologue detection only accepted:

```
mfspr r12,LR
stw   r12,-8(r1)
```

but many xam functions use the stack-check form:

```
mfspr r12,LR
bl    8181495C        ; helper
stwu  r1,-160(r1)
```

Those were invisible, so `fstart` walked back past them to the previous
function. Consequence: `fstart(81956584)` returned 81953EE0 when the true
owner is **81956318**. That single mis-attribution is what produced the
register/unregister flip-flopping across Phases 14-15: the disassembly was
right each time, the ownership was not.

Accepting either prologue form yields 18087 function starts and:

```
81956318  the register function (contains the 0x80300006 site)  39 call sites
819565B8  thin wrapper over it                                  34 call sites
81953EE0  separate function                                      1 call site
```

**Conclusions that survive the fix:** every "no internal callers" result was
derived from exact `bl` target addresses, which never depended on `fstart`.
Re-verified: export 81958948, heap-init 817BBD70, and 819A3008 all still have
zero internal callers.

**Conclusions that do not survive:** any claim about which function *contains*
a given address, including the Phase 14/15 labels for 819565B8.

## Corrected descriptor map

Decoding by field offset (class at +100, parent at +104) rather than by
operand order gives a trustworthy table. All 34 wrappers have a parent, and
**none registers a core class**:

```
8180EE78  BaseScene              <- XuiScene
8180F948  XuiGamerCard           <- XuiControl
819A2A88  ControlPackNuiVScroll  <- XuiElement
...
roots: none
```

There are also zero dword references anywhere in the image to the core class
name strings, so there is no descriptor table either. XuiScene, XuiElement,
XuiControl and XuiButton are consumed by everything and registered by nothing
inside xam.

Since the only path into the registry that xam does not itself drive is the
exported XuiRegisterClass (81958948, callerless), the core classes must be
registered from outside the module - the same structural pattern as heap init.

# Phase 17 - all XUI classes registered

xam's core XUI class registrars build their descriptor with the class name at
**+84** and the parent at **+88**. The hud-side wrappers use **+100/+104**.
Phase 15/16 scanned only the latter offsets and therefore concluded "no
function registers a core class", which was wrong - 38 of them do.

They are unreachable from xam's own code: no `bl`, no `b`, and no pointer
table (every hit outside .text was a .pdata unwind entry). Driving them
directly from the host, repeatedly until convergence:

```
pass 0:  2 ok, 36 failed
pass 1: 17 ok, 21 failed
pass 2: 19 ok, 19 failed
pass 3:  0 ok            <- converged, all 38 registered
registry @81D6D508: 40874CB0 40873CB0 40873F30 40871900 40874450 ...
```

**Zero xam errors.** CBaseScene::Register no longer fails.

## Scanning bugs that produced confident wrong answers

Three in this investigation, each yielding plausible output rather than an
obvious failure:

1. **UTF-16 endianness.** Names are UTF-16BE; decoding as LE matched one byte
   into each string, giving odd addresses (8161C221 for 8161C220).
2. **Prologue form.** `fstart` accepted only `mfspr r12,LR; stw r12,-8(r1)`
   and missed the stack-check form (`mfspr; bl helper; stwu`), so it walked
   back past such functions and mis-attributed addresses. This caused the
   register/unregister flip-flopping.
3. **Unbounded function extent.** Scanning a fixed 110 instructions from each
   start runs into the *next* function and picks up its strings, shifting
   every label by one. This is what hid **XuiControl <- XuiElement** at
   819524D0; without it XuiScene could never register, and neither could
   BaseScene.

The convergence counter is what exposed (3): `pass 2: 0 ok` proved ordering
was exhausted, so something was structurally missing rather than mis-ordered.

## Corrected: the crash is NOT caused by Register failing

Phase 14 claimed the fault at 818FAF98 was a downstream symptom of
CBaseScene::Register returning 0x80300006. Registration now succeeds with
zero xam errors and **the crash is unchanged**:

```
PC: 0x818FAF98   Access Violation: read at 0x0000000100000105
81902188  lwz r11,0(r31)     ; vtable pointer = 0xFFFFFFFF
81902198  lwz r11,260(r11)   ; faults
```

So an object still reaches this path with vtable pointer -1, for a reason
unrelated to class registration. That is the next thing to trace.

# Phase 18 - the remaining crash is self-inflicted

The fault at 818FAF98 is caused by the hand-built message payload, not by xam.

```
Guide: buffers inner=3017F000 buf=30180000 out_sz=30181000
crash: r3 = r31 = 0x30180000
```

`buf` is the exact pointer being virtual-called. The callers of the faulting
function do:

```
r31 = r3            ; this
r3  = [r3 + 12]     ; sub-object
bl  81902148        ; virtual call through it
```

so an object's +12 field ends up holding our synthetic buffer, which xam then
treats as a typed object. Its first word is 1 (we set `bw[0] = 1`), not a
vtable, so the dereference faults.

Two earlier readings of this crash were wrong:

- Phase 14 called it a downstream symptom of CBaseScene::Register failing.
  Registration now succeeds with zero xam errors and the crash is unchanged.
- Phase 17 read 0x30180000 as "inside the xam file image", because the log
  line "LLE xam: loaded at 30013000" prints `hmodule_ptr()` - the module
  handle structure - not the image base. Both addresses are in Xenia's kernel
  allocation region, which is also where SystemHeapAlloc hands out our
  buffers. That is why they are adjacent: 3017F000 / 30180000 / 30181000.

## The heap[0] alias is not implicated

```
alias=true   OOM=0  crashes=3  PCs= 81779604 x2, 818FAF98
alias=false  OOM=9  crashes=3  PCs= 81779604 x2, 913F9A58
```

Disabling it makes things worse - allocations start failing and the third
crash moves earlier, into hud.xex itself. XUI registration succeeds either
way (19 ok in pass 2). The alias is holding allocations together.

Also confirmed: the two faults at 81779604 are pre-existing. They appear in
runs that never dispatch 0x80000008, so none of this phase's work introduced
them.

## Consequence

The message structure passed to hud's handler is wrong. The layout was
guessed (`bw[0] = 1; bw[1] = inner`) and it happens to survive the create
message but not the open path. The fix is to stop hand-crafting the payload
and drive the documented entry instead - xam's XamShowGuideUI (ordinal 0x304)
- or to recover the real structure layout before sending 0x80000008.

# Phase 19 - XamShowGuideUI works; app 0xFE is not registered

Replacing the hand-built message with xam's own entry point removes the
self-inflicted crash (3 faults -> 2, and both survivors are the pre-existing
81779604 pair).

```
Guide: XamShowGuideUI (ord 0x304) -> 81787430
Guide: XamShowGuideUI returned 0000065B      (1627 = ERROR_FUNCTION_FAILED)
```

## What the export does

```
8178E630  XamShowGuideUI(user):
            if (user >= 4) -> assert
            tail-call worker(user, 1, 0)

8178D730  worker(user, flag, arg):
            build 12-byte payload on the stack {+4 = user, +8 = arg}
            r3 = gate(flag)                     ; 81797AF8
            if (r3 == 0) return 5
            r3 = 0xFE, r4 = 0x00021015, r5 = payload, r6 = 12
            bl 817689C8                          ; message send
```

So the gate passes and the Guide open is delivered as **message 0x00021015 to
app 0xFE (XamApp)**, not to hud's 0xFF. The log gives the failure directly:

```
WRN[XAM]: XMsgStartIORequest failed to find app id '0x000000FE (254)'   x2
```

App 0xFE is not registered. hud registers 0xFF (Phase 13); XamApp is xam's
own system app and nothing has registered it under our bootstrap - the same
callerless-driver pattern as heap init and the XUI registrars.

Note the failing call is XMsgStartIORequest, not XMsgInProcessCall. The
0x000000FC (XLiveBase) and 0x000000FA (XMP) misses in the same log are
pre-existing and unrelated to the Guide path.

## Next

Find what registers app 0xFE inside xam and drive it, exactly as was done for
the XUI class registrars.

# Phase 20 - the app-id sentinel was causing the "pre-existing" crashes

The two faults at 81779604 were dismissed across several phases as pre-existing
background noise. They were caused by our own Phase 10 workaround.

```
817807ec  lwz  r3,10224(r31)   ; [0x81D227F0] - the sentinel we force to -1
817807f0  bl   81D170FC        ; called with that value; returns 0
817807f4  cmplwi r3,0
817807fc  twi                  ; guard; Xenia does not stop here
81780804  lwzx r11,r11,r3      ; faults, r3 = 0
```

A/B:

```
sentinel=true   OOM=0  init=1  crashes=2  PCs= 81779604 x2
sentinel=false  OOM=0  init=1  crashes=0  PCs= none
```

Disabling it removes every crash **and costs nothing**: OOM stays at 0,
because the heap[0] alias (Phase 17) supersedes it. The sentinel earned its
place when it took heap failures 91 -> 6, but once heap[0] was aliased it was
redundant, and it was still breaking two init paths.

"Pre-existing" means "present before this change", not "not our fault". The
sentinel was old enough to have become part of the baseline, which is exactly
why it went unexamined for so long. It also blocked the system-app initialiser
in Phase 19: driving 81751428 hit this same fault.

Default is now false.

## Where the Guide stands

```
LLE xam boots, 0 heap errors, 0 crashes
38 XUI classes registered, registry populated
hud.xex loads, DllMain completes, Guide object constructed
XamShowGuideUI resolves and runs -> 0x65B (ERROR_FUNCTION_FAILED)
  because XMsgStartIORequest cannot find app 0xFE (XamApp)
```

App 0xFE is absent from the static descriptor table at 0x81604368 (which
covers 0xEF-0xFD), and XamRegisterSysApp has no internal callers. What
registers XamApp is still unknown.

# Phase 21 - system apps init, but 0xFE is not among them

With the sentinel gone the app initialiser no longer crashes. Two attempts:

**Outer root 81751428 blocks.** It spawns a worker thread and waits on
something that never signals here. Not a logging artefact - at log_level=2
(4642 lines instead of 531978) it still does not return in 90s.

**Table walker 8177FE50 completes.** Returns 0, no crash. But:

```
app FE entry @81D426D0: 00000000 x8
current-app ptr @81D426C8 = 00000000
XamShowGuideUI returned 0000065B
```

It populates what the descriptor table at 0x81604368 contains - ids 0xEF-0xFD -
and 0xFE is simply not in that table. So XamApp is registered by some other
mechanism, still unidentified.

## Note on 0xFF

81786078 special-cases 0xFF by returning [0x81D426C8], which is the *current
app* pointer, not a table slot. So "app 0xFF" means "whatever app is current",
and hud's XamRegisterSysApp stores its handler in a separate global at
0x81D42688. Earlier phases treated 0xFF as hud's table entry; it is not.
Worth dumping 0x81D42688 to confirm hud's registration actually landed - that
would settle the Phase 13 question directly.

## IoDismountVolumeByName

Declared in Xenia's export table (ordinal 0x3D) but never implemented, so
calls fell through to the "undefined extern" path. A system app polls it
against \Device\HdDvdRom. Added a stub - but returning X_STATUS_SUCCESS made
the poll *faster* (14k -> 30k calls), so the return value is wrong and the
loop is by design, not an error-retry. The stub is still worth having; it does
not fix anything.

# Phase 22 - why app 0xFE never registers

The outer root 81751428 does the right thing: it creates XamApp's main thread
(entry runtime 8177AD80, created suspended, then resumed) and waits for it.
That thread then dies immediately - its entire log is seven lines:

```
XThread::Execute thid 36 (handle=F80002AC)
KeTlsSetValue(00000000, 708DFF30)
undefined extern call to 81D0FC8C ExTerminateTitleProcess
KeTlsSetValue(00000000, 00000000)
ObDereferenceObject(301A2010)
RtlFillMemoryUlong(401EA390, 00000040, FEEDFEED)
Removed handle:F80002AC
```

So XamApp's thread takes a fatal path and terminates before registering
0xFE, which is why the app entry stays zero and why the outer root never
returns - it is waiting on a thread that killed itself.

ExTerminateTitleProcess (thunk runtime 81D0FC8C) has two callers in xam:
81760A50 (rt 81759850, a very short function - looks like the fatal handler)
and 81762BA8 (rt 8175B9A8).

## Two unimplemented Xenia exports are implicated

- **ExTerminateTitleProcess** - reached by XamApp's thread; unimplemented, so
  Xenia logs "undefined extern call" and continues.
- **IoDismountVolumeByName** (ordinal 0x3D) - polled by another system app.

Neither is the root cause on its own. The question is why XamApp's thread
reaches a fatal path at all.

## A likely contributor: twi is not a trap here

xam is full of `twi` guards (0FE00019) after assertions. Xenia does not halt
on them - Phase 20 showed execution continuing straight past one into a
faulting load. So a failed assertion inside XamApp's startup would not stop
the thread; it would run on with invalid state until something fatal happens.
That makes the *first* failed assertion the thing to find, not the terminate
call at the end.

# Phase 23 - XamApp's entry comes from unpopulated thread state

## The assertion theory was wrong

Phase 22 proposed that a failed assertion ran past a discarded `twi` and left
XamApp's thread in invalid state. Tested directly: with
`ignore_trap_instructions = false` (verified in the config dump, and note
xam's guard `0FE00019` decodes as `twi 31,0,25`, which Xenia's emitter
special-cases into an **unconditional** Trap), **zero traps fire** and the
thread still terminates. No assertion fails. The terminate is normal control
flow.

The `ignore_trap_instructions` default of true is still a real finding - Xenia
does discard xam's asserts - it is just not the cause here.

## What actually happens

Implementing ExTerminateTitleProcess (ordinal 0x1A, previously undefined) as a
stub that walks the guest stack gives the caller directly:

```
ExTerminateTitleProcess(code=00000000, unk=83C00002)
  frame[0] lr=81779D54  (ghidra 81780F54)
  frame[1] lr=8177AE54  (ghidra 81782054)   <- inside thread entry 81781F80
  frame[2] BCBCBCBC                          <- stack fill, end of chain
```

81780F50 is an indirect call:

```
81780f44  lwz  r3,52(r31)
81780f48  lwz  r11,48(r31)    ; app entry function pointer
81780f4c  mtctr r11
81780f50  bctrl               ; the callee terminates
```

and field +48 is installed by the thread entry from thread-local state:

```
81781fc4  lwz r9,256(r13)     ; r13 = PCR / thread base
81781fd8  lwz r9,332(r9)
81781fdc  stw r9,48(r31)      ; app entry <- [[r13+0x100]+0x14C]
```

**XamApp's main function is read out of a kernel structure at r13+0x100,
field +0x14C.** Xenia does not populate that the way xam expects, so the
thread calls whatever it finds there and that path terminates - which is why
app 0xFE never registers and why the outer root 81751428 waits forever.

Note it does not crash, so the value is not null; it resolves to some valid
code that terminates.

## Next

Log the actual value of [[r13+0x100]+0x14C] on that thread, and compare with
what Xenia puts at PCR+0x100.

# Phase 24 - correcting Phase 23

Phase 23 claimed "XamApp's main function is fetched from [[r13+0x100]+0x14C]".
That is wrong. Runtime values:

```
r13=301A5000  [r13+0x100]=301A2010  [+0x14C]=00000024
```

0x24 = 36 = the thread id (the log shows this thread as `thid 36`). Xenia's
PCR layout confirms it: r13+0x100 is `X_KPRCB prcb_data`, whose first field is
`current_thread`, and X_KTHREAD+0x14C is `thread_id`. So 81781FDC stores the
thread's own id into [r31+48] - not a function pointer.

The mistake: 81781FDC writes [r31+48] in the *thread entry*, while 81780F48
reads [r31+48] in a *different function* where r31 is a different object. Two
matching offsets in unrelated structures were read as one link. Reading the
value at runtime settled it immediately; the static reading had been wrong
twice before that.

## The actual chain

```
81781F80  thread entry
            switch -> several handlers (81781B68 / 81781D58 / 81781E18)
8178204C    lwz r3,12(r31)        ; obj = [ctx+12]
81782050    bl 81780EE0
81780EE0  r31 = obj
81780F48    lwz r11,48(r31)       ; function pointer in THIS object
81780F50    bctrl                 ; callee calls ExTerminateTitleProcess
```

So the terminating call is a virtual dispatch through `[[ctx+12]+48]`, reached
on one branch of a switch in the thread entry. What remains unknown is which
switch case is taken and why that object's +48 handler terminates.

## Standing facts

- No assertion fails: with ignore_trap_instructions=false, zero traps fire.
- `ignore_trap_instructions` defaults to **true**, so Xenia discards xam's
  `twi` guards entirely. Real finding, not the cause here.
- Two declared-but-unimplemented exports now stubbed: ExTerminateTitleProcess
  (0x1A, with a guest stack walk) and IoDismountVolumeByName (0x3D).

# Phase 25 - XamApp's main function slot holds "terminate"

Dumping the object that 81780EE0 virtual-calls, live at the moment of
termination:

```
obj @401EA330:
 +0  81603CB4   ; vtable in .rdata (2 entries: 817800A8, 8177F8D0 ghidra)
 +4  00000003
 +8  83C00002
+12  00000003
+16  00000020
+24  000000FE   ; <-- app id 0xFE. This object IS XamApp.
+32  401EA390   ; ctx, matches the ExCreateThread argument
+48  81759850   ; <-- ghidra 81760A50
+56  BAADF00D   ; uninitialised-heap fill
```

`+24 = 0xFE` identifies the object beyond doubt, and **`+48` - the function
81780F50 calls - is 81760A50, the five-instruction wrapper that calls
ExTerminateTitleProcess and returns 0.**

So XamApp's descriptor is constructed, given the correct app id, handed the
thread context - and its run/main slot holds the terminate stub. The thread
does exactly what it is told: it calls slot 48, which terminates. Nothing is
corrupt, nothing asserts, no pointer is wrong. The real main function was
simply never installed.

That is the same shape as every other blocker in this module: heap init, the
XUI class registrars, XamRegisterSysApp, the system-app table. Something
outside xam is expected to populate this, and loading xam as a plain guest
module never does.

Note +56 = BAADF00D, so the object sits in freshly allocated heap with parts
still unwritten - further evidence that construction stopped early rather
than that a wrong value was computed.

## Corrected en route

The switch in the thread entry reads `[ctx+8]`, not `[obj+8]`. obj+8 happens
to be 0x83C00002, which also appears as the `unk` argument to
ExTerminateTitleProcess - a coincidence of two structures, not a link. Given
Phase 23 made exactly this error, it is worth stating explicitly.

# Phase 26 - the terminating object is an app built to terminate

## The constructor does not install slot 48

XamApp's constructor (8177F7C8, a leaf function without the standard
prologue - fstart mis-attributes it to 8177F780) writes the vtable and then
**zeroes** +48:

```
stw r10,0(r3)     ; vtable 81603CB4
stw r6,4(r3)      ; 1
stw r11,48(r3)    ; 0   <- slot 48 cleared
stw r7,56(r3)     ; BAADF00D
```

So the terminate wrapper in slot 48 is written after construction.

## Only one place installs it

Searching for the wrapper's address as a code constant gives exactly one hit:
81762C1C, inside 81762BA8 (rt 8175B9A8):

```
r10 = 0x81400002              ; flags
r9  = 3
r3  = 0x81759850              ; the terminate wrapper
stack struct = {0x81400002, 3}
bl 81783AE0(fn, 0, &struct, &out)
```

(Note the constant is built in *runtime* form, 81759850, confirming code
pointers inside xam are runtime-form.)

So an app is deliberately created whose main function is "terminate". The
object we watched die is that app doing exactly its job.

## What this implies about the root we drove

The ancestors of 81762BA8 are 81764D00 <- {81764DC0, 81765360, 81765970} <-
{81765F88 (root), 81787660} - **none of which is 81751428**, the root this
phase has been driving. So either the object predates our call (created
during xam's DllMain or by dash) and our thread merely picked it up, or
81751428 reaches it by a path this static walk does not capture.

Either way the earlier framing - "the outer root creates XamApp's thread and
it dies" - is too confident. What is certain: the thread ran an app whose
main is the terminate wrapper, so it terminated by design. Whether 81751428
is the right root for XamApp startup at all is now open.

Careful readers should note this does not contradict Phase 25 - the object
really is app id 0xFE with slot 48 = terminate - it only changes the story
about *why* that object was running.

## Phase 26b - A/B settles it: the root causes the terminate app

```
sysapp_init=true   terminate_calls=1  [obj+48]=81759850  lines=4648
sysapp_init=false  terminate_calls=0  showguide=0x65B    lines=9206
```

Without the call, no terminate app is created and no ExTerminateTitleProcess
fires. With it, exactly one. So **81751428 does reach 81762BA8** and the
static ancestor walk in Phase 26 was incomplete - the path exists, this
tooling just could not see it (consistent with the leaf-function and
prologue-detection gaps already recorded).

Conclusion: driving 81751428 creates and runs an app whose main function is
the terminate wrapper. That makes it a teardown path, not XamApp startup.
Three phases were spent trying to make the wrong root work - it did not
crash, did not assert, and its thread did precisely what it was built to do.

The remaining symptom is unchanged and is *not* caused by this: with the root
disabled, XamShowGuideUI still returns 0x65B because app 0xFE is unregistered.
That is the real open problem, and 81751428 was never going to solve it.

Also note lines=4648 vs 9206 - with the root enabled the Guide thread blocks
before finishing, so the later diagnostics never print. Any result gathered
in that configuration is truncated and should not be compared against runs
without it.

# Phase 27 - XamApp's real handler and entry located

Searching for the Guide message constant 0x1015 in xam .text gives three
sites: two senders (8178D77C, the XamShowGuideUI worker; and 81BF5DBC) and
one **receiver**:

```
81A66570  cmpwi r11,0x1015   in func 81A66420 (rt 81A5F220)
```

81A66420 is a message dispatcher with cases 0x1006, 0x1007, 0x1008, 0x100A,
0x1015, 0x9180 and more - xam's XamApp message handler. The 0x1015 case
branches to 81A665E0.

Walking up from it:

```
81A66420 (rt 81A5F220)  XamApp message handler
  <- 81A60EE0 (rt 81A59CE0)   only caller
    <- 81A611E8 (rt 81A59FE8) only caller
      <- 81A555D0 (rt 81A4E3D0)  ROOT, referenced from .rdata at 8166C99C
```

8166C99C sits inside a run of code pointers in .rdata (a vtable; the run ends
at 8166C9C8 where string data begins). So **XamApp's entry is 81A4E3D0
(runtime) and it is reached through a vtable slot, not a direct call** -
which is why it has no callers and why nothing in our bootstrap runs it.

Note the terminate app object seen in Phase 25 had vtable 81603CB4 with only
two entries. The real XamApp class uses this much larger vtable around
8166C97C-8166C9C4. They are different classes; the object that terminated was
never XamApp's real implementation.

## Why this matters

XamShowGuideUI sends 0x00021015 to app 0xFE. The code that would handle it
exists at 81A5F220 and is reachable only if XamApp's entry 81A4E3D0 runs and
registers 0xFE. That is the missing link, and it is the eighth instance of
the same pattern: real code, reachable only through a table that something
outside xam populates or walks.

Next: try calling 81A4E3D0 directly. It is invoked from 81A611E8 with
arguments that still need to be recovered, so a naive call may not be enough.

# Phase 28 - driving the XamApp factory

81A34E78 (the factory that calls XamApp's constructor three times) was run
first inline, then on its own guest thread in the system process.

Results:

```
crashes=0   terminate=0
Guide: XamApp factory 81A34E78 (own thread)
Guide: app FE entry @81D426D0: all zero
Guide: XamShowGuideUI returned 0000065B
```

**terminate=0 is the useful signal**: 81751428 always produced exactly one
ExTerminateTitleProcess, and this path produces none. So the factory is
genuinely a different path, not another route into teardown.

But it does not register 0xFE. Tracing the thread shows why it cannot have
gotten far - it emits exactly two lines:

```
XThread::Execute thid 36 (handle=01000028, 'XamApp')
Guide: XamApp factory 81A34E78 (own thread)
```

and nothing after. No kernel calls at all. A message pump would show waits;
this shows silence, so the factory blocks or spins inside guest code almost
immediately.

Worth correcting an assumption from the inline attempt: "it blocks, therefore
it is running the app's message pump" was a guess, and the thread trace does
not support it. Blocking with zero kernel activity is more consistent with a
spin or a lock taken with nothing to release it.

## Harness note

Two runs in this phase failed with empty output and exit code 1 - the build
step never executed, so no log existed. That looks identical in a summary
table to "the emulator produced nothing". Runs now print the pid and the exit
path so "did not start", "exited early" and "killed at timeout" are
distinguishable.

# Phase 29 - "callerless root" does not mean "callable with no arguments"

81A34E78 is not a parameterless entry point. Its first instructions are:

```
81a3c0a0  stw r8,0(r3)      ; vtable -> [this]
81a3c0a8  stw r11,16(r3)
81a3c0b0  stw r10,8(r3)
81a3c0b8  stw r9,20(r3)
```

It is a **constructor taking `this` in r3**. Every call made to it in Phase 28
passed r3 = 0, so it wrote a vtable and a dozen fields to **guest address 0**.
Xenia maps low memory, so there was no fault - which is exactly why the thread
went quiet with no kernel calls. It was not spinning on a lock; it was
scribbling over the bottom of guest memory and then walking off into whatever
followed.

This invalidates the Phase 28 reading ("blocks or spins inside guest code")
and, more importantly, undermines the technique that has driven the last
several phases. "No internal callers" was being treated as "safe to call with
zero arguments". For static registrars that happened to hold - the XUI class
registrars build their descriptor on the stack and only read r3 as a value -
but it is not true in general, and there was no check distinguishing the two.

Any root considered for direct invocation must first be checked for stores
through r3 (or any argument register) before use. Where it is a constructor,
an object of the correct size has to be allocated and passed; that size is not
yet known for XamApp.

The call is now guarded and disabled rather than left to corrupt address 0.

## Consequence for earlier results

Runs in Phases 28 that enabled lle_xam_sysapp_init wrote to guest address 0
before reporting their numbers. Their "app FE entry all zero" and
"XamShowGuideUI 0x65B" lines are still consistent with every other run, but
those runs should not be treated as clean measurements.

# Phase 30 - status, and the limits of the current approach

## Verified state

```
dash.xex renders                                   (profile + sync shaders)
LLE xam boots                                      0 heap errors, 0 crashes
38 XUI classes registered, registry populated      @81D6D508
hud.xex loads, DllMain completes                   Guide object @401EA710
XamShowGuideUI resolves and executes               -> 0x65B
  because XMsgStartIORequest cannot find app 0xFE
XamApp's real code located                         entry 81A4E3D0,
                                                   handler 81A5F220 (0x1015)
```

## Where it stops

XamApp's factory 81A34E78 blocks immediately. With a valid `this` (a zeroed
4 KiB buffer) the thread still emits exactly two log lines and then nothing,
with no kernel calls at all. So the null-pointer bug fixed in Phase 29 was
real but was not the cause of the silence.

Blocking with zero kernel activity means guest code that neither waits on a
kernel object nor calls out - a spin on a memory location, most likely a lock
or a state flag that something else is expected to set. Nothing in the current
bootstrap sets it.

## The pattern, stated plainly

Nine separate entry points in xam have no callers anywhere inside the module:

```
817BBD70  heap init
81958948  XamRegisterSysApp (export)
819A3008  XUI class registrar group
81751428  teardown root
8177FE50  system-app table walker
81A34E78  XamApp factory
81A4E3D0  XamApp entry (vtable slot 23)
81765F88  terminate-app path root
81786440  XamRegisterSysApp internal
```

Some are exports; some are reached only through vtables or tables. All of them
are driven by something outside xam on hardware. Loading xam as a plain guest
module and calling DllMain reproduces none of that sequence, and each one
found so far has had to be driven by hand.

That is why the Guide does not open: not a missing export, not a bug in xam,
but an initialisation sequence that the real system performs and this
bootstrap does not.

## Honest limits

Driving roots by hand has worked where the target is a self-contained static
registrar (the XUI classes: 38 of 38 registered). It has not worked where the
target participates in xam's own startup ordering - the heap, the system app
table, XamApp - because those expect state that earlier steps establish.

# Phase 31 - the factory completes; sizing was the blocker

## The "block" was our own undersized buffer

Two earlier explanations for the XamApp thread's silence were wrong:
"it is running the app's message pump" (Phase 28) and "it spins on a lock or
state flag" (Phase 30). The real cause was the allocation.

The factory addresses sub-objects with addis/addi **pairs**:

```
addi  r3,r31,10832 / 17880 / 24928 / 31976 / 32416
addis r3,r31,0x0001     ; +65536
addi  r3,r3,-32628      ; net +32908
addi  r3,r3,-32136      ; net +33400
addi  r11,r11,-31584    ; net +33952
```

A scan of small `stw` displacements reported 76 bytes. Tracking registers
derived from r31 gives the true span: **33956 bytes (0x84A4)**. The 4 KiB
buffer was short by 29864 bytes, so the constructors wrote ~30 KB past it.
Nothing faulted - Xenia's heap absorbed the writes - which is why it read as
a hang rather than corruption.

With 64 KiB allocated:

```
Guide: XamApp factory 81A34E78 this=301A6000
Guide: XamApp factory returned          <-- completes, first time
lines 4628 -> 10255
```

## Entries run, but do not register

The factory builds three XamApp instances at this+10832, +17880, +24928.
Running 81A4E3D0 on each:

```
XamApp entry this=301A8A50 (+10832) -> 00000000
XamApp entry this=301AA5D8 (+17880) -> 00000000
XamApp entry this=301AC160 (+24928) -> 00000000
app FE entry @81D426D0: still all zero
XamShowGuideUI: still 0x65B
```

All three return success immediately rather than entering a message pump.
81A555D0 locks on this+204, reads this+232 and +236, and branches early when
[this+236] is zero - which it is in a freshly constructed object. So the
entry needs state that construction alone does not provide.

## Method note

Three times now a symptom blamed on xam turned out to be our own harness: the
hand-built Guide message (Phase 18), the null `this` (Phase 29), and the
undersized allocation here. The reflex to ask "what is xam waiting for" before
"what did we hand it" has been the most expensive habit in this investigation.

# Phase 32 - the Guide message is delivered

Registering xam's system apps by hand - writing the table entries that its own
startup would have written - makes XamShowGuideUI succeed for the first time.

The lookup at 817863F8 requires the entry's +8 and +16 to be non-zero with the
handler at +12. Filling those for app 0xFE, pointing +12 at xam's real XamApp
dispatcher 81A5F220 (the function that owns the 0x1015 case), and then walking
xam's static descriptor table at 0x81604368 to register the rest:

```
registered app FE @81D426D0 handler=81A5F220 ctx=301A8A50
registered app FC @81D42850 handler=81AAA160
registered app FD @81D42790 handler=81B15E20
registered app FB @81D42910 handler=81B1B6A0
registered app FA @81D429D0 handler=817D22C0
registered app F7 @81D42C10 handler=81BE8050
registered app F2 @81D42FD0 handler=81B96F30
registered app F3 @81D42F10 handler=81BBFA90
registered app F1 @81D43090 handler=81BD9DC0
registered app F0 @81D43150 handler=81BDBB70
registered app EF @81D43210 handler=81D3F3A0

XamShowGuideUI returned 00000000     <- was 0x65B on every prior attempt
app_misses = 0                        <- 4556 XLiveBase lookups gone
pipelines  = 16                       <- dash unaffected
```

All handler addresses come from xam's own data, not invented.

## Delivered is not processed

xui_lines after dispatch = 0. The Guide message reaches the app table but
produces no UI.

The likely reason is in the name: **XMsgStartIORequest** queues an
asynchronous request. Whatever consumes that queue is the app's message pump -
and the XamApp entry 81A4E3D0 returns immediately in our runs rather than
pumping (it branches early when [this+236] is zero). So the message is very
plausibly sitting in a queue with nothing draining it.

That is a hypothesis, not a finding: it fits the evidence (successful
delivery, zero handler-side activity) but has not been confirmed by observing
the queue itself.

## Status against the goal

```
dash renders                              yes
LLE xam boots clean                       0 heap errors, 0 crashes
38 XUI classes registered                 yes
hud.xex loads, Guide object built         yes
all system apps registered                yes, 0 app-id misses
Guide message delivered                   yes, XamShowGuideUI -> 0
Guide renders                             no
```

# Phase 33 - the pump runs and waits; the queue link is missing

81A66420 (rt 81A5F220) is not just a dispatcher, it is XamApp's **run**
function: it initialises (81A63BB8 with 4097, 81A63D28, 81A64438) and the
0x1015 comparison sits inside the message loop further in. That is the
consumer for what XMsgStartIORequest queues - the pump the Phase 32
hypothesis said was missing.

Run on its own thread with a constructed XamApp instance and a zeroed
2060+ byte scratch buffer:

```
XThread::Execute thid 37 ('XamApp pump')
Guide: XamApp pump 81A5F220 this=301A8A50 buf=301B6000
Added handle:F800029C for class xe::kernel::XObject
<blocks>
```

Three lines: it starts, creates one kernel object - an event - and waits.
That is exactly what a message pump should do, and it is the first time
XamApp's real run function has executed here.

It never wakes. XamShowGuideUI still returns 0, so the message is accepted,
but the pump is not signalled.

## What that says about the hand-registration

The Phase 32 entries set +8 (context), +12 (handler), +16 (non-zero) and +24
(app id) - the fields the *lookup* validator 817863F8 checks. A real entry
written by xam's own startup evidently carries more: at minimum the queue or
event that XMsgStartIORequest signals. Satisfying the validator was enough to
make delivery report success; it is not enough to make delivery actually
arrive.

So "XamShowGuideUI returned 0" means the lookup succeeded, not that the
message reached the handler. Worth stating plainly, because it is easy to
read a success code as more than it is.

## Status

```
dash renders                      yes
LLE xam boots clean               yes, 0 errors 0 crashes
38 XUI classes registered         yes
hud loads, Guide object built     yes
all 11 system apps registered     yes, 0 app-id misses
XamShowGuideUI succeeds           yes (lookup succeeds)
XamApp run function executes      yes, reaches its wait
Guide message processed           no - pump never signalled
Guide renders                     no
```

# Phase 34 - LLE xam breaks the dashboard (verified visually)

Every "dash renders" claim in this document before now came from log proxies -
pipeline counts, shader counts, absence of "Skipping draw". Capturing actual
guest output with Xenia's own F12 screenshot (which grabs the guest
framebuffer, not the desktop) shows those proxies were misleading.

```
baseline, HLE xam        full NXE dashboard   381064 bytes
--lle_xam=GAME:\xam.xex  grey gradient only   440991 bytes
+ XUI class init         grey gradient only   440991 bytes (identical)
+ everything else        grey gradient only   440991 bytes (identical)
```

Screenshots kept at research/shots/.

The baseline renders the complete interface: home/social/games/apps/settings
tabs, Play Game / My Pins / Recent tiles, the Xbox Live panel, the
Friends / Activity Feed / Avatar Store sidebar, the A/X/Y footer, gamertag.

**Loading LLE xam alone destroys it.** Not the XUI registration, not the
app-table writes, not the XamApp construction - the byte-identical output
across configs A, B and the full setup proves the regression happens at
`--lle_xam=` and nothing after it changes the picture.

## Why this makes sense

With LLE xam, dash's xam imports bind to the real guest xam instead of
Xenia's HLE implementation. Real xam is the module whose initialisation this
whole investigation has been unable to reproduce - nine callerless roots, none
of which a plain DllMain drives. So dash asks real xam for the services that
draw its UI and gets a module that was never brought up.

pipelines=16 stayed constant across all of this because those pipelines are
created early and keep being created whether or not the dashboard draws. It
was never evidence for the thing it was being used to prove.

## Consequence for the goal

The stop condition was "I wanted them to render". The honest position:

- dash renders **under HLE xam** (verified visually, baseline screenshot)
- dash does **not** render under LLE xam
- the Guide does not render under either

LLE xam currently trades a working dashboard for a broken one. Any future work
has to fix xam's initialisation, not add more hand-driven roots on top of it.

# Phase 35 - what happens when you press the Xbox button (visually verified)

Tested against the configuration where the dashboard actually renders (HLE
xam baseline), not the LLE configuration where nothing draws. The earlier
attempt at this question was run under LLE xam and was therefore meaningless -
there was no dashboard on screen to change.

Method: boot dash, capture guest output, send the Guide key, capture again.

```
winkey: "0x08" binds key 0x8 to controller input GUIDE     <- binding is live
press Backspace (0x08)
log grows 121795 -> 140017 lines (normal dashboard activity, 5 s)

md5 press_BEFORE.png  a6eb4cfaf079ff36f37f18916c9fb5d3
md5 press_AFTER.png   a6eb4cfaf079ff36f37f18916c9fb5d3
md5 baseline_out.png  a6eb4cfaf079ff36f37f18916c9fb5d3
```

**Byte-identical.** Pressing the Xbox Guide button on a fully rendering
dashboard changes nothing at all - not one pixel.

No guide handling appears in the log either; every "guide" hit is a config
line (guide_button, keybind_guide, guide_hud_path, guide_message,
guide_subcommand) plus XamShowNuiGuideUI sitting unused in dash's import
table.

## Why

This confirms the static reading from earlier phases:

- the key binding works - Xenia maps 0x08 to controller input GUIDE
- `SetGuideButtonAction` has **zero callers** in the entire codebase
- so `onGuidePressFunction_` is always null and the press is swallowed
- Xenia's source marks the spot `// GUIDE BUTTON - More info needed`

The button is wired as far as the input layer and then goes nowhere. There is
no Guide for it to open, because opening one requires hud.xex and XUI running
inside an initialised xam - which is the whole unresolved problem.

Answer, in one line: **the input is recognised and then discarded; nothing
happens, verified pixel-for-pixel.**

## Phase 35b - the LLE regression is not a timing artefact

Phase 34's conclusion rested on a single screenshot taken at a fixed 70 s.
Since the LLE run logs 3.5x more than the baseline (8989 vs 2581 lines), "real
xam is just slower and the UI arrives later" fitted the evidence equally well,
and would have made the conclusion wrong.

Sampling the same LLE configuration at three points:

```
t=90s   440991 bytes   md5 efb9b06cc3a69106fa74f9060737034b
t=150s  440991 bytes   md5 efb9b06cc3a69106fa74f9060737034b
t=210s  440991 bytes   md5 efb9b06cc3a69106fa74f9060737034b
baseline               md5 a6eb4cfaf079ff36f37f18916c9fb5d3
```

Byte-identical across 3.5 minutes. The dashboard never appears. LLE xam
genuinely breaks it.

Note also what the diff did *not* show: no errors in either run, an identical
set of Xam*/Xex*/Xe* calls, the same 77 XamUser/XamContent calls, and the same
single module loaded. Whatever differs is in what those calls return, not in
which ones dash makes - real xam answers differently than Xenia's HLE xam, and
dash draws its background but never builds its UI.

# Phase 36 - why LLE xam breaks the dashboard

## Correcting the previous comparison

Phase 35b reported "an identical set of Xam*/Xex*/Xe* calls" in both configs.
That was wrong. Those matches are **import-table listings** (`F 92000400
92939D84 210 ( 528) XamUserGetSigninState`), which are naturally identical
because it is the same dash.xex. At log_level=2 actual call logging is off
entirely, so the comparison measured nothing. Another proxy mistaken for the
thing itself.

## The real comparison

At log_level=3, counting actual HLE invocations (`d> ... Xam...`):

```
baseline (HLE xam)   20916 HLE Xam calls
LLE xam                  0 HLE Xam calls
```

dash's most frequent calls in the working configuration:

```
11929  XamLoaderGetMediaInfo
 5214  XamInputGetKeystrokeEx
 1879  XamGetDashContext        <- dash-specific
  850  XamNuiIsDeviceReady
  850  XamNuiGetDeviceStatus
   49  XamUserGetXUID
```

**Under LLE xam, dash's imports bind to real guest xam and Xenia's HLE xam is
bypassed completely.** Not partially - zero calls reach it.

So the dashboard is not asking a broken subsystem for help; it is asking a
different implementation entirely. Real xam answers those 20916 calls with
whatever an uninitialised module returns, and dash draws its background but
never builds its UI.

That is the whole regression, stated exactly: substituting xam is not a
drop-in change. Every service dash relies on - media info, dash context, input
polling, NUI status - moves from Xenia's working reimplementation to a real
module that this investigation has never managed to bring up.

`XamGetDashContext` is the obvious first suspect: 1879 calls, dash-specific by
name, and precisely the kind of state a dashboard would need before it can
build a scene.

## Phase 36b - XamGetDashContext is not the cause; the profile is

XamGetDashContext was named the obvious suspect on the strength of 1879 calls
and a dash-specific name. Wrong. The log shows dash calling
`XamSetDashContext(00000000)` once and then reading it back - the value is
zero, and real xam would return zero too. It cannot be the differentiator.

The profile is. In the working baseline dash reads it through HLE xam:

```
49  XamUserGetXUID
19  XamUserGetUserTenure
 8  XamUserGetSigninInfo
 8  XamUserGetMembershipTierFromXUID
 2  XamUserReadProfileSettingsEx
 2  XamUserReadProfileSettings
 2  XamUserGetGamerTag          <- the gamertag visible in the screenshot
```

Under LLE xam none of these reach Xenia; they go to real guest xam.
`logged_profile_slot_0_xuid` is a **Xenia cvar** - Xenia's profile emulation
lives entirely inside its HLE xam. Real xam has never heard of it, has no
profile data of its own, and was never initialised to load any.

This matches the very first blocker found in this project: with no profile
signed in, dash does not render. Substituting xam re-creates that condition,
because the profile support is part of what gets substituted away.

Stated as confidence: this is a well-supported inference, not a direct
observation. The evidence is the 20916-to-0 call split, the specific profile
calls dash makes in the working configuration, and the known dependency of
dash's UI on a signed-in profile. Real xam's actual return values for those
calls have not been observed.

## What that means for the approach

LLE xam cannot be a drop-in substitution while Xenia's profile, media and
input emulation live inside HLE xam. Either those services have to be
provided to real xam (a profile it can actually read), or the substitution has
to be partial - real xam for the Guide/XUI paths, HLE xam for everything dash
depends on. The second is closer to what the hud.xex work in Phase 13 was
already doing by accident.

# Phase 37 - partial substitution: the fix

The LLE override in SetupLibraryImports applied to **every** module importing
xam.xex. Nothing distinguished the importer, so dash.xex was rebound to real
xam along with hud.xex - which is the entire Phase 34 regression. The comment
on that code claimed "XamModule stays registered so host-side services keep
working", which is wrong: the guest imports bypass those services completely.

Scoping the override by the *importing* module (`name_`, defaulting to hud):

```
LLE xam: importer dash -> HLE xam
LLE xam: importer hud  -> REAL xam
LLE xam: importer xbdm -> HLE xam

md5 scoped2.png       a6eb4cfaf079ff36f37f18916c9fb5d3
md5 baseline_out.png  a6eb4cfaf079ff36f37f18916c9fb5d3   identical
HLE Xam calls  21162  (baseline 20916)
hud.xex loads, DllMain completes, XUI classes register
```

**The dashboard renders and hud has real xam at the same time.** That is what
Phase 1's "Route B" actually required; it had been implemented too broadly and
the cost was invisible until guest output was captured.

## A near-miss worth recording

The first attempt matched `find_name_from_guest_path(name_)` against
"hud.xex". Module names arrive without the extension - "hud" - so nothing ever
matched and the override was off for *everyone*. The dashboard came back and
the numbers looked right, but for the wrong reason: LLE was simply disabled.
Only the per-importer log line ("importer hud -> HLE xam") exposed it.

Without that line this would have been reported as a success. Logging the
decision, not just the outcome, is what caught it.

# Phase 38 - full stack on a working dashboard

Everything enabled at once, on the scoped configuration where dash renders:

```
crashes = 0
HLE Xam calls = 24720          (dash healthy, baseline 20916)
importer dash -> HLE xam
importer hud  -> REAL xam
38 XUI classes registered
app 0xFE + 10 more registered from xam's own descriptor table
XamApp constructed (33956-byte object) and its pump running
XamShowGuideUI returned 00000000

md5 full.png          a6eb4cfaf079ff36f37f18916c9fb5d3
md5 baseline_out.png  a6eb4cfaf079ff36f37f18916c9fb5d3   IDENTICAL
```

The whole stack now runs without breaking anything - and the Guide still does
not draw a single pixel.

This is the first time the question has been asked cleanly. Every earlier
"the Guide doesn't render" was measured in a configuration where *nothing*
rendered, so it carried no information. hud.xex is an overlay with no surface
of its own; testing it over a blank screen could never have shown anything.

## What is and is not established

Working, verified visually or by direct state reads:

- dash renders (HLE xam path intact under scoping)
- hud.xex loads, DllMain completes, registers as app 0xFF
- 38 XUI classes registered; class registry populated
- all 11 system apps registered; zero app-id misses
- XamApp constructed; its real run function executes and reaches its wait
- XamShowGuideUI's lookup succeeds

Not established:

- that the Guide message is ever *processed*. The pump creates an event and
  waits; nothing signals it. The hand-written app entries satisfy the lookup
  validator (+8, +12, +16, +24) but omit whatever queue/event field
  XMsgStartIORequest uses to wake a waiting app.

That single gap is the remaining blocker, and it is now isolated: not the
heap, not XUI, not app registration, not the dashboard, not a missing export.

# Phase 39 - what XamApp publishes, and the final gap

Reading app 0xFE's entry after the pump has run separates what we fabricated
from what real xam writes:

```
ours:  +8  = 301BFA50 (ctx)   +12 = 81A5F220 (handler)
       +16 = 00000001         +24 = 000000FE
xam's: +64 = 000000FE         +76 = FFFFFFFF      +92 = 01000000
```

So XamApp does populate its own entry once running - three fields we never
wrote. The hand-registration is not simply overwritten or ignored.

**No kernel handle appears anywhere in the first 96 bytes.** No F8000xxx
value. So the event the pump waits on is not published in the app-table entry;
it lives inside the XamApp object at 301BFA50. Since our entry's +8 points at
that object, the delivery path can reach it - the link is not missing in the
way assumed in Phase 33.

That refines the remaining question rather than answering it: XMsgStartIORequest
finds the app, can reach the object, and returns success, yet the pump's event
is never set. Whether the request is queued somewhere unread, or signalled
through a field inside the object that construction left zero, is not
established.

## Final state of this investigation

Verified by guest-output capture or direct memory reads:

```
dash renders (HLE xam retained via importer scoping)     verified visually
hud.xex loads, DllMain completes, registers app 0xFF     verified
38 XUI classes registered, class registry populated      verified
all 11 system apps registered, 0 app-id misses           verified
XamApp constructed (33956 bytes), run function executes  verified
XamShowGuideUI lookup succeeds, returns 0                verified
Guide renders                                            NO
Xbox button does anything                                NO - pixel-identical
```

The Guide's remaining blocker is one link: the wake path between
XMsgStartIORequest and XamApp's message wait. Everything else on the route -
heap, XUI, class registry, app table, module loading, the dashboard itself -
is working.

# Phase 40 - a visibility limit worth stating

The Guide message id 0x00021015 appears **nowhere** in any log, including at
log_level=3 with everything enabled. That is not evidence of failure - it is a
limit of what can be observed.

XamShowGuideUI is executed as real xam code (resolved by ordinal 0x304 from
the guest module). Its internal call to XMsgStartIORequest is therefore guest
code too, and Xenia never sees it. The only window into that path was real
xam's own DbgPrint on failure:

```
WRN[XAM]: XMsgStartIORequest failed to find app id '0x000000FE (254)'
```

Those messages are now absent, which is the positive signal - the lookup
succeeds. But "XamShowGuideUI returned 0" and "no failure printed" are the
*only* two observations available about that call. Whether a message was
actually queued cannot be seen from outside the guest.

## What the message traffic does show

```
5509  XMsgInProcessCall(0x000000FC)   <- dash -> HLE xam, XLiveBase
   1  XMsgStartIORequest(0x000000FB)
```

All HLE calls, all from dash, which under the scoping fix uses Xenia's HLE
xam. So Xenia's app manager and real xam's app table are **two separate
registries**: the hand-registration in Phase 32 populated real xam's table,
which only hud and XamShowGuideUI consult. dash's XMsg traffic goes through
Xenia's own manager and is unaffected either way.

That is consistent, and worth writing down because it would be easy to read
the 5509 successful 0xFC calls as evidence that the hand-registration is
working for everyone. It is not - those two paths never meet.

## Where a fix would have to come from

Observing the queue itself needs instrumentation inside guest execution -
tracing real xam's XMsgStartIORequest and its wait/signal pair - rather than
kernel-boundary logging. That is a materially larger tool than anything built
here so far.

# Phase 41 - 0x80000008 is teardown too

With XUI classes registered and the dashboard rendering, dispatching
0x80000008 to hud's handler produces:

```
XUI "class not registered" warnings: 0     (was 124 before registration)
Guide: create returned 00000000
Guide: object @91400690 = 401EA600
Guide: dispatch msg=80000008 -> 913E69C0
RtlFillMemoryUlong(40875CE0, 0000000E, FEEDFEED)
<thread stops>
```

Two things follow.

**The XUI registration works.** 124 "class is not registered" warnings became
zero. CBaseScene::Register no longer fails, which was the Phase 14-17
blocker - that part is genuinely fixed.

**0x80000008 is not "show".** FEEDFEED is free-poison and 40875CE0 lies in the
XUI class registry region (alongside 40874CB0, 40873CB0 seen in the registry
dump). The handler frees an XUI class entry and then stops. Phase 17 labelled
this message REGISTER/SHOW because it reaches
XuiGamerCardElement::RegisterClasses; with the classes already present it
takes the other branch and tears them down.

So of hud's message table, two of the cases examined in depth - 0x80000005 and
0x80000008 - are both teardown. The message that *opens* the Guide has not
been identified. The jump table has 15 entries (0x80000002-0x80000010) and
only 04 (create), 05 (destroy) and 08 (register/unregister) have been traced.

That is the honest state of this thread: the class-registration blocker is
solved, and the message that shows the Guide is still unknown.

# Phase 42 - the full message table, swept properly

Re-ran every reachable message in hud's jump table under the working
configuration (scoped LLE so dash renders, 38 XUI classes registered):

```
msg        return    teardown   framebuffer
80000002   00000000  no         (capture missed)
80000004   00000000  no         creates the Guide object
80000005   00000000  YES        destroys it (FEEDFEED on the object)
80000006   00000000  no         identical to baseline
80000007   00000000  no         identical to baseline
80000008   00000000  no*        frees an XUI class entry
80000009   00000000  no         identical to baseline
8000000A   00000000  no         identical to baseline
80000010   00000000  no         (capture missed)
```

The two missed captures were F12/focus failures, not hangs - both logs show
`handler returned 00000000`, no crashes, and the same 4121 lines as the
others.

**No message dispatched directly to hud's handler produces any visible
change.** Every one returns success and leaves the framebuffer byte-identical
to the dashboard alone.

The earlier sweep of these same messages was worthless - it ran with XUI
unregistered and dash drawing nothing. This one is a real negative result.

## What that implies

hud's message handler is reachable and responsive, its Guide object is
constructed, and its XUI classes are registered - and it still draws nothing.
So driving hud directly is not sufficient. Either the Guide UI is driven by
something else after the message (a render tick, a scene push, a frame
callback that nothing here calls), or it genuinely requires the XamApp/XMsg
pipeline whose wake path is the outstanding gap.

Both remain possible. What is established is that the message table itself has
been exhausted: create and destroy work, three cases are teardown or no-ops,
and none of the fifteen entries opens a visible Guide.

# Phase 43 - hud renders itself, and gets one call in

hud's import list settles how the Guide is supposed to draw. It imports the
XUI renderer directly:

```
XuiInit / XuiUninit
XuiRenderCreateDC / XuiRenderDestroyDC / XuiRenderUninit
XuiRenderBegin / XuiRenderEnd / XuiRenderPresent
XuiRenderGetBackBufferSize
XuiSceneCreate, XuiElementLayoutTree, XuiProcessInput
```

So hud is not composited by something else - it creates its own device
context and presents its own frames. That is why no amount of message
dispatching produced pixels: the messages build scene state, but drawing is
hud's own job through this renderer.

Tracing graphics calls on the Guide thread:

```
Guide thread Vd calls:  1  (VdQueryVideoMode)
whole-run Vd calls:     350 VdGetSystemCommandBuffer, 40 VdGetCurrentDisplayGamma,
                        8 VdQueryVideoMode, 5 VdSetSystemCommandBufferGpuIdentifierAddress
```

hud's path queries the video mode once and stops. It never acquires a command
buffer and never presents. So the renderer begins initialising and does not
complete - XuiRenderCreateDC is the obvious place for that to fail, since it
needs a graphics device that real xam, brought up by hand rather than by the
system, does not have.

## The complete picture

The Guide's route, end to end, with the state of each link:

```
hud.xex loads, DllMain completes                  works
registers as system app 0xFF                      works
Guide object constructed in xam's heap            works
XUI classes registered (38), registry populated   works
message handler reachable, all 15 cases return 0  works
XUI renderer initialisation                       STOPS HERE
  VdQueryVideoMode ......................... called
  command buffer / present ................. never reached
```

Everything up to the renderer works. The renderer is where it ends.

# Phase 44 - the "!!" markers are misleading; the binding is real

hud's import listing shows 116 of 156 entries flagged `!!`, including every
Xui* function:

```
F 913E05D0 913FE7E4 340 ( 832) !! XuiInit
F 913E05D4 913FE7F4 34C ( 844) !! XuiRenderCreateDC
```

That is Xenia's **HLE table** status, printed while listing imports. It does
not reflect the LLE resolution. The proof is negative and clean: the run
contains no `undefined extern call to ... Xui*` anywhere. Unbound imports
produce exactly that message - it fires for XamLogLocalizationEtx,
XamProfileGetLiveLegalLocale, XamPushBackURI and others - and not one Xui
function appears in that list.

So hud's XUI imports really are bound to real xam. The scoping fix works at
the level it was meant to.

## Which sharpens the last question

hud's XUI calls reach real xam, real xam's renderer is present, the classes
are registered, the scene objects construct - and the Guide thread still
issues a single VdQueryVideoMode and nothing more. The renderer is reachable
and does not proceed.

What has not been determined is whether hud ever calls XuiInit /
XuiRenderCreateDC at all. The Guide thread emits only two lines after a
message dispatch, so the render path may simply never be entered from the
message handler - the draw loop could belong to a thread hud expects the
system to create, which nothing here does.

That distinction - renderer failing versus renderer never invoked - is the
next thing to settle, and it needs visibility inside guest execution rather
than at the kernel boundary.

# Phase 45 - the renderer is never invoked

The open question was whether hud's XUI renderer fails or is never entered.
Checking thread creation after hud loads:

```
threads created after "Guide: loading GAME:\hud.xex":  none
```

hud does not spawn a render thread. Nothing drives its draw loop, so
XuiInit / XuiRenderCreateDC / XuiRenderBegin / XuiRenderPresent are never
called. The single VdQueryVideoMode on the Guide thread comes from the
message dispatch path, not from a renderer starting up.

**The renderer is not failing. It is never invoked.**

On hardware the system creates the Guide's thread and runs its message and
render loop; hud is a system app that expects to be *hosted*. Our bootstrap
loads the module and calls DllMain, which registers the message handler and
nothing more. Dispatching messages to that handler updates scene state, and
with no loop to render it, nothing reaches the screen - exactly what the
message sweep showed.

This is the same shape as every other blocker in this investigation, and the
tenth instance of it: xam and its system apps expect an initialisation and
hosting sequence that loading a module and calling DllMain does not
reproduce.

## Final answer for the Guide

The route is complete and understood end to end:

```
hud.xex loads, DllMain completes                  works
registers as system app 0xFF                      works
Guide object constructed in xam's heap            works
38 XUI classes registered, registry populated     works
message handler reachable, all 15 cases return 0  works
draw loop                                         NEVER STARTED
```

Every link works except the last, and the last is missing rather than broken.

## Phase 45b - an inconclusive search, recorded as such

An attempt to find hud's draw loop by locating callers of the XuiRenderBegin
and XuiRenderPresent thunks returned "no callers" for all four render imports.
That is not a finding - the address mapping was wrong.

hud's import listing gives two addresses per entry, e.g.

```
F 913E05F4 913FE874 34B ( 843) !! XuiRenderBegin
```

913FE874 maps to raw 0x1E874, which is past the end of .text (raw 0x5200 +
size 0x19094 = 0x1E294). So the scan searched .text for branches to an address
that does not lie in .text, and found none by construction. The earlier
XamRegisterSysApp lookup worked only because its thunk at raw 0x1E284 falls
just inside that boundary.

Whatever those addresses are - a separate stub region, or the second column
meaning something other than a code thunk - resolving hud's draw loop needs
the import layout worked out properly first.

Recording this because "no callers for the renderer" would be a dramatic and
completely false conclusion to leave in the notes.

## Phase 45c - static tooling exhausted on hud's import layout

Three attempts to find hud's draw loop, all returning nothing:

1. branches to the second listed address (913FE874) - that address is in
   .data, not .text, so the search was structurally void
2. loads from the first listed address (913E05F4, .rdata)
3. loads from the second listed address (913FE874, .data)

The import listing gives two addresses per entry and their sections differ by
import kind:

```
XamRegisterSysApp   913E0478 (.rdata)   913FE284 (.text)   <- code thunk
XuiRenderBegin      913E05F4 (.rdata)   913FE874 (.data)   <- data slot
```

So some imports resolve through a .text thunk and others through a .data slot,
and hud's references to the latter are not lis/addi + lwz pairs against either
address. They may be reached via a table base held in a register across
functions, or through a dispatch helper, neither of which this scanner models.

**This is where the static approach runs out.** Finding hud's draw loop needs
either a proper XEX import-table parser (reading the import descriptors rather
than pattern-matching instruction sequences) or tracing guest execution.

Recorded as a limitation, not a finding. "hud has no draw loop" would be the
false conclusion here; the correct statement is that this tooling cannot
locate it.

# Phase 46 - hud's renderer initialises and draws; still no pixels

Locating hud's own XUI entry points required searching .text in **flat**
address space for branches to the import thunks. Converting to PE-VA space and
comparing there had failed three times, because Xenia maps the XEX basefile
flat while the PE section table describes a different layout.

Reading Xenia's own import code settled the record format: record_type 0 is a
variable (a .rdata value slot), record_type 1 is a thunk (code). So
913FE874 is genuinely a code thunk even though it falls in the region the PE
header calls .data - the section-based reasoning that rejected it was wrong.

```
hud XUI init:   base + 0xA898   calls XuiInit + XuiRenderCreateDC
hud draw loop:  base + 0xAB28   calls XuiRenderBegin + End + Present
```

Both take `this` in r3 and only read from it, so they are safe to drive.
Called against the Guide object the create message produces:

```
Guide: hud XUI init 913EA898 this=401EA600
Guide: hud XUI init returned 00000000        <- XuiRenderCreateDC SUCCEEDS
6000-frame draw loop, no crashes
capture at t=70s (loop still running): md5 A6EB4CFA = baseline
```

**XuiRenderCreateDC does not fail.** Phase 43 guessed it would, for lack of a
graphics device; that guess was wrong. It was simply never being called,
because nothing drives hud's render path.

Now it is driven, it initialises cleanly, it runs thousands of frames - and
the presented framebuffer is unchanged.

## What that leaves

hud renders into a device context it created, and whatever it presents is not
reaching the guest's front buffer. Plausible reasons, none verified:

- the DC targets a surface that is never composited into the title's frame
- the Guide scene is empty because no scene was pushed (XuiSceneCreate is
  imported but its call site has not been traced)
- presentation requires the system compositor that hosts system apps

The honest summary is that the render path now executes end to end and
produces nothing visible, which is a different and better-characterised
failure than "the renderer is never invoked".

# Phase 47 - the scene is never created

With hud's entry points locatable, the picture completes:

```
XuiSceneCreate         5 call sites: 913E91E8, 913EB508, 913EB940,
                                     913EC6D0, 913F1EF0
XuiElementLayoutTree   1 call site:  913EAB28   <- the draw loop
XuiProcessInput        2 call sites: 913EA208, 913EAAE0
XuiCreateObject        2 call sites: 913E91E8, 913E96F8
```

**None of the five scene creators is the init or the draw function.** The draw
loop lays out and renders whatever scene is attached; scene creation is a
separate step, reached from somewhere this bootstrap never calls.

So the render path executing and presenting nothing is exactly consistent:
hud initialises its renderer, lays out an empty tree, and presents an empty
frame, 6000 times.

## Why this stops here

The scene builders take four or five arguments and dereference them -
913E91E8 reads through r5, 913EB508 reads [r3+8]. They need real parameters:
scene names, parent elements, resource locators. Calling them with invented
arguments is precisely what produced the address-0 corruption in Phase 29 and
the 30 KB heap overrun in Phase 31.

The correct route is not to guess arguments but to reach these functions the
way hud does - which returns to the same conclusion the investigation has
reached repeatedly: hud is a system app that expects to be hosted, and the
hosting sequence (create the app's thread, run its message loop, let *it*
build its scenes in response) is what is missing. Driving individual entry
points by hand has now been taken as far as it goes.

# Phase 48 - hud's draw is a virtual method, and that closes the picture

hud's draw function 913EAB28 has **zero callers inside hud**, and hud exports
nothing at all:

```
Export Table: 00000000
.edata contents: 4E800420 (bctr) / 7D6903A6 (mtctr) ... - import stubs,
                 not an export directory
```

A function that nothing in the module calls and that the module does not
export is reached through a **vtable**. hud's draw is a virtual method,
invoked by whatever holds a pointer to hud's object - the system.

That closes the structural picture and explains every observation:

- hud registers a message handler via XamRegisterSysApp and otherwise waits
- the system constructs/holds its object and calls its virtual methods
- scene construction happens in response to messages arriving through that
  hosting relationship
- the draw method renders whatever scene state exists

Driving 913EAB28 by hand was therefore the right *shape* of action - it is
what the host does - but with no scene built it renders an empty tree, which
is exactly what was observed: clean init, 6000 frames, no pixels.

## The single-sentence answer

hud.xex cannot draw the Guide without being hosted, because hosting is not a
formality here - it is the mechanism by which its scenes get built and its
virtual methods get called. Every individual piece works when driven directly;
the composition is what is missing, and the composition is xam's job.

That is the same conclusion reached from the heap, from XUI class
registration, from the system app table, from XamApp, and now from hud's
renderer - five independent routes to one structural fact.

# Phase 49 - the Xbox button: the complete answer

Pressing the Guide button does nothing, and there are **two** independent
reasons, not one.

**1. With keyboard input the press never reaches the detection code.**
ImGuiDrawer::UpdateGamepads() enumerates controllers and explicitly excludes
the keyboard:

```cpp
// Special case to skip keyboard being set in gamepad mode.
if (caps.gamepad.buttons == 0xFFFF &&
    caps.vibration.left_motor_speed == 0 &&
    caps.vibration.right_motor_speed == 0) {
  continue;
}
...
if (!is_gamepad_connected) {
  io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
  return;      // <-- returns before the GUIDE check
}
```

So with keyboard-only input the function returns before ever testing for the
Guide button. The winkey driver does map 0x08 to X_INPUT_GAMEPAD_GUIDE - that
part works - but this path discards it.

**2. The action was never registered.** Further down, the Guide check calls
`onGuidePressFunction_`, and nothing in the codebase ever called
SetGuideButtonAction, so the handler was permanently null. The source there is
marked "GUIDE BUTTON - More info needed".

Reason 2 is now fixed: EmulatorWindow registers a handler when the presenter
is set up, routing to Emulator::on_guide_button_pressed(), which logs the
press and reports whether LLE xam and hud.xex are configured. With a real
gamepad the press now reaches that handler.

Reason 1 remains by design - the keyboard is deliberately excluded from the
gamepad path - so a keyboard Guide press still goes nowhere. That is why the
verification run showed zero handler invocations: it was driven by synthetic
keyboard input.

## The answer, complete

Pressing the Xbox button previously did nothing at all, silently, for two
reasons stacked on top of each other. One is now fixed and one is by design.
And even with both resolved, opening the actual Guide requires hud.xex hosted
by xam - which is the structural conclusion this investigation reached from
five independent routes.

# Phase 50 - what happens when you press the Xbox button: final

Four independent reasons, each verified.

**1. The guest never receives it.** Xenia's winkey driver maps the Guide key
only in `GetState`:

```cpp
case ui::VirtualKey::kXInputPadGuide:
  buttons |= X_INPUT_GAMEPAD_GUIDE;      // winkey_input_driver.cc:311
```

`GetKeystroke` uses a separate, text-oriented binding table with no GUIDE
entry. And dash polls the wrong one:

```
XamInputGetKeystrokeEx   4561 calls
XamInputGetState            1 call
```

So the button is mapped into a stream the dashboard does not read.

**2. ImGui's path excludes the keyboard.** UpdateGamepads() skips keyboard
devices by design and returns before the GUIDE check when no real gamepad is
connected.

**3. The action was never registered.** onGuidePressFunction_ was permanently
null - nothing ever called SetGuideButtonAction. Now fixed: EmulatorWindow
registers a handler routing to Emulator::on_guide_button_pressed().

**4. There is no Guide to open.** hud.xex needs to be hosted by xam; every
piece works when driven by hand but the composition is missing.

## Verified answer

Pressing the Xbox button does nothing, and the framebuffer is byte-identical
before and after (md5 a6eb4cfa..., confirmed on a fully rendering dashboard).
Previously the input was discarded silently at three separate layers before
ever reaching a Guide that does not exist. One of those layers now
acknowledges it.

## Phase 50b - the precise reason the GUIDE check never runs

Testing the newly-wired handler with an Xbox Wireless Controller connected
(SDL driver loaded) still produced zero invocations. The reason is upstream of
the keyboard exclusion:

```cpp
if (!dialogs_.empty()) {
  UpdateGamepads();          // imgui_drawer.cc:622-623
}
```

`UpdateGamepads()` holds the GUIDE test, and it is called **only when an ImGui
dialog is open**. With a title running and no Xenia dialog on screen, the
function never executes, so the Guide button is never tested for - regardless
of input device.

This supersedes the earlier explanation. The keyboard exclusion inside
UpdateGamepads() is real, but secondary: the containing function does not run
in the first place during normal use.

So Xenia's Guide-button handling is effectively dialog-only. Wiring
SetGuideButtonAction (Phase 49) is still correct and necessary - the handler
was null - but it will only ever fire while a Xenia dialog is showing. Making
the Guide button work generally would additionally require polling gamepads
outside the dialog path, which is a design change to Xenia's input handling
rather than a fix.

# Phase 51 - the keyboard driver is disabled by default

`keyboard_mode = 0` is **Disabled**:

```cpp
InputType WinKeyInputDriver::GetInputType() const {
  switch (static_cast<KeyboardMode>(cvars::keyboard_mode)) {
    case KeyboardMode::Disabled:    return InputType::None;
    case KeyboardMode::Enabled:     return InputType::Controller;
    case KeyboardMode::Passthrough: return InputType::Keyboard;
  }
}
```

and FilterDrivers drops such drivers before any GetState call:

```cpp
if (driver->GetInputType() == InputType::None) { continue; }
```

So with the default config the keyboard driver **loads, parses its bindings,
and logs them** - `winkey: "0x08" binds key 0x8 to controller input GUIDE` -
while being excluded from the input system entirely.

That startup line was taken throughout this investigation as evidence the
binding was live. It only proves the config was parsed.

## Verification limit - stated plainly

Setting `keyboard_mode=1` (Enabled -> Controller) still produced zero handler
invocations with synthetic keyboard input. Several possibilities remain and
none has been isolated:

- synthetic keybd_event may not reach winkey's window listener the way it
  reaches the window's own key handler (F12 screenshots work, so *some* keys
  arrive)
- window focus may not be reliably held during the injected press
- the guide press may need a modifier/exact-match condition not met here

**So the new PollGuideButton() path is unverified.** It is written against the
documented input API, uses edge detection, and is inert when no handler is
registered - but it has not been observed firing. Anyone continuing should
test with a physical controller's Guide button rather than synthetic keyboard
input.

This is recorded as unverified rather than working. The honest state is that
several real obstacles were found and removed, and whether the button now
reaches the handler has not been demonstrated.

# Phase 52 - honest limits on the Guide-button work

Two problems, both mine rather than Xenia's.

## The poll is in a path that does not run

`PollGuideButton()` was first placed after an early return in
`ImGuiDrawer::Draw()`:

```cpp
if (dialogs_.empty() && notifications_.empty()) { return; }
```

Moving it above that return still produced **no diagnostics at all** - not
even a rate-limited line. So `Draw()` itself is evidently not called when
ImGui has nothing to present; the presenter skips it. Draw() is therefore the
wrong home for an unconditional poll, and the current placement does not work.

## The test harness cannot focus the window

```
focus match = False
```

`SetForegroundWindow` fails from this process (Windows foreground lock), so
injected key presses went to whatever window actually held focus. Any
keyboard-driven test in this investigation that did not explicitly verify
focus is unreliable.

The F12 screenshot captures did succeed, which proves focus was held at those
moments - so the framebuffer comparisons stand. But the Guide-press tests
never verified it, and at least the last several runs demonstrably did not
have focus.

## What this does and does not change

Unchanged, because it comes from reading the code rather than from injected
input:

- `UpdateGamepads()` contains the GUIDE test and runs only when a dialog is open
- `onGuidePressFunction_` was never registered by anything
- `keyboard_mode` defaults to Disabled, so winkey is filtered out of every
  GetState call
- winkey maps GUIDE only in `GetState`; dash polls `GetKeystrokeEx` 4561 times
  and `GetState` once
- hud has no draw loop running, so there is no Guide to open

Changed: the claim that pressing the button was *observed* to do nothing rests
on runs whose input delivery was not verified. The code-level reasons are
solid; the empirical confirmation is weaker than previously stated.

`PollGuideButton()` should be considered **non-functional in its current
placement**, not a working fix.

# Phase 53 - the control experiment: the harness, not the code

The Guide key is now handled in `EmulatorWindow::OnKeyDown`, the same window
key path F12 uses - chosen because F12 demonstrably works (it produced the
guest screenshots used throughout this document). It reads
`cvars::keybind_guide` so it honours the configured binding.

The test included **F12 as a control**, and the control failed too:

```
focus match = False
guide handler fired = 0
F12 control - screenshot log = 0
```

F12 has worked repeatedly in earlier runs. Its failure here, alongside
`focus match = False`, proves that in this run **no keys reached Xenia at
all**. The result says nothing about the Guide handler.

That is what a control is for. Without it, "guide handler fired = 0" would
have been read as "the handler does not work", and the next hours would have
gone into debugging correct code.

## The verification limit, precisely

`SetForegroundWindow` succeeds only sometimes from this environment (Windows
foreground lock). When it succeeds, injected keys work - the F12 screenshots
prove it. When it fails, every keyboard test is void, and it fails silently.

So the Guide key handler in OnKeyDown is **untested**: not shown to work, and
not shown to fail. The three obstacles it is designed to bypass are all real
and established from source (keyboard_mode filtering, the dialog-only
UpdateGamepads, and Draw() not running) - but whether this handler fires has
not been demonstrated.

Anyone continuing should press the key by hand with the window focused. That
is a ten-second check that this environment cannot perform reliably.

# Phase 54 - the Xbox button now works (verified)

Delivering WM_KEYDOWN with **PostMessage** instead of keybd_event bypasses the
focus problem entirely - PostMessage puts the message straight on the window's
queue and does not require the window to be foreground.

```
main hwnd=459886 class=XeniaWindowClass
guide handler fired = 4
Guide button: user=0 lle_xam=off hud=not loaded
Guide button: no hud.xex configured - set guide_hud_path to load it
```

Two presses, both handled. The chain works end to end:

```
WM_KEYDOWN -> XeniaWindowClass -> EmulatorWindow::OnKeyDown
           -> matches cvars::keybind_guide ("0x08")
           -> Emulator::on_guide_button_pressed(0)
```

**The Xbox button is no longer silently discarded.** It was, at every layer,
before this work.

Note the F12 control read 0 here - the screenshot path logs nothing at
log_level=2 unless a notification appears, so that control is inconclusive in
this configuration. The Guide result is a direct positive and does not depend
on it.

## What the button does and does not do

It reports the press and the current configuration. It does **not** open the
Guide, because opening the Guide needs hud.xex hosted by xam - the structural
conclusion this investigation reached from six independent directions.

What it provides is a real, reachable call site. Anyone continuing this work
now has somewhere to hook the Guide-open sequence once hosting exists.

# Phase 55 - what happens when you press the Xbox button (definitive)

Tested in the only configuration where the question is meaningful: dashboard
rendering, LLE xam scoped to hud, hud.xex loaded, 38 XUI classes registered,
and press delivery by PostMessage (which works, unlike focus-dependent
injection).

```
before md5=A6EB4CFA
after  md5=A6EB4CFA          <- byte-identical
guide presses handled = 2
Guide button: user=0 lle_xam=on hud=loaded
```

**The press is received and handled. Nothing appears on screen.**

Every earlier attempt at this question was flawed in at least one way: the
press never reached Xenia (focus failure), or the dashboard was not rendering
(the LLE regression), or hud was not loaded, or XUI classes were not
registered. Each of those made a null result uninformative rather than
negative. This run has all four resolved at once.

## Before and after this work

Before: the press was discarded silently at every layer - winkey filtered out
of GetState by `keyboard_mode=Disabled`, the ImGui GUIDE test running only
with a dialog open, and `onGuidePressFunction_` never registered by anything.
Nothing anywhere observed the button.

After: the press reaches `Emulator::on_guide_button_pressed()`, is logged, and
reports the live configuration. It does not open the Guide.

## Why it still does not open

hud.xex is a system app. Its draw function is a virtual method with no callers
inside the module, and hud exports nothing - the system holds its object,
calls its methods, and delivers the messages that make it build scenes. Driven
by hand, every individual step works: the module loads, DllMain completes, it
registers as app 0xFF, its Guide object is constructed in xam's heap, its XUI
renderer initialises, and its draw loop runs. What is missing is the
composition, and the composition is xam's job.

# Phase 56 - the button drives the Guide sequence (verified)

```
Guide button: user=0 lle_xam=on hud=loaded
XThread::Execute thid 41 (handle=01000030, 'Guide button dispatch')
Guide button: dispatching open to 913E69C0
Guide button: handler returned 00000000
crashes = 0    framebuffer unchanged
```

Pressing the Xbox button now: reaches OnKeyDown, matches the configured
binding, calls Emulator::on_guide_button_pressed(), creates a guest thread in
the system process, dispatches the open message to hud's registered handler at
base+0x69C0, and receives success back.

Two ordering bugs were fixed to get here, both mine:

- `guide_handler_` was published *after* the XUI class registration, which
  takes long enough that a press during startup found it still zero. Moved to
  immediately after the buffers are allocated.
- the first patch attempt matched two `Guide: buffers` log sites - the
  Bootstrap path and the guide_hud_path path - and the assertion caught it
  before the wrong block was edited.

The dispatch runs on a guest thread deliberately: hud's handler is PPC code
and must not be called from the host UI thread.

## Final answer

Pressing the Xbox button runs the Guide open sequence and the handler reports
success. The Guide does not appear, because hud.xex needs to be hosted by xam
to build and draw its scenes. Before this work the press was discarded
silently at every layer and nothing happened at all.

# Phase 57 - the button drives the whole sequence

```
Guide button: user=0 lle_xam=on hud=loaded
XThread::Execute thid 41 ('Guide button dispatch')
Guide button: dispatching open to 913E69C0
Guide button: handler returned 00000000
Guide button: XUI init returned 00000000
crashes = 0    dashboard unaffected
```

Pressing the Xbox button now runs, on a guest thread in the system process:

1. the open message to hud's registered handler (base + 0x69C0)
2. hud's XUI init (base + 0xA898 - XuiInit + XuiRenderCreateDC)
3. hud's render loop (base + 0xAB28 - XuiRenderBegin/End/Present)

Every step returns success. The renderer initialises against real xam and the
draw loop runs. The Guide still does not appear.

This is the correct structure regardless: the sequence is now causally tied to
the button rather than running on a fixed schedule at startup, which is how
the Guide is meant to work and which removes a timing trap that made several
earlier measurements uninterpretable.

## The remaining gap, stated once more

hud lays out and presents an **empty scene**. Scene construction happens in
five functions none of which is the init or the draw path, reached from the
hosting relationship this bootstrap does not reproduce. hud exports nothing
and its draw is a virtual method with no internal callers - the system holds
its object, calls its methods, and delivers the messages that build scenes.

Every individual step works. The composition is missing, and the composition
is xam's job.

# Phase 58 - the hosting API has a name, and Xenia implements none of it

hud imports XamAppLoad; dash imports XamAppRequestLoad and
XamAppRequestLoadEx. Checking Xenia's xam export table:

```
0x244 XamAppLoad              declared, not implemented
0x245 XamAppUnloadSelf        declared, not implemented
0x246 XamAppUnloadStack       declared, not implemented
0x248 XamAppRequestLoad       declared, not implemented
0x254 XamAppLoadPass2SysApps  declared, not implemented
0x299 XamAppRequestLoadEx     declared, not implemented
```

No `_entry` function exists for any of them. **This API is the hosting
mechanism**, and it is the concrete form of the conclusion reached six times
over: system apps are loaded and hosted through calls Xenia does not provide.

Real xam has the implementations. Resolved and driven:

```
XamAppLoadPass2SysApps 0x254 -> 81780B70 -> 00000000, one line of work
XamAppLoad             0x244 -> 81793AA0 -> 80004005 (E_FAIL)
```

0x254 is safe to call - it does `or r30,r3,r3` and builds a stack struct with
no stores through r3 - and it returns success while doing nothing at all. That
fits the name: "pass 2" presupposes a pass 1 that has not run. 0x244 fails
without real arguments.

## Where this leaves the work

The missing piece is no longer vague. It is a named API with six entries,
declared in Xenia and unimplemented, which hud and dash both import and which
exists in real xam. Implementing it - or reproducing the pass-1 state it
depends on - is the route to a hosted hud, and therefore to a Guide that
draws.

# Phase 59 - the hosting API is callable, succeeds, and does nothing

The full system-app cluster in xam's export table:

```
0x247 XamSendMessageToLoadedApps  -> 8177FF58
0x251 XamLoadSysApp               -> 81780158
0x252 XamUnloadSysApp
0x253 XamReloadSysApp
0x254 XamAppLoadPass2SysApps      -> 81780B70
0x24B XamRegisterSysApp           -> 8177F240   (hud uses this, it works)
```

**0x247 resolves to 8177FF58 - the function called "the enumerator" in Phase
19.** Its real name is XamSendMessageToLoadedApps, which fits exactly what it
does: walk apps from 0xFE downward and act on the registered ones.

XamLoadSysApp takes (id, arg), moves r3 into r4, overwrites r3 with an
internal constant, and stores nothing through the incoming r3 - safe to drive.
Driven against three ids:

```
XamLoadSysApp(FF) -> 00000000
XamLoadSysApp(FE) -> 00000000
XamLoadSysApp(F7) -> 00000000
crashes 0, log 4153 lines, framebuffer unchanged
```

All succeed. None does observable work - no module loads, no thread creation,
no growth in the log. Identical to XamAppLoadPass2SysApps, which also returned
success with one line of work.

## Conclusion on the hosting route

These functions are present, callable, and safe, and they return success while
doing nothing. That is the signature of an early-out on unmet preconditions:
they are steps in xam's boot sequence and expect state that sequence
establishes. Calling them individually, out of order, from outside, satisfies
none of it.

This is the same wall reached from every other direction, now at the level of
xam's own published API rather than its internals. Hand-driving cannot
substitute for the boot sequence - which is precisely why the missing piece
was described throughout this work as *composition* rather than any single
absent function.

# Phase 60 - the return code was never evidence

XamLoadSysApp installs a sys-app main at runtime 81780080 whose body is:

```
8178728c  cmplwi r3,0
81787290  bne -> 817872a0
81787294  lis r3,0x8000 ; ori r3,r3,0x4005    ; return E_FAIL for id 0
817872a0  bl 81786078                          ; app-table address for id
817872a4  cmplwi r3,0
817872a8  beq -> 817872bc                      ; not found -> fall through
817872ac  r4 = r5 = r6 = 0
817872b8  bl 81786788                          ; the actual load
817872bc  addi r3,r0,0                         ; <-- r3 = 0, unconditionally
817872c0  return
```

**The function returns 0 whether or not the load did anything.** Every
"XamLoadSysApp(..) -> 00000000" recorded in the previous phase is a hardcoded
zero, not a status. The same is true of the earlier
XamAppLoadPass2SysApps result.

So the correct statement is not "they succeed and do nothing" - it is **"their
return value carries no information"**. What is actually known:

- with the app table empty, the lookup at 81786078 fails and 81786788 is never
  reached
- with the table populated by hand (registration at log line 4106, the load
  calls at 4130 - the order was right), the calls still produced no observable
  work: log grew by one line, no module loads, no threads, framebuffer
  unchanged

Whether 81786788 ran and failed internally, or ran and did something invisible
at this log level, is not established. The return code cannot distinguish
them.

## Correcting the previous phase

Phase 59 concluded "all callable, all succeed, none does work". The first and
third clauses hold. "All succeed" does not - it was reading a constant. That
is the same error as trusting `pipelines=16` or the `winkey` startup line:
a value that cannot vary being read as a measurement.

# Phase 61 - the loader wants an index, and something in the chain is misread

The loader at 81786788 (ghidra 8178D988) opens:

```
8178d98c  or r28,r3,r3          ; save arg
8178d9b0  bl 81814B50           ; zero a 1612-byte struct
8178d9b8  cmplwi r28,0x4
8178d9c0  blt -> 8178d9d8       ; r3 < 4 proceeds
8178d9c4  ...
8178d9d4  bl 81D1651C           ; r3 >= 4: report path
```

**It expects r3 < 4** - a slot or user index, not an app id and not a pointer.

But the sys-app main passes it the result of 81786078, which for a valid id is
the app-table entry *address* (0x81D426D0 for 0xFE). That is far larger than
4, so the loader takes the report path.

Either 81786078's return is not what was assumed, or the sys-app main at
81780080 is not reached the way this reading suggests, or the calling
convention differs from the naive reading. It has not been resolved.

## Stopping point

This is the third layer down inside xam's app loader, and the reading has
outrun what static disassembly can confirm here. Distinguishing these
possibilities needs guest execution tracing - watching the actual register
values at 81786788 - which is the same tool this investigation has now needed
three separate times (the message queue, hud's import layout, and here).

What stands: the app lifecycle API is the hosting mechanism, Xenia declares
all six entries and implements none, real xam has them, they are reachable,
and driving them from outside has not reproduced the boot state they expect.

# Phase 62 - Xenia's breakpoints are inert; the control proved it

Xenia has a guest Breakpoint class (cpu/breakpoint.h) with an
AddressType::kGuest mode and a hit callback, and Processor::AddBreakpoint
installs it without needing debugger mode. That looked like the guest-tracing
tool this work has needed three times.

Installed one at 81786788 (xam's sys-app loader) to resolve the Phase 61
question. It never fired.

**A control saved this from becoming a false finding.** A second breakpoint
was placed on 8178D730 - XamShowGuideUI's worker, which provably executes
because XamShowGuideUI tail-calls it and the log shows its result.

```
loader hits  = 0
CONTROL hits = 0
Guide: XamShowGuideUI returned 00000000     <- 8178D730 definitely ran
```

**Neither fired.** Breakpoints do not work in this build, so "the loader is
never reached" would have been wrong.

## Why

```cpp
bool Processor::Setup(...) {
  // TODO(benvanik): query mode from debugger?
  debug_info_flags_ = 0;
```

Guest code is compiled without debug info, so the JIT emits no breakpoint
checks. `set_debug_info_flags()` exists on Processor but nothing calls it and
no cvar exposes it. The breakpoint infrastructure is present and permanently
inactive.

## The pattern, for the last time

This is the fourth instrument in this investigation that returned a confident
zero while being incapable of returning anything else: `pipelines=16` for
rendering, the winkey startup line for the key binding, XamLoadSysApp's
hardcoded `return 0`, and now breakpoints. In three of those cases a control
or a direct observation caught it; in the first it took capturing the actual
framebuffer, after many turns of reporting a false positive.

Enabling breakpoints means setting debug_info_flags_ before Setup() and
rebuilding - a small change, but one that alters codegen for all guest code,
so it belongs to a separate piece of work rather than the tail of this one.

# Phase 63 - Correction: Phase 62 was wrong. Breakpoints work.

**Phase 62's conclusion is retracted.** Guest breakpoints are not inert, and
`debug_info_flags_` has nothing to do with it. Two errors compounded.

## Error 1: wrong diagnosis

`debug_info_flags_` only gates *disassembly text* capture. The guest->host
source map that breakpoints need is emitted unconditionally - ppc_hir_builder
even says so:

```
// Mark source offset for debugging.
// We could omit this if we never wanted to debug.
SourceOffset(address);
```

And `PreLaunch()` (which sets execution_state_ = kRunning, the gate on
installation) is called unconditionally from KernelState::LaunchModule.

Logging the install proved it outright:

```
LoaderTrace: 8177F588 exec_state=0 patched=1 host=A0801BE4
```

exec_state 0 is kRunning, patched=1, real host address. The UD2 was written.
Installation was never the problem.

## Error 2: the control was at the wrong address

`8178D730` is a **Ghidra** address. Runtime is ghidra - 0x7200 = `81786530`.
The control was placed at runtime `8178D730` - an unrelated function - so of
course it never fired. The project's own address convention, applied to
everything else all session, was dropped here.

Confirmation of the conversion: XamShowGuideUI's runtime address from the
export table is 81787430; +0x7200 = 8178E630, exactly where the prologue
disassembles.

## What actually works, and its one real constraint

A breakpoint on XamShowGuideUI's entry (81787430) fired. So did one 0x40
bytes *into* the function, at the call site, returning real registers:

```
LoaderTrace: CALLSITE 81787470 r3=00000000 r4=00000001 r5=00000000
```

matching the disassembly exactly (`addi r5,0; addi r4,1; or r3,r31`). Both
entry and mid-function breakpoints work.

**But a hit does not resume.** After the entry breakpoint fired, the log lost
the `XamShowGuideUI returned 00000000` line it had always had, preceded by
three `Unable to read thread context for stack walk`. The thread dies at the
hit. That is why the call site never fired while the entry control was armed:
*the control was killing the code under test.* One breakpoint, one hit, one
observation per session.

## The answer the tool was built for

With only the loader breakpoint armed and nothing else interfering,
XamShowGuideUI returns 00000000 normally and **8177F588 is never hit**. The
sys-app loader genuinely does not run in the Guide open path. That negative is
now trustworthy, because the identical build fires at two other addresses.

## On the pattern

Phase 62 claimed a "fourth instrument returning a confident zero." It was one
- but the instrument was sound and I had aimed it at the wrong address, then
blamed the tool and committed the blame. The control did its job (it flagged
that something was wrong); I misread *what* it was telling me. A control that
fails should first indict the setup, not the mechanism.

# Phase 64 - hud builds its scene. We hand it a null context.

With a working tracer, the standing conclusion ("hud is unhosted and inert")
is wrong in an important way.

## The handler, read from the right file

First, a repeat of an old mistake: `work/hud.pe` is a *different build* of hud
than the one in dashroot. Disassembled against it, 913E69C0 had no prologue
and looked like a mid-function address - a 0x10 shift. `work/hud17489.pe` is
the matching extraction, and there 913E69C0 is a clean function entry. Caught
by cross-checking against the address hud's own code registers.

## What message 0x80000004 actually does

```
913e69cc  lis r11,0x8000
913e69d4  ori r27,r11,0x4        ; r27 = 0x80000004
913e69dc  subf. r11,r27,r3       ; r11 = msg - 0x80000004
913e69e0  beq -> 913e6a2c        ; msg == 0x80000004: sub-command switch
913e69e8  cmplwi r11,1           ; msg == 0x80000005: send to [0x91400690]
```

The sub-command path reads its selector out of the caller's block:

```
913e6a2c  lwz r11,4(r29)         ; r29 = r4 = our buffer
913e6a34  if r11 == 0: r30 = 0
          else r30 = [r11+8] or [r11+0] depending on bit 0 of [buf+0]
```

then switches on r30. Sub-command 0 is **not** a no-op - it is a real case:

```
913e6abc  addi r3,r0,1296        ; 0x510 bytes
913e6ac0  bl 913f9f38            ; hud's allocator
913e6ac8  beq -> 913e6adc        ; alloc failed -> bail
913e6ad0  lwz r4,12(r29)         ; context from [buf+12]
913e6ad4  bl 913ec578            ; construct the scene
```

## What actually happens on the button press

Breakpoint on the scene constructor:

```
LoaderTrace: SCENE 913EC578 r3=401EA6A0 r4=00000000 r5=00000001
```

It fires. So:

- **hud's allocator works** - r3 is a real heap pointer, not null. The heap
  work from the earlier phases is holding.
- **hud constructs its scene object.** It is not inert and it is not waiting
  to be hosted before it will do anything.
- **r4 = 0.** The scene's context argument, loaded from [buf+12], is null,
  because the bootstrap passes a freshly zeroed 0x40 buffer.

The scene is built around a null context, which is a sufficient explanation
for a scene that lays out empty. The blocker moves from "unimplemented hosting
API" to "we are passing an empty argument block" - which is ours to fix.

Note 913E91E8, one of the five candidate scene-construction functions found by
static scanning, never fires. 913EC578 - not in that list - is the one on the
live path. Static candidate lists were consistently worse than one breakpoint.

## Next

Populate the argument block: [buf+4] -> a descriptor carrying the sub-command,
[buf+12] -> a real context. The switch cases (0, 1, 2, 3..8, >=9) select
different constructors (913ec578, 913eca00, 913ecc58), so the sub-command
chooses *which* Guide screen is built.

# Phase 65 - The full chain from button press to blank screen

Traced end to end with breakpoints and field reads. Every step is now
observed rather than inferred.

## The chain

1. Button -> `Emulator::on_guide_button_pressed` -> hud's registered handler
   at 913E69C0 with message 0x80000004.
2. The handler reads a sub-command from the caller's block. Breakpoint on the
   scene constructor gives `lr=913ECA18`, so the live path is sub-command **1**
   (not 0): allocate 0x998, call 913ECA00, which calls 913EC578.
3. The object is stored to hud's global Guide singleton at **[0x91400690]**
   and the message is dispatched to it. Both succeed.
4. Our driver reads that global, calls the init at hud+0xA898, then the draw
   at hud+0xAB28 3600 times.

The Guide object is real: `obj=401EA6A0 vtable=913E1CB4`. Its allocator, its
constructor and its message dispatch all work. It is **not** inert, and it is
not waiting on the unimplemented xam hosting API to get this far.

## Where it dies

`hud+0xAB28` is vtable[17] - so that address, guessed earlier by static
scanning, is genuinely the object's draw method. It begins:

```
913eab3c  lis r4,0xff00        ; clear colour 0xFF000000
913eab44  lwz r3,12(r3)        ; the render DC
913eab48  bl 913fe874          ; XuiRenderBegin
913eab50  blt -> 913eabc8      ; failed -> return the error
```

Observed: `draw frame 0 returned 80070057` (E_INVALIDARG), every frame. The DC
at [obj+12] is null.

The init at hud+0xA898 was supposed to create it:

```
913ea8a4  lwz r11,20(r3)       ; [this+20]
913ea8b0  cmpwi r11,0
913ea8b4  beq -> 913ea920      ; zero -> skip the whole body
913ea8b8  lwz r11,12(r3)       ; else if DC null...
913ea8cc  bl 913fe7f4          ; XuiRenderCreateDC(&this->dc)
```

Field dump confirms it exactly:

```
pre-init  +8=00000000 +12=00000000 +20=00000000
post-init +8=00000000 +12=00000000 +20=00000000
```

`[obj+20]` is zero, so the init skips its entire body - **and still returns
0.** That zero was a false success that had been read as "XUI init worked" for
many phases. Fifth instrument in this project to return a confident zero while
being incapable of returning anything else.

## Forcing the gate

[this+20] is only a boolean gate in the init - the register holding it is
reloaded from [this+12] on the very next instruction, so it is never
dereferenced. Forcing it non-zero (cvar `guide_force_render_gate`) is
therefore safe, and moves the failure one layer down:

```
post-init +8=00000000 +12=00000000 +20=00000001
XUI init returned 8000FFFF        <- was 00000000
draw frame 0 returned 80070057
```

The init now really runs and **XuiRenderCreateDC fails**. That is progress:
the error is honest where the previous success was not.

## Next

XuiRenderCreateDC needs XUI to have been initialised against a D3D device.
`XuiInit` is import 913FE7E4 and hud+0xA898 does **not** call it - the earlier
note claiming it did was wrong. So the open question is who calls XuiInit and
with what device. That is the same hosting gap as before, but now pinned to
one named call rather than to "hud needs to be hosted" in general.

[obj+8] is also still zero and the draw uses it right after RenderBegin, so
expect a second failure there once the DC exists.

# Phase 66 - XuiInit runs and fails at 0x80300005

All addresses below are labelled (G) ghidra or (R) runtime; runtime = G - 0x7200
for xam .text. Getting this wrong cost a whole phase earlier.

## Correction to Phase 65

Phase 65 said hud+0xA898 does not call XuiInit. **Wrong.** Scanning hud for
branches to its XUI thunks:

```
913EA8CC  bl XuiRenderCreateDC
913EA900  bl XuiInit            <- inside the init at 913EA898
913EAB48  bl XuiRenderBegin
913EABAC  bl XuiRenderEnd
913EABC0  bl XuiRenderPresent
913EAC24  bl XuiRenderGetBackBufferSize
```

hud does call XuiInit - but *after* XuiRenderCreateDC, whose failure branches
to the error path first. So hud assumes its host initialised XUI before it
runs. The conclusion (a hosting gap) survives; the stated reason was wrong.

## The LLE thunks are real

The import dump marks hud's XUI imports "!!", which reads as "unimplemented".
That flag describes Xenia's HLE table and says nothing about the LLE override.
Reading the thunk code at runtime settles it:

```
thunk 913FE7E4: 3D608195 616B3760 7D6903A6 4E800420   -> XuiInit          81953760 (R)
thunk 913FE7F4: 3D60818F 616BB038 7D6903A6 4E800420   -> XuiRenderCreateDC 818FB038 (R)
thunk 913FE874: 3D60818F 616BAE68 7D6903A6 4E800420   -> XuiRenderBegin    818FAE68 (R)
```

lis/ori/mtctr/bctr into real xam. The binding works.

## Why CreateDC fails

xam's XuiRenderCreateDC (G 81902238):

```
81902258  cmplwi r31,0          ; null out ptr -> 80070057
8190227c  lwz r3,-13960(r10)    ; global at 81D6C978
81902280  cmplwi r3,0
81902290  ori r3,r3,0xffff      ; null -> 8000FFFF
```

The XUI context global at **81D6C978** is null. That is exactly the 8000FFFF
seen from hud's init.

## Calling XuiInit directly

XuiInit (G 8195A960 / R 81953760) accepts null params - both the null case and
the valid-size case branch to the same main path; only `[params] > 0xC` takes
the assert path. It returns 1 early if XUI is already initialised.

Called directly (new cvar `guide_call_xuiinit`):

```
XUI ctx before = 00000000
XuiInit returned 80300005, ctx now 00000000
```

So XuiInit really runs and really fails. Tracing 0x80300005 - only two sites
in all of xam construct it - the relevant one is G 81956370:

```
8195635c  bl 81950d60           ; (G)  R 81949B60
81956360  cmplwi r3,0
81956364  beq -> 8195637c       ; 0 = success, continue
81956368  .long 0x0fe00019      ; twi - assert
81956370  lis r3,0x8030
81956374  ori r3,r3,0x5         ; return 0x80300005
```

and G 81950D60 is a thin critical-section wrapper around G 8194AA40
(R 81943840), which is the call that actually returns non-zero.

Note the `twi` immediately before the error return: with Xenia's
`ignore_trap_instructions` defaulting true, xam's own assertion is discarded
and execution falls through to the error return. The guest is telling us
loudly that something is wrong and the emulator is muting it. Worth a run with
that cvar off.

## Where this leaves the button

Press -> handler -> Guide object built and stored in hud's singleton ->
init -> (gate forced) -> XuiRenderCreateDC -> blocked on a null XUI context ->
XuiInit -> blocked inside G 8194AA40. Every step before that is working.

# Phase 67 - XuiInit succeeds. My own class registration was breaking it.

Addresses labelled (G) ghidra / (R) runtime; runtime = G - 0x7200 for xam.

## Confirming the path with a breakpoint, not an assumption

Phase 66 found the 0x80300005 site by scanning for the constant and *assumed*
it was on XuiInit's path. Confirmed it properly instead. A breakpoint at the
mid-function error site killed the thread without firing its callback (a
mid-function patch), so it was moved to a function entry on the same path,
R 81949B60:

```
LoaderTrace: CSWRAP 81949B60 lr=8194F160 r3=815F3548 r4=00000001
```

lr R 8194F160 = G 81956360, exactly the instruction after `bl 81950d60`. The
path is confirmed, and the caller is identified from real registers.

## The argument names the problem

r3 = 815F3548 is in xam's .rdata (1:1, UTF-16BE):

```
va 815F3548 -> .rdata
utf16be: XuiElement          (next string: XuiControl)
```

So the failing call is a XUI **class registration** for "XuiElement", and the
error path is taken when the lookup *finds* an existing entry:

```
8194aa68  bl 819477e8         ; lookup
8194aa70  bne -> 8194aa80     ; found -> return [entry+8]  (non-zero = error)
8194aa78  addi r3,r0,0        ; not found -> return 0 = success
```

Not-found is success. Found is failure. This is duplicate-registration
detection.

And the duplicate is **mine**: an earlier phase called xam's XUI class
registrars by hand (`lle_xam_xui_init`, 38 classes) because "nothing inside
xam calls them". Something does - XuiInit. Pre-registering them makes XuiInit
fail on the first duplicate.

## Result

With `lle_xam_xui_init` off:

```
XuiInit returned 00000000, ctx now 00000000        (was 80300005)
```

XuiInit now succeeds. A whole earlier phase of work was actively causing this
failure - the fix was to delete an intervention, not add one.

## The next blocker is real, not self-inflicted

XuiInit sets its "initialised" flag at 81D6D0A4 but **not** the render context
at 81D6C978, which is what XuiRenderCreateDC requires. Scanning writers of
that global gives three sites; one is the uninit (G 81901F98), the other two
are inside exports:

```
G 81906478 (R 818FF278)  ->  inside ordinal 0x351 (R 818FF140)
G 819065F4 (R 818FF3F4)  ->  inside ordinal 0x352 (R 818FF2C8)
```

Ordinal 0x351 is xam's XUI render init. Its entry (G 81906340):

```
8190634c  or r28,r3,r3
81906350  or r30,r4,r4
81906388  cmplwi r30,0
8190638c  bne -> 819063d8      ; r4 == 0 falls into the assert path
```

so it takes two arguments and requires the second to be non-null - the D3D
device. hud never calls it; its host is expected to. That is a genuine
hosting gap and not something a null argument can be poked around.

## Status of the button

press -> handler -> Guide object built and stored in hud's singleton ->
init -> gate forced -> **XuiInit now OK** -> XuiRenderCreateDC still blocked
on the null render context -> needs ordinal 0x351 with a real D3D device.

# Phase 68 - Root cause: xam has no D3D device

Addresses labelled (G) ghidra / (R) runtime; runtime = G - 0x7200 for xam.

XuiRenderCreateDC needs the XUI render context at 81D6C978. Three sites write
it: one uninit, and two inside xam ordinals 0x351 (G 81906340) and 0x352
(G 819064C8).

## Ordinal 0x351 is not the host path

Scanning xam for internal callers of both:

- ordinal 0x351: **no internal callers**. It is a pure export for its host.
- ordinal 0x352: called once, from G 81794EB8.

In 0x351 the second argument is only null-checked and then written into a
params block at [r1+112] which is consumed by G 819042E8 - so it is a real
pointer, not a flag that can be faked.

## xam's own host path, and what it needs

G 81794EB8 sits in a function starting at **G 81794E58 (R 8178DC58)**:

```
81794eb4  lwz r3,13956(r11)     ; r11 = 0x81D40000 -> the device at 81D43684
81794eb8  bl 819064c8           ; ordinal 0x352(device, params, callback)
```

This is xam's XUI render-host init - the missing hosting step, now a single
named function rather than a vague "hud must be hosted".

Called directly (cvar `guide_call_render_host`):

```
Guide button: D3D device global 81D43684 = 00000000
DbgBreakPoint()
```

**The device global is null**, so the call hits xam's assert path and
DbgBreakPoint, killing the dispatch thread. The cvar therefore defaults OFF,
and the button path is verified unchanged with it off:

```
XuiInit returned 00000000
XUI init returned 8000FFFF
draw frame 0 returned 80070057
```

## Why the device is null - the structural answer

Scanning for writers of 81D43684 finds exactly one `stw`, at G 81799AF4, and
it is the **teardown**: release the object, store 0. Creation must pass the
global's address into a create function, so it does not appear as a direct
store; that creator has not been located yet.

But the shape of the problem is now clear and it is structural. On real
hardware **xam owns the display device** - it creates D3D and titles render
through the system's device. Under Xenia the title links its own D3D and talks
to the GPU directly, and xam's device global is never populated because
nothing ever asks xam to create one. hud's Guide cannot get a render context
because the module that is supposed to own the device was never asked to.

That is the honest boundary. Everything from the button press down to
XuiRenderCreateDC now works; the remaining gap is that real xam expects to be
the display owner, and in this emulator it is not.

# Phase 69 - xam has a D3D device and a XUI render context

The Phase 68 root cause is cleared, and it came down to one wrong constant in
Xenia.

## Finding the creator

Only one instruction takes the *address* of xam's device global: G 817969A0,
`addi r27,r11,13956`. Just below it:

```
817969d0  bl 819fbf28        ; CreateDevice(0, 2, 0, 0, 0, &81D43684)
```

Its enclosing function is G 81796948 (**R 8178F748**), and it opens with a
gate:

```
81796954  lis r11,0x815f
81796958  lwz r11,1164(r11)      ; [815F048C]
8179695c  lwz r11,0(r11)
81796960  rlwinm r11,r11,0,22,22 ; bit 0x200
81796964  bne -> 81796970        ; clear -> return 0 immediately
```

Called it (cvar `guide_create_xam_device`):

```
device gate [815F048C]=801D0030 [*]=00000020 bit200=clear
xam CreateDevice returned 00000000, device now 00000000
```

Returned **0 while doing nothing** - caught only because the device global was
logged next to the return value. Sixth "confident zero" in this project.

## The one wrong constant

801D0030 is **XboxHardwareInfo**, and Xenia hardcodes its flags:

```cpp
xe::store_and_swap<uint32_t>(lpXboxHardwareInfo + 0, 0x20);  // flags
```

Xenia's own comment admits the bits are guesswork ("Not sure what the other
flags are"). xam gates D3D device creation on bit 0x200, which Xenia never
sets. Made it configurable (`xbox_hardware_info_flags`, default unchanged at
0x20) and set 0x220:

```
device gate [*]=00000220 bit200=set
xam CreateDevice returned 00000000, device now 40883A80
```

**xam now has a D3D device.**

## And a render context

With a real device, xam's render-host init (R 8178DC58, Phase 68) works:

```
render host returned 00000000, XUI ctx now 4088A0A0
```

The XUI render context global at 81D6C978 is populated. The blocker named at
the end of Phase 68 is gone.

## No regression

XboxHardwareInfo is global, so the dashboard was re-checked: identical error
(80) and warning (14) counts, and the framebuffer is **byte-identical to the
known-good baseline** (md5 a6eb4cfa). dash is unaffected.

## What is still failing

hud's init still returns 8000FFFF and its DC ([obj+12]) is still null.
XuiRenderCreateDC now gets past the null-context check and fails inside the
context's own virtual method:

```
81902298  lwz r11,0(r3)      ; ctx vtable
819022a0  lwz r11,12(r11)    ; vtable[3]
819022a8  bctrl              ; ctx->CreateDC(ctx, &out)   <- LK set, a call
819022b0  bge -> 819022c0    ; failure returns the error
```

so the next question is what vtable[3] of the render context needs.

# Phase 70 - The whole Guide stack now runs clean. The scene is still empty.

## Every return is now zero

```
render host returned 00000000, XUI ctx now 4088A0A0
direct XuiRenderCreateDC -> 00000000, dc=40894610
XUI init returned 00000000          (was 8000FFFF)
draw frame 0 returned 00000000      (was 80070057)
post-init +8=00000000 +12=40897EC0 +20=00000001
```

hud has its own render DC at [obj+12] and its draw method - XuiRenderBegin,
layout, XuiRenderEnd, XuiRenderPresent - completes successfully every frame.
Every error from Phases 65-69 is gone.

**Honest caveat on ordering.** In the previous run the XUI context already
existed and hud's init still failed with 8000FFFF. It only started returning 0
once xam's XuiRenderCreateDC was called once directly, first. Something about
that first call is load-bearing - the success path caches the DC into a second
global (81D6C980) when null - but the exact mechanism is not established. The
direct call is currently doing real work, not just measuring.

## But nothing is on screen

Screenshot after a press is **byte-identical to the dashboard baseline**
(md5 a6eb4cfa). Two separate reasons, both now identified:

**1. The scene is empty.** [obj+8] is still null, and hud's draw does:

```
913eab54  lwz r3,8(r31)
913eab58  bl 913fe864        ; XuiElementLayoutTree
```

913FE864 is XuiElementLayoutTree - so [obj+8] is the **root XUI element**.
Laying out a null tree draws nothing, and reports success doing it. The draw
returning 0 means "the calls worked", not "something was drawn" - the seventh
zero in this project that reads as success and is not.

**2. Presentation.** Even a non-empty scene renders through *xam's* D3D device,
while Xenia presents the title's swap chain. Two devices, one presenter.

## Where the root element comes from

hud imports XuiSceneCreate (thunk 913FE6D4), called from five sites:

```
913E921C   913EB580   913EB9EC   913EC82C   913F1F1C
```

These sit inside the five functions the old static scan had flagged as "scene
construction" (913E91E8, 913EB508, 913EB940, 913EC6D0, 913F1EF0). That scan
identified the right functions after all - Phase 64 concluded they were the
wrong list because none of them ran. Both were true: right functions, never
called.

913EC82C sits in 913EC6D0, right next to the constructor 913EC578 that is on
the live path, so that is the most likely scene creator for this Guide object.

# Phase 71 - The render object is a sub-object at +16, and the scene root exists

## The scene file has a name

hud's XuiSceneCreate call site (913EC82C) passes a string at 913E1AA0:

```
913E1AA0: 'ConsoleContract.xur'
```

so the Guide's UI is a XUI scene resource. Nothing in the log ever opens a
.xur, and `huduiskin.xex` sits unloaded in dashroot next to hud.xex.

## The object vtable ends at 33

Dumping 48 slots shows the vtable is 34 entries; slots 34+ are string data
("XuiButton", "XuiScene" in UTF-16BE). Reading past the end would have given
plausible-looking function pointers, so the bound matters.

**vtable[27] = 913EB940** - one of hud's five XuiSceneCreate functions.

## Calling it revealed the real mistake

913EB940 takes three arguments, so calling it with one crashed the dispatch
thread. But its prologue is the important part:

```
913eb94c  or r31,r3,r3
913eb950  stw r4,28(r3)
913eb958  addi r3,r3,16       <- +16
913eb95c  addi r4,r0,0
913eb96c  bl 913ea898         ; the init, with this+16 and a second argument
```

**hud's render object is a sub-object at obj+16**, and the init takes two
arguments. Every call in this bootstrap has been passing `obj` and one
argument - a view shifted by 16 bytes that happened to be self-consistent,
because the same wrong base was used for the init, the forced gate and the
draw. It "worked" while operating on the wrong fields.

## Driving the correct object

Passing obj+16 and the second argument:

```
pre-init  +8=00000000 +12=00000000 +20=00000000
post-init +8=00010000 +12=40897EC0 +20=00000001
XUI init returned 00000000
draw frame 0 returned 00000000
```

**[render+8] is now 00010000** - a XUI handle, not a pointer, so the root
element the draw hands to XuiElementLayoutTree finally exists. The init
creates it itself once given the right `this`; no separate scene call was
needed.

## Still not on screen

Screenshot remains byte-identical to the dashboard baseline (a6eb4cfa). That
is the second barrier from Phase 70 and it is unchanged by any of this: hud
renders through **xam's** D3D device while Xenia presents the **title's** swap
chain. Two devices, one presenter. No amount of correctness inside hud will
put pixels on screen until something bridges that.

# Phase 72 - The Guide is not supposed to swap. Xenia stubs the hook it needs.

## hud never presents

Instrumented Xenia's VdSwap to log the calling thread. Every swap in a whole
session comes from one thread - the title's:

```
VdSwap #1500 from thread ''     (F800011C, dash's render thread)
VdSwap #3600 from thread ''
```

Not one swap from the Guide dispatch thread, even while its draw loop runs
3600 frames returning 0. Wiring xam's device into VdGlobalXamDevice (801E6FC8,
which Xenia stores as 0 with the comment "Pointer to the XAM D3D device, which
we don't have") changed nothing either.

That is not a bug. **The Guide is not supposed to present its own frames.**

## The real mechanism, and where it stops

The kernel exports a compositing hook:

```
0x1CD  VdRegisterGraphicsNotification
0x1CE  VdRegisterXamGraphicsNotification
0x1B1  VdCallGraphicsNotificationRoutines
```

From the import dumps:

- **xam imports 0x1CE**, `VdRegisterXamGraphicsNotification`, marked `!!` -
  no implementation exists anywhere in Xenia's source (grep outside the export
  table returns nothing).
- **dash imports 0x1B1**, `VdCallGraphicsNotificationRoutines`, and calls it
  as part of its frame.

So on hardware: xam registers its Guide renderer, the title calls the
notification routines during its own frame, xam's callback draws the Guide on
top, and the title swaps *once* with the Guide composited in. One device
presenting, exactly as observed - the Guide rides the title's frame.

Xenia's side of that:

```cpp
dword_result_t VdCallGraphicsNotificationRoutines_entry(...) {
  assert_true(unk0 == 1);
  // TODO(benvanik): what does this mean, I forget:
  // callbacks get 0, r3, r4
  return 0;
}
```

An implemented no-op that never calls a callback, and a registration function
that does not exist. dash dutifully calls the hook every frame and nothing
happens.

This also explains the KernelState TODO found earlier - "if VdGlobalXamDevice
is nonzero, should set X_PROCTYPE_SYSTEM" - which is about running exactly
these system callbacks with the right process type.

## Where this leaves the button

Everything hud needs now works: object, allocator, XUI init, render context,
DC, root element, and a draw that returns 0 every frame. The Guide is drawing.
It is drawing into a frame nobody composites, because the two kernel functions
that perform the compositing are unimplemented and stubbed.

That is a concrete, bounded piece of missing emulation - two named exports -
rather than the open-ended "hud must be hosted by xam" this investigation
carried for most of its length.

# Phase 73 - Implemented the compositing hook; the Guide now draws in the
# title's frame, on the title's thread

## Implemented two missing kernel exports

`VdRegisterGraphicsNotification` (0x1CD) and
`VdRegisterXamGraphicsNotification` (0x1CE) had no implementation at all;
`VdCallGraphicsNotificationRoutines` (0x1B1) was a no-op. All three are now
real: registration stores callback+context, and the call routine invokes each
with (0, r3, r4) as benvanik's original comment described.

Nothing registers in our session, so xam's own registration happens somewhere
in a boot path this bootstrap never runs.

## The guest told us why our thread could not draw

Pointing xam's device global at the title's device produced, from the guest's
own D3D runtime:

```
WRN[D3D]: The current thread (0x29) is trying to use a D3D device
object that is owned by a different thread (0x10).
```

**The title's D3D device is thread-affine.** That single message explains the
whole architecture: the Guide cannot render from its own thread, which is
exactly why a kernel callback mechanism exists - it runs the Guide's drawing
on the title's thread inside the title's frame.

## But the title only calls the hook once

```
VdCallGraphicsNotificationRoutines #1 (hook=00000000)
```

Once, at startup - not per frame. So that export is not the per-frame
compositing path. The per-frame point is the swap itself, which runs on
F800011C, the thread that owns the device. Moved the Guide draw there.

## Result

```
Guide composite draw #1 -> 00000000      (on F800011C, the title's thread)
Guide composite draw #300 -> 00000000
VdSwap #1500 ... #1800                   (dash still rendering)
```

The Guide's draw now executes on the correct thread, inside the title's frame,
immediately before its swap, every frame, returning success - and the
dashboard keeps running normally.

## Sharing the device breaks the title

With `guide_use_title_device` on, dash stops swapping entirely after the press:
zero VdSwap calls following the hook install. Handing xam's XUI a device owned
by another thread corrupts the title's rendering. The cvar defaults off now.
That was a real regression, caught only by checking that swaps continued.

## Still no pixels

Screenshot is still byte-identical to baseline (a6eb4cfa). hud renders into
**xam's** DC on **xam's** device; the title presents its own front buffer.
On hardware these are two front buffers composited by the display hardware -
the Guide is an overlay, not a draw into the title's back buffer. Xenia's
presenter shows exactly one front buffer, the one named by VdSwap.

So the final gap is display-level composition of two front buffers, and it is
not something the guest can be talked into doing: it is the one part of this
chain that lives in hardware.

# Phase 74 - The whole bootstrap runs on the title's thread. Device sharing is
# the thing that breaks, not the thread.

Moved the entire XUI bootstrap - render host, XuiRenderCreateDC, hud's init -
out of the Guide's own thread and into the title's swap, via
`QueueGuideBootstrap` (cvar `guide_bootstrap_on_title_thread`, default on).

## With the title's device

```
F800011C GuideBootstrap: xam device global -> title device 40952400
F800011C GuideBootstrap: render host -> 00000000, XUI ctx 4088A0A0
F800011C GuideBootstrap: XuiRenderCreateDC -> 00000000 dc=40894610
F800011C GuideBootstrap: hud init -> 00000000  +8=00010000 +12=40897EC0
```

Every step succeeds **and the thread-ownership warnings are gone** - running on
F800011C, the owning thread, satisfies the guest D3D runtime completely. That
confirms the thread was a real constraint and it is now satisfied.

But dash stops swapping: zero VdSwap after the bootstrap, and zero composite
draws. So handing xam's XUI the title's device breaks the title's rendering
**even from the correct thread**. The earlier phase blamed the thread; the
thread was only half of it. Sharing the device object is what kills dash -
xam's render host reconfigures device state the title depends on.

## With xam's own device

```
F800011C GuideBootstrap: render host -> 00000000, XUI ctx 4088A0A0
F800011C GuideBootstrap: XuiRenderCreateDC -> 00000000 dc=40894610
F800011C GuideBootstrap: hud init -> 00000000  +8=00010000 +12=40897EC0
F800011C Guide composite draw #1 -> 00000000
F800011C Guide composite draw #300 -> 00000000
```

Bootstrap and draw both on the title's thread, inside its frame, every frame,
all returning 0, dash unaffected. This is the correct and stable arrangement.

Screenshot: still byte-identical to baseline (a6eb4cfa).

## What that leaves

The two devices cannot be merged from the guest side - that is now tested, not
assumed, in both thread configurations. hud renders to xam's surface; the
title presents its own. On hardware the display block scans out both and
composites the Guide as an overlay.

So the remaining work is in Xenia's presenter, not in the guest: it would have
to locate xam's front buffer and blend it over the title's. Everything upstream
of that - app, object, heap, XUI classes, XuiInit, device, render context, DC,
scene root, per-frame draw on the correct thread - now works.

# Phase 75 - No scene was ever created. Now one is attempted, and it wants
# files.

## The empty-root finding

A breakpoint on hud's XuiSceneCreate thunk (913FE6D4) installs (patched=1) and
**never fires**, across 1500+ composite draws. So hud was drawing an empty root
element the whole time. Even perfect compositing would have shown nothing -
which means the presenter work proposed at the end of Phase 74 would have been
premature. Content first.

## hud's resource globals are fine

```
GuideBootstrap: hud globals 91400168=913E16AC 91400170=913E2368
                            91400690=401EA6A0
```

Read as UTF-16BE out of hud:

```
913E16AC: 'strings.xus'
913E2368: 'InfoUpsellLive.xur'
913E1B24: 'hud'
```

So the scene creator has valid data - a skin name, a string table and a scene
file. It simply was never called. Calling the bare init at hud+0xA898 creates a
root with nothing under it; the object's own scene creator is what builds the
scene.

## Calling the object's scene creator

vtable[27] (hud 913EB940) takes three arguments - (this, a, b) - stores a at
[this+28], calls the init itself with this+16, then reaches XuiSceneCreate at
913EB9EC with:

```
913eb9e8  lwz r4,368(r11)     ; [91400170] = "InfoUpsellLive.xur"
913eb9ec  bl 913fe6d4         ; XuiSceneCreate(basePath, file, 0, &out)
```

Driven from the title's thread with (obj, 0, 0):

```
HostPathDevice::ResolvePath()          <- twice
GuideBootstrap: scene creator 913EB940 -> D0000034
```

**It runs to completion and attempts real file I/O.** Two path resolutions,
then failure D0000034 (XUI facility). The root element is unchanged at
00010000 and the draw still returns 0 with nothing under it.

## What it is looking for

hud builds its base path from the string "hud" and "strings.xus" before the
scene call, so it expects a XUI skin registered under the name **hud** - and
`huduiskin.xex` is sitting unloaded in dashroot beside hud.xex, which is
exactly that package.

So the next blocker is concrete and local: load huduiskin.xex and make its
.xur/.xus resources resolvable under the name hud. That is a resource-loading
problem in the bootstrap, not an emulation gap in Xenia - a much better place
to be than the presenter.

# Phase 76 - Scene creation advances two blockers: D0000034 -> 80004005 ->
# 80300004

## What hud asks for

hud builds its resource path with **XamBuildResourceLocator** (xam ordinal
0x31B), whose format Xenia documents in its own HLE version:

```cpp
if (!module) path = u"file://media:/{container}.xzp#{resource}";
else         path = u"section://{module:X},{container}#{resource}";
```

hud calls it as (module=[guide+4], "hud", "strings.xus"/"InfoUpsellLive.xur").
Its constructor leaves [guide+4] zero, and the FS log showed the consequence
directly:

```
HostPathDevice::ResolvePath()      <- empty path
```

## hud carries its own skin

`huduiskin.xex` will not load as a module, and it turns out not to be needed:
hud.xex's own XEX resource table contains

```
hud      91401000-91429E9D, 167581b
```

which is exactly the container name it passes. The module it wants is itself.
(xam.xex likewise carries fusion / controlp / gamercrd / skin / shrdres.)

## Two separate module-identity bugs

**Mine.** hud's DllMain was being called with `hud->handle()`. DllMain's first
argument is an hmodule, and hud keeps it for later lookups. Fixed to
`hud->hmodule_ptr()`.

**Xenia's.** In Xenia an hmodule is a pointer to a guest LDR entry whose
`checksum` field stashes the kernel handle, so `XexGetModuleSection` only
accepts that form:

```
XexGetModuleSection: no module for hmodule F8000494 (section 'hud')
```

F8000494 is hud's kernel handle. XexGetModuleHandle hands out hmodule_ptr,
but hud holds a handle from some other path, and the lookup rejected it.
XexGetModuleSection now falls back to treating the value as an object handle.

## Result

```
scene creator 913EB940 -> D0000034     (empty locator)
                       -> 80004005     ([guide+4] set; section lookup failing)
                       -> 80300004     (section lookup fixed)
```

Two blockers cleared. 80300004 is a XUI-facility error from further inside
scene loading.

## A test that corrected me

Having seen [guide+4] hold 301B3000 at the call while the lookup still received
F8000494, I concluded that field was not the module source and removed the
write. The error immediately reverted D0000034. **It is load-bearing** - it
feeds XamBuildResourceLocator, while the handle reaching XexGetModuleSection
comes from elsewhere in hud. Both matter. Restored, and the reasoning that
looked airtight was wrong; only re-running showed it.

# Phase 77 - 80300004 is a null XUI resource provider

Traced 0x80300004 the same way as 0x80300005: three construction sites in xam,
of which the live one is G 81987F90. What guards it:

```
81987f18  lwz r3,-12116(r11)   ; the global at 81D6D0AC
81987f1c  cmplwi r3,0
81987f20  beq -> 81987f50      ; null -> assert -> return 80300004
81987f24  lwz r11,0(r3)        ; else virtual call
81987f30  lwz r11,4(r11)       ;   provider->method[1](provider, name, &out)
81987f38  bctrl
```

So **81D6D0AC holds a XUI resource-provider object** - a C++ object whose
second vtable slot opens a resource by name. Scene loading dereferences it, and
it is null.

## Where it comes from

XuiInit stores it from its params:

```
8195aa58  lwz r11,0(r31)       ; params size
8195aa64  blt -> r11 = 0       ; size < 0xC -> no field
8195aa68  lwz r11,8(r31)       ; else params[+8]
8195aa74  stw r11,-12116(r10)  ; -> 81D6D0AC
```

and hud's init builds default params when its **second argument** is zero:

```
913ea8dc  cmplwi r30,0
913ea8e0  bne -> 913ea8fc      ; non-zero: use the caller's params directly
913ea8e8  addi r10,r0,12       ; else size = 12
913ea8f4  stw r28,8(r11)       ; params[+8] = 0
```

So the host is expected to hand hud a params struct containing a resource
provider. hud's own scene creator passes 0, and this bootstrap passes 0, so the
provider is null by construction.

Disabling `guide_call_xuiinit` and letting hud call XuiInit itself changes
nothing - hud's call has the same null params. That ruled out the theory that
our XuiInit(NULL) was poisoning the state.

## A setter exists

Three sites write 81D6D0AC: XuiInit (G 8195AA74), G 81959608, and a small
standalone function at **G 81946AE0 (R 8193F8E0)**:

```
81946ae0  mfspr r12,8          ; entry
81946af0  or r31,r3,r3
81946afc  stw r31,-12116(r11)  ; provider = r3
81946b04  addi r3,r0,0         ; return 0
```

a one-argument setter for the provider. So the remaining question is not how to
install a provider but **what object to install** - who constructs the standard
one, and whether xam has a default implementation reachable without the full
system boot.

That is the next thread to pull. Note the draw loop and dashboard remain
healthy throughout all of this.

# Phase 78 - Correction: the provider is NOT null. It is installed and its
# open call fails.

## Who installs it

The only caller of the provider setter (G 81946AE0) is at **G 81794F64** -
inside the render-host function we already call. Its tail:

```
81794f3c  bl 81cc9c10          ; construct the object at 81D22A5C
81794f40  bl 81977540
81794f50  bl 81938ae0(1)
81794f64  bl 81946ae0          ; SetResourceProvider(0x81D22A54)
```

So the provider is a **static xam object at 81D22A54** and the host installs
it. Logging the global right after our host call:

```
render host -> 00000000, XUI ctx 4087D980, provider 81D22A54
```

**It is set.** Phase 77's conclusion - that 80300004 was a null provider - is
wrong and is retracted. It would have been null only if the host bailed early,
which is what happened in the earlier runs where XuiRenderCreateDC failed
inside the host before XuiInit and the provider setup.

Also worth noting: xam's own host calls XuiInit with params {12, 0, 0} - the
same "null" params hud builds. So params[+8] is not how the provider is
installed in practice; the dedicated setter is.

## Which site actually returns 80300004

Of the three construction sites, by elimination:

- G 81987F90 needs the provider to be null - it is not.
- G 81956690 is reached through a class-registry lookup at G 81956610. A
  breakpoint on that entry (R 8194F410) **never fires**.
- **G 81955E84** is left, and its guard is the provider's own virtual call:

```
81955e14  lwz r11,0(r3)        ; provider vtable
81955e20  lwz r11,4(r11)       ; vtable[1]
81955e28  bctrl                ;   open(provider, name, &out)
81955e30  blt -> error
81955e38  cmplwi r4,0          ; out == 0 -> error
```

So the provider exists, is called, and **fails to open hud's resource**.

## Where that leaves it

The failure has moved from "no provider" to "the provider cannot find
InfoUpsellLive.xur in hud's section". XexGetModuleSection no longer errors
(its failure log is gone since the handle fallback), so the section resolves;
what fails is the lookup of the named resource inside it.

The empty-path ResolvePath lines in the log are dash's own, occurring before
hud is loaded - not ours. Worth stating because they look exactly like the
symptom we chased in Phase 76.

# Phase 79 - The provider is a stub, and it is only the fallback

## The provider's open is E_NOTIMPL

Dumped the installed provider object and its vtable at runtime:

```
provider 81D22A54 vtable 81608258 [0]=817924E0 [1]=8178F600 [2]=8178F600
```

vtable[1] (R 8178F600 / G 81796800) is three instructions:

```
81796800  lis r3,0x8000
81796804  ori r3,r3,0x4001    ; E_NOTIMPL
81796808  blr
```

Slots [1] and [2] are the same address. So the object installed as the XUI
resource provider **cannot open anything** - by construction, in this xam
build.

## Which means it was never meant to

Re-reading the call site properly (rather than assuming, as in Phase 78):

```
81955dec  or r3,r31,r31
81955df4  bl 81951000        ; primary lookup
81955df8  r4 = r3
81955e00  bne -> 81955e90    ; non-zero: SUCCESS, provider never consulted
81955e04  lis r11,0x81d7
81955e08  lwz r3,-12116(r11) ; else fall back to the provider
81955e14  lwz r11,0(r3)      ;   provider->vtable[1](...)
```

The provider is a **fallback**. The normal path is **G 81951000**, and it
returned 0 - resource not found. The E_NOTIMPL from the fallback is then what
surfaces as 80300004.

So chasing the provider was chasing the error path. Installing a "better"
provider would be papering over a primary lookup that should have succeeded.

## Restated blocker

hud asks for InfoUpsellLive.xur; the primary resource lookup at G 81951000
does not find it; the fallback provider is a stub; the result is 80300004.

The question is now what G 81951000 searches - presumably a registry of loaded
XUI resource packages - and what registers hud's "hud" section into it. That is
one function to read, and it is the right one this time: it is on the success
path, not the failure path.

# Phase 80 - The primary lookup is a named-object registry

G 81951000 (the primary path, ahead of the stub provider) is a thin wrapper:
null-check, then G 81950D60 - the same critical-section-guarded lookup in the
table at **0x81D6C508** that Phase 67 saw during class registration.

A breakpoint on its runtime entry (81949E00) caught a live call:

```
LoaderTrace: FindClass name 'AsyncTaskManager0'
LoaderTrace: LOOKUP 81949E00 lr=81AA7404 ...
```

so that table is a **general named-object registry** - not a class table and
not a file system. The same lookup serves class registration (Phase 67, where
a hit meant "duplicate") and this resource open (where a hit means "found").

Note the breakpoint died on its first hit, from an unrelated caller
(lr 81AA7404, not the XUI path at R 8194EBF4). A one-shot fatal breakpoint
cannot be filtered by caller, so this could not be used to catch the XUI
lookup itself - the name for that path comes in as the function's first
argument from the scene loader.

## What this means

The XUI resource open does not go to disk at all on its primary path. It looks
the resource up **by name among already-registered objects**, and only falls
back to the (E_NOTIMPL) provider when that misses.

So nothing is going to open InfoUpsellLive.xur for us implicitly. Something has
to register hud's "hud" resource package into that registry first - the step a
real system boot performs before any Guide scene is created.

That is the next thing to find: the register-side counterpart of G 81950D60.

# Phase 81 - Class registration works after XuiInit; the scene then hangs

## Correction: the registry address

Phases 67/80 wrote the registry table as 0x81D6C508. It is **0x81D6D508** -
`addi r3,r11,-11000` off `lis 0x81D7`, not -15096. Scanning for the wrong
displacement found one unrelated reference; the right one finds six, including
the lookup wrapper (G 81950D94) and the insert path.

## The register side

```
819564f8  lwz r4,0(r28)        ; name
819564fc  addi r5,r1,80        ; value
81956500  addi r3,r11,-11000   ; the table at 81D6D508
81956504  bl 81951e10          ; insert
```

and the enclosing function starts at **G 81956318** - which is XuiRegisterClass,
the same function the earlier class-registration work drove. So classes and
cached resources live in one name-keyed registry, inserted through G 81951E10.

## Ordering matters, and it is the reverse of what was tried

`lle_xam_xui_init` ran xam's three extra registrars *before* XuiInit, which
made XuiInit fail on the duplicate "XuiElement" (phase 67) - the reason it was
turned off. Running the same three from the title thread *after* the host has
called XuiInit:

```
GuideBootstrap: registrar 817503E8 -> 00000000
GuideBootstrap: registrar 8199BE08 -> 00000000
GuideBootstrap: registrar 8176B2C8 -> 00000000
```

All three succeed. The ordering, not the registrars, was wrong all along.

## But the scene creator then hangs

With those classes registered, the scene creator no longer returns 80300004 -
it does not return at all. The title thread stops: no further VdSwap, no
composite draws, dashboard frozen. Getting further into scene loading reaches
something that blocks.

That is progress and a regression at once, so it sits behind
`guide_register_classes`, default **off**. With it off the stable path is
restored exactly:

```
scene creator 913EB940 -> 80300004
Guide composite draw #1 -> 00000000
```

Next: find what the scene loader blocks on once its classes exist - most
likely a wait for a resource load that nothing completes.

# Phase 82 - The hang is inside hud's XUI init, not the scene load

Localised the `guide_register_classes` hang by elimination, using the fact
that a breakpoint hit is fatal (harmless here, since the thread hangs anyway -
a hit proves the address was reached).

hud's scene creator, G 913EB940:

```
913eb950  stw r4,28(r3)
913eb958  addi r3,r3,16
913eb96c  bl 913ea898        ; hud's XUI init
913eb974  blt -> 913eba6c    ; init failed: skip the rest
913eb980  bl 913fe724        ; XamEnableSystemAppInput
...
913eb9ec  bl 913fe6d4        ; XuiSceneCreate
```

Breakpoints on the two later calls:

- **913FE6D4 (XuiSceneCreate)** - never fires.
- **913FE724 (XamEnableSystemAppInput)** - never fires.

and the bootstrap log stops immediately after `[guide+4] = skin module`, i.e.
at the scene-creator call itself. So execution enters G 913EB940 and never
gets past **913EA898, hud's XUI init**.

That is the same init that returns 0 cleanly when the extra classes are *not*
registered. Registering them changes its behaviour from "returns 0" to "never
returns" - so the init does more work once the classes it wants exist, and
that work blocks.

Note this also revises Phase 75/80's picture: XuiSceneCreate has never been
reached in any configuration. Both the 80300004 and the hang happen upstream of
it, inside the init. The resource lookup that produces 80300004 is therefore
called from the init, not from scene loading.

Stable defaults restored and verified:

```
scene creator 913EB940 -> 80300004
Guide composite draw #1 -> 00000000
```

# Phase 83 - hud's init ends in a registration routine, and that is the site

hud's init (G 913EA898) does not return normally on success - it tail-calls
through the render sub-object's vtable:

```
913ea924  lwz r10,0(r31)     ; sub-object vtable
913ea930  lwz r11,28(r10)    ; vtable[7]
913ea938  bctrl
```

Dumped at runtime:

```
render obj 401EA6B0 vtable 913E1C8C [7]=913F0DB0
```

**G 913F0DB0** is a flat registration sequence - one call after another, each
with r3 = 0:

```
913f0dbc  bl 913fea04        ; GamerCardRegisterControls  (xam ord 0x3E6)
913f0dc4  bl 913ee1a0
913f0dcc  bl 913ef358
913f0dd4  bl 913ef3f0
...       ~15 more
```

The first is an **import**: 913FEA04 resolves to xam's
`GamerCardRegisterControls`, and xam carries a `gamercrd` resource section
(36583b, seen in its XEX resource table). The rest are hud's own class
registrations.

A breakpoint on 913FEA04 produced the fatal-hit signature - five
"Unable to read thread context for stack walk" lines and a dead title thread -
so **the call is reached**, even though the callback's own log line was lost
when the thread died. (Worth noting as a limitation: a hit is not always
observable through its callback, only through its side effect.)

## Why this matters

Both symptoms now sit in one place. 80300004 comes out of this registration
sequence, not out of scene loading; and with xam's extra classes pre-registered
the same sequence hangs instead of erroring. XuiSceneCreate is downstream of
all of it and has never run.

So the Guide's failure is in **class/control registration**, and the most
specific suspect is GamerCardRegisterControls - a xam function that registers
controls from xam's own gamercrd resources, running here under a bootstrap
that never performed xam's normal resource setup.

# Phase 84 - 80300004 is a missing base class, not a missing file

Followed GamerCardRegisterControls down. Thunk 913FEA04 reads

```
3D608180 616B45D8 7D6903A6 4E800420   -> R 818045D8 / G 8180B7D8
```

and the chain is

```
G 8180B7D8  GamerCardRegisterControls
  -> G 8180F9F0
    -> G 8180EE78     ; builds a class descriptor on the stack
```

The descriptor's two string pointers (after fixing an arithmetic slip of mine -
`lis 0x8162; addi -15840` is 0x8161C220, not 0x8162C220):

```
8161C220  "BaseScene"     ; the class being registered
815FB498  "XuiScene"      ; its base class
```

So this registers **BaseScene deriving from XuiScene**.

## The whole story now fits

The failing lookup at G 81955DA8 - name in the shared registry, falling back to
the E_NOTIMPL provider - is looking up **"XuiScene", the base class**, not
InfoUpsellLive.xur. That is why the "resource" path is a name-keyed registry
with a load-if-missing fallback: it is find-or-load-class.

- Without xam's extra registrars, **XuiScene is not registered**. BaseScene's
  registration cannot resolve its base, the fallback provider is a stub, and
  the result is 80300004.
- With them (`guide_register_classes`), XuiScene exists, BaseScene registers,
  and execution proceeds further into the ~15 following registrations, where it
  hangs.

That retires the reading carried since phase 75 that this was about loading
.xur resources. hud has not reached any resource yet; it is still assembling its
class hierarchy. XuiSceneCreate, InfoUpsellLive.xur and the skin section are all
downstream of a class registry that is only half-populated.

## What follows

The registrars are necessary, not optional - they supply XuiScene. So the hang
they expose is the real next problem, and the question is which of the ~15
registrations after GamerCardRegisterControls blocks, and on what.

# Phase 85 - The guest logs its own diagnostics, and every registration
# succeeds in isolation

## A channel I had not been reading

Driving hud's 56 registrations individually surfaced this, straight from the
guest via DbgPrint:

```
ERR[XAM]: XuiGamerCardElement::RegisterClasses:  CBaseScene::Register() failed.
          HRESULT: 0x80300005
!> tw/td forced trap hit! This should be a crash!
```

xam is compiled with its diagnostic strings intact and prints **C++ symbol
names**. This independently confirms Phase 84 - the site really is
`CBaseScene::Register()` inside `XuiGamerCardElement::RegisterClasses` - and
confirms 0x80300005 is the duplicate-registration code, since that run had
already registered the classes moments earlier.

Every `ERR[XAM]` line in the log is xam telling us what it thinks is wrong. That
is a better instrument than breakpoints for this kind of work, and it has been
available the whole time.

## All 56 registrations succeed standalone

With `guide_step_registrations`, each call in G 913F0DB0 is driven separately:

```
reg[0]  913FEA04 -> 00000000     (GamerCardRegisterControls)
...
reg[55] 913EFA50 -> 00000000
all registrations done
```

**All 56 return 0**, including the GamerCard one that fails inside hud's own
path. So no individual registration is inherently blocking, and the list itself
is fine.

## But hud's own path still hangs, silently

With the registrars on and stepping off, the scene creator still hangs after
`[guide+4]`, and this time with **no ERR[XAM] line and no trap** - a silent
block, not a reported failure.

So the difference is not *which* functions run but *how* they are entered:
driven individually from the host they complete; entered through hud's init and
its vtable[7] tail-call they do not. That is the next thing to isolate.

Defaults restored: registrars and stepping both off.

# Phase 86 - The hang is entering via the scene creator, not the registrations

## A defensive fix that was not the cause

Xenia's DbgBreakPoint was `xe::debugging::Break()` unconditionally -
`__debugbreak()` with no debugger attached raises EXCEPTION_BREAKPOINT and
silently kills the calling guest thread, which for the title's renderer looks
exactly like an emulator hang. `IsDebuggerAttached()` sits three lines above it
in the same file and was unused. Now guarded, logging instead of breaking.

It did **not** fix this hang (no such log appears), but a guest assert should
never be able to take out an emulator thread, so it stays.

## Bisection

New cvar `guide_init_only` calls hud's init directly instead of its scene
creator. With the class registrars **on**:

```
after init -> 00000000  +8=00010000 +12=40899D40
Guide composite draw #1 -> 00000000
```

The init returns cleanly, produces both a DC and a root element, and the draw
loop runs. Crucially the init *itself* ends in the vtable[7] tail-call, so
**all 56 registrations execute on this path too** and complete.

So: registrations fine standalone (phase 85), fine via the init (here), and
hanging only when entered through the scene creator G 913EB940.

## What is different about that entry

913EB940 does very little before calling the init:

```
913eb950  stw r4,28(r3)      ; [obj+28]  = our second arg (0)
913eb964  stw r11,32(r31)    ; [obj+32]  = 1   -> render_obj+16
913eb968  stw r11,64(r31)    ; [obj+64]  = 1   -> render_obj+48
913eb96c  bl 913ea898        ; init(this+16, 0)
```

The init is called with identical arguments to ours. The only difference is
those three pre-set fields - in particular **render_obj+48 = 1**, which our
direct call leaves alone. That is the next thing to test: set it and call the
init directly, and see whether the hang follows the field rather than the
call site.

Defaults left off for both cvars; the stable path is unchanged.

# Phase 87 - Correction: the post-init call IS reached; the hang is later

## The fields were not it

`guide_preset_fields` sets [obj+28/32/64] exactly as hud's scene creator does
before calling the init. With the registrars on:

```
preset [obj+28,32,64] as the scene creator does
after init -> 00000000  +8=00010000 +12=40899D40
Guide composite draw #1 -> 00000000
```

Still clean. So the hang follows neither the fields nor the init.

## Correcting phase 82

Phase 82 concluded XamEnableSystemAppInput (913FE724) was never reached,
because its breakpoint callback logged nothing. Re-run watching for the
**fatal-hit signature** instead of the callback:

```
!> F800011C Unable to read thread context for stack walk   (x3)
```

on the title thread. The breakpoint **did** fire - the call is reached. Phase 83
had already established that a hit is sometimes only observable through its side
effect, and I failed to apply that when reading phase 82's negative. A
callback-based negative is not evidence of absence in this build.

So the hang is **after** XamEnableSystemAppInput, in the remainder of
G 913EB940: the XamBuildResourceLocator string building, the local call at
G 913EA7D8, and XuiSceneCreate itself. That region is exactly the resource path
- which makes the earlier "it never reaches resources" reading wrong too, at
least with the registrars on.

## Where things stand

- registrars off: init path returns 80300004 from the missing XuiScene base
  class; dashboard healthy.
- registrars on, init only: everything succeeds - DC, root element, 56
  registrations, draw loop.
- registrars on, full scene creator: reaches past XamEnableSystemAppInput and
  hangs somewhere in the resource/scene-creation tail.

Defaults restored to the stable combination.

# Phase 88 - XuiSceneCreate IS reached. The claim it never ran was wrong.

Probed the XuiSceneCreate thunk (913FE6D4) with the registrars on, watching for
the **fatal-hit signature** rather than the callback:

```
GuideBootstrap: thunk 913FEA04: ...
!> F800011C Unable to read thread context for stack walk   (x5)
```

on the title thread, immediately at the scene-creator call. **XuiSceneCreate is
reached.**

## What this retires

Phases 75, 82, 83 and 84 all asserted, with increasing confidence, that
XuiSceneCreate had never been reached in any configuration, and phase 84 built
on that to argue hud "has not reached any resource yet". That was wrong. Every
one of those negatives came from a breakpoint callback that logged nothing, and
phase 83 had already shown callbacks can be lost. The same mistake was made
four times because the negative was never re-tested with the signature.

The lesson is now recorded in the code comments and here: **in this build a
missing callback log proves nothing; only the fatal signature or a side effect
does.**

## Revised state, registrars on

init OK -> 56 registrations OK -> XamEnableSystemAppInput reached ->
XuiSceneCreate reached -> hang inside it, with no file I/O logged first.

## Hypothesis for the hang

XUI scene loading looks asynchronous: the named-object registry contains
**AsyncTaskManager0** (seen in phase 80). If XuiSceneCreate queues the .xur load
to a worker and waits, and we are calling it from inside VdSwap - i.e. holding
the title's render thread, which is also the thread that must keep swapping for
anything else to progress - that is a deadlock by construction.

That is testable: keep the draw in VdSwap (it must be there, the device is
thread-affine) but move scene creation off the render thread.

# Phase 89 - Not a deadlock: the scene creator hangs on any thread

Tested phase 88's hypothesis directly. New cvar `guide_scene_off_thread` splits
the work: the title thread keeps the device-touching part (render host,
XuiRenderCreateDC) and installs the draw hook, then sets a ready flag; the
Guide's own thread waits for it and calls hud's scene creator there.

```
F800011C GuideBootstrap: device work done; scene creation handed off
01000030 Guide button: scene creator 913EB940 off-thread
                                                          <- and nothing more
F800011C Guide composite draw #1 -> 80070057
```

The Guide thread reaches the scene creator and hangs in exactly the same place,
**while the title thread stays alive and keeps drawing**. So the hang is
intrinsic to that call path and has nothing to do with holding the renderer.
The AsyncTaskManager deadlock hypothesis is disproved.

Two useful by-products:

- The failure is now isolated to one thread. With this cvar on, the dashboard
  keeps running while the Guide thread is stuck, which is a much better
  diagnostic posture than freezing the emulator.
- The 80070057 draws are self-inflicted: this path returns before hud's init
  runs, so the render object never gets its DC. Fixing that ordering would
  re-introduce duplicate registrations, since the scene creator calls the init
  itself.

Cvar defaults off; the stable path is verified unchanged:

```
scene creator 913EB940 -> 80300004
Guide composite draw #1 -> 00000000
```

The next question is what inside the scene creator blocks with no diagnostic on
either thread - a wait, a spin, or a fault that leaves the thread parked.

# Phase 90 - Isolated: XuiSceneCreate hangs on a valid XUIZ package

Stepped hud's scene-creator sequence call by call from the Guide thread
(`guide_step_scene`, with `guide_scene_off_thread` so a hang does not take the
emulator with it):

```
step 1 init(render_obj,0)                      -> 00000000
step 2 XamEnableSystemAppInput(00000000, 1)    -> 00000000
step 3 locator -> 00000000 'section://301B3000,hud#strings.xus'
step 4 XuiSceneCreate calling
                                               <- never returns
```

So the first three succeed and **XuiSceneCreate is the hang**, precisely.

## Everything it is given is correct

- The locator is well-formed: `section://301B3000,hud#strings.xus`, built by
  real xam's XamBuildResourceLocator from hud's own hmodule and its "hud"
  section name.
- Xenia's `UserModule::GetSection` returns the resource table's address and
  size verbatim; for hud that is 91401000 / 167581 bytes, matching the XEX
  resource header exactly.
- The data is really there. At 91401000 the basefile holds:

```
5855495a 00000003 00028e9d ...
"XUIZ"   ver 3    size 0x28E9D == 167581
```

a valid **XUIZ** package whose internal size matches the section size.

So this is not a missing file, a wrong path, a bad handle or an empty section -
all of which earlier phases suspected in turn. xam is handed a correct locator
and correct data and does not come back.

## Reading

"XUIZ" is the compressed XUI package format. The hang is therefore most likely
inside xam's decompression or package walk. Two plausible causes: it is waiting
on something Xenia never signals, or it is spinning on data it cannot advance
through.

Note the step-4 call is now a self-contained repro: four arguments, no hud state
beyond [guide+4], reachable directly from the Guide thread. That is a far better
starting point than the whole Guide bootstrap.

# Phase 91 - The hang is not a spin and not a guest kernel wait

Three measurements on the isolated XuiSceneCreate repro.

## 1. Not spinning

CPU sampled over 8s of wall clock, with the Guide thread hung (off-thread mode,
so the dashboard keeps running and provides a comparable load):

```
hang:     cpu_delta=16.83s / wall=8.01s  => 2.10 cores   threads=94
baseline: cpu_delta=17.47s / wall=8.01s  => 2.18 cores   threads=93
```

Indistinguishable. A guest spin loop would show roughly one extra core, so the
thread is not burning CPU.

## 2. Not a guest kernel wait

Instrumented `KeWaitForSingleObject_entry` for the Guide thread - nothing. Then
found the reason that proves nothing on its own: `RtlEnterCriticalSection`
blocks by calling **xeKeWaitForSingleObject directly**, bypassing the export
wrapper. Instrumented the internal function too - still nothing. So the thread
is not parked on a critical section or any other guest waitable object.

## 3. It simply stops making kernel calls

With `log_level = 3` every kernel call is logged (178 MB for one run). The last
activity on the Guide thread:

```
RtlFillMemoryUlong(408A2230, 000035EF, FEEDFEED)
RtlFillMemoryUlong(408A2170, 00000018, FEEDFEED)
...
KeTlsSetValue(00000002, 00000001)
```

then nothing, while other threads continue normally.

Two details worth noting. **FEEDFEED is a debug-heap poison pattern**, so these
are frees, not allocations - this looks like a cleanup or error-unwind path
rather than forward progress. And `0x35EF` is not arbitrary: it appears in the
XUIZ header at offset 0x10, so the buffer being poisoned is sized from the
package it just tried to parse.

## Where that leaves it

Not spinning, not waiting on a guest object, and issuing no further kernel
calls. That combination points at either guest code looping without touching the
kernel (which should have shown as CPU, and did not), or the thread ceasing to
execute at all. Distinguishing those needs a host-side view of the thread, which
is the next tool to reach for.

# Phase 92 - The thread is parked in host code, not guest code

Added a host watchdog that samples the stuck guest thread's PPC context every
two seconds while XuiSceneCreate is outstanding:

```
Watchdog 0: lr=8174E228 r1=70C1F750 r3=00000000 r4=00000002
Watchdog 1: lr=8174E228 r1=70C1F750 r3=00000000 r4=00000002
...
Watchdog 5: (identical)
```

**Completely frozen** - every register identical across six samples spanning
twelve seconds. The thread is not executing guest instructions.

## The lr is not where it is stuck

lr 8174E228 (R) = G 81755428, which is the third instruction of a prologue:

```
81755420  mfspr r12,8
81755424  bl 8181496c        ; <- lr is the return address of this
81755428  stwu r1,-0xC0
```

and G 8181496C is `__savegprlr_29`:

```
fba1ffe0 / fbc1ffe8 / fbe1fff0   std r29,r30,r31
9181fff8                          stw r12,-8(r1)
4e800020                          blr
```

a four-instruction register-save helper that cannot block. So this is simply
the last guest state Xenia recorded, not the blocking point - worth stating
because an lr in a prologue looks like a stack-probe stall and is not one.

Also note r1 = 70C1F750 against the thread's stack of 70BA0000-70C20000: the
stack is nearly **empty**, not overflowed.

## Conclusion

Not spinning (phase 91), not waiting on a guest object (phase 91), not
executing guest code (here), and its stack is shallow. The Guide thread is
blocked inside **Xenia's own host code** while nominally running guest code.

The candidates are host-side locks. Xenia's `global_critical_region` is taken
widely across the kernel, and this bootstrap is unusual in calling guest code
from inside VdSwap on the title thread while another guest thread runs - if the
draw hook holds a host lock the Guide thread needs, that is a host-level
deadlock our own harness introduced.

That is directly testable: run the step-4 repro with the draw hook uninstalled.

# Phase 93 - Retraction: the frozen registers meant nothing

Phase 92 concluded, from a watchdog sampling the stuck thread's PPC context,
that the Guide thread "is not executing guest instructions" and is "parked in
host code". **That conclusion is withdrawn.**

## The control

Started the same watchdog *before* steps 1-3, which demonstrably complete
(init -> 0, XamEnableSystemAppInput -> 0, locator built):

```
Control 0: lr=8174E228 r1=70C1F750     <- 300ms in, during successful work
Control 7: lr=8174E228 r1=70C1F750
```

Identical, and identical to the "hung" samples. Xenia keeps guest registers in
host registers during JIT execution and does not write them back to
`thread_state()->context()` while running, so the sampled context is stale from
thread startup. It looks frozen whether the thread is working or wedged.

The whole Phase 92 argument - including the careful reading of lr into a
prologue and the note about the shallow stack - was built on an instrument that
cannot distinguish the two states.

## What else falls

- "Blocked inside Xenia's own host code": unsupported.
- The host-lock hypothesis that followed from it: unmotivated.
- The draw-hook test in this phase still stands on its own terms - with
  `guide_install_draw_hook = false` the hang is unchanged - so the hook is not
  the cause regardless.

## What actually survives

From the JIT instrumentation, which is sound: the Guide thread performs 372
DemandFunction calls, all balanced enter/defined, the last being 8174E220,
compiled successfully. So the JIT is not stuck, and the thread was still
demanding new functions right up to the point it stopped producing output.

That last fact is the useful one and points the opposite way from Phase 92: the
thread got far enough to compile 8174E220 on demand, which only happens when
guest code calls into it.

## Method note

This is the third time in this investigation that a negative from a weak
instrument was read as a positive finding (callback-less breakpoints twice,
register sampling now). The control here cost one build and caught it before it
propagated. Any instrument used to prove *absence* needs a control showing it
can detect *presence*.

# Phase 94 - Established with a control: the thread blocks after ~16ms

Replaced the discredited register sampling with **per-thread CPU time**
(GetThreadTimes on the guest thread's native handle), and bracketed the
known-good steps so the instrument has a control:

```
CpuMark before steps:     kernel=0ms  user=0ms
CpuMark after steps 1-3:  kernel=15ms user=0ms      <- detects real work
CpuProbe 0:               kernel=31ms user=0ms
CpuProbe 1..N:            kernel=31ms user=0ms      <- flat, indefinitely
```

The control is the point: the same probe shows time accruing while steps 1-3
run, so a flat reading afterwards is meaningful rather than an artefact.

Result: inside XuiSceneCreate the thread does about **16ms of real work** and
then stops consuming CPU entirely, indefinitely. It is **blocked**, not
spinning, and not doing slow work.

Phase 92 guessed this correctly from an instrument that could not support it;
it is now actually established. Worth separating: the conclusion being right
did not make the reasoning valid, and the control is what turns one into the
other.

## Caveat on the wait instrumentation

The earlier "not a guest kernel wait" result (phase 91) rests on logging
xeKeWaitForSingleObject for this thread and seeing nothing - and that negative
has **no control**. It is exactly the shape of the errors retracted in phases
88 and 93. Before building on it, it needs a run where the instrument
demonstrably fires for a wait that is known to happen.

## State

- Blocked after ~16ms inside XuiSceneCreate: established.
- Blocked on what: open. Guest kernel waits are ruled out only by an
  uncontrolled negative.

# Phase 95 - The "no guest wait" negative, now controlled

Phase 94 flagged the phase-91 result as an uncontrolled negative. Fixed by
making the same probe log every 500th wait from *any* thread alongside every
Guide-thread wait:

```
WaitProbe #9000:  thread='' reason=3 ...
WaitProbe #9500:  thread='' reason=3 ...
WaitProbe #10000: thread='' reason=3 ...
guide waits: 0        (of ~10000 waits observed)
step 4 reached: yes
```

The instrument is demonstrably live - roughly ten thousand waits pass through
it in one run - and **not one of them is from the Guide thread**. The negative
now stands.

(Method aside: the first count of this looked like 175 Guide waits. That was
PowerShell's `Select-String` matching case-insensitively, so "GUIDE" hit every
"Guide button" line in the log. Counting with a case-sensitive pattern gives 0.
A grep that is accidentally case-insensitive is another way to manufacture a
false positive.)

## Where this leaves the block

Two controlled instruments now agree:

- per-thread CPU is flat after ~16ms (phase 94, with control)
- the thread issues no guest kernel wait at all (here, with control)

and from the log_level 3 run, it issues **no kernel calls of any kind** after
`KeTlsSetValue(2, 1)`. So it is blocked in host code, reached from guest code,
without going through the guest kernel interface.

That resurrects phase 92's conclusion - but on evidence this time rather than
on a stale register read.

## What can block there

Not the JIT: DemandFunction enter/defined counts balance at 372 and the last
one completes. That leaves Xenia's own host-side machinery reached from guest
execution without a kernel call - most plausibly a guarded-memory access
handler, since Xenia protects pages for GPU and MMIO and traps writes into
host code that takes locks.

# Phase 96 - Not a guarded-memory trap either

Instrumented `MMIOHandler::ExceptionCallback` - Xenia's access-violation entry
point, which is how guest memory accesses to protected pages reach host code
without any kernel call. Same control pattern: log every 200th fault from any
thread, plus every Guide-thread fault.

```
FaultProbe #800:  thread='' code=1 addr=1BFA1DFE0
FaultProbe #1000: thread='' code=1 addr=1BFA44FA0
FaultProbe #1200: thread='' code=1 addr=1BF9E3840
FaultProbe lines: 6      GUIDE faults: 0
```

Over twelve hundred faults pass through the handler in one run - the instrument
is live - and **none are from the Guide thread**. The guarded-memory hypothesis
from the last phase is disproved.

Also checked and ruled out: no thread-suspension calls (the KeSuspendThread /
NtSuspendThread hits in the log are import-table entries, not calls).

## The negative space so far

The blocked Guide thread, each established with a control:

- consumes no CPU after ~16ms
- issues no guest kernel wait (0 of ~10000)
- takes no memory fault (0 of ~1200)
- makes no kernel call of any kind after KeTlsSetValue(2, 1)
- is not in the JIT (372 DemandFunction calls, all balanced, last one
  8174E220 defined)
- is not suspended

and its last act is to demand and compile 8174E220, which only happens when
guest code calls into it.

## Remaining candidates

Something in Xenia's guest-execution path that blocks without a kernel call,
a fault, or a wait record. The one class not yet examined is how the JIT
translates PowerPC synchronisation and hint instructions - lwarx/stwcx
reservation loops, `db16cyc`, and the `or rX,rX,rX` thread-priority hints. If
any of those lowers to a host sleep or wait, a guest spin-wait would present
exactly like this: no CPU, no kernel calls, no faults.

# Phase 97 - Closing a logging blind spot; the last code is XUI animation setup

## Hint instructions ruled out

`db16cyc` (0x7FFFFB78, `or r31,r31,r31`) lowers to `f.DelayExecution()`, and
with `delay_via_maybeyield` off - the default - that emits a bare `pause`. A
spin loop of PAUSE still consumes a core, and the measured CPU is flat, so the
PowerPC hint/sync instructions are not the block. Other `or rX,rX,rX` forms
(r13, r14, seen throughout xam) become `Nop`.

## A blind spot in my own evidence

"No kernel calls after KeTlsSetValue" was **uncontrolled**: Xenia suppresses
exports marked `kHighFrequency` unless `log_high_frequency_kernel_calls` is on,
and `RtlEnterCriticalSection` is one of them. So the busiest and most
deadlock-prone calls were exactly the ones invisible.

Re-run with that cvar on (505 MB of log). The Guide thread's true last calls:

```
RtlEnterCriticalSection(81D6D030)
RtlLeaveCriticalSection(81D6D030)      <- released
DemandFunction: enter/defined 8174EE20
DemandFunction: enter/defined 8174E220
```

The critical section is entered **and left**, so there is no CS deadlock -
which also re-confirms the wait probe from the other direction.

## What runs last

R 8174E220 / G 81755420 reads [r3+104], indexes a table at 81D21680, and
formats with the UTF-16 string at 815FB0F0:

```
"End%sTo%s"     followed by "Visual", "NuiHudHove..."
```

Those are XUI **named-frame / timeline** names. So the final code executed is
animation-transition setup inside scene creation - it is building the name of a
transition frame - and it blocks there.

That is consistent with everything else: the scene is far enough along to be
wiring up its animations.

# Phase 98 - The thread is alive and blocked, and it holds the XUI lock

`GetExitCodeThread` on the stuck guest thread's native handle, sampled
repeatedly:

```
ThreadState 0: STILL_ACTIVE
ThreadState 1: STILL_ACTIVE
ThreadState 2: STILL_ACTIVE
ThreadState 3: STILL_ACTIVE
```

So it has not died - which mattered, because a terminated thread would produce
every symptom seen so far (flat CPU, no kernel calls, no waits, no faults) and
would also leave xam's XUI critical section held forever, exactly matching the
title thread's behaviour. That hypothesis is now excluded.

## Full picture

The Guide thread is:

- **alive** (STILL_ACTIVE)
- **consuming no CPU** at all after ~16ms (controlled measurement)
- making **no guest kernel wait** (0 of ~10000, controlled)
- taking **no memory fault** (0 of ~1200, controlled)
- making **no kernel call** even with high-frequency logging on
- not in the JIT (all DemandFunction calls balanced)
- **holding a lock** the title thread needs - the title thread enters XUI
  drawing, compiles two XUI functions, and stops

A live thread with zero CPU is in a host-level wait. Since it is not a guest
wait, it is one of Xenia's own: the global critical region, the code-cache
mutex, or a memory/heap lock.

## The shape this suggests

A lock-order inversion across the guest/host boundary: the Guide thread holds
xam's XUI critical section (a guest lock) and blocks on a Xenia host lock,
while the title thread holds or needs the host side and blocks on the guest
lock. Nothing in the guest can break that, and it would explain why the hang is
insensitive to which thread runs the scene creator.

Next: instrument acquisition of Xenia's global critical region for this thread.

# Phase 99 - The global critical region is not the blocker

Probed Xenia's `global_critical_region` from the watchdog with `TryAcquire()`
every 500ms while the Guide thread is stuck:

```
GlobalLock 0: HELD
GlobalLock 1: HELD
GlobalLock 2: HELD
GlobalLock 3: HELD
GlobalLock 4: HELD
GlobalLock 5: FREE
```

Held for about 2.5 seconds and then **released**. That is a long hold and worth
noting on its own, but it rules out the specific hypothesis from the last
phase: if the Guide thread were permanently blocked on the global lock, or
permanently holding it, the lock would never come free. It does.

So the lock-order inversion story - Guide thread holding xam's XUI critical
section while blocked on Xenia's global lock - is **not supported** for that
lock. The stuck thread is blocked on something else.

Remaining host-side candidates, none yet tested: the x64 code-cache mutex, the
memory/heap locks, and any per-object mutex reached from an HLE path. Each can
be probed the same way, since TryAcquire-style checks from a watchdog are cheap
and give a direct answer rather than an inference.

## Standing summary of the blocked thread

alive; zero CPU; no guest kernel wait; no memory fault; no kernel call; not in
the JIT; not blocked on the global critical region; and the title thread stalls
behind it once it enters XUI drawing.

# Phase 100 - It is not a hang. It is a guest crash, and Xenia pauses.

Suspended the stuck thread, read its host RIP, and symbolized with dbghelp:

```
rip sym: ZwWaitForAlertByThreadId+20
stack[34] exe+13308D  xe::Emulator::Pause+685
stack[47] exe+907AB0  xe::gpu::CommandProcessor::Pause lambda
stack[80] exe+133A26  xe::Emulator::ExceptionCallback+374
```

**The thread is inside Xenia's exception handler, which called Emulator::Pause.**
Guest code faulted inside XuiSceneCreate; Xenia caught it and paused the whole
emulator. That is why every measurement looked the way it did:

- zero CPU, alive, no kernel calls: it is parked in Pause's wait
- the title thread stalls too: Pause suspends all guest threads
- the XUI critical section stays held: its owner was frozen mid-flight
- no guest kernel wait, no memory fault *recorded for this thread*: the fault
  went to the host handler, not through any guest path

None of those was wrong, but every one of them was describing a paused
emulator rather than a deadlock. Eleven phases of lock and wait analysis were
answering the wrong question.

## Reinterpreting the "fatal breakpoint signature"

The `Unable to read thread context for stack walk` lines, used since phase 83
as proof that a breakpoint fired, are emitted by this same crash-reporting
path. For the breakpoint runs the interpretation still holds - the UD2 *is* the
exception - so those reachability results stand. But the signature means "a
guest exception occurred", not "a breakpoint was hit", and the same lines appear
here with no breakpoints installed at all.

## What this changes

The problem is no longer "what is it waiting on" but **"what does hud crash
on"**. The scene creator gets as far as XUI animation setup (phase 97) and
faults. The exception is caught before it reaches any of the instrumented guest
paths, which is why it left no trace in the kernel, wait or fault probes.

Next: read what Emulator::ExceptionCallback records, and get the faulting guest
address out of it.

# Phase 101 - The crash, at last: bad pointer in XUI animation setup

## Why it was invisible

Xenia's `Emulator::ExceptionCallback` calls `Pause()` **before** building its
crash dump. Pause waits for the graphics system and command processor to
acknowledge, and when a guest thread faults mid-frame that wait does not
return - so the dump is never written and a diagnosable crash presents as a
silent freeze. That is exactly what has been chased for the last twelve phases.

Fixed by logging the essentials before Pause. This is a genuine Xenia
improvement independent of the Guide work: a crash you cannot see is much worse
than one you can.

## The crash

```
GUEST CRASH: access violation at guest PC 8174E22C (host A08F4D07),
             fault_addr 0000000100000068
```

Guest PC 8174E22C (R) = G 8175542C, which is the first body instruction of the
function whose prologue we had already identified:

```
81755420  mfspr r12,8
81755424  bl __savegprlr_29
81755428  stwu r1,-0xC0
8175542c  lwz r11,104(r3)     <- faults here, 104 = 0x68
```

and the fault address is 0x1_00000068 = **r3 + 0x68 with r3 = 0x100000000**.

So the function is entered with an invalid `this` pointer and dies on its first
dereference. It is the XUI animation-transition helper from phase 97 - the one
that formats "End%sTo%s" frame names - so the scene gets as far as wiring up
its transitions and is then handed a bad object.

## Note on the value

0x100000000 is exactly 1<<32: the low 32 bits are zero, with bit 32 set. A
valid guest pointer is a 32-bit value, so this is not a plausible truncated
address - it looks like a 64-bit value that was never meant to be a pointer, or
a computation that overflowed into the upper word.

## Next

Find the caller and what it passes in r3. That is now an ordinary
reverse-engineering question with a concrete address, rather than an
open-ended search for a phantom deadlock.

# Phase 102 - Root cause: an uninitialised xam global, and a discarded assert

## The caller

The crashing function's caller (G 81756020, the one compiled immediately
before the fault):

```
81756050  lis r10,0x81d4
81756054  lwz r3,-1756(r10)     ; r3 = [0x81D3F924]
81756058  cmplwi r3,0
8175605c  bne -> 81756064       ; non-zero: skip the trap
81756060  .long 0x0fe00019      ; twi - the guest's own assertion
81756064  lwz r4,4(r4)
81756068  stw r4,12(r11)
8175606c  bl 81755420           ; ...calls anyway, with r3 still null
```

And 0x100000068 is Xenia's guest membase (0x100000000) plus 0x68, so **r3 is
NULL** - not the odd 1<<32 value guessed last phase. That guess is corrected:
the register is zero, and the reported fault address is a host address.

## The chain, complete

1. The xam global at **0x81D3F924** is never initialised under this bootstrap.
2. xam checks it and traps - `twi` - which is the guest catching the problem
   itself, exactly as designed.
3. Xenia's `ignore_trap_instructions` defaults **true**, so the assertion is
   discarded and execution continues.
4. Four instructions later the null is dereferenced at `lwz r11,104(r3)` and
   the guest faults.
5. `ExceptionCallback` pauses before dumping, so nothing is reported.

Every step after (2) is a consequence of ignoring the guest's own guard. The
guest was correct and was overruled.

## Who should set it

One writer exists: G 8175711C, inside the function starting at **G 81756FE0 /
R 8174FDE0**. It asserts the global is currently null before storing, so it is
a one-time initialiser for whatever object the animation helper expects.

That is the missing bootstrap step - the same shape as every other gap in this
work: a xam init routine that the real system calls and this bootstrap does
not.

## Worth testing separately

Running with `ignore_trap_instructions = false` should turn this crash into a
reported guest assertion at 81756060 instead of a null dereference four
instructions later - a much more honest failure, and possibly several
earlier-phase mysteries too.

# Phase 103 - Correction: the assert is dropped by trap type 25, not by
# ignore_trap_instructions

Phase 102 said the guest's assertion was discarded because
`ignore_trap_instructions` defaults true. **The default is true in source, but
this project's config sets it false**, so traps are enabled here and that
explanation was wrong. Caught by checking the config rather than the source
default.

## What actually happens

The caller is confirmed, not inferred this time. LR at the crash is
8174E228 - the function's own `__savegprlr_29` return, useless for this - but
that helper stores the real LR at [r1-8] before the stwu, so it is recoverable:

```
GUEST CRASH: saved lr candidate [r1+B8] = 8174EE70
```

R 8174EE70 = G 81756070, the instruction after `bl 81755420` at G 8175606C. So
the caller is G 81756020, as guessed - but now on evidence.

And the global really is null:

```
GUEST CRASH: [81D3F924] = 00000000
GUEST CRASH: r3=0 r4=2 r5=0
```

So the `twi` at G 81756060 **did execute**. It reported nothing because of
this, in x64_emitter.cc:

```cpp
void X64Emitter::Trap(uint16_t trap_type) {
  switch (trap_type) {
    case 20: case 26:  ... TrapDebugPrint
    case 0:  case 22:  ... TrapDebugBreak
    case 25:
      // ?
      break;                      <- emits nothing
    default: XELOGW("Unknown trap type {}"); db(0xCC);
```

xam's assertion form is `twi 31,r0,0x19` - **trap type 25** - and Xenia's
emitter has an empty case for it with a `// ?` comment. The guard is dropped
silently even with trapping fully enabled, and even the "unknown trap type"
warning is skipped because 25 is explicitly listed.

## Why this matters beyond the Guide

Every `0x0FE00019` in xam - and they are everywhere in the code read during
this work, guarding exactly the conditions this bootstrap violates - is a
silent no-op. xam has been reporting its own failures all along and none of
them could reach us. That is likely behind several earlier phases where a
condition was violated and execution simply continued into nonsense.

## Corrected chain

1. xam global 81D3F924 is never initialised here.
2. G 81756058 checks it, finds null, and executes `twi 31,r0,25`.
3. Xenia's emitter has an empty case for trap type 25: nothing happens.
4. Execution continues to `bl 81755420`, which dereferences the null at
   `lwz r11,104(r3)` and faults.
5. ExceptionCallback pauses before dumping, so it presents as a freeze.

Steps 3 and 5 are both Xenia gaps, and both hide the guest's own diagnosis.

# Phase 104 - Implemented trap type 25; xam has been asserting all along

Filled in the empty `case 25` in `X64Emitter::Trap` with a native that reports
the trap, behind a new `log_guest_asserts` cvar.

## First attempt was unusable, and that is informative

Logging every hit produced **3,860,079 lines in one run** - all from one
background thread at the same call site with r3 = 0xC000000D
(STATUS_INVALID_PARAMETER). So trap 25 is not a rare fatal assertion; a single
failing guest loop can hit one millions of times. Xenia's empty case is
defensible on volume grounds even if the `// ?` comment suggests it was never
investigated.

Rate-limited to the first three hits per call site, which gives **17 lines**
and a clear picture:

```
F80000DC lr=8177AC78  r3=C000000D r4=81D42528 r5=1
F80000E8 lr=81750FB8  r3=0        r4=04A00004 r5=81603920
01000024 lr=817519F4  r3=1        r4=2        r5=0
F800011C lr=8178DC6C  r3=0        r4=7042F9F0 r5=FFBE2008
F800011C lr=819F4434  r3=1        r4=0        r5=200
F800011C lr=817F77F0  r3=00810001 r4=0        r5=0
```

## The one that matters

**lr=8178DC6C is inside xam's render-host function at R 8178DC58** - the
function this bootstrap calls directly. It is the assert seen back in phase 68:

```
81794e68  bl 8177fdb8        ; lr = 8178DC6C at runtime
81794e6c  cmpwi r3,0
81794e70  bne -> skip
81794e74  twi                ; fires - r3 == 0
```

So the very first call inside the render host returns 0 and xam immediately
declares its precondition violated. That was noted in phase 68 and dismissed
because "Xenia ignores traps" - which was wrong twice over: traps are enabled
here, and this one was being dropped by the empty case rather than by the cvar.

Everything built on top of that render-host call has been running on a
foundation xam had already flagged as broken.

## Value beyond this project

Any Xenia user seeing unexplained guest crashes has this same blind spot: the
guest often reports the problem first, at the right place, and the emitter
throws it away.

# Phase 105 - xam's render host asserts it is on a specific thread

Decoded the call whose failure xam flags at the top of the render host
(G 8177FDB8, eight instructions):

```
8177fdbc  lwz r10,256(r13)     ; current thread, via the PPC thread pointer
8177fdc0  addi r11,r11,9152    ; r11 = 0x81D423C0
8177fdc4  lwz r11,352(r11)     ; [0x81D42520] - a recorded thread
8177fdc8  subf r11,r10,r11
8177fdcc  cntlzw r11,r11
8177fdd0  rlwinm r3,r11,27,31,31   ; r3 = 1 iff the two are equal
8177fdd4  blr
```

It returns 1 only when the calling thread matches the one stored at
**0x81D42520**. The render host calls this first and traps when it returns 0.

So xam's XUI render host has a **thread-affinity precondition**: it must run on
the thread xam recorded, and this bootstrap calls it from whichever thread is
convenient - originally the Guide dispatch thread, later the title's swap
thread. Neither is xam's thread, so the assert fires every time.

## What this reframes

Phase 74 established that the *device* is thread-affine by reading D3D's own
warning. This is a second, independent affinity requirement, in xam rather than
D3D, and it was being violated from the very first render-host call - which
means the XUI context, the DC, the class registrations and the scene creation
have all been performed from a thread xam does not consider legitimate.

That is a far better explanation for a null global deep inside animation setup
than anything proposed in the last ten phases: state that xam initialises on
its own thread was never initialised, because its owner thread never ran.

## The obvious next question

Who writes 0x81D42520, and can that thread be made to exist? If xam records its
UI thread during a startup path this bootstrap skips, then the fix is upstream
of everything attempted so far.

# Phase 106 - The thread check is real but not the cause

Read the two values xam compares at the top of the render host:

```
GuideBootstrap: xam UI thread recorded=30030010 current=30058010
```

Both are real guest thread pointers and they differ - so xam **did** register a
UI thread of its own (30030010, created during LLE xam init) and this
bootstrap calls from a different one. The precondition is genuinely violated,
as phase 105 said.

## But satisfying it changes nothing

Added `guide_spoof_ui_thread`, which points 0x81D42520 at the calling thread
across the render-host call and restores it afterwards:

```
GuideBootstrap: spoofing xam UI thread 30030010 -> 30058010
GuideBootstrap: render host -> 00000000, XUI ctx 4087D980, provider 81D22A54
GUEST CRASH: access violation at guest PC 8174E22C, fault_addr 100000068
GUEST CRASH: [81D3F924] = 00000000
```

Identical crash, identical null global. So the thread check being satisfied
does not cause the missing state to appear.

Phase 105 claimed this affinity violation "explains a null global deep inside
animation setup far better than anything proposed in the last ten phases".
That claim is **weakened to the point of withdrawal**: the mechanism is real,
but the experiment shows it is not what leaves 81D3F924 null. Being able to
test a hypothesis cheaply is worth more than how good it sounded.

## What stands

- xam records a UI thread at 0x81D42520 and its render host requires callers to
  match it; we never do. Real, and worth fixing properly rather than spoofing.
- The global at 0x81D3F924 is null for some other reason, and the one writer
  found (G 8175711C, in the function at R 8174FDE0) needs a valid object in r3
  that we do not have.
- Making the check pass does not make the crash go away, so these are two
  independent gaps rather than one chain.

# Phase 107 - The missing init is a XUI command, never dispatched

Traced the one writer of 0x81D3F924 upward. Its only caller is at G 817571FC,
inside a small dispatcher starting at **G 817571D0**:

```
817571d0  lwz r11,4(r3)        ; bounds check on r3
817571e8  lwz r11,4(r4)        ; command type from [r4+4]
817571ec  cmplwi r11,0x13
817571f0  bne -> 81757200
817571f4  addi r5,r4,8
817571f8  lwz r4,16(r4)
817571fc  b 81756fe0           ; tail-branch to the animation initialiser
81757200  cmplwi r11,0x27      ; -> 81755E80
81757214  cmplwi r11,0x0F      ; -> ...
```

So the function that sets the animation global is **command handler 0x13** in a
XUI command dispatcher. It is not called directly by anything; it runs when a
command of type 0x13 is dispatched, with its arguments unpacked from the
command block (`r5 = cmd+8`, `r4 = [cmd+16]`).

## What that means

The null global is not a missing function call this bootstrap could simply make
- it is a **command that was never issued**. XUI here is driven by a stream of
commands, and the Guide's scene setup depends on earlier commands having run.

This is the same shape as every other gap in this work, one level lower down:
not "call the init that nothing calls", but "the thing that would have called it
is a message pump we never start". It also fits phase 105's finding that xam
records a UI thread - a command-driven subsystem with an owning thread is
exactly what a pump implies.

## Consequence for approach

Driving individual xam entry points, which has carried this investigation from
phase 60 onward, reaches its limit here. The remaining state is produced by a
command stream, and reproducing it call-by-call means reimplementing the pump
rather than calling into it. The alternative is to find what starts that pump in
a real boot and run that instead - which is where the next attempt should go.

# Phase 108 - The dispatcher is XuiCanvas's message handler

The command dispatcher (R 8174FFD0) has no direct callers. Searching xam's data
for the pointer finds it exactly once, in .rdata at 815FAD38, immediately after
a UTF-16 string:

```
815FAD20: "XuiCanvas"
815FAD34: 8174BB68        ; another entry
815FAD38: 8174FFD0        ; <- the dispatcher
815FAD44: "CHUDBkgndScene::Setu..."   (ASCII, an assert string)
```

So this is a **XUI class descriptor for XuiCanvas**, and the dispatcher is its
message handler. Command 0x13 to a XuiCanvas is what initialises the animation
global at 81D3F924.

Incidentally this confirms the address convention from the other direction:
stored pointers in xam's data are runtime addresses (8174FFD0), not the
ghidra-space values (817571D0), which is why the search for the latter found
nothing.

Also worth noting: xam's .rdata contains ASCII symbol names like
`CHUDBkgndScene::Setup`, i.e. hud's own class names appear in xam's assert
strings. The two modules are built together and xam knows about hud's scenes.

## Where this lands

The chain is now fully traced from the crash back to its origin:

```
button -> handler -> scene creator -> XuiSceneCreate
  -> animation setup reads [81D3F924]  (null)
  -> set only by command handler 0x13
  -> which is XuiCanvas's message handler
  -> reached only by XUI message dispatch to a canvas object
```

hud imports XuiSendMessage, XuiBubbleMessage and XuiBroadcastMessage, so the
mechanism is XUI's own message system. The Guide's canvas never receives its
initialisation message because the scene it belongs to is the very thing being
constructed when the crash happens.

That is a genuine ordering dependency inside XUI, not another missing entry
point, and it is the clearest statement yet of why hand-driving xam from
outside cannot finish this: the remaining state is produced by objects sending
each other messages during a construction sequence this bootstrap enters
halfway through.

# Phase 109 - The crash has a name: CHUDBkgndScene::PlayTransition

xam's .rdata carries ASCII symbol names used in its assert messages. Four are
HUD-related:

```
CHUDBkgndScene::MessageBox
CHUDBkgndScene::PlayTransition
CHUDBkgndScene::SetupCursorControl
CHUDBkgndScene::SetupNuiTipsScene
```

so **xam owns the HUD background scene class**, not hud.xex.

Finding the code that references those strings:

```
"CHUDBkgndScene::PlayTransition"  (815FB014) referenced from
    G 81755510   <- inside the crashing function at G 81755420
    G 817555C0   <- same function
```

**The function that crashes is `CHUDBkgndScene::PlayTransition`.** That matches
everything observed about it: it formats "End%sTo%s" named-frame strings
(phase 97), looks up named frames, and dies on `lwz r11,104(r3)` with r3 null.

## The complete, named chain

```
Xbox button
  -> hud's registered handler (913E69C0), sub-command 1
  -> hud's scene creator (913EB940) -> its XUI init -> 56 registrations
  -> XuiSceneCreate
  -> CHUDBkgndScene::PlayTransition  (G 81755420)
       reads the CHUDBkgndScene singleton from [81D3F924] -> NULL
       dereferences it at +0x68 -> access violation
```

and the singleton at 81D3F924 is created by **XuiCanvas message command 0x13**
(phase 108), which is never dispatched because the canvas belongs to the scene
currently being built.

## What is actually missing, stated plainly

The Guide's background scene object is never constructed. Everything else -
device, XUI context, DC, class registry, resources, the scene creator itself -
now works. The single missing thing is one object, created by one message, sent
during a construction sequence that this bootstrap enters partway through.

That is the whole result: pressing the Xbox button runs the real Guide code as
far as playing its background-scene transition, and stops there because the
scene that owns the transition was never built.

# Phase 110 - The Guide scene is created

Two fixes, one of them mine.

## 1. Do not patch guest code

The first attempt at getting past CHUDBkgndScene::PlayTransition NOPed the call
site in guest memory. That **killed the emulator process outright** - guest code
pages are not writable from the host that way, and the store faults with no log.
Replaced with a data-only probe: allocate a zeroed 0x400 block and put it in the
null CHUDBkgndScene slot at 81D3F924, so the dereference lands on readable
memory.

That is explicitly a probe, not a fix - the object has no vtable and no state.
But it answers the question it was built for.

## 2. The scene creator's third argument is an out-pointer

With the transition survivable, the crash moved into **hud** at 913EBA18:

```
913eba14  lwz r11,24(r31)     ; the created scene handle
913eba18  stw r11,0(r29)      ; store it through the caller's out pointer
```

r29 is the scene creator's **third argument**, and this bootstrap has been
calling it as `(obj, 0, 0)` since phase 84 - storing through null. Passing a
real 16-byte out slot:

```
GuideBootstrap: scene creator 913EB940 -> 00000000, scene=00010000
Guide composite draw #1   -> 00000000
Guide composite draw #600 -> 00000000
```

**The scene creator returns success and hands back a XUI scene handle**, and the
draw loop runs continuously without the emulator stalling. That is the first
time scene creation has completed in this project.

Note the earlier reading that the registrars "hang the title thread" was wrong
in attribution: they were never hanging, they were reaching the CHUDBkgndScene
crash. `guide_register_classes` now defaults on, since XuiScene comes from
there and scene creation fails 80300004 without it.

## Still not on screen

Screenshot after the press remains byte-identical to the dashboard baseline
(md5 a6eb4cfa). The scene exists and draws succeed, so what remains is the
presentation gap identified in phases 72-74: hud renders through xam's D3D
device and Xenia presents the title's front buffer. Nothing about scene
creation changes that.

# Phase 111 - Why the Guide's rendering goes nowhere: no GPU ring

With the scene now created, checked what GPU work the Guide path actually
produces. Every Vd call after the button press:

```
9 VdSwap        (all from the title thread, i.e. dash)
```

and nothing else - no `VdInitializeRingBuffer`, no ring or command-buffer setup
for xam's device. So **xam's D3D device never gets a GPU ring**. Its rendering
queues into a command buffer nobody submits, which is why hud's draws return
success while producing no visible output and never reaching VdSwap
(phase 72).

That completes the presentation story: it is not that Xenia presents the wrong
front buffer, it is that the Guide's device produces no GPU work at all.

## Retrying the title's device

The obvious response is to render through the title's device, which does have a
live ring. Retried now that the scene builds:

```
GuideBootstrap: xam device global -> title device 40952400
GuideBootstrap: scene creator 913EB940 -> 00000000, scene=00010000
GUEST CRASH: access violation at guest PC 819E567C, fault_addr 100010007
             lr=819E5640 r3=0 r4=00000104 r5=816604D8
```

Scene creation still succeeds, and the crash moves into xam's own D3D code
(819E5xxx) with a null object. So handing xam a device it did not create is not
sufficient either - xam keeps per-device state alongside the pointer, and that
state is missing.

Reverted to xam's own device, which is the configuration where everything up to
and including scene creation works.

## Position

Working: app, object, heap, XUI classes, XuiInit, device creation, render
context, DC, root element, resources, **scene creation**, and a per-frame draw
on the title thread that returns success.

Missing: any GPU ring for the device the Guide renders through. That is the one
remaining thing between this and pixels, and it is a Xenia-side gap - a second
guest D3D device is not something the emulator currently supports.

# Phase 112 - The named gap: the system command buffer is not implemented

System software does not render through the title's ring. It renders through
the **system command buffer**, and that is what Xenia lacks.

xam imports all three of the relevant exports:

```
815F0A08  1BD  VdGetSystemCommandBuffer
815F0A10  1D8  VdSetSystemCommandBuffer                   !!  (unimplemented)
815F0A20  1D9  VdSetSystemCommandBufferGpuIdentifierAddress
```

and Xenia's side of them:

```cpp
void VdGetSystemCommandBuffer_entry(lpunknown_t p0_ptr, lpunknown_t p1_ptr) {
  p0_ptr.Zero(0x94);
  xe::store_and_swap<uint32_t>(p0_ptr, 0xBEEF0000);
  xe::store_and_swap<uint32_t>(p1_ptr, 0xBEEF0001);
}
DECLARE_XBOXKRNL_EXPORT1(VdGetSystemCommandBuffer, kVideo, kStub);

void VdSetSystemCommandBufferGpuIdentifierAddress_entry(lpunknown_t unk) {
  // r3 = 0x2B10(d3d?) + 8
}
DECLARE_XBOXKRNL_EXPORT1(..., kStub);
```

`VdGetSystemCommandBuffer` hands back two magic placeholders and nothing real;
`VdSetSystemCommandBufferGpuIdentifierAddress` has an empty body; and
`VdSetSystemCommandBuffer` has no implementation at all.

## The complete answer to "what happens when you press it"

The press runs the real Guide code end to end. The Guide app is created, its
XUI classes are registered, XuiInit succeeds, xam creates a D3D device, the XUI
render context and DC are created, resources resolve out of hud's own XEX
section, **the scene is created and returns a live handle**, and hud's draw runs
on the title's render thread every frame returning success.

Nothing appears because the Guide submits its drawing to the system command
buffer, and in Xenia that buffer is two magic constants. There is no ring, no
submission and no composition path - which is also why the graphics
notification routines (phase 72) had to be implemented before the draw could
even be invoked.

So the remaining work is not in the guest at all. It is: implement the system
command buffer, and the second-device GPU path behind it, in Xenia's GPU
backend. Everything above that line now runs.

# Phase 113 - The remaining gap is tractable, not architectural

Checked whether Xenia's GPU side could execute system-submitted commands at
all. It can:

```
src/xenia/gpu/pm4_command_processor_declare.h
  void ExecuteIndirectBuffer(uint32_t ptr, uint32_t count);
  void ExecutePacket(uint32_t ptr, uint32_t count);
  virtual uint32_t ExecutePrimaryBuffer(uint32_t start, uint32_t end);
```

The command processor already knows how to execute PM4 packets from an
arbitrary guest address - `ExecutePrimaryBuffer` is just the ring-driven case.

So the missing piece is not a new GPU architecture, it is wiring:

1. `VdGetSystemCommandBuffer` currently hands back 0xBEEF0000 / 0xBEEF0001.
   Give it a real allocated guest buffer and record its address and size.
2. `VdSetSystemCommandBuffer` (0x1D8, currently no implementation) records
   what the system sets.
3. At submission - the swap is the natural point, since that is where the
   composited frame is expected - hand the buffer to `ExecutePacket` or
   `ExecuteIndirectBuffer` so the Guide's drawing executes against the same
   GPU state as the title's.

That is a bounded piece of work in Xenia's kernel and GPU layers, of the same
kind as the graphics-notification exports implemented in phase 73, rather than
"a second guest D3D device is unsupported".

## Summary of the whole investigation

Pressing the Xbox button now runs the entire real Guide stack: app creation,
XUI class registration, XuiInit, xam D3D device creation, render context, DC,
resource resolution from hud's own XEX section, **scene creation returning a
live handle**, and a per-frame draw on the title's render thread that returns
success every frame, with the dashboard unaffected throughout.

The Guide does not appear because its drawing is submitted to a system command
buffer that Xenia stubs out with two constants. Everything above that line
works; the line itself is three kernel exports and one call into an existing
command-processor API.

# Phase 114 - Correction: the system command buffer claim is not established

Phase 112 concluded that "the Guide submits its drawing to the system command
buffer, and in Xenia that buffer is two magic constants", and phase 113 built a
three-step implementation plan on it. **Both overstate the evidence.**

Instrumented the stub. It is called constantly - over 4000 times in a 45-second
run, all from the title thread with the same argument addresses:

```
VdGetSystemCommandBuffer #5    from '' p0=7042FA40 p1=7042F9E4
VdGetSystemCommandBuffer #2000 from '' p0=7042FA40 p1=7042F9E4
VdGetSystemCommandBuffer #4000 from '' p0=7042FA40 p1=7042F9E4
```

The first five all occur **before** the Guide scene is created, and the
dashboard renders correctly throughout. So a caller receives those two magic
constants thousands of times and still draws fine. Whatever the stub breaks, it
does not break rendering in general, and there is no evidence tying it to the
Guide's specific failure.

The reasoning was: xam has no ring, so its drawing must go somewhere else, and
the system command buffer is the obvious somewhere else. That is an inference
about a mechanism, dressed up as a finding, and the check that would have
tested it - does anything actually route Guide drawing through it - was not
done before writing the conclusion.

## What is still established

- xam's device never gets a GPU ring (no VdInitializeRingBuffer for it).
- hud's draws return success every frame and produce no VdSwap and no pixels.
- Xenia's command processor can execute PM4 from an arbitrary guest address,
  so a submission path is implementable if one is identified.

## What is not

- That the Guide's drawing goes through the system command buffer.
- That implementing those three exports would produce pixels.

The honest statement of the remaining gap is narrower: **the Guide's draw calls
succeed but nothing they produce reaches the GPU, and the route by which system
drawing is meant to reach it has not yet been identified.**

# Phase 115 - Confirmed: the Guide's draw never touches the system command
# buffer

Phase 114 retracted the system-command-buffer claim as unproven. Now it is
disproven, properly.

Added a `thread_local in_guide_draw_scope` flag set around the Guide's draw
inside VdSwap, and tagged the VdGetSystemCommandBuffer probe with it:

```
GUIDE DRAW tagged: 0
```

with the untagged calls still numbering in the thousands - so the instrument is
live and the Guide's draw makes **none** of them. The route the last two phases
proposed does not exist.

This is the difference between "not established" and "false", and it took one
flag and one run. The lesson is the same one this project keeps re-learning: a
mechanism is cheap to test and expensive to assume.

## Where the Guide's drawing actually goes

hud's draw calls XuiRenderBegin / layout / XuiRenderEnd / XuiRenderPresent on
xam's DC, all of which return success. Those are xam's own XUI functions
operating on xam's D3D device - a device with no ring buffer and, as now shown,
no system command buffer either. The D3D calls therefore accumulate in
device-side memory that nothing ever submits.

So the failure is one step earlier than any of the recent phases placed it: not
a missing submission route, but a device that was created without the plumbing
that makes submission possible. xam's CreateDevice succeeded (phase 69) because
the flag bit was set, but nothing after it wired that device to the GPU.

## Honest status

Everything from the button press through scene creation works. The Guide draws
into a device that is not connected to anything. Identifying what connects a
guest D3D device to Xenia's GPU - and why xam's device never got it - is the
next question, and it is a question about device creation, not about
submission.

# Phase 116 - The DC is real; the device is the disconnected part

Dumped the render DC the Guide draws into:

```
DC 40899D40 contents:
  dc[00] = 8163E4E8 00000001 3F800000 00000000
  dc[10] = 00000000 00000000 00000000 3F800000
  dc[20] = 00000000 00000000 00000000 00000000
  dc[30] = 3F800000 00000000 00000000 00000000
  dc[40] = 00000000 3F800000 3F800000 00000000
  dc[50] = 00000000 00000000 00000000 3F800000
```

[0] is a vtable in xam's .rdata and the 3F800000 values (1.0f) sit at diagonal
positions - identity transform matrices. So the DC is a properly constructed
XUI device context, not a stub or a null.

That rules out the last "maybe it is a no-op by construction" explanation. The
Guide has a real scene, a real DC with sane state, and a draw that runs every
frame and reports success.

## The disconnected piece, stated exactly

xam's D3D device (created in phase 69 by setting the XboxHardwareInfo flag bit)
was never wired to the GPU:

- no VdInitializeRingBuffer for it - the title's device made that call once
- no system command buffer - disproven for the Guide draw in phase 115
- no VdSwap from it - measured repeatedly

On real hardware there is one GPU and one ring; the title owns it and system
software shares it. Under Xenia the title's ring is set up through the Vd
globals, and xam's separately created device has no relationship to them. Its
D3D calls have somewhere to write and nowhere to go.

## Status after this stretch

Working end to end: button press, app creation, XUI class registration,
XuiInit, xam device creation, render context, DC, resources, scene creation
returning a live handle, and a per-frame draw on the title's render thread
returning success, with the dashboard unaffected.

Not working: nothing the Guide draws reaches the GPU, because the device it
draws through is not connected to one. That is a device-creation problem in the
emulator, and the three phases that tried to name a submission route (112, 113)
were retracted and then disproven (114, 115).

# Phase 117 - The title-device path, and a hypothesis killed before it shipped

The only device with a live GPU ring is the title's, so the crash on that path
is the one worth understanding. It is at G 819EC87C:

```
819ec870  lwz r11,11036(r30)   ; device field at +0x2B1C
819ec874  cmplwi r11,0
819ec878  beq -> 819ec884
819ec87c  stw r11,8(r28)       ; faults
```

with fault address 0x100010007, i.e. guest 0x10007, so **r28 = 0xFFFF** - a
16-bit value being used as a pointer, and non-zero so it passes the null check
just above.

The +0x2B1C offset is D3D device state; Xenia's own comment on
`VdSetSystemCommandBufferGpuIdentifierAddress` reads `r3 = 0x2B10(d3d?) + 8`,
placing that region in the same structure.

## The hypothesis I did not commit

The obvious explanation was a layout mismatch: xam's D3D reading the title's
device at a large fixed offset would break if the two were built differently.
The dashboard was described as 17559 at the start of this project and xam is
from a 17489 extraction, which fitted neatly.

Checked it instead of writing it up. Every module in the log reports **17489**:

```
Version: 2.0.17489.0        Min Version: 2.0.17489.0
Version: 0.0.17489.32
```

Same build throughout, so the versions match and the mismatch explanation is
dead. r28 = 0xFFFF has some other cause.

Recording this because the previous few phases went the other way - a neat
mechanism written up as a finding and then retracted twice. One grep was enough
here, before rather than after.

## Position unchanged

Everything through scene creation works. Rendering reaches no GPU because xam's
device is unconnected, and the title's device cannot simply be substituted:
xam's D3D crashes on it with a malformed pointer whose origin is not yet known.

# Phase 118 - Consolidated state

Verified the current build end to end in its default configuration.

```
GuideBootstrap: render host -> 00000000, XUI ctx 4087D980, provider 81D22A54
GuideBootstrap: XuiRenderCreateDC -> 00000000 dc=40896490
GuideBootstrap: scene creator 913EB940 -> 00000000, scene=00010000
Guide composite draw #1    -> 00000000
Guide composite draw #2700 -> 00000000
```

No guest crash. Dashboard framebuffer still byte-identical to the known-good
baseline (md5 a6eb4cfa), so nothing in this work regresses the title.

(The error count is higher than the old baseline only because guest assertion
traps are now reported at all - phase 104 - which is new information, not new
failure.)

## What pressing the Xbox button does, definitively

1. `Emulator::on_guide_button_pressed` dispatches on a guest thread in the
   system process.
2. hud's registered handler (913E69C0) runs with message 0x80000004 and takes
   its sub-command path.
3. The Guide object is allocated, constructed, and stored in hud's singleton
   at [0x91400690].
4. The XUI bootstrap runs on the title's render thread: xam's render host,
   38+56 class registrations, XuiInit, XuiRenderCreateDC.
5. hud's scene creator runs, resolving its resources from hud.xex's own "hud"
   XEX section via a `section://` locator, and **returns a live XUI scene
   handle**.
6. hud's draw method executes once per swap on the title's render thread,
   calling XuiRenderBegin, laying out the scene, XuiRenderEnd and
   XuiRenderPresent, returning success every frame.

Nothing appears on screen because the device those calls target - the one xam
created - is not connected to Xenia's GPU: no ring buffer, no system command
buffer, no swap. The drawing is real and it goes nowhere.

## Emulator-side fixes made along the way

Independent of the Guide, this work fixed or implemented in Xenia:

- `VdRegisterGraphicsNotification`, `VdRegisterXamGraphicsNotification` and
  `VdCallGraphicsNotificationRoutines` (previously absent or no-ops)
- guest crash reporting before `Pause()`, which was hiding every guest fault
  behind an apparent freeze
- guest assertion traps (twi type 25), previously an empty case in the x64
  emitter, so xam's own diagnostics now reach the log
- `DbgBreakPoint` guarded on debugger presence instead of always breaking
- `XexGetModuleSection` accepting an object handle as well as an hmodule
- `xbox_hardware_info_flags` exposed, with bit 0x200 required for xam's D3D
  device creation
- LLE xam importer scoping, so the dashboard keeps Xenia's HLE xam while hud
  gets the real one

# Phase 119 - How a device gets connected, and why xam's is not

Captured the title's video startup sequence at log level 3, in call order:

```
VdSetStudioRGBMode
VdInitializeEngines
VdSetGraphicsInterruptCallback
VdSetSystemCommandBufferGpuIdentifierAddress
VdInitializeRingBuffer
VdEnableRingBufferRPtrWriteBack
...
```

That is the recipe for connecting a device to the GPU, and it runs **once**, from
the title, for the single ring.

## Re-testing the system-buffer route where it might still have applied

Phase 115 showed the Guide's *draw* never calls VdGetSystemCommandBuffer. That
left a gap: xam's device might request the buffer once at *creation* instead of
per frame. Tagged xam's CreateDevice with its own scope flag and re-ran:

```
Guide button: xam CreateDevice returned 00000000, device now 40877380
(no [XAM CREATEDEVICE] tagged calls)
```

So neither the draw nor the device creation touches it. The system command
buffer is not on the Guide's path at any point, which closes the question
phases 112-115 circled.

## The actual position

xam's CreateDevice succeeds and produces a device object without making any of
the connection calls above - no engines, no interrupt callback, no ring, no
write-back. It constructs a software object and nothing more.

On hardware those calls are made once for the one GPU, by whoever boots the
display; system software then shares it. Under Xenia the title makes them, and
a device created afterwards by other means has no relationship to that setup.
So the Guide draws through a device that was never wired to anything, and there
is no per-device wiring call it could be given, because the wiring is global and
already spent.

That is the end of what can be established from the guest side. Making the
Guide visible requires either running xam's device creation inside the boot
sequence that owns the ring, or teaching Xenia to composite a second device -
both emulator-side changes well beyond driving entry points.

# Phase 120 - The title-device substitution is rejected by xam itself

Traced the crash on the "give xam the title's device" path to its function:
**G 819EC550**, which takes (device, index, ..., ...) and immediately asserts
its second argument:

```
819ec55c  or r30,r3,r3         ; device
819ec560  or r29,r4,r4         ; index
819ec57c  cmplwi r29,0x1A
819ec580  blt -> 819ec588
819ec584  .long 0x0fe00019     ; twi - index must be < 0x1A
```

The crash reported r4 = **0x104**, far outside that range, and with trap type 25
now reported the guest says so out loud before dying:

```
GUEST ASSERT (twi 25) #1 at lr=81A012B8: r3=0 r4=00000104 r5=816604D0
GUEST ASSERT (twi 25) #2 at lr=81A012B8: r3=0 r4=00000104 r5=816604D0
GUEST ASSERT (twi 25) #3 at lr=81A012B8: r3=0 r4=00000104 r5=816604D8
```

then computes r28 = 0xFFFF from the bad index and stores through it.

So the substitution does not fail for an obscure reason: **xam checks the value,
declares it invalid, and only crashes because the check does not stop it.** A
device object it did not create yields an out-of-range index on the very first
lookup.

This is the payoff of the trap-25 work from phase 104. Before that fix this path
looked like an unexplained access violation at a random D3D address; now the
guest's own diagnosis arrives three times before the fault, naming the bad value.

## Closing the route

Handing xam the title's device is not viable, and the guest says why. Combined
with phase 119 - xam's own device is created without any of the GPU connection
calls, and those calls are global and already spent by the title - both paths to
putting the Guide on screen are closed from the guest side.

# Phase 121 - The drawing produces no command data at all

Sampled xam's device around +0x2B00 - the region its own D3D code indexes - on
the Guide's first draw and again 200 draws later:

```
draw1   dev+2B00: 00020000 44518000 00000000 00000001
draw1   dev+2B10: 00000000 00000000 00000000 00000000
draw1   dev+2B20: 00000000 0000200E 00000000 00000000
draw1   dev+2B30: 00000000 00000000 00000000 80200000

draw200 dev+2B00: 00020000 44518000 00000000 00000001
draw200 dev+2B10: 00000000 00000000 00000000 00000000
draw200 dev+2B20: 00000000 0000200E 00000000 00000000
draw200 dev+2B30: 00000000 00000000 00000000 80200000
```

**Byte-identical.** No write pointer advances, no counter moves, nothing
accumulates across 200 frames of drawing.

Note dev+0x2B10 is all zeros - and that is precisely the field Xenia's
`VdSetSystemCommandBufferGpuIdentifierAddress` stub is supposed to set, its
comment reading `r3 = 0x2B10(d3d?) + 8` and its body empty.

## Correcting my own phrasing

Several recent phases said "the drawing is real and it goes nowhere", implying
commands are produced and merely not submitted. That is **too generous**. In
this 0x40-byte window nothing is produced at all: xam's XUI render calls return
success while generating no command data, because the device they target has
none of the state that would let them.

Stated with the right caution: absence of change in one 0x40 window is not proof
of no output anywhere. But combined with no ring, no swap, no system command
buffer and no advancing device state, the weight is clearly on "inert" rather
than "unsubmitted".

## What that means for the two closed routes

It also explains why substituting the title's device failed so immediately
(phase 120): the problem is not that xam's device lacks a *destination*, it is
that xam's device lacks its own initialised D3D state, and the missing state is
supplied by kernel calls Xenia stubs out rather than by anything the guest can
be driven to do.

# Phase 122 - Nothing calls the missing exports; tidied and verified

Checked whether anything reaches the unimplemented video exports. Xenia logs
`undefined extern call` for every call to an unimplemented import, and across a
full session the only video one is `VdReadEEDIDBlock`. **`VdSetSystemCommandBuffer`
is never called at all.**

So implementing those stubs would change nothing: the state xam's device is
missing is not requested by anybody in this configuration. That closes the last
thread left open by phases 112-121.

Removed the device sampler that produced the phase 121 result - it had served
its purpose - and re-verified the build:

```
GuideBootstrap: scene creator 913EB940 -> 00000000, scene=00010000
Guide composite draw #1   -> 00000000
Guide composite draw #300 -> 00000000
```

## Final position on rendering

The Guide runs. It is created, registered, initialised, given a render context
and a DC, resolves its resources, creates its scene, and draws every frame
returning success. Its device produces no command data, is attached to no ring,
requests no system command buffer, and issues no swap - and nothing in the guest
asks for the setup that would change that, because on real hardware that setup
belongs to the boot path that owns the GPU.

Both guest-side routes are closed and now for stated reasons rather than by
exhaustion:

- **xam's own device**: created without any connection calls; those calls are
  global and already spent by the title.
- **the title's device**: xam asserts the resulting index is out of range,
  three times, before faulting on a pointer derived from it.

What remains is emulator work: either run xam's device creation inside the
sequence that owns the ring, or give Xenia a way to composite a second device.
Neither can be reached by driving guest entry points, which is the technique
this entire investigation has been built on.

# Phase 123 - xam never initialises the ring, by design

Scanned xam's entire .text for calls to the three connection imports:

```
VdInitializeRingBuffer          (thunk 81D10A3C)  - no callers
VdInitializeEngines             (thunk 81D10AAC)  - no callers
VdSetGraphicsInterruptCallback  (thunk 81D10A9C)  - no callers
```

xam imports all three and calls none of them. So the theory that "xam's device
creation skipped the connection calls" is wrong in its implication: those calls
are not part of xam's device path at all, missing or otherwise. **xam's D3D is
built never to initialise the GPU** - it expects to run on a GPU somebody else
has already brought up, which on hardware is the boot path, and to share it.

That is the cleanest statement of the situation:

- the title calls the connection sequence once and owns the ring
- xam's device is created without any such call because xam never makes one
- the sharing mechanism between them is whatever the console's kernel provides,
  and Xenia provides only the single title-owned path

So there is no missing call to add on the guest side, and no ordering fix that
would help: the code that would connect xam's device does not exist in xam.

## Conclusion of the rendering line of enquiry

Every guest-side avenue is now closed with a reason:

1. Give xam's device the connection calls - xam has none to give.
2. Substitute the title's device - xam rejects it with three assertions and
   then faults.
3. Route through the system command buffer - neither the draw nor the device
   creation requests it, and nothing in the session calls the unimplemented
   setter.

The Guide runs to completion and draws every frame. Making that visible is a
change to Xenia's GPU layer, not to the guest bootstrap.

# Phase 124 - Why the Guide renders where these files came from

The xam.xex and hud.xex used throughout this work came from F:\FuzionFrenzy,
which is not a game directory - it is **Microsoft's own Xbox 360
backwards-compatibility emulator package**:

```
Emu.exe                 D3D12Core.dll        DX12EdramResolveShaders.sbin
LaunchArguments.txt:    titleId=4D530856  (Fusion Frenzy)
Flash/  hud.xex  huduiskin.xex  xam.xex  ximecore.xex
        xboxkrnlcf.bin   xboxkrnlcf.hvdata
```

The decisive file is **xboxkrnlcf.bin**: 1,507,328 bytes beginning `MZ` - a PE
image, i.e. the **real Xbox 360 kernel**, shipped alongside a hypervisor data
blob.

## What that explains

That emulator runs the actual kernel binary. Xenia does not: it implements
xboxkrnl as host functions (HLE), which is why every export examined in this
project is either a C++ reimplementation, a stub, or absent.

So the reason the Guide renders under MS's emulator and not here is not a
missing call or a wrong argument - it is that the system boot which brings up
the GPU, creates xam's device and establishes sharing between system and title
**is kernel code that Xenia does not run at all**. There is no boot path for
xam's device to attach to, because the boot path is the kernel and the kernel is
replaced.

Everything this investigation found downstream is consistent with that:

- xam never calls VdInitializeRingBuffer - the kernel does it (phase 123)
- xam asserts on a device it did not create (phase 120)
- the connection calls are global and spent by the title (phase 119)
- xam records a UI thread this bootstrap can never be (phase 105)

Each is the shape of guest code expecting a kernel that is present, running
under one that is emulated at the API boundary.

## Honest limits of this claim

This is inferred from the files' presence and format - a PE kernel image and a
hypervisor blob in a Flash directory - not from running or analysing that
emulator. But it is a coherent explanation for the wall this work reached, and
it matches every guest-side finding above.

# Phase 125 - Xenia's own system-title path, and what it does not do

Xenia already has a path for system titles in `Emulator::LaunchXexFile`:

```cpp
if (!kernel::IsSystemTitle(kernel_state_->title_id())) return result;
file_system_->RegisterSymbolicLink("\SystemRoot", mount_path);
auto module = kernel_state_->LoadUserModule("xam.xex");
if (!module) module = kernel_state_->LoadUserModule("$flash_xam.xex");
if (module) result = kernel_state_->FinishLoadingUserModule(module, false);
```

and `title_id_utils.h` carries `static_assert(IsSystemTitle(kDashboardID))`, so
the dashboard is classified correctly by construction.

Two observations.

**It does less than this bootstrap.** The whole path is: register a symlink,
load xam.xex, finish loading it. There is no device creation, no XUI setup, no
boot sequence - so discovering it does not supersede any of this work. Loading
the module was never the hard part.

**It appears not to complete here.** The log shows

```
ResolvePath(\SystemRoot) failed - device not found
ResolvePath(\SystemRoot\systemupdate.xex) failed - device not found
```

which is what one would expect if the symlink was never registered, despite the
dashboard qualifying. Worth a closer look as a Xenia issue in its own right, but
it does not change anything about the Guide: even fully working, that path stops
where this investigation started.

## Closing note on the whole line of work

The Guide's software runs. What it needs beyond that is a kernel Xenia does not
have - which is exactly what the emulator these files came from ships as
xboxkrnlcf.bin.

# Phase 126 - A real Xenia ordering bug: \SystemRoot is registered too late

Instrumented the system-title branch. The symlink **is** registered:

```
line 4578: System title: registering SystemRoot -> '\Device\Harddisk0\Partition1'
                                       (exe '\Device\Harddisk0\Partition1\dash.xex')
```

but the guest has already tried to use it, twice, ~2000 lines earlier:

```
line 2514: ResolvePath(\SystemRoot) failed - device not found
line 4316: ResolvePath(\SystemRoot\systemupdate.xex) failed - device not found
```

So this is not a misclassification or a bad path - it is **ordering**.
`Emulator::LaunchXexFile` calls `CompleteLaunch()` first, which starts the
title, and only afterwards registers `\SystemRoot` and loads xam.xex. A system
title that queries `\SystemRoot` during startup - which the dashboard does,
looking for systemupdate.xex - finds nothing, because the link it is meant to
resolve does not exist yet.

This is independent of the Guide work and would affect any system title on
Xenia. The fix is to register the symlink (and probably load xam) before the
title's threads run rather than after, though moving work across
`CompleteLaunch` needs care since that function also drives UI-thread callbacks.

Recorded rather than patched: changing launch ordering is a behavioural change
to every title's startup, and this project's changes are already extensive. It
belongs in its own change with its own testing.

# Phase 127 - Fixed the \SystemRoot ordering bug

Registered the symlink early, in `CompleteLaunch` before the title's modules
load, behind `system_root_early` (default on):

```
Early SystemRoot -> 'GAME:'
```

Result:

```
before:  ResolvePath(\SystemRoot) failed - device not found
         ResolvePath(\SystemRoot\systemupdate.xex) failed - device not found
after:   0 such failures
         HostPathDevice::ResolvePath(\systemupdate.xex)
```

The dashboard now resolves through the link and performs a real lookup for
systemupdate.xex - correctly not finding one, since none is present - instead of
failing on a missing device. One fewer error in the session, and the dashboard
framebuffer is still byte-identical to the baseline (md5 a6eb4cfa).

I said last phase this belonged in its own change rather than being patched
here. It turned out to be four lines behind a cvar, with the risky part - moving
work across CompleteLaunch - avoided entirely by registering in the early
bootstrap block that already exists. So the caution was right but the estimate
was wrong, and the cheap version was worth doing once that was clear.

## Does it affect the Guide?

No. Scene creation and the per-frame draws are unchanged, and the screenshot is
identical. The dashboard was looking for a system update, not for anything the
Guide needs. Worth stating plainly: this is a genuine Xenia fix found while
investigating the Guide, not a step toward rendering it.

# Phase 128 - Using the new assertion channel: a permanent spin in LLE xam

With guest assertions now visible, a normal session shows twelve distinct trap
sites. The one worth chasing is the highest-frequency by orders of magnitude -
millions of hits in one run, always the same:

```
GUEST ASSERT (twi 25) at lr=8177AC78: r3=C000000D r4=81D42528 r5=1
```

r3 = 0xC000000D is STATUS_INVALID_PARAMETER. Following the call site
(G 81781E74) to its import thunk and resolving it in the import table:

```
815F073C  81D0FF3C  0AF (175)  KeWaitForMultipleObjects
```

and Xenia's implementation returns exactly that when it cannot resolve one of
the objects:

```cpp
auto object_ref = XObject::GetNativeObject<XObject>(
    kernel_state(), object_ptr, UndefinedObject, true);
if (!object_ref) {
  return X_STATUS_INVALID_PARAMETER;
}
```

So xam waits on an array of dispatcher objects, at least one of which Xenia's
object table does not recognise; the wait fails immediately; xam asserts and
retries; forever.

## Why this matters beyond the noise

This is the HLE/LLE boundary in its purest form. Real xam creates its own
dispatcher objects in guest memory. Xenia's kernel only knows objects **it**
created, so any xam-created event or semaphore is invisible to it, and every
wait on one fails.

That is a systemic cost of running real xam under an HLE kernel, and it was
completely invisible before trap type 25 was implemented - the failure produces
no log, no crash and no stall, just a thread burning CPU on a retry loop for the
entire session.

It also puts the earlier CPU measurements in context: the ~2.1 cores of
background load measured in phase 91 was, at least in part, this.

# Phase 129 - Named the defect: Xenia cannot wrap guest-created timers

Followed the millions-of-hits spin to its root. `XObject::GetNativeObject`
wraps guest dispatcher objects by switching on the dispatch header's type, but
only handles three:

```
EventNotificationObject / EventSynchronizationObject -> XEvent
MutantObject                                         -> XMutant
SemaphoreObject                                      -> XSemaphore
everything else                                      -> assert_always(); nullptr
```

Added a one-line-per-type log to that default case, and the answer is
immediate:

```
GetNativeObject: unsupported dispatcher type 9 at 81D424A8
```

Type 9 is **TimerSynchronizationObject**. Real xam creates a KTIMER in its own
memory and waits on it; Xenia cannot wrap it; `KeWaitForMultipleObjects`
returns STATUS_INVALID_PARAMETER; xam asserts and retries forever.

Confirming the gap is structural rather than incidental: `XEvent` declares
`InitializeNative(void*, const X_DISPATCH_HEADER*)`, and **`XTimer` declares no
such method at all**. Guest-created timers are not supported anywhere in the
object layer.

## Why this is worth writing down carefully

- It is a concrete, bounded Xenia feature: give XTimer a native initialiser and
  add the two timer cases to the switch.
- It is invisible without the trap-25 work: `assert_always()` compiles out in
  release, so the failure returns null silently and the guest's own complaint
  was being discarded.
- It costs a thread spinning for an entire session, which is why the background
  CPU baseline in this project was around two cores.

Not attempting the implementation here: a timer wrapped from guest memory needs
signalling semantics tied to the guest structure, and that deserves its own
change rather than being appended to a Guide branch. The diagnosis is complete
enough to act on.

# Phase 130 - Guest timers are unusable in three ways, so the small fix is a trap

Assessed implementing `XTimer::InitializeNative` rather than assuming its size,
after the last scoping call turned out to be four lines.

The wrapper itself **is** small - XEvent's equivalent is twelve lines and
XTimer::Initialize already has the manual/synchronization split:

```cpp
case TimerNotificationObject:    CreateManualResetTimer();
case TimerSynchronizationObject: CreateSynchronizationTimer();
```

But arming is the problem. Both timer-setting exports are unimplemented:

```
815F06FC  81D0FE3C  0A6 (166)  !! KeSetTimer
815F0740  81D0FF4C  0A7 (167)  !! KeSetTimerEx
```

marked `!!` in the import dump, imported by **xam, dash and hud alike**, and
present in Xenia's export table with no implementation behind them.

So guest-created timers fail three ways: they cannot be wrapped
(`GetNativeObject`), they cannot be armed (`KeSetTimer`), and they cannot be
armed with a callback (`KeSetTimerEx`).

## Why the small fix would make things worse

Adding only the wrapper would make `KeWaitForMultipleObjects` succeed and then
wait on a timer nothing can ever signal. That converts a visible spin into a
silent permanent block - the exact failure mode this project spent twelve phases
diagnosing when the Guide thread appeared to hang.

So the earlier decision not to implement stands, but now for a reason rather
than an estimate: **the parts must land together or not at all.**

## Scope of the defect

This is not Guide-specific or LLE-specific. dash and hud import the same two
timer exports, so any title using kernel timers on Xenia is relying on functions
that are declared and absent. Whether they are commonly used is a separate
question, but the Guide path demonstrably hits all three gaps at once.

# Phase 131 - Timer exports implemented as observation stubs; nothing calls them

`KeSetTimer` (0xA6) and `KeSetTimerEx` (0xA7) were declared in Xenia's export
table with no definition, so calls fell through as "undefined extern call".
Gave them real definitions that log their arguments and return 0 without arming
anything, which removes the `!!` marks and makes any use visible.

Result over a full session: **not called once.** The imports are present in xam,
dash and hud, and none of them invokes either function.

That is a useful negative. It means:

- implementing the arming functions properly would not fix xam's spin, because
  nothing calls them;
- and it confirms phase 130's reasoning with data rather than inference - if the
  `GetNativeObject` wrapper were added alone, the wait would block on a timer
  that nothing in the session would ever arm, turning a spin into a permanent
  silent block.

So xam waits on a KTIMER whose signalling comes from somewhere other than the
documented arming path - the kernel's own timer machinery, which under Xenia
does not exist for guest-created objects.

The stubs stay: an export that is declared should have a definition, and one
that logs is strictly better than one that vanishes into an "undefined extern
call" message. Guide behaviour is unchanged - scene creation and per-frame draws
still succeed.

# Phase 132 - The timer defect, traced end to end

`KeInitializeTimerEx` **is** implemented, and that is where the asymmetry
starts:

```cpp
void xeKeInitializeTimerEx(X_KTIMER* timer, uint32_t type, ...) {
  timer->header.process_type = proctype;
  timer->header.inserted = 0;
  timer->header.type = type ? TimerSynchronizationObject
                            : TimerNotificationObject;
  timer->header.signal_state = 0;
  util::XeInitializeListHead(&timer->header.wait_list, context);
  timer->due_time = 0;
  timer->period = 0;
}
```

It initialises the **guest structure** faithfully and creates no host object.
Compare `XEvent`, where the guest structure is wrapped on first use by
`GetNativeObject` and backed by a real host event.

So the full chain behind the spin:

1. xam calls `KeInitializeTimerEx`; Xenia writes a valid KTIMER with
   `header.type = 9` and no host counterpart.
2. xam waits on it via `KeWaitForMultipleObjects`.
3. `GetNativeObject` switches on type 9, finds no case, returns null - and
   `assert_always()` is compiled out in release, so nothing is reported.
4. The wait returns `STATUS_INVALID_PARAMETER`.
5. xam asserts (trap 25, previously discarded) and retries, for the session.

Note also that `XeInitializeListHead` on the wait list overwrites the field
`GetNativeObject` uses as its "already wrapped" signature, so the two mechanisms
are not merely incomplete but mutually inconsistent for timers.

## The complete fix, for the record

- `XTimer::InitializeNative(void*, const X_DISPATCH_HEADER*)`, mirroring
  `XEvent`'s, choosing manual vs synchronization from the header type.
- The two timer cases in `XObject::GetNativeObject`.
- `KeSetTimer` / `KeSetTimerEx` arming through `XTimer::SetTimer`, which already
  exists and is used by `NtSetTimerEx`.
- `KeCancelTimer` (0x54), also currently declared and absent.

All four are needed together; any subset changes a visible failure into a
quieter one. That is the whole reason this was diagnosed rather than patched.

# Phase 133 - Implemented guest timers; the warning applied to me

Implemented all four parts identified in phase 132:

- `XTimer::InitializeNative`, mirroring XEvent's
- the two timer cases in `XObject::GetNativeObject`
- `KeSetTimer` / `KeSetTimerEx` arming via the existing `XTimer::SetTimer`
- `KeCancelTimer`

It works, exactly as intended:

```
asserts with C000000D:        0   (was millions)
unsupported dispatcher type:  0
```

xam's permanent retry loop is gone.

**And the session got worse.** Errors rose from ~127 to 190, a xam thread that
had been spinning harmlessly now ran on and faulted at guest PC 817286C0, and
the Guide bootstrap never executed because the emulator was in the crash-pause
path before it got there.

The cause is the piece I left out: `XTimer::SetTimer` is called with routine 0,
so the DPC a guest timer carries is never dispatched. The timer signals, the
waiter wakes, and then guest code that expects its callback to have run finds it
has not.

Phase 130 said "any subset changes a visible failure into a quieter one, which
is why this was diagnosed rather than patched". I then implemented what I
believed was the whole set, and the same sentence turned out to describe my own
change - the set had a fifth member I had not identified.

Gated behind `guest_native_timers`, default **off**. With it off the previous
behaviour returns exactly: 114 errors, no guest crash, scene created, draws
succeeding.

## Value of keeping it

The code is correct as far as it goes and is the larger half of the fix; whoever
adds DPC dispatch can turn the cvar on and have the rest already in place. The
cvar description names the missing piece so it is not rediscovered.

# Phase 134 - Why adopting guest timers corrupts them

Added DPC dispatch - reading routine and context from the KDPC and passing them
to XTimer::SetTimer, which already enqueues an APC on the setting thread. It
made no difference, and the reason is phase 131's finding: **KeSetTimer is never
called**, so there is no DPC to dispatch. The fifth piece was real but not the
blocker.

The actual crash, at G 8172F8B8, is a linked-list unlink:

```
8172f8b8  lwz r11,0(r31)      ; flink of a list head
8172f8bc  addi r3,r11,-308    ; CONTAINING_RECORD, offset 0x134
8172f8c0  lwz r10,0(r11)      ; faults - r11 is 0
8172f8c8  stw r10,0(r9)       ; the classic flink/blink fixup
```

r3 = 0xFFFFFECC confirms r11 = 0: the list head is empty where the guest
requires it to be linked.

## The mechanism, and why it is not a small fix

`GetNativeObject` associates a guest dispatcher object with its host wrapper by
**overwriting the object's wait_list**: `StashHandle` puts a magic signature in
`flink_ptr` and the handle in `blink_ptr`. That is safe for objects whose wait
list the guest never walks itself.

It is not safe for timers. `xeKeInitializeTimerEx` explicitly calls
`XeInitializeListHead(&timer->header.wait_list)`, and xam then walks and unlinks
entries from that list. Adopting the timer destroys the list head, and the next
traversal dereferences null.

Phase 132 noted the two mechanisms "disagree about that structure rather than
merely being incomplete". This is that disagreement causing a crash.

So the correct implementation cannot use StashHandle for timers: it needs a
side table keyed by guest address, leaving the guest's own structure untouched.
That is a different and larger change than adding two switch cases, and it is
the real reason `guest_native_timers` stays off.

Config restored; default path unaffected.

# Phase 135 - Correction: StashHandle was not the cause

Phase 134 concluded the crash came from `StashHandle` overwriting the timer's
`wait_list`, a list the guest walks. Implemented the fix that follows from that
- a side table keyed by guest address, with the timer's own structure left
untouched and StashHandle skipped for timer types.

Result:

```
C000000D asserts: 0        (spin still fixed)
errors:           190      (unchanged)
GUEST CRASH: access violation at guest PC 817286C0   (identical)
```

**The crash is byte-for-byte the same.** So the wait_list corruption theory is
wrong, or at least is not what produces this fault. Retracted.

What the experiment does establish:

- The side table works and is the right association strategy regardless -
  overwriting a structure the guest initialises and walks is a genuine hazard,
  even if it is not this bug.
- The crash follows from the **wait succeeding** rather than from how the timer
  is associated. Once `KeWaitForMultipleObjects` stops returning
  STATUS_INVALID_PARAMETER, xam proceeds down a path it has never taken in this
  configuration, and that path unlinks from a list nothing has populated.

That is a much more ordinary explanation than structure corruption: the guest is
reaching new code, and the new code depends on more of the kernel than is
present.

`guest_native_timers` stays off. The side table stays in - it is a strict
improvement over StashHandle for this object type - but the honest summary is
that enabling guest timers moves xam from a harmless spin to an unrelated crash,
and closing that gap means implementing whatever populates the list at offset
0x134, which has not been identified.

# Phase 136 - Closing the timer thread: the list was never initialised

Characterised the crash function (G 8172F848 / R 81728648), which matches the
crash's lr:

```
8172f868  bl KeEnterCriticalRegion
8172f86c  addi r27,r31,4192          ; r31 + 0x1060
8172f874  bl RtlEnterCriticalSection ; lock at +0x1060
8172f884  lwz r10,0(r11)             ; walk the list at [r31]
...       CONTAINING_RECORD at -0x134, flink/blink unlink
```

It is a **drain-the-queue-under-a-lock** loop: take a critical region, take the
object's lock, walk its list and unlink each entry.

The decisive detail: an initialised empty LIST_ENTRY points at **itself**, never
at null. This one is null, so the head was never initialised - the object was
not fully constructed.

## What that means, and why the timer thread stops here

With the spin, xam never reached this code. Without it, xam reaches it holding a
partially-constructed object, because the construction that would have
initialised that list belongs to xam's normal startup, which this bootstrap does
not perform.

So the timer work ends where everything else in this project ends: fixing one
gap lets the guest walk further into code that assumes an initialisation
sequence Xenia never runs. That is not a reason the timer fix is wrong - it is
correct and gated - but it is a reason enabling it does not help here.

## Net result of the timer investigation

Delivered, all behind `guest_native_timers` (off):

- `XTimer::InitializeNative`
- timer cases in `XObject::GetNativeObject`, associated through a side table
  rather than by overwriting the guest's `wait_list`
- `KeSetTimer`, `KeSetTimerEx` with DPC routine extraction, `KeCancelTimer`,
  all previously declared and absent

and, permanently on, the observation that made any of this visible: guest
assertion traps, without which xam's millions-of-iterations retry loop produced
no output at all.

# Phase 137 - Climbing to xam's UI startup, and why APC injection fails

Climbed the call graph above the render host. It has exactly one caller,
G 8179C838, inside a function starting at **G 8179C748 / R 81795548**, which
opens with:

```
8179c74c  bl 81814968        ; stack check
8179c754  bl 8177fdb8        ; the thread-identity check
...
8179c82c  bl 817972c0
8179c838  bl 81794e58        ; the render host
```

So xam's UI startup **also** begins by verifying it is on xam's recorded UI
thread. That reframes phase 106: spoofing the check satisfied a comparison, but
the requirement is real and applies to the whole startup path, not just the one
function.

## The idea, and the test

If the whole path must run on xam's UI thread, then run it there. Xenia can
queue an APC onto a guest thread (XTimer already does), so the Guide button now
locates the XThread whose `guest_object()` matches the recorded UI thread and
queues xam's UI startup on it:

```
Guide button: queueing xam UI startup 81795548 as an APC on xam's UI thread
              30030010
```

The thread is found and the APC is queued. **Nothing runs.**

The reason is measurable: every sampled wait in the session is non-alertable.

```
alertable=0   60 of 60 sampled waits
```

APCs are only delivered at an alertable wait, so a queued APC on xam's threads
is never dispatched. (Sampled evidence - every 500th wait - rather than
exhaustive, but uniform.)

## Where that leaves injection

We cannot make xam's own thread do work for us by this route. The thread exists,
it is identifiable, and it is unreachable: it waits non-alertably, so the only
way to run code on it is to be the code it was already going to run.

Cvar defaults to 0 with the reason recorded. Guide behaviour unchanged - scene
still created, draws still succeeding, 114 errors, no crash.

# Phase 138 - The two experiments do not combine

Tested whether enabling guest timers would make xam's UI thread block properly
and therefore become alertable, which would let the queued APC run. It does not:

```
guest_native_timers = true, guide_xam_ui_startup = 81795548

alertable=0        8 of 8 sampled waits   (still never alertable)
GUEST CRASH: access violation at 817286C0 (before the button press)
errors: 190
scene creator: never runs
```

The thread stays non-alertable, the timer crash still occurs, and it now happens
early enough that the Guide bootstrap never executes at all.

So the two lines of work are independent and neither rescues the other. Both
cvars are off, and the default configuration is verified back to its best state:

```
errors: 114
GuideBootstrap: scene creator 913EB940 -> 00000000, scene=00010000
Guide composite draw #1 -> 00000000
```

## Standing conclusion

Injecting work onto xam's UI thread is not possible by any mechanism tried:
- calling from another thread - rejected by the identity check
- spoofing the identity check - satisfies the comparison, changes nothing
- queuing an APC - never delivered, the thread never waits alertably
- fixing timers so it might wait properly - it still does not, and the fix
  introduces an earlier crash

The thread runs code it was already going to run, and nothing this bootstrap can
do adds to that list. That is the same boundary reached from the device side and
the kernel side: the parts of the system that would drive the Guide are the parts
Xenia replaces.

# Phase 139 - hud cannot be the title: it is a DLL

Tried the one structural idea not yet attempted: launch hud.xex **as the title**,
so it would own the GPU through Xenia's normal title path instead of riding
along as a passenger.

```
LLE xam: loaded at 30013000
Loading module GAME:\hud.xex
Failed to load user module ...\dashroot\hud.xex
Failed to launch target: C00000BB
```

C00000BB is STATUS_NOT_SUPPORTED, and the reason is in hud's own header, logged
much earlier in this work:

```
Module Flags: 00000008   XEX_MODULE_DLL_MODULE
```

hud.xex is a **DLL**. Its entry point is DllMain, not a title entry, so it cannot
be launched as an executable and cannot own a device.

That is consistent with everything else established about it: xam owns
CHUDBkgndScene and the HUD scene classes, hud provides classes and resources and
is loaded into a host process. The Guide is not an application that could be run
standalone - it is a component of the system, and the system is the part Xenia
does not run.

Quick negative, but worth recording: it was the last remaining idea that did not
require emulator changes.

# Phase 140 - Verifying a load-bearing claim three ways

The conclusion that "xam never calls the GPU connection functions" underpins the
whole rendering account, and it rested on a scan for **direct** `bl` branches.
That is exactly the scan that missed the XuiCanvas dispatcher earlier, which was
reached through a class descriptor rather than a call. Checked it properly.

**1. Direct branches** - `bl` to the thunks: none. (phase 123)

**2. Static pointer tables** - the thunk addresses appearing anywhere in the
image, the technique that found the dispatcher:

```
VdInitializeRingBuffer           0 references
VdInitializeEngines              0 references
VdSetGraphicsInterruptCallback   0 references
```

**3. Calls through the import variable slot** - `lwz` from 815F0A1C and friends,
then `bctrl`. A naive displacement scan reports 2, 4 and 10 candidate matches,
which looks like a refutation. Requiring the base register to be loaded with
`lis rX,0x815F` first:

```
VdInitializeRingBuffer           0 confirmed
VdInitializeEngines              0 confirmed
VdSetGraphicsInterruptCallback   0 confirmed
```

The candidates were unrelated structure and stack offsets that happen to share a
displacement.

So the claim survives all three forms of call, and the middle step is worth
noting: the unfiltered scan produced sixteen apparent hits, and reporting those
without the base-register check would have overturned a correct conclusion on
noise. A scan is only as good as its filter, in both directions.

# Phase 141 - xam ships an exact function table, and I never used it

While verifying indirect references, all three lookups landed in **`.pdata`** -
the exception-handling RUNTIME_FUNCTION table. That is not a call reference, so
the claims hold. But it is something better: `.pdata` lists **every function's
exact start address**.

```
.pdata va=816F5200 size=23BE8
entries with plausible function starts: 18301
```

Validated against three functions derived the hard way, by scanning backwards
for an `mfspr r12,8` prologue:

```
817286C0 -> 81728648    (crash function)      matches
8174E22C -> 8174E220    (PlayTransition)      matches
81795638 -> 81795548    (xam UI startup)      matches
```

All three agree, so those derivations were right - but they were right by luck
of the prologue pattern. The same heuristic **crossed a function boundary** in
phase 107 and returned an entry 49 instructions too early; that was caught only
because the disassembly showed a `.long 0x00000000` padding word and a fresh
prologue just after it.

Wrote `work/fnlookup.py`: given a basefile and an address, it reports the
containing function and where it ends, from `.pdata`. Two seconds of work at any
point in the last hundred phases would have made every "find the enclosing
function" step exact instead of heuristic.

Recording it plainly: the information was in the file the whole time. Reaching
for a scan because a scan was the tool already in hand is the same mistake as
trusting an instrument without a control - just quieter, because a heuristic
that usually works produces no obvious failures.

# Phase 142 - Correcting phase 141: .pdata omits leaf functions

Used the new lookup to re-check the boundary that the prologue heuristic got
wrong in phase 107. It disagreed:

```
8174FFFC is in function 8174FF38   (.pdata)
```

which would mean the XUI command dispatcher starts at 8174FF38, not 8174FFD0,
and that phase 107's correction was itself wrong.

It is not. The disassembly shows the previous function ending cleanly:

```
817571c8  blr
817571cc  .long 0x00000000      <- padding
817571d0  lwz r11,4(r3)         <- new function, no prologue
```

A function that never touches LR and allocates no frame needs **no unwind
record**, so `.pdata` does not list it. The dispatcher is exactly that kind of
leaf function, and `.pdata` silently attributes its body to the function before
it.

So phase 141's claim - ".pdata lists every function's exact start address" - is
**wrong**, and the tool built on it will mislead in precisely the cases where
the prologue heuristic also struggles: functions without prologues.

Corrected the tool's documentation rather than deleting it. It remains useful
for confirming a boundary, and useless for discovering one; the reliable signal
for a function end is still a `blr` followed by a `00000000` padding word.

Worth noting what nearly happened: a new, apparently authoritative instrument
was about to overturn a correct earlier finding, and the only thing that stopped
it was disassembling the boundary rather than trusting the table. That is the
same discipline that produced the three retractions in this project, applied in
the other direction.

# Phase 143 - Auditing the derivations that used the prologue heuristic

`.pdata` omitting leaf functions means every function boundary derived by
scanning backwards for a prologue needed re-checking, using the reliable signal:
a `blr` followed by a `00000000` padding word.

| derivation | method | result |
|---|---|---|
| PlayTransition = G 81755420 | .pdata (has unwind data) | confirmed |
| xam UI startup = G 8179C748 | .pdata | confirmed |
| crash function = G 81728648 | .pdata | confirmed |
| XUI command dispatcher = G 817571D0 | blr+padding | confirmed (leaf, absent from .pdata) |
| animation initialiser = G 81756FE0 | blr+padding scan across the range | confirmed - no boundary between it and its writer at G 8175711C |
| thread-identity check = G 8177FDB8 | ends in blr, next word is a prologue | confirmed (leaf) |

All hold. Two were confirmed only by the padding rule because they are leaf
functions with no `.pdata` entry, which is exactly the class the new tool cannot
see.

So the conclusions built on those boundaries stand: the animation global is set
by command handler 0x13; the dispatcher is reached from the XuiCanvas class
descriptor; PlayTransition is where the Guide crashes; xam's UI startup and the
render host both begin with the thread check.

The audit was worth running. Not because it found errors - it found none - but
because the boundaries were load-bearing for five separate conclusions and had
been derived by a method now known to be unreliable. Confirming them costs one
pass; leaving them unconfirmed would have left every downstream claim resting on
a heuristic I had just documented as fallible.

# Phase 144 - The hud side audits clean too

hud carries its own `.pdata` (595 entries). One detail first, because it cost a
wrong result before it was noticed: hud's link base is 98000000 and its runtime
base is 913E0000, so link-time addresses were the obvious expectation - but the
entries already hold **runtime** addresses:

```
913E6000 40001904
913E6080 40001104
913E60C8 40001A03
```

The basefile is stored already relocated, so `.pdata` needs no conversion. xam's
is the same (817xxxxx). Filtering for the link-time range returns zero entries
and looks like "hud has no unwind data", which is wrong.

With the right range, every key hud address is confirmed exactly:

```
handler        913E69C0 -> 913E69C0  MATCH
scene creator  913EB940 -> 913EB940  MATCH
xui init       913EA898 -> 913EA898  MATCH
draw           913EAB28 -> 913EAB28  MATCH
registrations  913F0DB0 -> 913F0DB0  MATCH
scene ctor     913EC578 -> 913EC578  MATCH
```

All six are function starts, not mid-function addresses - which matters most for
913E69C0, the handler hud registers with xam, since an earlier phase read it as
mid-function purely because it was disassembled against the wrong build of hud.

## State of the audit

Both modules are now verified. Twelve function boundaries underpin the Guide
account; six were confirmed by `.pdata`, two by the blr-plus-padding rule
because they are leaf functions, and the rest by range scan. None was wrong.

That is the end of the verification work. The account of what the Xbox button
does rests on addresses that have each been confirmed by a method independent of
how they were found.

# Phase 145 - Cleanup: removed the spent probes

Three diagnostic probes added during this work logged unconditionally in every
session and had served their purpose:

- **FaultProbe** in `MMIOHandler::ExceptionCallback` - answered "does the Guide
  thread take memory faults" (no), phase 96
- **WaitProbe** in `xeKeWaitForSingleObject` - answered "does it wait on guest
  objects" (no), phases 91 and 95
- **VdSwap counter** - answered "which thread presents" (only the title's),
  phase 72

All three are removed. Each was a controlled measurement, each produced a
finding recorded above, and none needs to keep running.

Kept, because they report real defects rather than answer a question:

- `GetNativeObject: unsupported dispatcher type N` - one line per type, names a
  class of guest object Xenia cannot wrap
- `GUEST ASSERT (twi 25)` behind `log_guest_asserts` - the guest's own
  diagnostics, off by default and rate-limited per call site
- `GUEST CRASH` before `Pause()` - a guest fault should never be silent

Verified after removal: 114 errors, scene creator returning a live handle,
composite draws succeeding, no crash.

A probe that has answered its question is noise for everyone who runs the
emulator afterwards. Leaving all of them in would have made the log worse for
the next person while telling them nothing they could act on.

# Phase 146 - Generality: the Guide path is tied to a presenting host

Everything in this project has been tested under one title, dash.xex. Tried a
second: **xshell.xex** (Title ID FFFE07FF), which launches successfully - unlike
hud.xex, it is an executable rather than a DLL.

The Guide setup initialises identically:

```
Guide: loading GAME:\hud.xex
LLE xam: importer hud -> REAL xam
hud      91401000-91429E9D, 167581b
```

and so does the press itself:

```
Guide button: user=0 lle_xam=on hud=loaded
Guide button: dispatching open to 913E69C0
Guide button: handler returned 00000000
Guide button: obj=401EA6A0 vtable=913E1CB4
```

Same handler, same object address, same vtable. So the Guide object creation is
**not dashboard-specific**; it works under any host title.

## But the bootstrap never runs

No `GuideBootstrap:` line appears. xshell issues no Vd calls beyond reading
VdGlobalDevice and never swaps - it is a shell utility that does not present
frames.

This bootstrap is driven from inside `VdSwap`, because that is the only place
the title's render thread is available (the D3D device is thread-affine). Under
a title that never presents, that hook never fires, so the render host, class
registration, DC creation and scene creation never happen.

That is a real limitation of the approach and worth stating: **the Guide can
only be brought up under a title that is actively rendering.** It is also a
neat restatement of what the Guide fundamentally is - not an application, but
something that draws inside somebody else's frame.

# Phase 147 - A second non-presenting title confirms the dependency

Tried `bootanim.xex` as a third host, expecting a title that renders. It behaves
exactly like xshell: the handler dispatches and the Guide object is created, and
no `GuideBootstrap:` line follows.

Checked directly rather than assuming it presents. Pressing F12 gives:

```
Failed to capture guest output for screenshot
```

Xenia has no guest frame to capture, so bootanim never presented one either -
despite being, by name, an animation. Whatever it needs to start rendering under
Xenia, it does not get.

So of three system titles:

| title | launches | presents | Guide object | bootstrap runs |
|---|---|---|---|---|
| dash.xex     | yes | yes | yes | yes |
| xshell.xex   | yes | no  | yes | no  |
| bootanim.xex | yes | no  | yes | no  |
| hud.xex      | no (DLL) | - | - | - |

The pattern is consistent: **object creation works everywhere, the bootstrap
needs a presenting host.** That is not a property of the Guide but of this
bootstrap, which hooks the title's swap because that is the only handle on the
render thread.

Worth noting the check that made this solid: "bootanim is an animation so it
must render" is exactly the kind of assumption that has been wrong repeatedly in
this project. One F12 press settled it.

# Phase 148 - Could the bootstrap leave the title's thread? Not cleanly.

Phase 147 showed the bootstrap needs a presenting host because it is hooked to
`VdSwap`. But the reason for that hook - the thread-affine D3D device - may no
longer apply: xam's device is created by the **Guide's own thread**, so it should
be affine to that thread, not the title's. The title-thread move was motivated by
the *title's* device, which is a path now abandoned.

Tested by turning `guide_bootstrap_on_title_thread` off. The result does not
answer the question:

```
Guide button: XUI init returned 00000000
(no render host line, no scene creator)
```

The two paths have **diverged**. The title-thread path (`GuideBootstrap`) carries
every fix from the last thirty phases - the render host call, the class
registrars, the skin module, the scene creator with its out-pointer. The
non-title-thread path is the older sequence in emulator.cc and has none of them,
so switching the cvar does not run the same bootstrap somewhere else, it runs an
obsolete one.

Answering the question properly means refactoring the two paths into one. Not
doing that, and the reason is worth stating: the benefit would be to initialise
the Guide under titles that never present - and it still would not render there,
because the device is unconnected either way. That is extending a non-visible
result to more hosts, which does not justify restructuring the one configuration
that works.

Restored and verified: 114 errors, scene creator returning a live handle,
composite draws succeeding.

# Phase 149 - Clean-room rebuild verification

Every build in this project has been incremental, which can hide a broken tree:
a stale object file keeps linking long after its source stopped compiling. Wiped
all 936 targets and rebuilt from scratch.

```
Cleaning... 936 files.
[936/936] Linking CXX executable bin\Windows\Release\xenia_canary.exe
elapsed: 188s
no errors, no warnings
```

(The lines matching "error" in the output are filenames - error.c,
error_private.c, xboxkrnl_error.cc.)

The freshly built binary behaves identically:

```
errors: 114
GuideBootstrap: scene creator 913EB940 -> 00000000, scene=00010000
Guide composite draw #1   -> 00000000
Guide composite draw #300 -> 00000000
```

and the dashboard framebuffer is still byte-identical to the original baseline
(md5 a6eb4cfa) after a Guide press.

So the branch compiles standalone, with warnings-as-errors enabled, and the
result is reproducible from a clean tree rather than dependent on the state of
an incremental build. That was worth confirming before describing the changes
as usable by anyone else.
