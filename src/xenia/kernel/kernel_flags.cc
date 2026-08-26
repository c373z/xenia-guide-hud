/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_flags.h"

DEFINE_bool(headless, false,
            "Don't display any UI, using defaults for prompts as needed.",
            "UI");
DEFINE_bool(log_high_frequency_kernel_calls, false,
            "Log kernel calls with the kHighFrequency tag.", "Logging");
DEFINE_string(lle_xam, "",
              "Guest path to a real xam.xex (e.g. \"GAME:\\\\xam.xex\") to "
              "load as a guest module. Guest imports of xam.xex then resolve "
              "against its real export table instead of Xenia's HLE xam. "
              "Leave blank to use the HLE xam.",
              "Kernel");
DEFINE_bool(lle_xam_trace_loader, false,
            "Install a guest breakpoint on xam's sys-app loader (81786788) "
            "and log its register arguments when hit.",
            "Kernel");
DEFINE_bool(lle_xam_app_host, false,
            "Call real xam's app lifecycle entries (XamAppLoadPass2SysApps "
            "0x254, XamAppLoad 0x244). Xenia declares these but implements "
            "none of them, and they are how system apps get hosted.",
            "Kernel");
DEFINE_uint32(xbox_hardware_info_flags, 0x20,
              "XboxHardwareInfo flags word (guest 801D0030). Xenia has always "
              "hardcoded 0x20. Real xam gates its D3D device creation on bit "
              "0x200 of this word, so without that bit xam never creates a "
              "device and the Guide can never get a render context.",
              "Kernel");
// xam's XUI render host (runtime 8178DC58) begins by calling a thread-identity
// check at ghidra 8177FDB8: it compares the current thread, read from the PPC
// thread pointer at [r13+256], against a thread recorded at 0x81D42520, and
// returns 1 only if they match. The render host traps when that returns 0.
// This bootstrap has never satisfied it - it calls the render host from the
// Guide dispatch thread or the title's swap thread, neither of which is xam's.
// Everything downstream (XUI context, DC, class registration, scene creation)
// therefore runs from a thread xam does not consider legitimate, which is the
// most likely reason state it would normally set up is missing.
DEFINE_bool(guide_skip_bkgnd_transition, true,
            "NOP the call to CHUDBkgndScene::PlayTransition at runtime "
            "8174EE6C. Its caller reads the CHUDBkgndScene singleton from "
            "81D3F924, finds null, traps, and calls anyway - dereferencing "
            "null and killing Guide scene creation. Skipping the call tests "
            "whether the rest of scene creation can complete without the "
            "background transition."
            ,
            "Kernel");
DEFINE_uint32(guide_xam_ui_startup, 0,
              "Runtime address of xam's UI startup (ghidra 8179C748), the "
              "only caller of the render host. Like the render host it "
              "begins with the thread-identity check, so it must run on "
              "xam's recorded UI thread. Queued there as an APC rather than "
              "called from ours. Defaults 0: xam's threads wait with "
              "alertable=0, so a queued APC never runs. Injecting work onto "
              "xam's UI thread needs a different mechanism."
              ,
              "Kernel");
DEFINE_bool(guest_native_timers, false,
            "Adopt guest-created KTIMERs so they can be waited on, and "
            "implement KeSetTimer/KeSetTimerEx/KeCancelTimer. Fixes a "
            "permanent retry loop in real xam, but the DPC a timer carries "
            "is not dispatched yet, so guest code that depends on the "
            "callback proceeds further and then fails. Off until DPC "
            "dispatch exists."
            ,
            "Kernel");
DEFINE_bool(system_root_early, true,
            "Register the SystemRoot symlink before the title starts "
            "rather than after CompleteLaunch returns. Xenia registers it "
            "too late, so a system title querying it during startup - the "
            "dashboard looking for systemupdate.xex - gets device not "
            "found."
            ,
            "Kernel");
DEFINE_bool(guide_spoof_ui_thread, false,
            "Temporarily point xam's recorded UI thread (81D42520) at the "
            "calling thread across the render-host call, then restore it. "
            "xam checks that slot against [r13+256] and traps when they "
            "differ, which this bootstrap always does. Diagnostic only: it "
            "satisfies the check without making the call legitimate."
            ,
            "Kernel");
// Note: 8174FDE0 cannot usefully be called directly. Its only caller is a XUI
// command dispatcher at ghidra 817571D0, which reaches it by tail-branching on
// command type 0x13 with arguments unpacked from a command block (r5 = cmd+8,
// r4 = [cmd+16]). The global is set by a command that was never issued here,
// not by a function nothing calls - so the remaining gap is the XUI command
// pump itself, not another entry point to invoke.
DEFINE_uint32(guide_xui_anim_init, 0,
              "Runtime address of xam's initialiser for the XUI animation "
              "global at 81D3F924 (0 disables). Without it that global is null, "
              "xam's own twi assert at 81756060 catches it, Xenia's "
              "ignore_trap_instructions discards the assert, and four "
              "instructions later the null is dereferenced at 8174E22C - the "
              "crash that stops Guide scene creation.",
              "Kernel");
DEFINE_bool(guide_install_draw_hook, true,
            "Install the per-swap Guide draw hook. Turn off to test whether "
            "the hook - which runs guest code from inside VdSwap on the title "
            "thread - is what blocks the Guide thread in host code.",
            "Kernel");
DEFINE_bool(guide_step_scene, false,
            "With guide_scene_off_thread, drive hud's scene-creator sequence "
            "one call at a time from the Guide thread to identify which one "
            "blocks.",
            "Kernel");
DEFINE_bool(guide_scene_off_thread, false,
            "Run hud's scene creator from the Guide's own thread instead of "
            "from inside the title's swap. Scene loading appears to be "
            "asynchronous, so calling it while holding the title's render "
            "thread might deadlock. It does not help: the scene creator hangs "
            "on either thread, and the title thread then blocks too - almost "
            "certainly on xam's XUI critical section, which the stuck Guide "
            "thread is holding. Defaults OFF - with it on, xam asserts an "
            "out-of-range index three times and then faults, and the dashboard "
            "stops rendering. Only the config file was set to false during "
            "development, so the source default was left dangerously wrong.",
            "Kernel");
DEFINE_bool(guide_preset_fields, false,
            "With guide_init_only, also set [guide+28/32/64] the way hud's "
            "scene creator does before it calls the init, to test whether the "
            "hang follows those fields or the call site.",
            "Kernel");
DEFINE_bool(guide_init_only, false,
            "Call hud's XUI init (hud+0xA898) directly instead of its scene "
            "creator, to separate the init from the vtable[7] registration "
            "tail-call when diagnosing the hang.",
            "Kernel");
DEFINE_bool(guide_step_registrations, false,
            "Drive the 56 calls in hud's registration routine (913F0DB0) one "
            "at a time with logging instead of calling hud's scene creator, "
            "to identify which registration blocks the title thread.",
            "Kernel");
DEFINE_bool(guide_register_classes, true,
            "Run xam's extra XUI class registrars (817503E8, 8199BE08, "
            "8176B2C8) from the title thread AFTER XuiInit. All three return 0 "
            "there, unlike lle_xam_xui_init which runs them before XuiInit and "
            "makes it fail on the duplicate XuiElement. Required: XuiScene is "
            "registered here, and without it scene creation fails 80300004. "
            "(This used to appear to hang; that was the CHUDBkgndScene crash, "
            "now handled separately.)",
            "Kernel");
DEFINE_string(guide_skin_path, "",
              "Guest path to hud's XUI skin package. hud builds its resource "
              "locators with XamBuildResourceLocator(module, \"hud\", "
              "\"strings.xus\"), and its module argument comes from "
              "[guide+4], which its constructor leaves 0 - so the locator is "
              "empty and the scene cannot load. Leave blank to use hud itself, "
              "which carries its skin as a resource section named \"hud\".",
              "Kernel");
DEFINE_bool(guide_bootstrap_on_title_thread, true,
            "Run the whole XUI bootstrap (render host, CreateDC, hud init) "
            "from inside the title's swap, on the title's render thread. Keep "
            "this ON. Turning it off does NOT run the same bootstrap on another "
            "thread - it falls back to an older sequence in emulator.cc that "
            "predates the class registrars, the skin module and the scene "
            "creator's out-pointer, so it gets an XUI init and nothing else. "
            "The two paths were never merged; see research/FINDINGS.md phase "
            "148.",
            "Kernel");
DEFINE_bool(guide_use_title_device, false,
            "Point xam's D3D device global (81D43684) at the title's device "
            "from VdGlobalDevice (801E6FC4) before running xam's XUI render "
            "host, so the Guide renders into the title's back buffer and is "
            "presented by the title's own swap. Without this the Guide "
            "renders on a second device that nothing composites.",
            "Kernel");
DEFINE_bool(guide_create_scene, true,
            "Call the Guide object's vtable[27] (hud 913EB940) after its XUI "
            "init. That slot is one of hud's five XuiSceneCreate sites, and "
            "the root XUI element at [guide+8] - which the draw hands to "
            "XuiElementLayoutTree - is null without it, so the draw succeeds "
            "while laying out nothing.",
            "Kernel");
DEFINE_bool(guide_fake_gpu_writeback, false,
            "After the stall probe locates the word xam polls - the system "
            "writeback at [device+2B10] - advance it from the host and watch "
            "whether the guest leaves its wait. This is an EXPERIMENT, not an "
            "implementation: the real fix is a working system command buffer, "
            "since VdGetSystemCommandBuffer hands back two magic constants and "
            "nothing consumes what xam submits. It answers one question only - "
            "whether that word is what the wait is actually gated on.",
            "Kernel");

DEFINE_int32(guide_stall_probe_seconds, 0,
             "If non-zero, sample the guest PC of the thread that runs the "
             "Guide's device creation, starting this many seconds after the "
             "button press. Samples are taken by suspending the host thread "
             "and reading RIP, then resolving it through the same code-cache "
             "lookup the crash handler uses. Several samples are taken so a "
             "spin can be told from a block: a moving PC is a loop, a fixed "
             "one is a wait. DemandFunction cannot answer this - it logs only "
             "a function's first compilation, so a thread looping in already "
             "compiled code is silent.",
             "Kernel");

DEFINE_bool(guide_create_primary_device, false,
            "Call xam's OTHER device creator, 8178E9F0, instead of 8178F748. "
            "Both funnel into 819F4D28, whose second argument selects a mode: "
            "8178F748 passes 2 and 8178E9F0 passes 1, and 819F4D28 asserts on "
            "anything else. Mode 2 sets a flag bit and skips the branch that "
            "reaches VdInitializeRingBuffer, which is why the device the Guide "
            "button creates has no ring buffer - that is deliberate, not a "
            "failed bring-up. Mode 1 takes the bring-up path, but first calls "
            "KeGetCurrentProcessType and asserts if a device is already "
            "registered: type 2 (SYSTEM) checks VdGlobalXamDevice, anything "
            "else checks VdGlobalDevice, which the title has already filled "
            "in. See guide_system_process_type.",
            "Kernel");

DEFINE_bool(guide_system_process_type, false,
            "Report X_PROCTYPE_SYSTEM from KeGetCurrentProcessType for the "
            "duration of the Guide's device creation, by setting the calling "
            "thread's X_KTHREAD process_type. Xenia's kernel_state.h carries a "
            "system_process with the comment \"no idea when this runs\"; xam's "
            "mode-1 device path is one place it matters. Restored afterwards.",
            "Kernel");

DEFINE_bool(guide_call_boot_entry, false,
            "Call xam's 81751428 before the device creator. Mode-1 bring-up "
            "waits on three notification events (81D433C8, 81D43398, "
            "81D433A8); the only function that signals them is 817915A0, "
            "reached only from 81792880, reached only from 81751428 - which "
            "has no caller anywhere inside xam and never runs. Like the "
            "mode-1 device creator it is an entry point the system boot "
            "invokes and Xenia does not. Its arguments are unknown; it is "
            "called with none, as 8178E9F0 is.",
            "Kernel");

DEFINE_int32(guide_force_cmdbuf_complete, 0,
             "Seconds after the button press to start setting bit 1 of "
             "[device+2B3D] from a host thread. That bit is the ONLY exit "
             "819F4488 has - every other path through it returns 'still "
             "waiting' - so mode 1's async command buffer wait ends when "
             "something sets it, and nothing in Xenia ever does because the "
             "system command buffer is stubbed. The exit condition is read "
             "from the disassembly rather than guessed. Setting it from the "
             "host is a probe of what the bring-up does next, not a fix.",
             "Kernel");

DEFINE_bool(guide_word_diff, false,
            "Word-level diff of FE030000-FE050000 across the Guide's draw, "
            "reporting contiguous runs of changed words. The command stream's "
            "extent cannot be found by walking headers - packets sit between "
            "long runs of NOP padding, so a walker grabs a fragment - but the "
            "words the draw actually wrote ARE its extent, and those can be "
            "measured instead of guessed.",
            "Kernel");

DEFINE_bool(guide_execute_command_stream, false,
            "After the Guide's composite draw, walk the PM4 stream it wrote "
            "into FE03xxxx/FE04xxxx and run it through the command processor. "
            "The Guide builds real draw calls every frame and nothing executes "
            "them; this is the direct test of whether executing them puts "
            "anything on screen. NOT thread safe - it drives the command "
            "processor from the title thread - and is an experiment, not a "
            "mechanism. The real path inserts an indirect buffer into the "
            "title's stream, which is what xam's own InsertAsyncCommandBufferCall "
            "diagnostic refers to.",
            "Kernel");

DEFINE_bool(guide_diff_draw_writes, false,
            "Checksum guest memory in 64KB blocks either side of the Guide's "
            "composite draw and report which blocks changed, flagging any that "
            "contain PM4 type-3 headers. Every attempt to guess where xam "
            "writes its command stream from the descriptor has failed, so stop "
            "guessing and observe the writes directly. One draw completes in "
            "the deep configuration, which makes a single before/after diff "
            "meaningful.",
            "Kernel");

DEFINE_int32(guide_syscmdbuf_buffer_kb, 0,
             "If non-zero, allocate a buffer of this many KB once and hand it "
             "to the guest in VdGetSystemCommandBuffer's descriptor as "
             "p0+0x04 = address and p0+0x08 = size, then report whether the "
             "guest ever writes PM4 into it. Those two fields are the "
             "principled candidates: the guest stores +0x04 into "
             "[device+0x60C0] as persistent state, and +0x08 gates roughly "
             "0xE0 bytes of processing that is skipped while it is zero. The "
             "test is falsifiable - either type-3 packet headers (0xC0......) "
             "appear in the buffer or they do not.",
             "Kernel");

DEFINE_bool(guide_syscmdbuf_fields, false,
            "Fill the two fields of VdGetSystemCommandBuffer's p0 descriptor "
            "that the only guest caller actually reads: +0x30 = 0x500 and "
            "+0x34 = 0x5BE, the values 819FE138 compares them against. Xenia "
            "zeroes the whole 0x94-byte block, so both comparisons fail today "
            "and the guest takes a path written for a descriptor it never "
            "received. This is a probe to find the next expectation, not an "
            "implementation of the system command buffer.",
            "Kernel");

DEFINE_string(guide_system_root, "",
              "Host directory holding xam.xex and hud.xex, mounted as the "
              "guest device SYS:. Without it those files have to live on the "
              "title's own GAME: device, which only works when the title is "
              "the dashboard folder - launching a real game disc makes GAME: "
              "the disc and both loads fail. Set lle_xam and guide_hud_path "
              "to SYS:\\... when using this.",
              "Kernel");

DEFINE_int32(guide_auto_press_seconds, 0,
             "Seconds after the title starts to fire the Guide button "
             "automatically, as if the Xbox button were pressed. Sending the "
             "key with SendKeys depends on the emulator window holding focus, "
             "which it does not reliably do under automation - the press is "
             "silently dropped and the load-time path gets measured instead "
             "of the button path. Zero leaves the button manual.",
             "Kernel");

DEFINE_uint32(guide_force_obj14, 0,
              "Value to store into the Guide object's [+0x14] before hud's "
              "XUI init (913EA898) runs. That init reads [obj+0x14] and, when "
              "it is zero, bails to 913EA920 without ever calling "
              "XuiRenderCreateDC - which is why [obj+0x0C] (the device "
              "context slot) stays null and no frame has a device context at "
              "all. The constructor at 913EA840 zeroes it and something we do "
              "not perform is meant to set it. On the DC-creating branch the "
              "field is only tested, never dereferenced, so a non-zero value "
              "is enough to take it.",
              "Kernel");

DEFINE_bool(guide_patch_null_render, false,
            "Nop the store at runtime 818FDF14, which copies the XUI "
            "context's null-render flag [ctx+0x1C] over the device context's "
            "[dc+0x134]. The device context constructor (81900E70) already "
            "initialises that field to 0; the copy is what sets it to 1, and "
            "a non-zero [dc+0x134] makes XuiRenderBegin skip vtable[20] and "
            "the draw emitter 819F5D18 never build a DRAW_INDX packet. With "
            "the copy removed the zero survives and the Guide's geometry "
            "should reach the command stream. Applied to xam's image at load "
            "time, before anything JITs the function.",
            "Kernel");

DEFINE_uint32(guide_coverage_fn, 0,
              "Guest address of a function whose per-instruction execution "
              "counts to report after the Guide's draw. Requires "
              "trace_function_coverage and a trace_function_data_path, which "
              "make the JIT emit a counter increment per guest instruction. "
              "Reports the furthest instruction reached, which is how to find "
              "where 819F5D18 stops instead of building a draw packet - its "
              "draw construction sites are reachable but sit behind ~390 "
              "branch points, too many to read.",
              "Kernel");

DEFINE_int32(guide_front_buffer_shift, 0,
             "Bytes to shift the front-buffer clone's source by. The bound "
             "colour surface carries what looks like a fetch constant at "
             "+0x24, while the draw emitter 819F5D18 reads six consecutive "
             "words from +0x1C - the two are 8 bytes out of step, which is "
             "structural evidence that r14 is a different object type. "
             "Setting 8 copies from source+8 so the constants line up. A "
             "probe of that mismatch, not a fix.",
             "Kernel");

DEFINE_int32(guide_front_buffer_format, -1,
             "If >= 0, force the low 6 bits of [front_buffer+0x20] to this "
             "value after cloning. 819F7F20 compares exactly those bits "
             "against 0x3D (\"rlwinm r10,r10,0,26,31 / cmplwi cr6,r10,0x3d\"), "
             "and the draw emitter 819F5D18 reads the same field and passes "
             "the masked value to 819FC1E0. The clone inherits the colour "
             "surface's format, which is not a front buffer's - so 0x3D is a "
             "value taken from the guest's own comparison rather than guessed.",
             "Kernel");

DEFINE_bool(guide_fake_front_buffer, false,
            "If the present path's device has a colour surface on RT0 but no "
            "front buffer at +3F74, clone the colour surface into that slot. "
            "The fault is a null dereference of a six-dword fetch-constant "
            "descriptor, so any structurally valid surface should move it - "
            "which is the point. This is a probe of what lies past the front "
            "buffer, not a front buffer: a real one is allocated by 819E7310 "
            "and would have its own memory.",
            "Kernel");

DEFINE_bool(guide_bootstrap_create_dc, true,
            "Have the bootstrap call XuiRenderCreateDC itself. hud creates its "
            "own device context during scene creation and the composite draw "
            "uses that one, not this one - the bootstrap's DC has a different "
            "address and is never drawn with. Off tests whether the call is "
            "load-bearing or leftover scaffolding.",
            "Kernel");

DEFINE_bool(guide_watch_null_render, false,
            "Poll the XUI context global 81D6C978 and, once it is set, the "
            "null-render flag at [ctx+1C], logging every transition from a "
            "host thread. The flag is 1 by the time the render host returns "
            "and no constant 1 is stored to that offset anywhere in the XUI "
            "code, so the value is computed - watching when it appears "
            "narrows which of the five candidate writers that actually "
            "execute is responsible.",
            "Kernel");

DEFINE_bool(guide_watch_front_buffer, false,
            "Poll [device+3F74] - the front buffer - from a host thread and "
            "log every transition, for both xam device globals. A code "
            "breakpoint on the store cannot answer this: installing one "
            "changes scheduling enough that the draw path is never taken, so "
            "the store and the fault can never be observed in the same run. "
            "Reading guest memory from the host perturbs nothing, and catches "
            "a value that is written and then cleared.",
            "Kernel");

DEFINE_int32(guide_probe_threads_seconds, 0,
             "Seconds after the Guide button press to suspend every guest "
             "thread in turn, sample its host RIP, and resolve it to a guest "
             "address. xam spawns a Guide thread in response to "
             "XamInputSendXenonButtonPress; it runs a short setup, returns "
             "from 81BF8550, then parks somewhere and never JITs or exits "
             "again. A counting breakpoint proved it executes exactly once, so "
             "it is blocked rather than pumping - but where it blocks is "
             "outside any code read so far, and only its program counter "
             "answers that.",
             "Kernel");

DEFINE_string(guide_trace_stores, "",
              "Comma-separated runtime addresses to install counting "
              "breakpoints on, reporting which fired with the register file. "
              "Written to find which of the eleven writers of the "
              "command-buffer cursor [dev+0x2B4C] zeroes it during the Guide's "
              "draw: the begin sets it correctly and the emitter still reads "
              "0, and reasoning about which writer runs has been less "
              "reliable in this project than breakpointing all of them at "
              "once.",
              "Kernel");

DEFINE_uint32(guide_trace_pump, 0,
              "Install a counting breakpoint on this runtime address and "
              "report how often it is hit. Written to settle whether xam's "
              "Guide thread - the one XamInputSendXenonButtonPress spawns - is "
              "alive and pumping or parked. It stops writing to the log once "
              "everything in its loop is JITed, so an absence of log lines "
              "proves nothing either way. 81BF73E8 and 81BF8238 are the two "
              "functions its loop at 81BF8550 calls per 44-byte table entry.",
              "Kernel");

DEFINE_int32(guide_xam_button_api, -1,
             "Call xam's own XamInputSendXenonButtonPress (ordinal 0x506) with "
             "this value as its second argument, instead of hand-rolling the "
             "Guide hosting. Aurora - a custom dashboard that successfully "
             "shows the real Guide - imports 292 xam functions including 86 "
             "XUI ones, but NOT a single render entry point: no XuiInit "
             "(0x340), no XuiRenderCreateDC (0x34C), no "
             "XuiRenderBegin/End/Present (0x34B/0x34F/0x353). It builds scenes "
             "and asks xam to open the Guide; xam hosts and renders it. That "
             "is the opposite of this bootstrap, which loads hud itself and "
             "drives its draw loop. Negative leaves it off.",
             "Kernel");

DEFINE_bool(guide_patch_cmdbuf_reset, false,
            "Nop the store at 81A01464, which zeroes the command-buffer cursor "
            "[dev+0x2B4C]. xam calls 81A013B8 (its frame end/flush) from "
            "81A06080 during the Guide's draw, and that clears the cursor set "
            "by the begin - which is why a command buffer prepared before the "
            "draw is always gone by the time packets are emitted. With the "
            "reset removed the buffer survives long enough for the Guide to "
            "write into it. Needs guide_second_context_kb to be useful.",
            "Kernel");

DEFINE_int32(guide_second_context_kb, 0,
             "Give xam its own command buffer of this many KB and submit what "
             "the Guide writes into it as a second rendering pass inside the "
             "title's frame. The title owns the single GPU ring and mode-1 "
             "bring-up takes it away irrecoverably, so xam cannot have a ring "
             "of its own; but Xenia can execute a guest PM4 buffer directly. "
             "Each frame this resets the cursor, calls xam's own begin "
             "(81A01358) so the buffer is set up through the lifecycle that "
             "owns it rather than by poking fields, runs the Guide's draw, "
             "then submits whatever was emitted. Pair with "
             "guide_bind_title_rt so the Guide draws into the back buffer the "
             "title is about to present.",
             "Kernel");

DEFINE_int32(guide_bind_cmdbuf_kb, 0,
             "Allocate a command buffer of this many KB and point the Guide "
             "device's write cursor [dev+0x2B4C] at it. Packet emission "
             "(81A015B8) asserts its caller's cursor equals that field, "
             "reserves words by advancing it, and writes each packet word "
             "through it; on a mode-2 device the cursor is 0 so the first "
             "word stores to guest address 4. Mode 2 is the only "
             "configuration in which the draw emitter is actually reached - "
             "mode 1 brings the device up properly but the emitter is never "
             "entered and no draws are dispatched - so this binds the one "
             "field standing between the emitter and somewhere real to write.",
             "Kernel");

DEFINE_uint32(guide_device_init_fn, 0,
              "Runtime address of an xam device routine to call as f(device, "
              "0) on the Guide's device before it draws, to try to bring up "
              "the state mode 2 leaves zeroed. An address rather than a bool "
              "so candidates can be tried without a rebuild. Known writers of "
              "the command-buffer pointer [dev+0x2B10] are 81A0F858 (calling "
              "it is a no-op here - its writes sit behind branches an "
              "un-brought-up device does not reach) and 81A0FE48.",
              "Kernel");

DEFINE_bool(guide_device_begin, false,
            "Call xam's own device setup routine, runtime 81A0F858, on the "
            "device the Guide draws with. It takes only the device, and it is "
            "the single routine that writes [dev+0x2B10] (the command-buffer "
            "pointer whose nullness faults packet emission), writes "
            "[dev+0x3F74] (the front buffer the draw emitter takes as its "
            "sixth argument), and binds RT0 - the three fields that were "
            "otherwise being patched by hand, one fault at a time, without "
            "converging. It asserts [dev+0x38F0] and [dev+0x4020] are zero and "
            "calls the unbind-all at 819F4C00 first, so run it BEFORE "
            "guide_bind_title_rt rather than after.",
            "Kernel");

DEFINE_bool(guide_restore_title_ring, false,
            "Save the GPU ring registers before xam's mode-1 device creator "
            "and restore them after the Guide has drawn. Mode 1 is the only "
            "creator that brings a device up properly - real presentation "
            "parameters, a ring buffer, and all the device state mode 2 "
            "leaves zeroed - but it re-points the ring from the title's 1MB "
            "buffer to its own 4KB one, so the title's packets stop reaching "
            "the GPU and it never presents again. The ring is only "
            "re-pointed, not torn down, so handing the registers back should "
            "let the title resume and present the buffer the Guide drew into.",
            "Kernel");

DEFINE_bool(guide_bind_title_rt, false,
            "Bind the TITLE's live render target as RT0 on the device the "
            "Guide's present path uses. Only xam's mode-1 creator produces a "
            "device with RT0 bound, and mode 1 re-runs GPU bring-up, which "
            "stops the title's own rendering dead at the button press - so it "
            "can never be the overlay path. Mode 2 leaves the title running "
            "but its device has no render target. The title's RT sits at "
            "[VdGlobalDevice+0x3AC4]: scanning the title device for pointers "
            "whose [+0x24] unpacks as a fetch constant finds exactly one "
            "candidate, and it decodes to the title's real resolution. "
            "Binding it means the Guide draws into the title's back buffer, "
            "which is what an overlay should do.",
            "Kernel");

DEFINE_bool(guide_bind_depth_copy, false,
            "Before the first Guide draw, if the device has a colour surface "
            "on RT0 but nothing on the depth slot, clone the colour surface "
            "and bind the clone with SetDepthStencilSurface (819F38C8). This "
            "is a TEST of one reading, not a fix: the post-redirect fault is "
            "an unguarded read of [r14+20], the low 6 bits of which are "
            "compared against 0x3D by a caller - the shape of a surface format "
            "field - and the device never gets a depth surface because "
            "819F38C8 is never called in any run. Cloning the colour surface "
            "is used rather than fabricating one because it is already "
            "structurally valid. If r14 is not the depth surface this will "
            "change nothing, which is the point.",
            "Kernel");

DEFINE_bool(guide_use_bound_device, false,
            "Before the Guide bootstrap runs, point xam's device global "
            "(81D43684) at whatever VdGlobalXamDevice (801E6FC8) holds, when "
            "the two differ. Under mode 1 two xam devices exist and only one "
            "has a render target: measured in a single run, 81D43684 held "
            "40870D00 with RT0 null - the object the faulting present used - "
            "while 801E6FC8 held 40883A80 with RT0 bound to a real surface. "
            "The DC is built from the global, so it inherits the wrong one.",
            "Kernel");

DEFINE_bool(guide_trace_setrendertarget, false,
            "Break on xam's SetRenderTarget (819F31A8) and log its arguments: "
            "device, index, surface. Counting DemandFunction entries only says "
            "the function ran, and the reset loop 819F4C00 calls it with a null "
            "surface to UNBIND - so a call count cannot distinguish binding "
            "from unbinding. This reads the surface argument.",
            "Kernel");

DEFINE_bool(guide_bootstrap_before_device, false,
            "Queue the Guide bootstrap onto the title thread BEFORE calling "
            "xam's device creator, instead of after. Only matters with "
            "guide_create_primary_device: the mode-1 creator never returns, so "
            "in the normal order the queue call is never reached and the "
            "bootstrap never runs at all. Reversing it lets the mode-1 device "
            "come up - it does bind render targets, which mode 2 never does - "
            "while the bootstrap proceeds on the title thread.",
            "Kernel");

DEFINE_bool(guide_clear_null_render, false,
            "Clear [xui_ctx+1C] after xam's XUI render host runs, before hud "
            "creates its device context. The DC initialiser 818FDE98 copies "
            "that word into [dc+134], which is the flag that makes "
            "XuiRenderPresent return S_OK without presenting and makes "
            "XuiRenderBegin skip its call to dc->vtable[20]. Clearing it at "
            "the source is not the same as guide_force_real_present clearing "
            "[dc+134] per frame: the DC is then BUILT non-null, so Begin runs "
            "vtable[20] too, which is where any render-target setup would "
            "happen. The context is the singleton at 81D6C978.",
            "Kernel");

DEFINE_bool(guide_force_real_present, false,
            "Zero [dc+134] on the Guide's XUI device context before each "
            "composite draw. XuiRenderPresent (dc->vtable[21], runtime "
            "818F9290) returns S_OK without presenting anything whenever that "
            "field is non-zero, and XuiRenderBegin likewise skips its call to "
            "dc->vtable[20]. Ours is 1, so the whole XUI frame runs in a "
            "null-rendering mode that reports success. hud discards Present's "
            "HRESULT (its draw ends in li r3,0), which is why the composite "
            "draw has always logged 00000000. Clearing the field forces the "
            "real path, which tail-calls [dc+1CC]->vtable[24].",
            "Kernel");

DEFINE_bool(guide_create_xam_device, false,
            "Call xam's D3D device creation (runtime 8178F748). It is the "
            "only site that takes the address of xam's device global "
            "(81D43684) and passes it to xam's CreateDevice at ghidra "
            "819FBF28. Gated inside xam on bit 0x200 of [[815F048C]].",
            "Kernel");
DEFINE_bool(guide_call_render_host, false,
            "Call xam's XUI render-host init (runtime 8178DC58) after "
            "XuiInit. It is the only caller of xam ordinal 0x352, which is "
            "one of the two writers of the XUI render context global at "
            "81D6C978 that XuiRenderCreateDC requires. It takes the D3D "
            "device from the global at 81D43684. Defaults OFF: that global "
            "is null under this bootstrap, so the call hits xam's assert and "
            "DbgBreakPoint and kills the dispatch thread.",
            "Kernel");
DEFINE_bool(guide_call_xuiinit, true,
            "Call real xam's XuiInit (81953760) before hud's XUI init. xam's "
            "XuiRenderCreateDC returns E_UNEXPECTED while xam's XUI context "
            "global at 81D6C978 is null, and hud calls XuiInit only after "
            "CreateDC - it assumes its host initialised XUI first.",
            "Kernel");
DEFINE_bool(guide_force_render_gate, true,
            "Force [guide+20] non-zero before hud's XUI init. That field only "
            "gates DC creation in the init at hud+0xA898 and is never "
            "dereferenced there, so with it clear the init skips its whole "
            "body and still returns 0, leaving the render DC null and every "
            "draw failing with E_INVALIDARG.",
            "Kernel");
DEFINE_bool(lle_guide_draw, true,
            "Drive hud's own XUI init and render loop directly. hud is a "
            "system app that expects the system to create its thread and run "
            "its draw loop; nothing in this bootstrap does.",
            "Kernel");
DEFINE_string(lle_xam_scope, "hud.xex",
              "Which importing module gets the real guest xam. Blank means "
              "every importer, which sends dash.xex to real xam as well and "
              "costs it Xenia's profile/media/input emulation.",
              "Kernel");
DEFINE_bool(lle_xam_fake_app_fe, true,
            "Fill xam's system-app table entry for 0xFE by hand, pointing it "
            "at xam's own XamApp message handler, so XamShowGuideUI can "
            "deliver the Guide message.",
            "Kernel");
DEFINE_bool(lle_xam_sysapp_init, false,  // DANGEROUS: see kNote below

            "Call xam's system-app initialiser (81751428) before opening the "
            "Guide. It walks the static app descriptor table at 0x81604368; "
            "nothing inside xam calls it.",
            "Kernel");
DEFINE_bool(lle_xam_xui_init, false,
            "Call xam's XUI class registrars (8199BE08, 817503E8, 8176B2C8) "
            "before opening the Guide. Keep this OFF: XuiInit registers the "
            "built-in classes itself, and pre-registering them makes XuiInit "
            "fail with 0x80300005 on the duplicate \"XuiElement\". With it "
            "off XuiInit returns 0.",
            "Kernel");
DEFINE_bool(lle_xam_heap0_alias, false,
            "Copy xam heap[1]'s descriptor over heap[0], which is left as the "
            "0xCCCC placeholder. Requests in the 0x10000000 class map to index "
            "0 deterministically, so they all fail against the placeholder.",
            "Kernel");
DEFINE_bool(lle_xam_heap_init, false,
            "After xam's DllMain, call its heap-creation routine (817B4B70) "
            "directly. xam's heap descriptors are never built under our "
            "bootstrap because nothing outside the module drives that call.",
            "Kernel");
DEFINE_bool(lle_xam_heap_patch, false,
            "Patch out the trap in xam's heap selector (817BAE38) so title "
            "threads whose app id differs from the current one still resolve "
            "a real heap instead of the zero-sized placeholder.",
            "Kernel");
DEFINE_bool(lle_xam_appid_sentinel, false,
            "Force xam's current-app-id sentinel (81D227F0) to -1 after LLE "
            "init so the getter reports XamApp instead of trapping the heap "
            "selector on title threads.",
            "Kernel");
DEFINE_bool(lle_show_guide, false,
            "After the LLE attach sequence, call the real xam's "
            "XamShowGuideUI (ordinal 0x304) to open the Xbox Guide.",
            "Kernel");
DEFINE_string(guide_hud_path, "",
              "Guest path to hud.xex (e.g. \"GAME:\\hud.xex\"). When set, the "
              "Guide is loaded as a system app alongside the running title and "
              "the open message is dispatched to it.",
              "Kernel");
DEFINE_uint32(guide_message, 0x80000004,
              "Message id dispatched to the Guide (hud.xex) system app. It "
              "accepts 0x80000002 through 0x80000010.",
              "Kernel");
