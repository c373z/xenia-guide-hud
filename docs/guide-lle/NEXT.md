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
