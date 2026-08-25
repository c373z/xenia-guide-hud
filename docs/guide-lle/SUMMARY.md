# Xbox Guide under Xenia — summary

**Goal:** press the Xbox button and have the real Guide (hud.xex) render.
**Outcome:** the Guide's software runs end to end; nothing reaches the screen.
The remaining gap is emulator-side and is described below.

## What pressing the Xbox button does now

1. `Emulator::on_guide_button_pressed` dispatches on a guest thread.
2. hud's registered handler (913E69C0) runs with message 0x80000004.
3. The Guide object is allocated, constructed, and stored in hud's singleton
   at [0x91400690].
4. On the **title's render thread**: xam's render host, XUI class registration,
   XuiInit, XuiRenderCreateDC.
5. hud's scene creator resolves its resources from hud.xex's own `hud` XEX
   section via a `section://` locator and **returns a live XUI scene handle**.
6. hud's draw runs once per swap — RenderBegin, layout, RenderEnd, Present —
   returning success every frame.

The dashboard is unaffected throughout (framebuffer byte-identical to baseline).

## Why nothing appears

xam's D3D device is never connected to the GPU, and cannot be:

- xam never calls `VdInitializeRingBuffer`, `VdInitializeEngines` or
  `VdSetGraphicsInterruptCallback` — it imports them and calls none. Its D3D is
  built to run on a GPU the boot path already brought up.
- Those calls are made once, by the title, for the single ring.
- Substituting the title's device makes xam assert an out-of-range index three
  times and then fault.
- The system command buffer is not on the Guide's path — neither the draw nor
  the device creation requests it.
- The device produces no command data: its state is byte-identical across 200
  frames of drawing.

The system boot that would create and wire xam's device is **kernel code**.
Xenia implements xboxkrnl as host functions, so that boot never runs. The
emulator these files came from (Microsoft's own, in F:\FuzionFrenzy) ships
`xboxkrnlcf.bin` — the real kernel — which is why the Guide works there.

## Fixes made to Xenia along the way

- `VdRegisterGraphicsNotification`, `VdRegisterXamGraphicsNotification`,
  `VdCallGraphicsNotificationRoutines` — previously absent or no-ops
- **Guest crash reporting before `Pause()`** — every guest fault previously
  presented as a silent freeze
- **Guest assertion traps (twi type 25)** — an empty `case` in the x64 emitter,
  so xam's own diagnostics were discarded; this alone explained several
  otherwise-invisible failures
- `DbgBreakPoint` guarded on debugger presence instead of always breaking
- `XexGetModuleSection` accepting an object handle as well as an hmodule
- `\SystemRoot` registered before the title starts, not after
- `xbox_hardware_info_flags` exposed (bit 0x200 gates xam's device creation)
- LLE xam importer scoping, so the dashboard keeps the HLE xam
- Guest native timers, behind `guest_native_timers` (off — see below)

## Known defects documented but not fixed

- **Guest timers**: `GetNativeObject` cannot adopt them, `KeSetTimer`,
  `KeSetTimerEx` and `KeCancelTimer` were declared and absent. Implemented
  behind a cvar; enabling it trades xam's harmless retry loop for a crash in
  code that then runs with uninitialised state, so it stays off.
- **`StashHandle` association**: overwrites a guest structure's `wait_list`,
  which is unsafe for objects the guest walks itself. Timers now use a side
  table instead.

## Method notes worth keeping

- A negative from an instrument needs a control proving the instrument can
  detect a positive. Three findings here were retracted for lack of one.
- In this build: a missing breakpoint-callback log proves nothing; sampled PPC
  context is stale during JIT execution; `kHighFrequency` exports are hidden
  unless `log_high_frequency_kernel_calls` is on.
- The guest is often the best instrument. xam names its own failures once trap
  type 25 is implemented.
