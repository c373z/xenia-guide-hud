/**
 *****
*************************************************************************
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

DEFINE_bool(guide_insn_divergence, false,
            "Phase 1096bu: list the instructions of the coverage target whose "
            "execution count is EXACTLY 1 when the function ran more than once. "
            "81795548 runs twice once the task pool works (1096bj) and the "
            "second pass is destructive; instructions executed once are exactly "
            "where the two passes DIVERGE, which is a sharper question than any "
            "asked so far. Bounded output.",
            "Kernel");

DEFINE_uint32(guide_insn_count_addr, 0,
              "Phase 1096bs: report the execution count of ONE guest "
              "instruction from the live coverage reporter. Coverage prints "
              "unexecuted SPANS of >= 4 instructions, so a single instruction - "
              "such as xam's `twui` assert at 81795560 - is invisible in them "
              "whether it ran or not. Must be a uint32: guide_watch_alloc is an "
              "int32 and a guest address like 0x81795560 overflows it, which "
              "makes the emulator fail to start with no log at all.",
              "Kernel");

DEFINE_bool(lle_xam_system_process, false,
            "Phase 1096br: run xam's LLE boot thread as X_PROCTYPE_SYSTEM for "
            "the whole of its DllMain, because that is what xam IS on hardware "
            "- a system module. Measured need: xam's own allocator 8177B0A8 "
            "begins `bl KeGetCurrentProcessType ; cmpwi 2 ; bne -> return null`, "
            "so it hands back NULL to anything not running as SYSTEM. That is "
            "why 8177BFC8 (the skin-callback manager constructor) returns null "
            "and why the host had to substitute a stand-in at [81D43C50+0x28]. "
            "This is an emulation-correctness change, not a fabricated value: "
            "the guest allocates its own object with its own code once it is "
            "told the truth about which process it is.",
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

DEFINE_bool(guide_overlay_at_swap, false,
            "Hand the Guide's finished command stream to the GPU thread and "
            "execute it in the swap packet, immediately before the title's "
            "frame is presented. This is the mechanism rather than a probe. "
            "guide_execute_command_stream runs the same words from the title "
            "thread, which drives the command processor concurrently with the "
            "GPU thread and inserts the work at an arbitrary point in the "
            "title's frame; an overlay wants the frame finished, the render "
            "target still bound, and its geometry drawn last. "
            "ExecutePacketType3_XE_SWAP is exactly that point.",
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
DEFINE_path(guide_hdd_path, "",
            "Phase 1099n: host folder mounted WRITABLE as "
            "\\Device\\Harddisk0\\Partition1 - the console's hard drive, where "
            "xam keeps its cache and profiles. An .xex title's own folder is "
            "then mounted read-only at \\Device\\TitleXex instead. Empty = "
            "Xenia's default (the .xex folder is Partition1).",
            "Kernel");
DEFINE_int32(guide_storage_dump_seconds, 0,
             "Research probe: seconds after launch to dump xam's storage "
             "device table (81D3E1A8), and again 15 s later. 0 = off.",
             "Kernel");
// Phase 1099g: a second press, to capture the Guide CLOSING as well as opening.
DEFINE_int32(guide_auto_press_again_seconds, 0,
             "Seconds after the automatic Guide press to press it again. Zero "
             "presses once.",
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

DEFINE_bool(guide_patch_present_gate, false,
            "Nop the branch at runtime 818F92E4 inside XuiRenderPresent, so it "
            "performs the real present regardless of [dc+0x134], WITHOUT "
            "clearing that field. guide_patch_null_render and "
            "guide_clear_null_render both clear the field itself, which also "
            "makes XuiRenderBegin run vtable[20] - that path crashes at "
            "819DE94C or hangs the draw call, giving 0 draws. Leaving the "
            "field set keeps Begin skipping it (2100+ draws) while this makes "
            "Present actually present. Use INSTEAD of those two, not with.",
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

DEFINE_bool(guide_split_reserve_buf, false,
            "Allocate a SEPARATE buffer for the reservation window "
            "[dev+0x30]/[0x34] instead of pointing it at the command buffer. "
            "Phase 812: GuideBindDeviceCmdbuf widens the reservation onto the "
            "same allocation it binds as the command buffer, so reservation "
            "payload (matrices, clip planes) lands inside what reads back as "
            "the command stream - which invalidated every packet count in "
            "phases 806-811. Splitting them makes the command stream "
            "readable.",
            "GuideResearch");
DEFINE_bool(guide_clear_dc_134, false,
            "Zero [dc+0x134] immediately before the composite draw. Phase 821: "
            "818F92E4 branches past the virtual call into the draw emitters "
            "whenever that field is non-zero, and it reads 1 on every draw in "
            "every configuration - which is why the entire draw subsystem is "
            "unreached.",
            "GuideResearch");
DEFINE_uint32(guide_force_46d0, 0,
              "Write this value into [dev+0x46D0] before the composite draw. "
              "Phase 825: 819FE980 gates the call into the draw emitters on "
              "that word being non-zero, and phase 830 found every writer that "
              "would set bits in it is unreachable in this build. 8 sets the "
              "bit the first gate tests.",
              "GuideResearch");
DEFINE_bool(guide_force_prim4, false,
            "Patch 81A14228 from `addi r11,r11,5` to `li r11,4`, forcing the "
            "primitive type the XUI draw path defaults to. Phase 834: case 4 "
            "is the only one that emits DRAW_INDX_2 and 81A14110 can never "
            "default to it.",
            "GuideResearch");
DEFINE_bool(guide_force_primcount, false,
            "Patch 81A13A4C from `mullw r31,r27,r26` to `li r31,6`, giving the "
            "XUI draw dispatcher a non-zero primitive count. Phase 841: with "
            "the type forced to 4 the draw case runs and exits at once because "
            "the count is zero.",
            "GuideResearch");
DEFINE_bool(guide_exec_emitted_range, false,
            "Execute the composite's whole emitted range instead of the runs "
            "the word diff finds. Phase 855: the diff-driven execution stops 90 "
            "words before the first DRAW_INDX, so the draws are never "
            "submitted.",
            "GuideResearch");
DEFINE_string(guide_status_fns, "",
              "Comma-separated hex guest addresses. On the coverage readback, "
              "report each one's symbol status (DEFINED-translated = it ran, "
              "DECLARED-not-run = it did not). Phase 795: probing reachability "
              "one address per 60s run made a single level of a call-graph "
              "walk cost six runs; this answers the same question for a whole "
              "level at once. ONLY VALID FOR FUNCTION ENTRY ADDRESSES: "
              "phase 902 - LookupFunction declares a new symbol at any address "
              "it does not already know, so a mid-function address always "
              "reports not-run. 81A015C0 reads not-run while 81A015B8, the "
              "function containing it, reads RAN in the same batch. Probing "
              "call sites or instruction addresses with this measures nothing.",
              "GuideResearch");
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

DEFINE_bool(guide_register_all_classes, false,
            "Call every XUI class registrar in xam, not just the three the "
            "bootstrap knows about. xam has 39 per-class registrars (callers of "
            "the internal registration function 8194F118) and the class "
            "registry ends up holding only 16 entries, so most classes are "
            "never registered. GuideMain.xur, GuideMainServer.xur and "
            "MiniMediaPlayer.xur all fail with E_FAIL while 20 leaf scenes "
            "load - the shape of a scene asking for a control class that is "
            "not registered.",
            "Kernel");

DEFINE_string(guide_scene_override, "",
              "Create this scene from hud's package instead of only the one "
              "hud picks. hud always creates InfoUpsellLive.xur - the \"no Xbox "
              "Live\" upsell page - which is a nearly empty page even when it "
              "works. The package also contains GuideMain.xur, GamesTabSignedIn"
              ".xur, Diagnostics.xur and 32 other scenes, so this loads a named "
              "one directly through XuiSceneCreate and reports its children and "
              "their visuals.",
              "Kernel");

DEFINE_bool(guide_static_locator, false,
            "Set [guide+8] = -1 so hud builds its resource locator with "
            "XamBuildResourceLocator instead of XamBuildDynamicResourceLocator. "
            "hud's scene creator tests [guide+8] against -1 and takes the "
            "dynamic path for anything else, passing [guide+8] itself as the "
            "module - and it is 0, which produces the locator "
            "\"section://@0,hud#strings.xus\": a null package, so "
            "XuiSceneCreate(\"InfoUpsellLive.xur\") finds nothing and returns "
            "an empty scene. The static path uses [guide+4] instead, which the "
            "bootstrap already sets to hud's module.",
            "Kernel");

DEFINE_bool(guide_fake_ring, false,
            "Give the mode-2 Guide device the physical buffers a mode-1 device "
            "has. Scanning both, a mode-1 xam device holds physical pointers at "
            "[dev+0x3B64] and [dev+0x3DC4] while a mode-2 device holds none at "
            "all - which is why nothing it draws can be emitted, not even a "
            "Clear. Mode 2 is the only configuration where the draw emitter is "
            "actually reached, so this is the one way to test whether the "
            "absent ring is what stops emission: allocate physical memory and "
            "fill those slots.",
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

DEFINE_bool(guide_predraw_surfaces, false,
            "Before EVERY composite draw, point the drawing device's "
            "render-target slots and front buffer at the title's live "
            "surface. The RTs are an indexed array - [dev+(idx+0xCA8)*4], "
            "i.e. 0x32A0 + idx*4 for idx 0..3, with idx==4 special-cased "
            "to the depth slot 0x32B0 - plus the front buffer at 0x3F74, "
            "which 819F5D50 shows is the draw emitter's sixth argument. "
            "guide_bind_depth_scan writes the same slots but at button "
            "time on the dispatch thread, and 819F4C00 (the unbind-all) "
            "clears them before the title thread ever draws, which is why "
            "it changes nothing. This writes them on the drawing thread "
            "against [dc+0x1CC] - the device the draw actually uses - so "
            "they survive to 819DE94C and 819F5EC4.",
            "Kernel");
DEFINE_int32(guide_vis_extra_mask, 0,
             "Extra bits OR'd into the visibility mask. Part of the phase-278 "
             "best configuration (--guide_vis_extra_mask=4096). Definition "
             "lost in the refactor incident; type and default recovered from "
             "the pre-loss run-log config dump.",
             "Guide");
DEFINE_bool(guide_call_present_bracket, false,
            "Call 819FEB78(dev) - the device function that brackets the draw - "
            "instead of relying on our injected paint. The emitter reads its "
            "cursor from a CALLER STACK LOCAL (phase 485), so no device field "
            "we write can supply it; the cursor has to come from the begin that "
            "819FEB78's own call chain performs. Its unwind chain is exactly "
            "819FECB4 -> 819FE9B4 -> 819FDD74 -> 81A0A6A4 -> the emitter.",
            "Guide");
DEFINE_bool(guide_force_drawgate, false,
            "Set the low 12 bits of [dev+0x10] before each composite draw. "
            "819F6BC0 keeps exactly those bits (rldicl r10,r11,0,52) and skips "
            "DRAW_INDX when they are zero; on dashroot they are measured as "
            "000 every draw, which is why its 9623-word stream carries no draw "
            "packets. Neither guide_force_dirty nor guide_dirty_via_api moves "
            "them, so the render-tree dirty API is not what sets this. Writing "
            "them directly asks whether the gate is the only thing suppressing "
            "the draw, or merely the first.",
            "Guide");
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
DEFINE_string(lle_xam_scope, "",
              "Phase 1095ah: BLANK IS NOW THE DEFAULT - every importer, "
              "including dash.xex, binds to the real guest xam. Measured "
              "BETTER than the old \"hud.xex\" scope on the full plan: 436 "
              "paints vs 433, 28 nav, 0 host faults, 0 guest crashes "
              "(p1095ah). It also removes an HLE/LLE split that could not "
              "be fixed any other way: with the dash on HLE xam it created "
              "notify listeners through Xenia (xam_notify.cc), and those "
              "handles then crossed into real xam, which dereferenced "
              "them and got ObReferenceObjectByHandle's 0xDEADF00D "
              "sentinel - see 1095ae/af/ag, where BOTH attempts to change "
              "that sentinel took the Guide from 433 paints to 0. With "
              "blank scope XamNotifyCreateListener resolves to real xam's "
              "817680C0 and the HLE listener path is gone entirely.",
              "Kernel");
DEFINE_bool(lle_xam_fake_app_fe, false,
            "Phase 1095ay: OFF. This filled xam's system-app table entry "
            "for 0xFE BY HAND. Measured against a run with it off and "
            "lle_xam_sysapp_init on: IDENTICAL - 432 paints, 28 nav, 0 host "
            "faults, 0 guest crashes - and the two \"failed to find app id "
            "0x000000FE\" warnings appear in BOTH, so the hand-filled entry "
            "was never satisfying those lookups anyway. Host authorship "
            "with no measured benefit; removed rather than kept.",
            "Kernel");
DEFINE_bool(lle_xam_sysapp_init, true,
            // Phase 1095ax: this carried "DANGEROUS: see kNote below" and
            // FINDINGS recorded "Phases 28 that enabled lle_xam_sysapp_init
            // wrote to guest address 0". That verdict is ~1067 phases old.
            // Re-measured (p1095aw): 432 paints, 28 nav, 0 host faults,
            // 0 guest crashes. No write to guest 0. The danger was almost
            // certainly cleared by this session's kernel fixes, and running
            // xam's OWN initialiser is what lets lle_xam_fake_app_fe go.

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
DEFINE_bool(guide_borrow_front_buffer, false,
            "Copy the title device's front buffer pointer [+0x3F74] into the "
            "Guide's own device, rather than repointing the wrapper at the "
            "title device. The title device has the only real front buffer "
            "(A240A380) but faults in 819E5350 because [dev+0x3308] is a "
            "0000FFFF sentinel there; the Guide device has xam's tables but "
            "no buffer. This lends just the missing field.",
            "Kernel");

DEFINE_bool(guide_rebind_wrapper_device, false,
            "Call the wrapper's SetDevice (8191BAC8) once to point it at the "
            "device mode 1 set up, captured by guide_trace_devsetup. Needs "
            "that flag on. The third argument is a 124-byte block memcpy'd "
            "into wrapper+0x10, so it must be readable - the wrapper's own "
            "+0x10 is used, making it a self-copy.",
            "Kernel");
DEFINE_bool(guide_repair_wrapper_device, false,
            "When [wrapper+0x0C] reads 0, write the last known device back into "
            "it from the GuideLendFB poller. The 819DE94C crash with "
            "[dc+0x134]=0 passes that slot down two frames and faults on it. "
            "Requires guide_borrow_front_buffer (the poller). Warns whenever "
            "the slot is null even when off.",
            "Kernel");

DEFINE_bool(guide_trace_hang, false,
            "Trace 819DEA70 and 819DE8F8, the frames below 8191AFD0 in the "
            "vtable[20] path, to bound where the draw call hangs when "
            "[dc+0x134] is cleared. The last site to log is the one that "
            "does not return.",
            "Kernel");

DEFINE_bool(guide_trace_emitter, false,
            "Log the arguments the draw emitter 819F5D18 is called with. It "
            "executes but the frame carries only xam's own begin word, so the "
            "draw emits nothing; guide_coverage_fn would say where it stops "
            "but its per-instruction counters crash the skin-init path.",
            "Kernel");

DEFINE_bool(guide_trace_devsetup, false,
            "Breakpoint 81A0FE48, the device setup mode 1 runs and mode 2 "
            "skips, and log its first argument - the device it is setting "
            "up. That device gets a front buffer at +0x3F74 and is never "
            "published to any global, so this is the only way to obtain a "
            "pointer to it. The wrapper is bound to a different device "
            "around log line 5026, long before mode 1 runs.",
            "Kernel");
DEFINE_bool(guide_force_front_buffer, false,
            "Call 81A0FE48(device, 0) once, the front-buffer setup that "
            "mode 2 skips. 819F4D28 runs it only when its mode argument "
            "is not 2, and the creator this bootstrap reaches passes 2 as "
            "a literal, so [device+0x3F74] stays null and the draw "
            "emitter faults on it. The second argument mirrors r7 at that "
            "call site, which is zero.",
            "Kernel");
DEFINE_bool(guide_reuse_xui_ctx, false,
            "Skip the render-host init (8178DC58) when a XUI context "
            "already exists. Calling it a second time builds a new "
            "context and frees the live one, leaving any device "
            "context created against the old one dangling - measured "
            "as 81D6C978 changing 40877DC0 -> 408BCA60 and a later "
            "call through freed memory holding the string XuiScene.",
            "Kernel");
DEFINE_bool(lle_xam_font_init, false,
            "Phase 1045: drive xam's font-subsystem initialiser 8178DE50 on the "
            "LLE xam init thread, before the skin loader. It opens the system "
            "typefaces (file://media:/XenonCLatin.xtt via 81754C10) that every "
            "XUIFONT rasterises from; our bootstrap never called it, so every "
            "font had an empty typeface and DrawText culled every glyph. Needs "
            "guide_media_link so media: resolves.",
            "GuideResearch");
DEFINE_bool(guide_media_link, true,
            "Phase 1043: xam's font engine opens its typefaces as "
            "file://media:/XenonCLatin.xtt (and JKLatin, SCLatin, the .xttp "
            "patches); every open failed with 'ResolvePath(media:) failed - "
            "device not found', leaving each XUIFONT with no glyph data, so "
            "DrawText's per-glyph cull rejected everything. The dashroot "
            "folder carries those .xtt files; link media: to the same device "
            "as SYS:.",
            "GuideResearch");
DEFINE_bool(lle_xam_render_host_first, false,
            "Phase 1041: run xam's XUI render host (8178DC58) on the LLE xam "
            "init thread - the thread xam records as its UI thread - BEFORE the "
            "skin loader. The host creates the XUI context with its texture-"
            "load callback (8178DBD8) installed, so the skin loader's DC is "
            "built against a real context and the DC loader's null check "
            "(81901E88) no longer needs to be nop'd for skin init to survive; "
            "the nop was what made every later texture load return E_FAIL.",
            "GuideResearch");
DEFINE_bool(guide_launcher_scan, false,
            "Scan xam's data (81600000..81E00000) once, 4 s in, for pointers "
            "to xam's sys-app launcher chain (817C27C8 / 817C2700 / 817C2480). "
            "Phase 1096en concluded that chain is unreachable from STATIC "
            "evidence only - no bl callers, not exported, one dword reference "
            "which is its own .pdata entry, not a thread entry - and a pointer "
            "stored at RUNTIME would appear in none of those. 1096es ran it: "
            "each address occurs exactly once, at 816F9FD0/E0/E8, which are "
            "those .pdata entries. So the chain is genuinely unreferenced at "
            "runtime, not merely statically invisible.",
            "GuideResearch");
DEFINE_uint32(guide_insn_value_addr, 0,
              "Capture the value of one guest register each time the "
              "instruction at this address executes (decimal, like every "
              "other address flag here). Reported alongside the coverage "
              "line. Phase 1096 needed exactly this three times - 'what does "
              "8177F78C actually load', 'what is in [record+0xC] at the "
              "send', 'is [81D42688] zero when the pass reads it' - and had "
              "only execution counts, which cannot answer any of them.",
              "GuideResearch");
DEFINE_uint32(guide_insn_value_reg, 11,
              "Which guest GPR guide_insn_value_addr captures. Default 11 "
              "because the loads under investigation land in r11.",
              "GuideResearch");
DEFINE_bool(guide_hud_load_before_title, false,
            "Load hud.xex BEFORE launching the title, so it registers with xam "
            "before xam's one-shot sys-app pass runs. "
            "Phase 1096ec measured the ordering at 250 ms resolution: all 13 "
            "invocations of 8177F588 complete ~1.0 s into guest execution, "
            "hud's handler does not reach [81D42688] until ~2.6 s, and the "
            "pass never runs again - so all four reads of that slot got zero "
            "and none of them dispatched to hud. guide_hud_load_delay_ms is "
            "already 0; the 2.6 s is the load itself, and the load thread "
            "already starts immediately after LaunchModule, so nothing short "
            "of loading earlier can win that race. "
            "emulator.cc:5573 notes system DLLs are normally loaded by xam, so "
            "this ordering is xam's on hardware and the host has taken it "
            "over - loading it earlier is fixing the host's ordering, not "
            "authoring guest behaviour. The existing comment on the load site "
            "already suspected this of 'losing xam's single message 0x7EC'.",
            "GuideResearch");
DEFINE_uint32(guide_coverage_interval_ms, 5000,
              "Interval between GuideLiveCoverage reports. Phase 1096eb read "
              "ordering out of the report sequence (calls=13 by report #2, "
              "frozen after), but at 5000 ms the brackets are far too wide: "
              "hud's handler lands in [81D42688] at +2533 ms, so anything "
              "between 2533 and the second report is unresolved. Lowering "
              "this narrows the bracket without adding machinery - the "
              "reports already carry the call count.",
              "GuideResearch");
DEFINE_bool(guide_event_census, false,
            "Log every DISTINCT object passed to KeSetEvent/KePulseEvent, "
            "bounded. Phase 1096dn measured xam's only non-pool thread parked "
            "forever in KeWaitForSingleObject at 8175EDF8 on the event the "
            "GuideInfWait census reported as obj=401E12A0. That address is a "
            "guest heap allocation and moves between runs, so the question "
            "'is it ever signalled' has to be answered WITHIN one run by "
            "comparing this census against GuideInfWait's line. The existing "
            "GuidePoolSyncLog cannot do it - it filters to the pool KTIMER "
            "range 81D42400..81D42540 and this object is nowhere near it.",
            "GuideResearch");
DEFINE_bool(guide_guest_xenon_button, false,
            "Route the Guide button into the guest through xam's OWN "
            "automation API instead of the host opening hud itself. "
            "81723E08 is xam ordinal 0x3D7, XAutomationpInputXenonButton - on "
            "the 360 the 'Xenon button' IS the Guide button. It takes a user "
            "index in r3, stores 1 into [81D3C728 + user*4] (81723E44 stwx), "
            "and notifies through [81D4F610] -> 817C2090. "
            "Phase 1096db measured that table EMPTY: xam's XamInputGetState "
            "(817C3090) returns ERROR_DEVICE_NOT_CONNECTED 5260 times and its "
            "success path never runs, which is the first link in the chain "
            "that ends with ShowHud never being called. This is xam's own "
            "entry point for the press, so nothing is fabricated and no "
            "argument is invented - the only value passed is the user index "
            "the host already knows.",
            "GuideResearch");
DEFINE_bool(guide_pass_guide_button, false,
            "Let the GUEST see the Guide button. XamInputGetState currently "
            "detects the rising edge, raises the host-side g_guide_button_edge, "
            "and then CLEARS X_INPUT_GAMEPAD_GUIDE out of the gamepad state "
            "before returning it - so xam can never learn the button was "
            "pressed and never asks for its own UI. "
            "Phase 1096cy measured the consequence end to end: no sender of "
            "the 0x2100x 'show a xam UI' message ever runs (81789578 never "
            "translated), so the pump 81780460 only ever sees 0x2000A, "
            "0x20005, 0x8000000C and 0x80000007, so its ordinal-580 case "
            "((r3 & 0x2F000) == 0x21000) never fires, so [81D43C50+0xA8] "
            "stays 0 and the HUD-manager loop never reaches 81795058 -> "
            "ShowHud 8174FDA0. "
            "With this set the bit is passed through untouched. That is "
            "routing the user's input INTO the guest, which is what the host "
            "is supposed to do; stripping it and drawing the Guide ourselves "
            "is what it is not.",
            "GuideResearch");
DEFINE_bool(guide_diag_call_launcher, false,
            "DIAGNOSTIC ONLY, and like guide_diag_call_ordinal_580 it passes "
            "ARGUMENTS THE GUEST DID NOT SUPPLY - three zeros to 817C2480, "
            "xam's sys-app launcher (it matches \"hud.xex\" at 816044FC and "
            "uses sys-app id 0xFF). 1096fp closed the causal loop: hud has no "
            "sys-app record, so no thread, so it never polls 0x839, so no "
            "Guide press is seen, so ordinal 580 is never called, so no record "
            "is created. The loop needs one entry from outside it, and on "
            "hardware that entry is this function - measured unreachable here "
            "and in retail (1096en/1096fc). "
            "This answers the one question left: if the launcher IS entered, "
            "does it create hud's record ([81D426C8] non-null) and start the "
            "ring? A yes means the loop is breakable at exactly one point; a "
            "no means the launcher needs real arguments to do anything. "
            "NEVER default this on - entering it from the host is the host "
            "starting the Guide, which is what the goal forbids.",
            "GuideResearch");
DEFINE_bool(guide_diag_call_ordinal_580, false,
            "DIAGNOSTIC ONLY, and it passes ARGUMENTS THE GUEST DID NOT "
            "SUPPLY - six zeros to xam ordinal 0x244 (81793AA0). Phase 1096cv "
            "declined to do this because inventing six argument values is the "
            "fabrication this goal forbids, and that judgement stands for any "
            "FIX. As a MEASUREMENT it answers one question nothing else can: "
            "ordinal 580 is the only non-circular route to 81793980, which "
            "arms 817935B8 (81793A50), which sets [81D43C50+0xA8] = 1 and "
            "drives the state to 8 - and that gate is what stops the "
            "HUD-manager loop reaching 81795058 -> ShowHud. If the chain "
            "advances, the downstream code works and only the trigger is "
            "absent; if it does not, the gap is larger than the trigger. "
            "81793AC4-AE0 accepts r5 and r6 both zero, so all-zeros satisfies "
            "the function's own assert. NEVER default this on.",
            "GuideResearch");
DEFINE_bool(guide_guest_show_via_xam, false,
            "Ask xam to show the Guide through ITS OWN EXPORTED ENTRY POINT "
            "instead of the host originating the show. 8178E240 is xam "
            "ordinal 0x245 (581), takes no arguments, and is the only code in "
            "the image that moves the HUD state word: 8178E170 compare-and-"
            "swaps [81D43C50+0] from 0 to 0x10 (8178E1D0 li r7,0x10; 8178E1E0 "
            "lwarx; 8178E1EC stwcx.). 0x10 is one of exactly two values the "
            "HUD-manager loop 81794BC8 accepts at 81794C68/81794C74 before it "
            "will run its body and reach 81795058, the single call site of "
            "ShowHud 8174FDA0. "
            "Phase 1096cs measured that 8178E240 NEVER RUNS - not even on the "
            "arm that paints - and that [81D43C50] therefore never leaves 0, "
            "so xam's HUD state machine is never entered and guide_host_show "
            "is driving host-side rendering entirely beside it. This calls "
            "the export the way a Guide button press is supposed to, which is "
            "routing the user's input INTO the guest rather than drawing for "
            "it.",
            "GuideResearch");
DEFINE_bool(guide_diag_kick_hud_loop, false,
            "DIAGNOSTIC ONLY - THIS VIOLATES THE GOAL ON PURPOSE. After the "
            "guest-dispatched skin loader completes, call xam's OWN kick "
            "8177BF38 on the task at [81D43C50+0x28], whose proc the loader's "
            "tail has just set to the HUD-manager loop 81794BC8. "
            "Phase 1096cp: the loop is armed with flags 1 (proc, no ENQUEUE) "
            "and the only kick site for that slot (817907BC inside 81790758) "
            "is measured never to run, so the loop never executes and "
            "81795058 -> ShowHud 8174FDA0, the ONLY guest route to showing "
            "the Guide, is never reached. This flag answers one question and "
            "no other: if the loop were dispatched, would the guest show "
            "itself? A yes means the remaining work is finding the legitimate "
            "trigger; a no means the gap is larger. It is NOT a fix and must "
            "never be defaulted on - it is the host driving guest code, which "
            "is exactly what guide_guest_dispatch_skin_loader removed.",
            "GuideResearch");
DEFINE_uint32(guide_guest_dispatch_wait_ms, 4000,
              "With guide_guest_dispatch_skin_loader, how long to wait for "
              "the guest's own pool to run the skin loader to completion "
              "before continuing. Completion is xam's OWN signal: 81795948 "
              "stores 1 to [81D43C50+0xB4] at the loader's tail, which is "
              "AFTER it re-arms the shared task with the HUD-manager loop at "
              "8179593C. The host writes nothing and drives nothing here - it "
              "only observes a guest flag, the same one the existing "
              "guide_hud_app_init_wait_ms block already polls.",
              "GuideResearch");
DEFINE_bool(guide_guest_dispatch_skin_loader, false,
            "Inside the lle_xam_skin_init block, do NOT call xam's skin "
            "loader (81795548) with processor()->Execute. Leave it to the "
            "guest's own task pool, which 81795970 already armed with "
            "flags 5 (proc | ENQUEUE) and kicked at 81795A20. "
            "Phase 1096ci: the host's direct call is a SECOND invocation on "
            "top of the guest's queued one. With guest_native_timers the "
            "pool actually dispatches, so 81795548 runs TWICE (measured: "
            "calls=2, tids 6 and 7, the extra caller being the dispatcher "
            "81779D54). The second pass re-enters the context replacer "
            "818FF2C8 with [81D6C978] already populated, so its release "
            "block runs, the render context drops to refcount 0, and its "
            "destructor clears [81D6C9C8] - which 819138F0 then "
            "dereferences. Neither pass reaches the loader's tail at "
            "8179593C, so the HUD-manager loop 81794BC8 is never armed. "
            "The host driving guest code that the guest already queued is "
            "exactly what the goal forbids, and it is the race's second "
            "runner. This flag removes the host's copy.",
            "GuideResearch");
DEFINE_bool(lle_xam_skin_init, false,
            "After xam's DllMain, call its skin loader (81795548) "
            "directly. That routine opens \\SystemRoot\\huduiskin.xex, "
            "reads its skin.xur, and is the only path that ever reaches "
            "XuiVisualRegister - without it the visual registry stays "
            "empty and every control gets a null visual. It takes no "
            "arguments and has no callers inside xam.",
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
DEFINE_int32(guide_hud_load_delay_ms, 0,
             "Phase 1096: how long the host waits before loading hud.xex. This "
             "was a hardcoded 8-second sleep with the comment \"give the title "
             "time to bring up graphics\" - a host-authored timing value, and "
             "the goal forbids those. It matters: xam originates its ONLY "
             "message 0x7EC once (81956ED0, coverage 20/20 calls=1) and "
             "forwards it once (8174C730, 16/16 calls=1), and hud's dispatcher "
             "913E9AB0 runs 297 times without ever seeing it. If the single "
             "message is emitted during xam init, inside this window, hud is "
             "not loaded yet and cannot receive it. Default keeps the old "
             "MEASURED AND NOW REMOVED (default 0): 8000ms, 500ms and 0ms all "
             "give the same clean run - 429/435/429 paints, 0 host faults, 0 "
             "guest crashes - and identical 0x7EC coverage, so the sleep was "
             "neither load-bearing nor the cause. The host no longer waits. "
             "Kept as a flag so the timing stays measurable, not asserted.",
             "Guide");

DEFINE_string(guide_hud_path, "",
              "Guest path to hud.xex (e.g. \"GAME:\\hud.xex\"). When set, the "
              "Guide is loaded as a system app alongside the running title and "
              "the open message is dispatched to it.",
              "Kernel");
DEFINE_uint32(guide_message, 0x80000004,
              "Message id dispatched to the Guide (hud.xex) system app. It "
              "accepts 0x80000002 through 0x80000010.",
              "Kernel");

// ---- recovered 2026-08-29 after the refactor incident ----
DEFINE_bool(guide_add_render_children, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_call_render_begin, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_claim_device_thread, true,
             "Phase 1054 dbg: before each paint, store the paint thread's xam "
             "thread id into the D3D device's owner field ([device+0x2B08], "
             "xam's 817F6C30) and restore the previous owner after it, so "
             "xam's per-call ownership guard passes: the debug xam (xamd.dll) "
             "otherwise prints 'trying to use a D3D device object that is "
             "owned by a different thread' on every D3D call, which cost half "
             "a paint in xenia's DbgPrint and string exports.",
             "Guide");
DEFINE_bool(guide_diff_draw_objects, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_dirty_via_api, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_force_dirty, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_force_flag17, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_force_visible, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_navigate_manual, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_navigate_scene, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_fix_device_dims, false,
            "Fill the Guide device's uninitialised width/height fields from "
            "the title's. Phase 958: nine fields on the title's device hold "
            "1280/720 or their halves and every one reads zero on the "
            "Guide's, which is the single upstream zero behind the null "
            "surface pitch, the zero viewport and the infinite projection.",
            "Guide");
DEFINE_bool(guide_zero_arena, false,
            "Zero the reservation arena before each paint. Phase 901: none of "
            "the sixteen DRAW_INDX emitters in xam runs, yet the arena parses "
            "with 416 of them - so either the paint writes them by some other "
            "route, or they are stale bytes being replayed.",
            "Guide");
DEFINE_bool(guide_overlay_copy_stream, false,
            "Publish a private copy of the paint's command stream (in a "
            "system-heap buffer) rather than the arena range itself. Phase "
            "1012: later paints rewrite the arena, so with "
            "guide_overlay_repeat the consumer was re-parsing whatever the "
            "arena held by then and emitting no draws (996). A copy keeps the "
            "last good stream runnable every frame until the next valid "
            "paint replaces it.",
            "GuideResearch");
DEFINE_bool(guide_patch_dc_loader, false,
            "Phase 1038: the DC's locator loader 81901E40 still returns E_FAIL "
            "at its null-callback check after the callback is installed, while "
            "the XUI default loader called by hand with the DC's sub-object "
            "succeeds (S_OK, 18x18 texture for sharedres://A-Button.png). "
            "Make the check's branch at 81901E88 unconditional so the DC "
            "calls the installed callback the way a real boot would. Needs "
            "guide_install_ctx_callback.",
            "GuideResearch");
DEFINE_bool(guide_patch_xui_warn, false,
            "Phase 1034: XUI's warning routine 81970F60 formats its message "
            "and hands it to xam's trace sink 817F7738 (ETW, invisible here) "
            "and to the provider slot 8178F600 (E_NOTIMPL). Retarget the "
            "'bl 817F7738' at 819710D0 to the module's DbgPrint thunk "
            "81D0F98C so the formatted text lands in xenia.log as (DbgPrint).",
            "GuideResearch");
DEFINE_uint32(guide_xui_debug_level, 0,
              "Phase 1032: XUI's own diagnostics (XUIFONT::DrawText CLIPPED / "
              "TRUNCATION, texture-load failures, 'XUI RIP') are gated on the "
              "debug-level word at 81D27738 (>=1 logs through 81970F60, >=2 "
              "also breaks). Write this value there once, before the paint, so "
              "the runtime reports why glyphs are culled and images fail.",
              "GuideResearch");
DEFINE_bool(guide_install_ctx_callback, false,
            "Phase 1029: the live XUI context (vtable 8163E200) has a null "
            "texture-load callback at [ctx+0x0C]; 81901E40 returns E_FAIL on "
            "it, so no image ever loads. xam's render host 8178DC58 would "
            "create a context with 8178DBD8 in that slot via 818FF2C8, but "
            "under this bootstrap it is skipped or fails. Store 81904480 (the XUI default loader 8178DBD8 falls back to, minus the dash thread check) into "
            "the live context's slot once, before the paint.",
            "GuideResearch");
DEFINE_bool(guide_patch_text_cull, false,
            "Phase 1028: XUIFONT::DrawText (81915410) rejects every glyph at "
            "its two vertical clip compares (81915C88 blt, 81915CA4 bgt) "
            "although the element rectangle is a sane (0,0,190,30). Nop both "
            "branches so the glyph quads are emitted regardless, to learn "
            "whether the rest of the text path works and where the glyphs "
            "land. A probe, not a fix.",
            "GuideResearch");
DEFINE_bool(guide_init_hud_dc, false,
            "Phase 1027: hud's device context (the first DC registered, so also "
            "the texture device at [81D6C980]) is constructed but never "
            "initialised - 818FDE98 never runs on it - so [dc+0x1C8] is null "
            "and 81901E40, the locator texture loader, returns E_FAIL for "
            "every image. Fonts survive because 81902140 has a fallback. Once "
            "per session, before the paint, run 818FF428(dc) on it, which is "
            "the initialiser with the live XUI context as parameter.",
            "GuideResearch");
DEFINE_bool(guide_publish_whole_paint, false,
            "Publish the paint's output from its first word to its last, "
            "instead of scanning for a packet-looking word. Phase 1016: the "
            "walker parses the whole range from the reserve cursor cleanly, "
            "while the scan picks a data word that reads as a 16257-word NOP "
            "and desyncs everything after it. Also lifts the 40960-word "
            "look-back cap that dropped the shader-load head once the "
            "dispatch made the paint bigger.",
            "GuideResearch");
DEFINE_bool(guide_paint_dispatch, false,
            "Phase 1014: enable the per-node handler-chain dispatch of the "
            "render message (id from guide_paint_msg_id; hud's own render "
            "message carries id 0).",
            "GuideResearch");
DEFINE_uint32(guide_paint_msg_pre, 0,
              "Phase 1015: if non-zero, dispatch this message id to each node "
              "immediately before guide_paint_msg_id (e.g. 0x28, the arm that "
              "resolves an image's resource handle, before the render id).",
              "GuideResearch");
DEFINE_uint32(guide_paint_msg_id, 0,
              "Phase 1014: after the visual paint of each node, send the paint's "
              "render message with THIS id through xam's handler-chain "
              "dispatcher (81949F50: global hook, base handler, own handler) "
              "to every visited node, so class handlers such as XuiImage and "
              "XuiText see it. XuiSendMessage refuses id 0xA (199) and the "
              "direct element paint only draws visuals, which is why text "
              "and images have never been emitted. 0 = off.",
              "GuideResearch");
DEFINE_bool(guide_publish_geometry, false,
            "Publish from the geometry section rather than the resolve "
            "section. Phase 889: the tail holds 125 kCopy draws (resolves); "
            "below it sit 416 DRAW_INDX in kColorDepth, which is the actual "
            "geometry.",
            "Guide");
DEFINE_bool(guide_tail_from_start, false,
            "Begin the published tail at the start of the paint's whole "
            "extent rather than 8192 words before the cursor. Phase 885: the "
            "Guide's own resolves fail because vf0 holds no vertex fetch, and "
            "vf0 is written by the CPU - so the setup may sit earlier in the "
            "stream than the tail window begins.",
            "Guide");
DEFINE_bool(guide_overlay_search, true,
            "Run the search for the tail's first type-3 header. Phase 882: "
            "with this off, start stays 0, which also skips the truncation "
            "loop and the publish - leaving the EmitBlocks half as arithmetic "
            "and one log line.",
            "Guide");
DEFINE_bool(guide_overlay_walk, true,
            "Run the PM4 walk and hex dumps inside the guide_overlay_at_swap "
            "block. Phase 878: half of the bisect for a read-only body that "
            "blanks the frame.",
            "Guide");
DEFINE_bool(guide_overlay_blocks, true,
            "Run the EmitBlocks/tail half of the guide_overlay_at_swap block. "
            "The other half of the phase 878 bisect.",
            "Guide");
DEFINE_bool(guide_overlay_dry, false,
            "Skip the entire body of the guide_overlay_at_swap block while "
            "leaving the flag itself on. Phase 877: the flag blanks the frame "
            "even with every publisher gated and nothing executed, which the "
            "block's contents cannot explain. This separates the block from "
            "the flag.",
            "Guide");
DEFINE_uint32(guide_tail_packets, 0,
              "Execute only the first N PM4 packets of the paint's draw tail; "
              "0 runs all of it. Phase 876: the tail blanks the display "
              "wherever it is executed, so the packet that does it is found by "
              "stepping N up until the frame goes black.",
              "Guide");
DEFINE_uint32(guide_paint_clear, 0xFF000000,
              "Clear colour the synthetic paint passes to RenderBegin. Phase "
              "874: the paint has always passed 0xFF000000 - opaque black - "
              "and the title's frame is black whenever the paint runs. An "
              "overlay wants a transparent clear, or none at all.",
              "Guide");
DEFINE_bool(guide_paint_present, true,
            "Call Present at the end of the synthetic paint. Phase 874: the "
            "paint presents the Guide's device in the middle of the title's "
            "own frame, which is a second way to lose it. Off leaves the "
            "painted surface for the compositor instead.",
            "Guide");
DEFINE_bool(guide_paint_frame, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_probe_class, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_root_to_visual, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_int32(guide_scene_bounds_h, 0,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_int32(guide_scene_bounds_w, 0,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_int32(guide_stall_watchdog, 0,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_trace_assert_src, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_trace_draw, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_widen_at_draw, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_clear_cmd_overflow, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_string(guide_dump_title_path, "",
              "Phase 1096gw: after the title loads, write its image to this "
              "path as a flat VA-indexed .bin, the same shape "
              "guide_dump_xam_path produces for xam. Off unless set. Needed "
              "because dash.xex's own retry loops cannot be disassembled "
              "without an image, and dumping six words at a time through the "
              "xam probe list costs a rebuild per address.",
              "Kernel");
DEFINE_string(guide_dump_xam_path, "",
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_inject_shader, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_render_thread, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_string(guide_scene_name, "",
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(guide_supply_ring_base, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(lle_xam_find_heaps, false,
             "Guide research flag. Definition recovered 2026-08-29 after a refactor script destroyed kernel_flags; default verified against the run-log config dump.",
             "Guide");
DEFINE_bool(lle_xam_device_init, false,
             "Guide research flag. Definition recovered 2026-08-29; the declaration survived but the definition did not.",
             "Guide");
DEFINE_bool(guide_bind_depth_scan, false,
             "Guide research flag. Definition recovered 2026-08-29; the declaration survived but the definition did not.",
             "Guide");
DEFINE_bool(guide_fix_draw_device, false,
             "Guide research flag. Definition recovered 2026-08-29; the declaration survived but the definition did not.",
             "Guide");
DEFINE_bool(guide_make_surface, false,
             "Guide research flag. Definition recovered 2026-08-29; the declaration survived but the definition did not.",
             "Guide");
DEFINE_bool(guide_fix_hud_dc, false,
             "Guide research flag. Definition recovered 2026-08-29; the declaration survived but the definition did not.",
             "Guide");
DEFINE_bool(guide_trace_cursor, false,
             "Guide research flag. Definition recovered 2026-08-29; the declaration survived but the definition did not.",
             "Guide");
DEFINE_bool(guide_patch_cursor_writers, false,
             "Guide research flag. Definition recovered 2026-08-29; the declaration survived but the definition did not.",
             "Guide");

// Phase 502: force 819F4C00's slot-vs-default compare to always skip, so the
// helper never calls SetRenderTarget(dev, idx, NULL). It is the only remaining
// code path in the image that can zero an RT slot on this device; see the
// comment at the patch site for the enumeration that establishes that.
DEFINE_bool(guide_patch_rt_unbind, false,
            "Patch 819F4C34 to an unconditional branch so the Guide's own "
            "render target is not unbound between the paint and the read at "
            "819F5F60.",
            "Kernel");

// Phase 1047: see GuideSkipHead in xboxkrnl_video.cc.
DEFINE_bool(guide_publish_skip_head, false,
            "After guide_publish_whole_paint, advance the published start to "
            "the first word from which the packets parse cleanly to the end "
            "of the paint (text paints prefix the block with non-PM4 records).",
            "Kernel");

// Phase 1048: see PaintHidden in xboxkrnl_video.cc.
DEFINE_int32(guide_paint_tab_index, -1,
             "In the per-node paint dispatch, paint only this child (page) of "
             "each XuiTabScene, in first-seen order; -1 paints all pages.",
             "Kernel");

// Phase 1050: see GuideTimers in xboxkrnl_video.cc.
DEFINE_bool(guide_run_timers, false,
            "Call XuiTimersRun (ordinal 0x365) once per paint so XUI "
            "transitions and timelines advance.",
            "Kernel");

DEFINE_bool(guide_trace_tabcmp, false,
            "Breakpoint at 913E9250: log the current-tab / loading-tab "
            "comparison in hud's tab loader.",
            "Kernel");

DEFINE_int32(guide_goto_tab, -1,
             "After navigation, find the XuiTabScene under the draw root and "
             "call XuiTabSceneGoto(index) once; -1 off.",
             "Kernel");

DEFINE_bool(guide_paint_honor_visibility, false,
            "In the paint walk, skip (with subtree) elements whose flag word "
            "bit 0 is clear or whose opacity is 0.",
            "Kernel");

DEFINE_int32(guide_send_notify, -1,
             "After guide_goto_tab, send message 0x1D with this payload type "
             "to the navigated scene once; -1 off.",
             "Kernel");

DEFINE_int32(guide_send_notify_id, 0x1D,
             "Message id used by guide_send_notify.",
             "Kernel");

// Phase 1052: hud shows its tab strip and frame from scene-transition
// notifications (XM_NOTIFY 0x1D types 1-4 raised inside
// XuiScenePlayTo/FromTransition). The open path never plays a transition on
// the created scene, so play one from the host after navigation.
DEFINE_int32(guide_play_transition, -1,
             "After guide_goto_tab, call XuiScenePlayToTransition once on: "
             "1 = the tab scene's parent (HUDScene), 2 = the draw root, "
             "3 = both; -1 off.",
             "Kernel");

// Phase 1052: hud's own render loop calls XuiAnimRun every frame; that is
// what advances timelines and scene transitions (XuiTimersRun only services
// XuiSetTimer timers). The harness never called it, so nothing hud or xam
// started ever played.
DEFINE_bool(guide_anim_run, false,
            "Call XuiAnimRun(dt) once per paint after XuiTimersRun, with the "
            "real elapsed seconds; dump the tree again 150 paints later.",
            "Kernel");

// Phase 1052b: navigation. hud's sysapp tick reads XamInputGetKeystrokeHud and
// hands the keystroke to XuiProcessInput; the harness never runs the tick, and
// LLE xam's HUD input queue is not fed. Poll xenia's own input system in the
// paint hook and call XuiProcessInput directly.
DEFINE_bool(guide_input, false,
            "Each paint, poll the host input system for a keystroke (user 0, then "
            "any user) and pass it to XuiProcessInput.",
            "Kernel");
DEFINE_bool(guide_focus_nudge, true, "Phase 1053: after a keystroke, nudge the focused element (opacity) so it re-renders."
            "any user) and pass it to XuiProcessInput.",
            "Kernel");
DEFINE_bool(guide_toggle, true, "Phase 1053: Guide button hides/shows the overlay; hud close requests hide it."
            "any user) and pass it to XuiProcessInput.",
            "Kernel");
DEFINE_int32(guide_open_burst, 2,
             "Phase 1054 open: plan handles rendered per swap for "
             "guide_open_burst_ms after a show, while hud's authored entrance "
             "plays, so the published frame keeps up with the blade fan-out: "
             "2 = xam's root and hud's canvas on one swap, the page and list "
             "on the next (30 frames/s); 4 = everything every swap (60, at "
             "~45 ms a swap); 1 = the normal four-swap round.",
             "Kernel");
DEFINE_int32(guide_open_burst_ms, 700,
             "Phase 1054 open: length of the faster-round window after a "
             "show, in ms (hud's %uClose entrance is 36 frames, 600 ms).",
             "Kernel");
DEFINE_bool(guide_tab_nav, true,
            "Phase 1054 tabs: dpad left/right (and the shoulder keys) switch "
            "the Guide's tab with XUI's authored slide (XuiTabScene's "
            "NavTabForward/Backward play the 12-frame %uTo%u range). false: "
            "the keys go to XuiProcessInput unchanged (focus only).",
            "Kernel");
DEFINE_int32(guide_tab_nav_mode, 3,
             "Phase 1054 tabs: how a dpad left/right switches the tab: 3 = "
             "call XuiTabScene's NavTabForward/Backward (8195CAE8/8195CC10) "
             "on the Tabscene object - the authored 12-frame %uTo%u slide, "
             "no key to the focused list; 1 = fed to XuiProcessInput as the "
             "shoulder key (0x5805/0x5804) the console's bumpers send, so the "
             "Tabscene's key handler runs the same (the focused list shows its "
             "bumper indicator first); 2 = XuiTabSceneGoto (a seek to "
             "%uTo%uEnd: snaps). Real shoulder keys always go to XUI.",
             "Kernel");
DEFINE_bool(guide_hot_compose, true,
            "Phase 1054 tabs: while an authored animation plays (the open "
            "entrance, a tab slide: the guide_open_burst_ms / guide_tab_burst_ms "
            "windows) each swap renders only xam's root and hud's canvas - the "
            "parts that move - and the publish composes the page scenes from "
            "their last render, so a frame costs ~30-45 ms instead of a whole "
            "round. false: the spread round with guide_open_burst entries a swap.",
            "Kernel");
DEFINE_int32(guide_paint_sampler_ms, 0,
             "Phase 1054 fps: >0 = a host thread samples the Guide paint thread's "
             "instruction pointer every N ms while a paint runs and logs the top "
             "guest functions every 5 s (GuideSampler). Diagnostic; 0 = off.",
             "Kernel");
DEFINE_int32(guide_paint_sampler_top, 24, "Phase 1054 fps: functions per GuideSampler report.", "Kernel");
DEFINE_bool(guide_render_pages, true,
            "Phase 1054 dup: render the tab pages and their nested scenes as "
            "separate plan handles. They must be: the canvas's own recursion "
            "draws them too, but a later sibling covers that copy (run dup_b: "
            "the page area stays flat grey with the canvas alone).",
            "Kernel");
DEFINE_bool(guide_hide_pages_in_canvas, true,
            "Phase 1054 dup: hide the plan's pages while hud's canvas renders "
            "and restore them right after, so the canvas stream no longer "
            "carries a covered copy of every page (~3800 of 8200 words on "
            "Home); the pages' own renders show them.",
            "Kernel");
DEFINE_int32(guide_walk_window_every, 4,
             "Phase 1054 walk: inside a transition window the paint plan is "
             "rediscovered (the full tree walk) on the window's first paint and "
             "then every N paints; 0 = first paint only.",
             "Kernel");
DEFINE_bool(guide_input_any_user, false,
            "Phase 1054 walk: after user 0 reports no keystroke, also ask every "
            "user (four more driver calls a paint). Off: the Guide takes user "
            "0's keystrokes only.",
            "Kernel");
DEFINE_bool(guide_page_cache, false,
            "Phase 1054 alloc: freed single 4 KB read-write physical pages wait "
            "in a small cache and single-page MmAllocatePhysicalMemory(Ex) "
            "requests with the same protection take them back without the "
            "heap or a host commit (xam's debug D3D allocates and frees such "
            "pages 120-290 times a second). HOST-SIDE workaround, off by "
            "default since phase 1099z153: cached pages a game freed stay "
            "allocated after it exits and fragment physical memory, so the "
            "next game's large contiguous allocation fails (Sonic Generations "
            "-> SASRT: MmAllocatePhysicalMemoryEx(256 MB) failed -> hang).",
            "Kernel");
DEFINE_bool(guide_alloc_tally, false,
            "Phase 1054 dbg: tally MmAllocatePhysicalMemory(Ex) and "
            "MmQueryAddressProtect calls by guest return address and size, "
            "reported every 5 s (GuideAllocTally). Diagnostic.",
            "Kernel");
DEFINE_bool(guide_paint_thread, true,
            "Phase 1054 fps: the Guide's swap-time work (input, animation, "
            "layout, renders, publish) runs on its own guest thread (\"Guide "
            "Paint\"), once per burst of swaps, so the title's swap never waits "
            "for a paint; false = inline in VdSwap on the title thread as before.",
            "Kernel");
DEFINE_int32(guide_dispatch_guard, 3,
             "Phase 1056: how XObject::GetNativeObject decides a guest "
             "dispatch header is safe to read. 0 = the page's protection bits "
             "must not be kNoAccess (the phase-516 stopgap: it refuses xam's "
             "own task-pool semaphores, so xam's workers never wait); 1 = the "
             "heap has the page allocated, whatever its protection bits say "
             "(no use for an LLE module: xam's image is not in the page table "
             "at all); 2 = the host page is mapped (xe::memory::QueryProtect, "
             "cached per 64 KB) - the condition whose absence faulted; 3 = 2, "
             "plus: accept a page the heap's own page table reports committed "
             "and readable. Phase 1095: QueryProtect answers no for the worker "
             "KTHREADs xam's task pool creates, whose page entries read "
             "state 3 alloc 3 cur 3, so KeResumeThread refused them and every "
             "pool worker stayed suspended for the whole run.",
             "Kernel");
DEFINE_bool(guide_log_pool_sync, false,
            "Phase 1056: log every signal (KeSetEvent, KeReleaseSemaphore, "
            "KePulseEvent) and every wait on an object inside xam's task pool "
            "(81D42400..81D42540), with the guest caller, to see whether a "
            "scheduled task ever wakes a worker.",
            "Kernel");
DEFINE_bool(guide_xam_app_task, true,
            "Phase 1056: create the app manager's own task at [81D43C50+4] "
            "with xam's task allocator when xam's boot has not (it never runs "
            "here). XamAppLoad stores the load callback through that slot, so "
            "a null there faults the caller.",
            "Kernel");
DEFINE_bool(guide_xam_task_init, false,
            "Phase 1056/1095: call xam's task-pool initialiser (8177BF80) "
            "from the Guide bootstrap. OFF, and it must stay off. 1056 added "
            "it because the pool at 81D423C0 looked uninitialised, but that "
            "was guide_dispatch_guard refusing xam's own dispatch objects, "
            "not a missing init: xam's boot runs 8177BF80 itself and, with "
            "the guard fixed, leaves 8 workers at 8177AD80 all RUNNING. "
            "Calling it again is a SECOND init that re-runs KeTlsAlloc and "
            "moves xam's per-thread index at [81D227F0] from 0 to 2, so "
            "every worker's KeTlsGetValue returns a block it never set and "
            "81779604 faults on a null base (phase 1095, p1095tls).",
            "Kernel");
DEFINE_bool(guide_system_process, true,
            "Phase 1055 menus: the Guide's host threads (\"Guide Paint\", "
            "\"Guide Loader\") are created in the system process, so "
            "KeGetCurrentProcessType reports 2 to xam as it does for a real "
            "system app. XamShowMessageBox (hud's exit confirmation, and "
            "every other UI request a press makes) returns 80004005 to a "
            "title-process caller before doing anything.",
            "Kernel");
DEFINE_bool(guide_paint_ui_thread, true,
            "Phase 1055 menus: before every paint the paint thread writes its "
            "own KTHREAD into xam's UI-thread slot when it is not there "
            "already. xam's UI requests (XamShowMessageBox and the rest) "
            "assert that the caller is that thread.",
            "Kernel");
DEFINE_bool(guide_trace_xam_imports, false,
            "Phase 1055 menus: log hud's calls into xam's UI/app/notify "
            "exports (a host trampoline in each import's IAT entry; up to 24 "
            "calls a name with r3..r8, the caller and the result).",
            "Kernel");
DEFINE_bool(guide_round_all, true,
            "Phase 1055 bugs: a spread round renders every plan handle on one "
            "paint, so a frame is published every paint. false = one handle "
            "per paint as before: a frame every 4 paints on Home and every 6 "
            "on Media (10 Hz at rest), and a close key's content cut waited a "
            "round. A whole round costs ~1.5 ms now.",
            "Kernel");
DEFINE_bool(guide_publish_whole_packet, true,
            "Phase 1054 marker: a published frame ends at the end of its last "
            "packet rather than at XUI's cursor, which stops one word short of "
            "it (the frame's final draw - the Home list's Open Tray disc icon - "
            "was dropped from every spread round).",
            "Kernel");
DEFINE_bool(guide_hot_raw_when_full, false,
            "Phase 1054 marker: a hot paint that rendered every plan handle "
            "publishes its own stream unchanged (the way a spread round does) "
            "instead of composing its segments.",
            "Kernel");
DEFINE_string(guide_seg_dump_tex, "",
              "Phase 1054 marker: hex virtual address of a texture whose memory "
              "(guide_seg_dump_tex_n bytes) is logged with every guide_seg_dump "
              "dump (GuideTexDump).",
              "Kernel");
DEFINE_int32(guide_seg_dump_tex_n, 4096, "Phase 1054 marker: bytes of guide_seg_dump_tex to log.", "Kernel");
DEFINE_string(guide_seg_dump, "",
              "Phase 1054 marker: comma-separated 16-bit hex element handles (0001xxxx) whose "
              "stored segment (packets) and the arena range its render "
              "allocated (vertex data) are logged as hex (GuideSegDump, "
              "GuideRingDump), once from hot paint #4 and once from the first "
              "spread paint after a hot window; \"\" = off.",
              "Kernel");
DEFINE_int32(guide_seg_dump_n, 4, "Phase 1054 marker: at most this many guide_seg_dump dumps per run.", "Kernel");
DEFINE_string(guide_track_handles, "",
              "Phase 1054 marker: comma-separated 16-bit hex element handles "
              "(0001xxxx) whose position, size, scale, pivot and opacity are "
              "logged on every tracked paint (GuideElemTrack; needs "
              "guide_track_log), through a transition and 800 ms after the "
              "tab-settle window.",
              "Kernel");
DEFINE_string(guide_script, "",
              "Phase 1054 tabs: an in-emulator test plan, run after the first "
              "show with keystrokes injected into the Guide's own input poll and "
              "frames captured from the presenter (no window focus, no "
              "keyboard). Comma-separated steps: wait:<ms> | key:<name>[:<hold "
              "ms>] | shot:<name> | film:<name>:<count>:<interval ms>; keys up "
              "down left right a b x y guide start back lb rb. Captures are raw "
              "R8G8B8X8 dumps <guide_script_tag>_<name>[_NN_<ms>ms].raw next to "
              "the executable (research/rawtopng.py converts them).",
              "Kernel");
DEFINE_string(guide_script_tag, "run",
              "Phase 1054 tabs: file prefix of guide_script captures.", "Kernel");
DEFINE_bool(guide_hot_root_compose, true,
            "Phase 1054 slide: during a tab slide compose xam's root scene "
            "(frame, legend, clock: static then) from its last render instead "
            "of re-rendering it every hot paint (10-15 ms); the open keeps it "
            "hot since its timeline moves the frame and the legend/clock.",
            "Kernel");
DEFINE_int32(guide_hot_page_settle_ms, 350,
             "Phase 1054 slide: a page scene that entered the plan keeps "
             "rendering while the animation clock since then is below this "
             "(the 12-frame slide plus hud's list updates: its \"Open Tray\" "
             "icon, the page crossfade), and is composed from its last render "
             "afterwards.",
             "Kernel");
DEFINE_int32(guide_hot_page_cheap_ms, 25,
             "Phase 1054 slide: while the transition plays, the plan's page "
             "scenes render on every hot paint when their last renders summed "
             "to at most this many ms (Home: ~17), else on every "
             "guide_hot_page_every-th (Media's four scenes: ~60).",
             "Kernel");
DEFINE_int32(guide_hot_page_every, 2,
             "Phase 1054 slide: inside a hot window the layout pass runs on "
             "every N-th hot paint and the pages (while still rendering, "
             "above) on the paints in between - hud's list rendered in the "
             "paint of its own layout shows unmeasured items (its scroll knob "
             "at \"Open Tray\"). 1 = layout and pages every paint.",
             "Kernel");
DEFINE_int32(guide_hot_dcc_every, 0,
             "Phase 1054 slide: inside a hot window (open entrance, tab slide) "
             "XuiRenderDCDeviceChanged - which makes every visual re-emit its "
             "geometry - runs at the window's first paint only; N > 0 also "
             "runs it every N-th hot paint (safety against stale geometry).",
             "Kernel");
DEFINE_int32(guide_anim_max_step_ms, 33,
             "Phase 1054 slide: the XUI animation clock advances at most this "
             "many ms per paint while the Guide is up (the authored keyframes "
             "are unchanged; a paint slower than this stretches the animation "
             "instead of skipping its frames). 0 = real time, clamped at 100.",
             "Kernel");
DEFINE_int32(guide_tab_burst_ms, 450,
             "Phase 1054 tabs: faster-round window (guide_open_burst entries "
             "per swap) after a tab switch, ms; the slide is 12 frames.",
             "Kernel");
DEFINE_bool(guide_paint_root_only, false, "Phase 1054: render only the roots (hud's canvas, xam's HUD root scene) and let XUI recurse, as hud's own frame does; the walk re-rendered every element a parent had already drawn.", "Kernel");
DEFINE_uint32(guide_dump_vb, 0, "Phase 1054: at paint 20, dump the DRAW_INDX packets and vertex data of the element with this handle (e.g. 0x1027D, the Sign In row).", "Kernel");
DEFINE_bool(guide_walk_verbose, false, "Phase 1054: the per-element visual/handle-table/type-chain diagnostics of the paint walk (eight log lines per element without a visual, every paint) and the per-paint stage marks; off, the Guide paints at the title's frame rate.", "Kernel");
DEFINE_bool(guide_paint_novisual, false, "Phase 1054: send the render message to elements that have no visual too (XuiText children of a scene, e.g. the blade labels); the walk used to skip them, and only elements inside a visual were ever drawn.", "Kernel");
DEFINE_int32(guide_close_ms, 350,
             "Phase 1091c: BACKSTOP ONLY. On the Guide press the panel keeps "
             "painting until XAM clears its own transition flag "
             "([CHUDBkgndScene+0x14], set by PlayTransition at 8174E3A0), which "
             "is the guest's own answer to how long the close takes - measured "
             "at about 240 ms in reg1091b, where this literal kept painting for "
             "seven more paints. This value is only reached when xam sets that "
             "flag and never clears it, and that case is logged as a fault so it "
             "can never pass for the mechanism. 0 = never wait, hide at once.",
             "Kernel");
DEFINE_bool(guide_publish_from_root, false, "Phase 1054: publish the paint from the first painted element's reserve cursor instead of the arena head (which starts with constant data, so the packet walk resynced only by luck).", "Kernel");
DEFINE_int32(guide_font_atlas, 0, "Phase 1054: size of every XUI font glyph atlas (81913A48/58/64 pick 128/256/512 by point size). The harness replays one paint's stream later, so glyph cells recycled within a paint show as missing or wrong letters; 512 holds a whole page. 0 = xam's own sizes.", "Kernel");
DEFINE_bool(guide_paint_hud_root, false, "Phase 1054: paint xam's HUD root scene (hudbkgnd.xur under the boot canvas: backdrop, legend, gamertag, clock) before hud's canvas.", "Kernel");
DEFINE_int32(guide_bkgnd_state, -1,
             "Which state the Guide opens to, through xam's own named "
             "transitions on CHUDBkgndScene. -1 = do not play at all - and "
             "since 1091 that is ALL it means: the Guide is still shown, so "
             "-1 is the control run for \"what does the guest's own state "
             "produce when the host plays nothing\". Before 1091 it also "
             "suppressed the first show, which made that run impossible. "
             "-2 (what the harness passes) = XAM'S OWN state, latched out "
             "of the singleton's [+0x10] before anything of ours writes it; "
             "phase 1089 measured that word as 1 (Half) on a run where "
             "nothing of ours played a transition, and if it cannot be read "
             "nothing is played rather than a number of ours being "
             "substituted. 0..4 = an explicit override (Closed, Half, Full, "
             "Error, NuiFull) FOR EXPERIMENTS ONLY - it was a hardcoded 2 "
             "(Full) until 1089, and at Full xam's Blade_Center grows to "
             "595x375 at (126,48) while hud's own Blade_Center 00010150 "
             "stays at its authored 386x235 at (231,122), so two nested "
             "panels draw and the clock and legend leave the frame.",
             "Kernel");
DEFINE_int32(guide_tab_play, 0, "Phase 1054: blade experiment at paint 45: 1 = play 1To2..1To2End on the tab scene, 2 = evaluate 1To2End only, 3 = XuiTabSceneGoto(0) then Goto(1, animate) at paint 75.", "Kernel");
DEFINE_int32(guide_open_anim, 2,
             "Phase 1053/1054: on show, play hud's authored entrance on the HUD "
             "scene and the tab scene (as hud's 913E8A48 does) and let "
             "XuiAnimRun advance it. 2 = the current tab's %uClose..%uCloseEnd "
             "(hud's answer to its to-transition-end notification on the "
             "console: the centre panel fades in, then the blades fan out in a "
             "staggered cascade, 36 frames); 1 = %uOpen..%uOpenEnd (the "
             "drill-in; it hides the strip); 0 = off.",
             "Kernel");
DEFINE_int32(guide_redraw_mode, 1, "Phase 1053: force XUI to re-record the whole frame. 1 = XuiRenderDCDeviceChanged every paint, 2 = only when the Guide wants a redraw (focus change, show), 0 = off."
             "opacity in percent.",
             "Kernel");
DEFINE_bool(guide_paint_idle, true,
            "Phase 1054 fps: skip the paint (pre-dirty walk, device-changed, "
            "layout, render walk) while nothing changed: no keystroke within "
            "guide_active_ms, no toggle or timer pending, and a full paint "
            "within guide_keepalive_ms. The GPU thread keeps replaying the last "
            "published paint.",
            "Kernel");
DEFINE_int32(guide_active_ms, 900,
             "Phase 1054 fps: keep painting this long after a keystroke reaches "
             "hud (its own animations run inside this window).",
             "Kernel");
DEFINE_int32(guide_keepalive_ms, 4000,
             "Phase 1054 fps: paint at least this often while the Guide is "
             "shown and idle (the clock text).",
             "Kernel");
DEFINE_int32(guide_composite_draw, 0,
             "Phase 1054 fps: call hud's render entry from the swap hook: 0 = "
             "never, 1 = only on swaps that paint, 2 = every swap (19 ms each; "
             "its output is not used since the paint walk publishes its own).",
             "Kernel");
DEFINE_string(guide_hide_handles, "",
              "Phase 1063 probe: comma-separated 16-bit hex element handles "
              "(e.g. 0150,0399) whose visible bit is cleared on every active "
              "paint. Answers \"what covers the descent's copy of the tab "
              "pages\" by removing one candidate at a time. Diagnostic only.",
              "Kernel");
DEFINE_bool(guide_system_app_fallback, false,
            "Phase 1085: when a BARE module name is not on the title's own "
            "device, retry it on SYS: (guide_system_root). xam asks for its "
            "system apps that way - createprofile.xex - and they live on the "
            "console's system partition, which SYS: stands in for. OFF by "
            "default because it also satisfies the title's own xbdm.xex probe, "
            "which dashroot happens to contain, and that would change what the "
            "title sees on every run.",
            "Kernel");
DEFINE_bool(guide_skin_visuals_only, false,
            "Phase 1091r: register the HUD skin's visuals WITHOUT driving "
            "xam's orphan skin loader 81795548 to its tail. The loader's "
            "useful half is XexLoadImage(the huduiskin path string)  "
            "-> 8178E340(handle, L\"skin\", L\"skin.xur\", &uri, 0x80) -> "
            "8193D4B8(uri, 0) = XuiVisualRegister; only its TAIL reads "
            "[81D43C50+0x28] and needs the fabricated manager that phases "
            "1091e-1091k mistook for guest state. Every value here is xam's "
            "own - its path string 816080F4, its flags, its format strings "
            "81608850/81608B24, its version call 817AB9E8 - so nothing is "
            "authored and no stand-in is needed. Run it INSTEAD OF "
            "lle_xam_skin_init, not alongside.",
            "Kernel");
DEFINE_bool(guide_hud_app_init_tls, false,
            "Phase 1095e: before driving the gated block, install xam's "
            "OWN per-thread block on the calling thread with xam's own "
            "installer 81778D38, and clear it afterwards with 81778DA8. "
            "81795970's failure string calls it \"initialize UI Thread\", "
            "and xam gates on thread ownership elsewhere: 8177B0A8 bails "
            "at `bl 81778CD8; beq` - the per-thread block getter - which "
            "is why 8177BFC8 returns 0 for any thread that did not enter "
            "through the worker entry 8177AD80 (phase 1095b). The Guide-"
            "open thread is a host thread and has no block, so xam sees it "
            "as a thread it does not own.",
            "Kernel");
DEFINE_int32(guide_hud_app_init_wait_ms, 2000,
             "Phase 1095f: after driving the gated block, wait for xam's "
             "skin loader 81795548 to finish before the Guide bootstrap "
             "continues. 8177BF38 QUEUES the loader (it tail-calls "
             "81779920), so it runs on a pool worker while our thread "
             "carries on into device creation, XUI init and scene "
             "navigation - both touching XUI state. The signal is the "
             "guest's own: 81795948 writes [81D43C50+0xB4] = 1 at the "
             "loader's tail, right after it registers 81794BC8. A "
             "timeout here is a FAULT, not a fallback - it means the "
             "loader never completed and the log says so.",
             "Kernel");
DEFINE_string(guide_flash_root, "",
              "Phase 1095z: host directory to mount as the Flash device. "
              "xam resolves real flash paths - the run log shows "
              "ResolvePath of Device/Flash/xstudio.xex and a Guide item "
              "failing ResolvePath(createprofile.xex) since 1091w - and "
              "there has never been a Flash device to resolve them "
              "against. Point this at the files extracted by "
              "guide_extract_update (the $flash_ prefix stripped). This "
              "supplies the guest's OWN modules at the path it asks for; "
              "it does not author any value.",
              "Kernel");
DEFINE_string(guide_extract_update, "",
              "Phase 1095y: with guide_mount_update, also EXTRACT every "
              "mounted package's files into this host directory, using "
              "Xenia's own container reader. su20076000_00000000 holds "
              "the real flash modules - $flash_deviceselector.xex, "
              "$flash_createprofile.xex, $flash_dash.xex, $flash_hud.xex, "
              "$flash_huduiskin.xex, $flash_xam.xex - which is where the "
              "caller of xam ordinal 2798 (RegisterDevice) should be, and "
              "createprofile.xex is the module a Guide item has been "
              "failing to ResolvePath since 1091w.",
              "Kernel");
DEFINE_string(guide_mount_update, "",
              "Phase 1095x: a directory of Xbox 360 system-update "
              "packages (PIRS/CON/LIVE). Each is mounted with Xenia's "
              "own XContentContainerDevice and its contents listed, so "
              "the emulator extracts the update instead of a hand-written "
              "container parser. Needed because nothing in this "
              "environment imports xam ordinal 2798 (RegisterDevice via "
              "817BFB60 -> 81731818), so no kind-3 device is ever "
              "registered and 817319F4's use-path faults at 817286C0.",
              "Kernel");
DEFINE_bool(guide_host_show, true,
            "Phase 1095az: the harness originates the Guide's FIRST SHOW - it "
            "arms g_guide_bkgnd_open_at 300 ms out and then raises "
            "g_guide_button_edge itself. 1091 justified that with \"xam's "
            "own HUD-manager loop 81794BC8, which is what calls ShowHud "
            "8174FDA0, does not run here\". Since 1095b/c that premise is "
            "testable: 81794BC8 is registered as a callback on the "
            "skin-callback manager, and with guide_hud_app_init the manager "
            "is xam's own (401EA421) and the skin loader completes. Set "
            "this false to find out whether the guest shows itself. A "
            "false run that never opens the Guide is a MEASUREMENT, not a "
            "regression - it says the premise still holds.",
            "Kernel");
DEFINE_bool(guide_hud_app_init, false,
            "Phase 1095c: drive xam's OWN HUD-app initialiser 81795970 "
            "instead of handing [81D43C50+0x28] a host stand-in. 81795970 "
            "sets r31 = 81D43C50 (8179599C), creates the skin-callback "
            "manager with 8177C9E8(&[r31+0x28]), registers the skin loader "
            "81795548 as callback 5 (8177BFB0) and the HUD-manager loop "
            "81794BC8 - the thing that calls ShowHud 8174FDA0 and so is why "
            "the host has to originate the show at all - then kicks it with "
            "8177BF38. xam's boot reaches it from 81751220, but only past "
            "the device gate [[815F048C]] & 0x200 at 817511EC, and our "
            "harness creates the device long AFTER xam boots, so xam took "
            "that branch with the bit clear and skipped the init for good. "
            "Takes no arguments; returns an HRESULT.",
            "Kernel");
DEFINE_bool(guide_real_skin_mgr, false,
            "Phase 1091m: ask XAM for the skin-callback manager instead of "
            "handing its slot a host-allocated stand-in. lle_xam_skin_init "
            "used to SystemHeapAlloc 0x100 and store it at [81D43C50+0x28] "
            "so xam's skin loader 81795548 could finish; that block is what "
            "phases 1091e-1091k mistook for guest state. xam's own "
            "constructor is 8177BFC8 (no arguments, returns 8177B0A8() | 1), "
            "and the tag bit it sets is exactly what 8177BFB0 asserts on the "
            "slot. If xam returns 0 the slot is LEFT ALONE - no stand-in is "
            "substituted, because the alternative is putting our own object "
            "back (the 1089 rule).",
            "Kernel");
DEFINE_int32(guide_watch_alloc, 0,
             "Phase 1091l probe: log every NtAllocateVirtualMemory that "
             "returns exactly this address, with the guest LR of the "
             "caller. Written for 0x30052000, the single committed 4 KB "
             "page whose base ends up UNTAGGED in [81D43C50+0x28] and "
             "which is never constructed into a pool task - the one "
             "unmeasured step between xam's DllMain and the refused "
             "dispatch of the HUD show loop. 0 = off.",
             "Kernel");
DEFINE_bool(guide_dispatch_hud_task, false,
            "Phase 1091e: dispatch XAM'S OWN HUD SHOW TASK. 81794BC8 - the "
            "loop that pumps 81793230 and then calls ShowHud 8174FDA0(mgr, "
            "[81D43C50+0xBC]) until it takes - is referenced nowhere in the "
            "image and is not exported; its address is formed once, at "
            "81795924, and handed to 8177BFB0([81D43C50+0x28], 1, 81794BC8, "
            "0, 0), i.e. the pool's 8177B2C0 with flag bit 0 = '[task+0x30] "
            "= procedure'. The [+0xB4]=1 / [+0xB8]=1 stores that follow are "
            "the gates 1080 measured, so the task IS armed here and only "
            "never dispatched - the same wall, and the same shape, as the "
            "app task 1084 dispatched with xam's own 8177B490. NOT FREE: "
            "81794BC8 asserts the UI thread and loops until ShowHud takes.",
            "Kernel");
DEFINE_bool(guide_dispatch_app_task, false,
            "Phase 1084: call xam's dispatcher 8177B490 on the app manager's "
            "armed task. XamAppLoad arms it ([task+0x30] := 817935B8, the "
            "callback that loads createprofile.xex) and nothing dispatches it "
            "([task+0x10] stays 0). EXPERIMENTAL: 817935B8 loads a .xex "
            "mid-session; expect a fault before it works.",
            "Kernel");
DEFINE_bool(guide_resume_task_workers, false,
            "Phase 1071: resume xam's six task-pool worker threads. They are "
            "created with ExCreateThread flags 0x83 (bit 0 = CREATE_SUSPENDED) "
            "and the resume that follows is gated on a caller flag that is "
            "clear, so the worker procedure 8177AD80 never runs, [pool+0x154] "
            "stays at 6, and no scheduled task is ever dequeued - which is why "
            "Sign In / Download Profile / Kinect Tuner / Create Profile return "
            "0x65B. EXPERIMENTAL: starts six guest threads mid-session.",
            "Kernel");
DEFINE_bool(guide_bkgnd_watch, false,
            "Phase 1089: log the CHUDBkgndScene singleton's state fields on "
            "change - [81D3F924] +0x10 current state, +0x64 REQUESTED state, "
            "+0x70 request flag (1 = go Closed, 2 = go to +0x64). 8174EBF4 "
            "applies that pair on the UI thread and 8174EDFC takes the state "
            "from [r31+0x10] unless the callback at [r31+0x18] overwrites it, "
            "so the state is REQUESTED, never hardcoded, on the console. Says "
            "whether the guest ever asks for one here.",
            "Kernel");
DEFINE_int32(guide_dim_scan, 0,
             "Phase 1088 dim: >0 = the paint at which the first full scan of "
             "the guest address space for words holding ??0F0F0F is taken. "
             "hudbkgnd.xur's COLR table carries that one colour at five alphas "
             "(00 32 64 80 FF), keyed across Closed/Half/Full/Error - the "
             "authored full-canvas dim. Five scans are taken, "
             "guide_dim_scan_every paints apart, and the words that MOVED "
             "between them are reported with the element object that contains "
             "them: a parsed XUR table cannot move, a live animated property "
             "must. Diagnostic; 0 = off.",
             "Kernel");
DEFINE_int32(guide_dim_scan_count, 5,
             "Phase 1088 dim: how many guide_dim_scan scans to take.", "Kernel");
DEFINE_int32(guide_dim_scan_every, 30,
             "Phase 1088 dim: paints between guide_dim_scan's scans.", "Kernel");
DEFINE_bool(guide_dump_collections, false,
            "Phase 1062: at the HUD-root probe, log the raw head of the "
            "element object and of its node for xam's root scene, the "
            "AppHost element and hud's draw root, before and after the "
            "guide_host_in_apphost re-link. 8195AA00 forwards the render "
            "message by iterating a {ptr,count} pair that is NOT the "
            "navigable child tree; this locates it and reads its count.",
            "Kernel");
DEFINE_bool(guide_track_log, false,
            "Phase 1054 transitions: log xam's AppHost and Legend opacity, scale, "
            "position and pivot on every paint of a transition (GuideTrack).",
            "Kernel");
DEFINE_bool(guide_fade_hud, true,
            "Phase 1054 transitions: copy the opacity xam's timelines give the "
            "AppHost element onto hud's draw root and page/list scenes every "
            "active paint, so the panel fades with the legend and clock.",
            "Kernel");
DEFINE_bool(guide_host_in_apphost, true,
            "Link hud's draw root under xam's AppHostElementId (hudbkgnd.xur), "
            "so xam's own descent renders hud and its Closed/Half/Full "
            "timelines drive the panel with the legend and clock. ON since "
            "phase 1065: xam's root goes 2065 -> 6245 words and hud's draw root "
            "leaves the plan, i.e. xam renders the Guide rather than the "
            "harness rendering it beside xam. The tab pages are nested "
            "XuiScenes, which no parent render enters, so they stay separate "
            "plan handles and guide_fade_hud gives them - and only them - the "
            "opacity xam computed. Off: hud's draw root is a plan handle of its "
            "own and nothing xam animates reaches it.",
            "Kernel");
DEFINE_bool(guide_paint_spread, true,
            "Phase 1054 fps: keep one XUI frame open across swaps: RenderBegin "
            "and layout on the first, one plan handle per swap, RenderEnd and "
            "the publish on the last. 10-20 ms a swap during interaction "
            "instead of 45, with no cross-paint composition.",
            "Kernel");
DEFINE_bool(guide_paint_interleave, false,
            "Phase 1054 fps: with the render plan cached, render one plan "
            "handle per swap and compose the published stream from the latest "
            "segment of each handle (9-20 ms a swap instead of 45).",
            "Kernel");
DEFINE_int32(guide_force_render, 3, "Phase 1053: before each element render message OR bits into [obj+0xB4]: 1 = bit 17, 2 = bit 1, 3 = both (0 = off)."
             "opacity in percent.",
             "Kernel");

// Phase 513: stamp the paint's reserve range with a sentinel before the paint,
// to distinguish words the paint actually writes from memory it merely reserves.
DEFINE_bool(guide_paint_sentinel, false,
            "Fill the paint's output range with 0xDEADBEEF beforehand so the "
            "walk shows what the paint writes rather than what was already "
            "there.",
            "Kernel");

// Phase 517: skip the 81901EAC indirect call, which dispatches through a field
// holding locale text and crashes skin initialisation ten times per run.
DEFINE_bool(guide_button_via_automation, true,
            "Deliver the Guide button through xam's own "
            "XAutomationpInputXenonButton (ordinal 0x3D8 with the 0xFFFF "
            "sentinel) instead of the host's synthesised DrvXenonButtonPressed. "
            "The synthesised one crashes the guest at 817A6174 on CLOSE and "
            "leaves the Guide half torn down on screen.",
            "Guide");
DEFINE_bool(guide_trace_live_auth, false,
            "Hook xam's Live AUTHENTICATION exports and report every call with "
            "its caller and arguments. 17559 ADDRESSES ONLY - they are wrong on "
            "17489 (which does not even export four of them), so do not pass "
            "this on a dashroot run. Two always-called exports are hooked as "
            "POSITIVE CONTROLS; if those do not fire, the hooks never installed "
            "and a zero count for the rest means nothing.",
            "Guide");
DEFINE_bool(guide_trace_fable_views, false,
            "Phase 1099z168 RESEARCH PROBE, Fable III (4D5308D6) ONLY. Hook the "
            "title's render-view slot registry (the vector at guest 8355F450) "
            "and report (a) every slot the game allocates, (b) the title's own "
            "'Out Of Memory allocating 1440' branch at 82C4F228, and (c) every "
            "draw submission that finds a NULL slot - which is the crash at "
            "821DBAEC a couple of seconds after the first save. ADDRESSES ARE "
            "FABLE III RETAIL ONLY; the first word at each site is checked and "
            "a mismatch disarms that hook, so a different title reports zero "
            "rather than corrupting itself.",
            "Guide");
DEFINE_bool(kernel_log_alloc_failures, true,
            "Report every NtAllocateVirtualMemory / MmAllocatePhysicalMemoryEx "
            "that fails for want of address space, with the request and the "
            "guest caller. The console has no such log; this is host-side "
            "DIAGNOSTIC output only and changes no guest-visible behaviour.",
            "Kernel");
DEFINE_bool(guide_automation_input, true,
            "Route controller input into LLE xam through xam's OWN XAutomation "
            "API (ordinals 0x3D5 XAutomationpBindController and 0x3D9 "
            "XAutomationpInputSetState). Without it xam's input stack finds no "
            "USB device and answers ERROR_DEVICE_NOT_CONNECTED to every poll, "
            "so the Guide draws but cannot be navigated.",
            "Guide");
DEFINE_string(guide_input_script, "",
              "Synthetic button sequence for driving the Guide without a "
              "physical pad, as delay_ms:button pairs separated by commas, "
              "e.g. \"2000:down,600:down,600:a\". Names: up down left right "
              "a b x y start back lb rb. Each entry waits delay_ms, then holds "
              "the button for guide_input_hold_ms. Applied through the same "
              "XAutomation path as real input.",
              "Guide");
// Phase 1099q: test runs must not read the user's controller while they use it
// for something else.
DEFINE_bool(guide_xam_boot_launch, false,
            "Phase 1099z6: load the title image but let xam's launcher start "
            "it (XamLoaderLaunchTitle -> XexLoadExecutable adopts the loaded "
            "image -> XexStartExecutable), as a console boots the dashboard. "
            "Without it xam never records the title as running (launcher "
            "state 1, no title id), so dash: URIs relaunch the dashboard "
            "instead of navigating in place.",
            "Guide");
DEFINE_string(guide_xam_boot_launch_path, "\\SystemRoot\\dash.xex",
              "Guest path guide_xam_boot_launch passes to XamLoaderLaunchTitle.",
              "Guide");
DEFINE_bool(guide_title_switch_close_handles, true,
            "ExTerminateTitleProcess Ob slot: close handles created by title "
            "threads (phase 1099z47).",
            "Guide");
DEFINE_path(kernel_xinputd_record_path, "",
            "TEST TOOLING: record controller port 0 button changes seen by "
            "the kernel controller driver (kernel_xinputd) to this file in "
            "hid_test_pad_script format, for replay with --hid=nop "
            "--hid_test_pad_script=@<file> (phase 1099z156).",
            "Kernel");
DEFINE_bool(kernel_title_unload_dlls, false,
            "Title terminate also unloads DLLs a title-process thread loaded "
            "(dash's dashnui.xex), as the console does (phase 1099z161).",
            "Kernel");
DEFINE_bool(kernel_guest_mutants, false,
            "KeInitializeMutant / KeReleaseMutant for mutants the guest builds "
            "itself, after the 17489 kernel (phase 1099z161).",
            "Kernel");
DEFINE_bool(kernel_pv03_host_shim, false,
            "HOST-SIDE: answer hypervisor expansion 'PV03' commands 1 (present) "
            "and 3 (license descriptor returned unchanged) with success. The "
            "real expansion is not available; phase 1099z161.",
            "Kernel");
DEFINE_bool(kernel_xex_load_headers_fixup, false,
            "XexLoadImageHeaders stores the absolute security-info address in "
            "header+0x10, as the real kernel's 800A08C8(header, 1, 0) does "
            "(phase 1099z159). The header RSA check is not performed.",
            "Kernel");
DEFINE_path(kernel_hv_image_path, "",
            "Hypervisor image whose built-in PUBLIC RSA keys back "
            "XeKeysVerifyRSASignature (17489: research\\kernel17489\\"
            "se_17489_hv_kernel.bin). Empty = the export is not implemented "
            "(returns r3 unchanged, as the undefined extern did). Phase 1099z159.",
            "Kernel");
DEFINE_path(kernel_system_ext_path, "",
            "Host folder served read-only as \\Device\\Harddisk0\\SystemExtPartition "
            "(the system update's extended system files). Empty = not present "
            "(phase 1099z159).",
            "Kernel");
DEFINE_path(kernel_system_aux_path, "",
            "Host folder served read-only as \\Device\\Harddisk0\\SystemAuxPartition "
            "(the system update's avatar asset pack; for 17559 the importer's "
            "_packages\\FFFE07DF00000002). Empty = not present (phase 1099z159).",
            "Kernel");
DEFINE_bool(kernel_terminate_lock_check, false,
            "Title terminate: never kill a title thread while it owns the "
            "emulator's global lock (retry until it releases it). Without it, "
            "launching a game while the dash is busy can hang the switch "
            "(phase 1099z159).",
            "Kernel");
DEFINE_bool(kernel_ob_keep_guest_dispatchers, false,
            "Title terminate Ob slot: do not close Xenia's handle entries for "
            "dispatcher objects the guest built in its own memory (events, "
            "semaphores, mutants, timers); the console has no handle for "
            "them. Without it, xam's wait for a still-running title task "
            "returned at once and the task crashed in the freed title heap "
            "(Fable III fast launch, phase 1099z160).",
            "Kernel");
DEFINE_bool(kernel_svod_create_device, false,
            "SvodCreateDevice mounts a Games-on-Demand package (a disc "
            "installed to the hard drive) with Xenia's XContent reader; the "
            "device object/extension are zeroed stand-ins (phase 1099z164).",
            "Kernel");
DEFINE_uint32(kernel_game_region, 0,
              "Console game region written to 8E038602, the hypervisor's key "
              "vault copy that the kernel and retail xam's XGetGameRegion "
              "read (00FF NTSC-U, 01FE NTSC-J, 02FE PAL). 0 = leave zero; "
              "4294967295 = derive from the xconfig country like Xenia's HLE "
              "XGetGameRegion (phase 1099z164).",
              "Kernel");
DEFINE_bool(kernel_hv_console_flags, true,
            "Write the hypervisor's console flags word at 8E038614 "
            "(0x00070000, or 0x107 for game region 0102) like the 17489 "
            "hypervisor does. Every dashboard's System Settings > Initial "
            "Setup is greyed out without it.",
            "Kernel");
DEFINE_int32(kernel_hdd_size_gb, 0,
             "Size of the modelled hard drive behind --guide_hdd_path, in GB "
             "(FATX geometry; free space = the host folder's free space, "
             "capped at this size). 250 = the Xbox 360 S 250 GB drive. 0 = "
             "Xenia's fixed 64 MB volume (phase 1099z159).",
             "Kernel");
DEFINE_int32(guide_test_notify_seconds, 0,
             "DIAGNOSTIC (HOST-SIDE): N seconds after start, call real xam's "
             "XNotifyQueueUI on a guest thread to show a test notification "
             "toast. 0 = off.",
             "Guide");
DEFINE_bool(kernel_check_title_system_version, false,
            "Refuse to load a title whose xam import library needs a newer "
            "system than the running xam (C0000059), as the console's "
            "hypervisor does (17489 HV 2AC6C). OFF by default (user choice, "
            "2026-09-16): every game is loaded on every dashboard, and xam "
            "ordinals the old xam lacks bind to Xenia's HLE xam (HOST-SIDE, "
            "xex_module.cc SetupLibraryImports).",
            "Kernel");
DEFINE_string(guide_test_tray_seconds, "",
              "TEST-ONLY (HOST-SIDE, 2026-09-16): \"open,close\" seconds after "
              "start at which to move the virtual DVD tray, as the console's "
              "eject button does (SMC tray motion, no kernel call). Lets an "
              "automated run insert guide_tray_disc_path while a dashboard "
              "runs. Empty = off.",
              "Guide");
DEFINE_int32(guide_test_notify_type, 3,
             "DIAGNOSTIC: XNOTIFYQUEUEUI type for guide_test_notify_seconds "
             "(3 = generic).",
             "Guide");
DEFINE_bool(kernel_probe_on_request, false,
            "DIAGNOSTIC: poll for a file named probe_now next to the exe; when "
            "it appears, write the guest thread probe (probe.txt) and delete "
            "the file.",
            "Kernel");
DEFINE_int32(kernel_sample_from_ms, 0,
             "DIAGNOSTIC: sampling profiler for guest threads. Between these "
             "two times (ms since process start) every running guest thread "
             "is sampled each ~1 ms; at the end the log gets each thread's "
             "hottest guest functions and host routines (phase 1099z159).",
             "Kernel");
DEFINE_int32(kernel_sample_to_ms, 0,
             "DIAGNOSTIC: end of the --kernel_sample_from_ms window; 0 = off.",
             "Kernel");
DEFINE_int32(kernel_log_long_waits_ms, 0,
             "DIAGNOSTIC: log every guest wait/delay export call that blocks "
             "at least this many ms (caller lr, requested timeout, elapsed). "
             "0 = off (phase 1099z159).",
             "Kernel");
DEFINE_bool(kernel_audio_ducker, false,
            "XAudioGet/SetDucker{Level,Threshold,AttackTime,ReleaseTime,"
            "HoldTime} keep their float like the 17489 kernel's accessors "
            "(8016DA38..8016DB34). Off: getters leave f1 untouched, as when "
            "the exports were undefined (phase 1099z155).",
            "Kernel");
DEFINE_bool(guide_power_on_with_guide_button, false,
            "Start powered off: a black window with nothing loaded until the "
            "Guide button (any controller, or the keyboard's Guide binding) "
            "is pressed, which starts the cold boot - the way the console's "
            "Guide button powers it on. Xenia's launcher hotkeys (Start = run "
            "recent title, D-pad title select) are off while waiting. "
            "Host-side: the SMC power-on path itself is not modelled.",
            "Guide");
DEFINE_bool(guide_engine_notifications, false,
            "VdInitializeEngines / VdShutdownEngines send xam graphics "
            "notifications 4 / 5 as the 17489 kernel does (800F7E68). With "
            "5 xam runs its launch fade: it presents the persisted frame and "
            "ramps gamma to black over ~32 vblanks (phase 1099z138).",
            "Guide");
DEFINE_bool(guide_cold_boot, false,
            "Cold boot: before loading xam, start the boot animation the way "
            "the 17489 kernel's phase-1 init does (AniStartBootAnimation(0), "
            "boot progress 0x72, before LOAD_XAM 0x79). xam's UI thread and "
            "title start then wait for it in AniBlockOnAnimation. Needs "
            "xbox_hardware_info_flags with 0x200 and without 0x8 "
            "(phase 1099z125).",
            "Guide");
DEFINE_string(guide_cold_boot_path, "\\Device\\Flash\\bootanim.xex",
              "Boot animation module. The kernel loads only "
              "\\Device\\Flash\\bootanim.xex; without --guide_flash_root use "
              "SYS:\\bootanim.xex (the dashroot copy).",
              "Guide");
DEFINE_bool(guide_trace_signin, false,
            "Guest hooks on signin.xex's status scene: log its state changes "
            "and messages with host timestamps (phase 1099z74).",
            "Guide");
DEFINE_bool(guide_trace_dash_lua, false,
            "Guest hooks (translator-emitted host calls) on dash.xex luaD_throw (928E4838) and Lua Sleep "
            "(928F6C58): log Lua errors and wait sites (phase 1099z60).",
            "Guide");
DEFINE_int32(guide_probe_threads_at_terminate_index, 2,
             "Which ExTerminateTitleProcess arms "
             "--guide_probe_threads_at_terminate (1 = first; 0 = every one).",
             "Guide");
DEFINE_int32(guide_probe_threads_at_terminate, 0,
             "Seconds after the second ExTerminateTitleProcess to run the "
             "guest thread probe (phase 1099z52).",
             "Guide");
DEFINE_bool(guide_title_switch_release_memory, true,
            "ExTerminateTitleProcess Mm slot: release title-owned "
            "NtAllocateVirtualMemory regions (physical memory is kept; phase "
            "1099z47).",
            "Guide");
DEFINE_bool(kernel_isr_process_type, true,
            "Graphics interrupt callbacks run as the SYSTEM process while "
            "VdGlobalXamDevice is set, TITLE otherwise (17489 kernel "
            "80102E00). false = always TITLE (old Xenia behaviour).",
            "Kernel");
DEFINE_int32(guide_capture_on_launch, 0,
             "With guide_trace_transitions: at the Nth xam launch request, "
             "capture guide_capture_count frames guide_capture_interval_ms "
             "apart to launchN_MM.raw (phase 1099z170).",
             "Guide");
DEFINE_bool(guide_trace_transitions, false,
            "Diagnostic (phase 1099z170): log XUI timeline / scene transition "
            "calls in xam and the 17559 dash, xam's launch request, launcher "
            "and terminate, the dash's launch callback and display persist, "
            "and every VdSwap for 5 s after a launch request.",
            "Guide");
DEFINE_bool(guide_system_phys_bottom_up, true,
            "MmAllocatePhysicalMemoryEx: system-process allocations (xam's "
            "Guide textures) go bottom-up so they do not split the physical "
            "memory the next title allocates top-down (phase 1099z169). "
            "HOST-SIDE placement.",
            "Guide");
DEFINE_bool(guide_ob_insert_forward_waits, true,
            "ObInsertObject placeholders forward waits to the object's "
            "default dispatcher object (phase 1099z25). false = old "
            "unwaitable placeholder, for A/B tests.",
            "Guide");
DEFINE_int32(guide_watch_write_delay_seconds, 0,
             "Arm guide_watch_write_addr this many seconds after xam loads "
             "(for heap pages not committed at load). 0 = off.",
             "Guide");
DEFINE_string(guide_dump_launched_title_path, "",
              "Research probe: at XexStartExecutable, write the launched "
              "title's image as a flat VA-indexed .bin to this path.",
              "Guide");
DEFINE_path(guide_tray_disc_path, "",
            "Disc image that goes into the optical drive when the tray closes "
            "(Guide > Open Tray, then Close Tray). Mounted at "
            "\\Device\\CdRom0; opening the tray removes it.",
            "Guide");
DEFINE_bool(guide_tray_disc_at_boot, false,
            "Start with guide_tray_disc_path already in the closed tray.",
            "Guide");
// Phase 1099z165: the host-side game library.
DEFINE_path(guide_game_library_path, "",
            "Host folder of disc images (*.iso) to install into the emulated "
            "hard drive so the dashboard's My Games lists them. Installed "
            "once at startup (discs already there are skipped), and the "
            "folder the Disc > Add Game Library Folder picker starts in. "
            "HOST-SIDE: the package payload is a link to the disc image on "
            "the host, not the 6-8 GB copy a real install makes.",
            "Guide");
DEFINE_bool(guide_game_library_prompt, true,
            "At startup, when there is an emulated hard drive but no "
            "guide_game_library_path, ask for a games folder. Cleared by the "
            "prompt's \"Don't show this again\".",
            "Guide");
DEFINE_bool(guide_game_library_cover_art, true,
            "HOST-SIDE: when the game library installs a game (or finds one "
            "installed without cover art), look its cover up once in "
            "Microsoft's store catalog and save it under <hdd>/BoxArt. The "
            "store is not asked again for that title.",
            "Guide");
DEFINE_bool(guide_game_library_at_boot, true,
            "Install guide_game_library_path's discs when the emulator "
            "finishes initializing. false = only from the Disc menu.",
            "Guide");
DEFINE_int32(guide_net_probe_seconds, 0,
             "Research probe: seconds after the xam boot launch to call real "
             "xam's XNetGetEthernetLinkStatus and XNetGetTitleXnAddr and log "
             "the results. 0 = off.",
             "Guide");
DEFINE_bool(guide_dump_launcher, false,
            "Research probe: dump xam's launcher object (*81D41318) at "
            "XexLoadExecutable and 15 s after XexStartExecutable.",
            "Guide");
DEFINE_bool(guide_input_host_off, false,
            "Never read the host controller or keyboard in the input pump "
            "(scripts and fake-pad input still work). For automated runs.",
            "Guide");
// Phase 1099p: record a play session for replay.
DEFINE_path(guide_input_record_path, "",
            "Append every button press delivered to xam (pad, keyboard Guide "
            "key) to this file in guide_input_script format, with real "
            "delays. Replay with --guide_input_script=@<file>.",
            "Guide");
// Phase 1099k: verification stand-in for the controller READ only.
DEFINE_string(guide_input_fake_pad, "",
              "Test: replace the host controller read with a sequence of "
              "delay_ms:hex_buttons (X_INPUT_GAMEPAD bits, e.g. 0002 = dpad "
              "down), each held for guide_input_hold_ms. Everything after the "
              "read - press detection, InputPress, InputSetState - is the real "
              "path. Empty = read the real controller.",
              "Guide");
DEFINE_int32(guide_input_hold_ms, 120,
             "How long a scripted button is held down.", "Guide");
DEFINE_bool(guide_patch_skin_dispatch, false,
            "Patch 81901E88 to a nop so the null path is taken and the bad "
            "indirect call at 81901EAC is skipped, letting skin init continue.",
            "Kernel");

// Phase 1046: the nop at 81901E88 is what poisons every image load for the
// session (81901E40 is translated with it in place and returns E_FAIL for
// good), and the crash it was added for (phase 517) turned out not to be in
// skin init at all: the no-nop runs p1041 show the skin loader returning
// S_OK and the 81901EAC fault on the bootstrap thread, after the bootstrap's
// own render-host call replaced the live XUI context (818FF3D0 releases the
// old one) while the texture device [81D6C980] kept the DC built against it.
// That is the case guide_reuse_xui_ctx already exists for. This flag lets the
// nop be dropped while keeping the [81D6C9C8] stand-ins that share the
// guide_patch_skin_dispatch gate.
DEFINE_bool(guide_skin_dispatch_nop, true,
            "With guide_patch_skin_dispatch, also nop the branch at 81901E88. "
            "Set false to leave the DC image loader intact (needs "
            "guide_reuse_xui_ctx so the bootstrap does not replace the XUI "
            "context skin init built its DC against).",
            "Kernel");

// Phase 540: dump the presented frame to <exe>/guide_capture.raw after N
// seconds, so the rendered result can be inspected directly rather than
// inferred from checksums.
DEFINE_int32(guide_capture_seconds, 0,
             "Capture the presented frame to guide_capture.raw after N seconds.",
             "Kernel");

// Phase 546: make 819E01E0 return its input instead of the converted address,
// so the pointer the Guide hands it becomes observable in the fetch constant.
DEFINE_bool(guide_patch_addr_passthru, false,
            "Patch 819E0218 to `mr r31, r3` so the CPU-to-GPU conversion "
            "returns its argument, revealing the pointer being converted.",
            "Kernel");

// Phase 557: [navObj+76] selects the scene in hud's real dispatcher 913EC6D0 -
// 0 or 4 give GuideMain.xur, 7/3/5/6/8 give specific scenes, anything else is
// E_FAIL. It reads 1 in every run, which selects nothing. Negative leaves it
// alone and uses the hardcoded Status entry as before.
DEFINE_int32(guide_nav_state, -1,
             "Set [navObj+76] to this value and navigate via 913EC6D0 instead "
             "of the Status-hardcoded 913EB7D8.",
             "Kernel");

// Phase 559: store the resolved scene object as the draw root rather than its
// handle. 818FB110 dereferences its argument as an object and never resolves.
DEFINE_bool(guide_draw_root_object, false,
            "Store the resolved scene object pointer at the draw root instead "
            "of the XUI handle.",
            "Kernel");

// Phase 568: 0x395 takes an object pointer, not a XUI handle - it casts its
// first argument directly. Resolve handles before calling it from the walk.
DEFINE_bool(guide_resolve_paint_handles, false,
            "Resolve XUI handles to objects before passing them to xam calls "
            "that expect object pointers.",
            "Kernel");

// Phase 578: pass null as the dispatcher's DC argument so hud skips its release
// path and constructs its own, instead of releasing the bootstrap's DC and then
// calling through it.
DEFINE_bool(guide_draw_on_swap, false,
            "Invoke the Guide's composite draw from VdSwap instead of "
            "VdCallGraphicsNotificationRoutines. Phase 654: the latter is the "
            "documented place for it, but PvZ calls it exactly once per run - "
            "at startup, before the hook is installed - so the composite draw "
            "has never fired (phase 653). VdSwap runs every frame on the same "
            "thread at the same point in the frame.",
            "Kernel");

DEFINE_bool(guide_transplant_ring, false,
            "Copy the ring-association fields from mode 1's device onto the "
            "device the present path uses. Phase 650: the two devices are "
            "disjoint (phase 648) and the object graph cannot be moved "
            "(phases 641-649), so the remaining question is whether the ring "
            "can be moved to the graph instead. Copying [2B14] alone already "
            "advanced the paint loop by a frame (phase 640).",
            "Kernel");

DEFINE_bool(guide_patch_class_reg, false,
            "Patch 8194F164 to an unconditional branch so class registration "
            "treats an existing entry as success instead of returning "
            "80300005. Phase 649: the render host fails only because its "
            "classes are already registered (phase 606), and a fresh XUI "
            "context is the only way to root the object graph on mode 1's "
            "device (phases 644, 648).",
            "Kernel");

DEFINE_bool(guide_force_cmdbuf_alloc, false,
            "Patch the allocator guard at 81A02988 to an unconditional branch "
            "so 81A02940 allocates regardless of [dev+0x2B3D] & 0x20. Phase "
            "683 RAN this and it FAILS - do not retry expecting a different "
            "result. The allocator gets further (51/102 vs 22) but still "
            "returns NULL: [dev+0x3A54] is null, so it calls 81A02688, which "
            "opens by asserting that this very bit is clear. With "
            "ignore_trap_instructions the twui is a no-op, so the patch walks "
            "past a precondition the guest declares must hold, and the run "
            "gains a guest crash at 81A019B8. The bit is not a gate - it is a "
            "cached statement that the device has no command-buffer pool, and "
            "clearing it does not create one. Kept only so the negative "
            "result is reproducible.",
            "Kernel");

DEFINE_bool(guide_ctx2_title_device, false,
            "Point the second context's wrapper at the title's D3D device "
            "([wrap+0x0C]) at the site that actually runs - unlike "
            "guide_render_on_title_device, whose block in emulator.cc never "
            "executes here (GuideTitleDev logs zero lines). Phase 692 RAN "
            "this: the redirect applies (dev=40AE4D00) and the run gains a "
            "guest crash at 819DE94C. The title's device is live and owns its "
            "own command buffer at BF7CA2BC; writing our cursor into "
            "[dev+0x30] leaves it describing two buffers at once. Kept so the "
            "negative result is reproducible.",
            "Kernel");

DEFINE_bool(guide_stub_gpu_alloc, false,
            "Replace 81A02940, the GPU-visible data allocator, with a bump "
            "allocator over harness-owned physical memory. Phase 693: the "
            "Guide can write commands fine (81A015B8 runs 934 times) but "
            "cannot allocate DATA memory, so its draws bail before emitting. "
            "Unlike guide_force_cmdbuf_alloc, which forced the guard and hit "
            "the assert in 81A02688, this satisfies the contract: it returns "
            "a real address for 819E01E0 to convert. Installed early, before "
            "the function is first translated.",
            "Kernel");

DEFINE_uint64(guide_alloc_lr, 0,
              "Serve guide_stub_gpu_alloc only to the caller whose return "
              "address is this value; 0 serves every caller. Phase 696: "
              "gating on 819DCD00 (819DCCA0's site) removed the crash but "
              "dropped DRAW_INDX 16 -> 0, so the geometry needs allocations "
              "from more than one of the 10 call sites. Runtime-settable so "
              "each can be tested without a rebuild.",
              "Kernel");

DEFINE_uint64(guide_alloc_lr_hi, 0,
              "Upper bound of the served return-address range; 0 means "
              "equal to guide_alloc_lr (a single site). Phase 697: ranges let "
              "the allocator's 10 call sites be bisected in about four runs "
              "instead of nine.",
              "Kernel");

DEFINE_bool(guide_patch_resolve_dest, false,
            "Rewrite RB_COPY_DEST_BASE in the buffer about to be submitted so "
            "the Guide's resolve lands in a scratch surface we own. Phase 700 "
            "measured dest_base = 0: the frame renders 16 colour draws and "
            "resolves them to address zero. Phase 702 closed the route to "
            "finding the writer statically, so supply the destination instead "
            "- the move that worked for the allocator in phase 694. Also "
            "counts non-zero pixels afterwards, which is the first direct test "
            "of whether the draws rasterise at all.",
            "Kernel");

DEFINE_bool(guide_submit_from_base, false,
            "Submit the second context from the composite buffer's base "
            "rather than from [dev+0x30]. Phase 703: [dev+0x30] is the "
            "CURRENT cursor, which after emission points at the end of what "
            "the Guide wrote - so the submission covered 36 words past the "
            "buffer and the 6917 words holding all 19 draws were never handed "
            "to the command processor (GPU draws +0 every frame). The comment "
            "there assumed the cursor gets cleared before we read it, which "
            "stopped being true once emission worked.",
            "Kernel");

DEFINE_bool(guide_nop_waits, false,
            "Rewrite WAIT_REG_MEM (0x3C) packets to NOP in the Guide's buffer "
            "before submitting it. Phase 704: with the virtual executor the "
            "stream is parsed for real and the call never returns - the buffer "
            "carries 25 waits for fences that never arrive, because it is not "
            "part of the title's ring flow. The count field is preserved so "
            "the parser still skips the body.",
            "Kernel");

DEFINE_bool(guide_ctx2_kick_ptr, false,
            "Give [dev+0x2B14] a real buffer at the second-context site, for "
            "both the Guide's device and the title's. 819FCE50 kicks the GPU "
            "through that field and faults on null - phases 630 and 632. The "
            "existing guide_fix_kick_ptr does this in emulator.cc but is "
            "nested under guide_bind_boot_rt under guide_set_render_dc, and "
            "phase 701 measured that enabling that chain drops DRAW_INDX from "
            "16 to 0. Same fix, at a site that runs.",
            "Kernel");

DEFINE_bool(guide_nop_draws, false,
            "Rewrite DRAW_INDX and DRAW_INDX_2 to NOP in the Guide's buffer, "
            "keeping every register write. Phase 708: the presented frame is "
            "entirely black with the Guide's stream running; this separates "
            "'the draws paint black' from 'the state writes alone blank the "
            "frame'. If the frame returns to normal game output, the geometry "
            "and its resolve are responsible; if it stays black, the 949 "
            "register writes are.",
            "Kernel");

DEFINE_bool(guide_isolate_regs, false,
            "Snapshot the GPU register file before running the Guide's stream "
            "and restore it afterwards. Phase 708: the Guide's 949 register "
            "writes alone turn the presented frame entirely black - with every "
            "draw and the resolve NOPed - because the stream runs at swap and "
            "the title then presents against the Guide's state.",
            "Kernel");

DEFINE_bool(guide_nop_regs, false,
            "Rewrite the Guide's type-0 register writes to type-3 NOPs of the "
            "same length. Phase 709: restoring the register file after the "
            "stream did not stop the frame going black, so the emulator must "
            "act on the writes as they arrive rather than on their final "
            "values. With this and guide_nop_draws the stream becomes pure "
            "NOPs - if the frame is still black, submitting at all is what "
            "breaks it.",
            "Kernel");

DEFINE_bool(guide_keep_surface_regs, false,
            "Replace the Guide's writes to RB_SURFACE_INFO, RB_COLOR_INFO and "
            "RB_DEPTH_INFO with the values the emulator already holds, so its "
            "stream cannot retarget EDRAM. Phase 709 isolated the blank frame "
            "to the register writes; phase 699 measured the Guide setting "
            "pitch 1280 and colour base tile 0x2AA, a different EDRAM layout "
            "from the title's. Rewriting the data words keeps the packet "
            "framing, unlike NOPing whole packets which would take neighbours "
            "with them.",
            "Kernel");

DEFINE_uint64(guide_keep_reg_lo, 0,
              "Low bound of the register range whose writes are replaced with "
              "the emulator's current values. Phase 710: preserving only "
              "0x2000-0x2002 left the frame black, so the culprit is elsewhere "
              "among the 949 writes; a range makes them bisectable the way "
              "phase 697 bisected the allocator's call sites.",
              "Kernel");

DEFINE_uint64(guide_keep_reg_hi, 0,
              "High bound of that range; 0 means equal to guide_keep_reg_lo.",
              "Kernel");

DEFINE_int32(guide_capture_interval_ms, 8,
             "Milliseconds between capture shots. Phase 879: at the default of "
             "8ms a whole capture set covers under 50ms, so every result in "
             "phases 872-878 describes one wall-clock instant that nothing "
             "synchronises with. Widen it to sample a trajectory instead.",
             "Guide");
DEFINE_bool(guide_capture_on_burst, false,
            "Take the capture when the Guide's burst first emits draws, "
            "instead of after guide_capture_seconds. Phase 997: the burst "
            "emits draws on exactly ONE frame - re-executions of the same "
            "published range produce 0 draws (996) - so a wall-clock capture "
            "has never photographed a frame the Guide drew in.",
            "Kernel");
DEFINE_int32(guide_capture_count, 1,
             "Capture this many consecutive presented frames, 8ms apart, to "
             "guide_capture_N.raw. Phase 718: the Guide resolves to 1E69E000 "
             "every frame while the scanout alternates between the title's two "
             "buffers, so a single capture cannot tell 'never visible' from "
             "'visible every other frame'.",
             "Kernel");

DEFINE_bool(guide_arm_overlay, false,
            "Publish the Guide's stream to guide_overlay_ptr_/words_ instead "
            "of executing it inline, so ExecutePacketType3_XE_SWAP runs it on "
            "the GPU thread just before the present. Phase 721: the block that "
            "normally arms this is unreachable (its unconditional OverlayGate "
            "log never prints), and the comment dismissing the swap hook - "
            "'only ONE XE_SWAP packet is seen in a whole run' - is stale: this "
            "session sees thousands.",
            "Kernel");

DEFINE_bool(guide_truncate_at_ramp, false,
            "Submit only up to the first packet that writes the 0x1000-0x1FFF "
            "register block. Phases 720 and 724: that block is a 769-entry "
            "gamma ramp being parsed as PM4, and it produces both an all-zero "
            "palette load that blanks the display and a bogus XE_SWAP "
            "presenting from address zero. Truncating removes both without "
            "needing guide_keep_reg_lo to paper over them.",
            "Kernel");

DEFINE_bool(guide_retarget_interrupt, false,
            "Re-register the graphics interrupt callback with the device the "
            "present path uses. Phase 645: xam registers it with mode 1's "
            "device (407CB880), whose submitted/completed counters never "
            "move because submissions go to 40870D00 - so the service "
            "routine at 819FCB10 finds nothing to do and never writes the "
            "fence the command processor waits on.",
            "Kernel");

DEFINE_bool(guide_dc_after_mode1, false,
            "Defer XuiRenderCreateDC until VdGlobalXamDevice is populated, "
            "so the DC, its wrapper and the render object are built on the "
            "device mode 1 creates rather than the one that predates it. "
            "Phase 644: repointing the global afterwards moves the pointer "
            "but not the objects (phases 641, 643), and the creator does not "
            "hold the bootstrap's thread (phase 643), so waiting is safe.",
            "Kernel");

DEFINE_bool(guide_render_on_xam_device, false,
            "Point [[bootDC+0x1CC]+0x0C] at the device that owns xam's ring "
            "(the one XamDeviceSlot reports). Phase 639: the mode-1 ring is "
            "created with r31 = 407CB880 and the interrupt handler is "
            "registered with the same device, but the present path uses "
            "40870D00 - so submission and servicing are on different "
            "devices. Unlike guide_render_on_title_device this stays within "
            "xam's device layout.",
            "Kernel");

DEFINE_bool(guide_fix_kick_ptr, false,
            "Copy [dev+0x2B14] from the mode-1 device onto the device the "
            "present path uses, when the latter is null. Phase 630: the GPU "
            "kick at 819FCE50 stores through that pointer and faults when it "
            "is null; runs that take that fault stop the present early "
            "(819FEC64), runs that do not reach 819FECB0. Only the mode-1 "
            "setup writes the field (81A0FF3C).",
            "Kernel");

DEFINE_bool(guide_prefer_mode1_device, false,
            "Prefer the device the mode-1 creator set up over the title's "
            "when rebinding. Phase 616: the existing preference for the "
            "title device was written when mode 1 never ran and no such "
            "device existed. Only the mode-1 path sets [dev+0x2B14] (stored "
            "at 81A0FF3C), and the GPU kick at 819FCE50 stores through it - "
            "so on the title's device that kick dereferences null.",
            "Kernel");

DEFINE_bool(guide_skin_dispatch_real, false,
            "Install the real object 0x81D6CA00 into [81D6C9C8] rather than a "
            "fabricated stand-in. Phase 603: 819106F8 compares [81D6C9C8] "
            "against 81D6CA00 and, when they differ, calls vtable[0] on that "
            "object - which fails and is the 8000FFFF the render host "
            "forwards. The comparison wants the real object, so give it one.",
            "Kernel");

DEFINE_bool(guide_render_on_title_device, false,
            "Point [[bootDC+0x1CC]+0x0C] at the title's D3D device so hud "
            "renders through it. Phase 593: hud renders against 40870D00, "
            "which is neither xam's device (407CB880) nor the title's "
            "(40AE4D00); only the title's buffers are ever executed by the "
            "command processor.",
            "Kernel");

DEFINE_bool(guide_patch_window_reset, false,
            "Nop the two stores at 81A02B08/81A02B14 that re-point "
            "[dev+0x30]/[dev+0x34] at the device's internal 0x12C0-byte "
            "command buffer. Phase 590: that reset is why a widened window "
            "never survives into the reservation, and 0x12C0 (4800) cannot "
            "hold the Guide's first request of 9236 bytes at any time.",
            "Kernel");

DEFINE_uint32(guide_bind_boot_cmdbuf_kb, 0,
              "Allocate and bind a command buffer of this many KB on the "
              "device hud renders against. Phase 589: with a render target "
              "bound, the run reaches 81A01638, which stores a packet word "
              "through a cursor that is zero because nothing ever gave that "
              "device a buffer. 0 disables.",
              "Kernel");

DEFINE_bool(guide_bind_boot_rt, false,
            "Create a surface and bind it as the render target on the device "
            "hud's render is driven against, reached as "
            "[[bootDC+0x1CC]+0x0C]. Phase 588: both [dev+0x32A0] and "
            "[dev+0x32B0] are null there, which is what makes the render "
            "fault once it is given a real DC.",
            "Kernel");

DEFINE_bool(guide_patch_present_rt, false,
            "Make 819DE934 an unconditional branch so the block that reads "
            "[dev+0x32A0] / [dev+0x32B0] is skipped. Phase 588: with a real "
            "DC installed, hud's render reaches 819DE94C and faults - both "
            "render-target slots are null on the device it is handed, and "
            "819DE94C dereferences +0x24 of the result. The code already "
            "skips this block when r30 != 0, so the branch target is a path "
            "xam itself takes.",
            "Kernel");

DEFINE_bool(guide_set_render_dc, false,
            "Store the bootstrap device context into [renderObj+0xC] before "
            "driving hud's render. Phase 587: 913EAB28 loads [this+0xC] and "
            "passes it to 913FE874; that field is null, the call fails, and "
            "the render returns at 913EAB50 on all 2000 frames without "
            "drawing anything.",
            "Kernel");

DEFINE_bool(guide_nav_clear_slot, false,
            "Zero [navObj+0x18] before invoking the navigation dispatcher. "
            "Phase 585: 913EABE0 (vtable[0x24] of the sub-object at "
            "navObj+0x10) returns E_UNEXPECTED when [this+8] is already "
            "non-null, and that slot is the out-parameter it fills on "
            "success - so an earlier caller having filled it is what makes "
            "our call fail.",
            "Kernel");

DEFINE_bool(guide_nav_dc_null, false,
            "Pass null rather than the bootstrap DC as the navigation "
            "dispatcher's second argument.",
            "Kernel");

DEFINE_string(guide_sysreq_button, "",
              "Phase 1097: deliver the Guide button press through the "
              "callback xam registers with the kernel's DrvSetSysReqCallback "
              "(xboxkrnl ordinal 0x20C), which is the path the console "
              "itself uses and which Xenia never implemented - the export was "
              "declared in xboxkrnl_table.inc with no body, so every "
              "registration was an \"undefined extern call\" that discarded "
              "the pointer. Value is \"device,class,kind\", the three "
              "arguments the real kernel's own HID dispatch passes at "
              "800BBE30 (device from the HID event, class 0, kind 0 or 1). "
              "Passing device 0 takes xam's own null-device path at 817C243C "
              "rather than inventing a user index here. Empty leaves the "
              "press on the existing host-driven path.",
              "Kernel");

DEFINE_int32(guide_xenon_press, -1,
             "Phase 1097: on a Guide press, call xam's own exported "
             "XamInputSendXenonButtonPress (ordinal 0x506, 817C4CD8) with "
             "this value. That export loads xam's input context from "
             "[81D4F610] and tail-calls 817C2090 -> 817C1FF8, which posts the "
             "code into [ctx+0x10] and calls KeSetEvent on the context - so "
             "it skips 817C23A8's three gates, one of which "
             "([81D4F614] != 0) is measured to be closed. 817C1FF8 maps the "
             "value 0xFF to 0 and the consuming worker accepts 0..3 (a user "
             "index) or 0xFF, so 0 is the plain 'user 0' press. Negative "
             "leaves this off.",
             "Kernel");

DEFINE_bool(guide_sysreq_kernel, true,
            "Phase 1097: hold the callback xam registers through xboxkrnl "
            "0x20C DrvSetSysReqCallback, so 0x278 DrvXenonButtonPressed can "
            "call it - the console's own Guide-button path. Both exports "
            "were declared in xboxkrnl_table.inc with no body, so the "
            "pointer xam registered was discarded and no press could ever "
            "reach xam. On by default: an A/B on one binary measured it "
            "neutral - off and on give the same PvZ run - and holding the "
            "pointer changes nothing on its own, since nothing calls it "
            "unless guide_sysreq_button is set. Off restores the previous "
            "behaviour of answering success and keeping nothing.",
            "Kernel");

DEFINE_uint32(guide_insn_value_match, 0,
              "Phase 1097: alongside guide_insn_value_addr, count EVERY "
              "time the chosen register holds this value there, and keep "
              "the last four link registers at a match so the caller is "
              "named. The eight-slot history next to it is a sample, and "
              "reading its last eight values as a complete census is a "
              "mistake - a 606-call run printed eight ids and could not "
              "say whether a ninth was ever sent. 0 disables it, so this "
              "cannot be used to count a zero value.",
              "GuideResearch");

DEFINE_bool(guide_xex_header_security_ptr, false,
            "Phase 1097: in the XEX header Xenia copies into guest memory, "
            "rewrite the field at +0x10 from the file-relative "
            "security_offset to an absolute guest pointer at the security "
            "info. Real xam dereferences that field: 81747DE8 does "
            "lwz r5,4(r11) with r11 = [xex_header+0x10], and the value it "
            "faults on is 000000E8 - exactly ximecore.xex's "
            "security_offset as it reads on disk. [security_info+4] is "
            "image_size, which is what the surrounding code gathers. "
            "Phase 1099z154: confirmed as KERNEL behaviour, not a "
            "workaround - the 17489 kernel itself reads the loaded "
            "header's +0x10 as a pointer with no base added ([ldr+0x58] "
            "-> +0x10 -> +0x10C at 800A04C0, -> +0x160 at 800A131C). Off by "
            "default only so existing configs behave the same; covrun and "
            "the 17559 runs turn it on.",
            "Kernel");

DEFINE_bool(guide_guest_hud_init, false,
            "Phase 1097r: do NOT call hud's init (hud_base+0xA898) from the "
            "Guide bootstrap. Measured: hud's init is entered with "
            "lr = BCBCBCBC - the sentinel processor()->Execute leaves - and "
            "no guest thread has that address as an entry point, so the "
            "host is the only thing that ever runs it. That is the same "
            "defect as the host driving the skin loader, and removing THAT "
            "is what let the guest arm its HUD-manager loop and open the "
            "UI gate. With this on, [hudobj+0x14] and the null DC stop "
            "being host artefacts and the real question - whether xam ever "
            "initialises hud itself - becomes measurable.",
            "Kernel");

DEFINE_uint32(guide_watch_write_addr, 0,
              "Phase 1097zd: protect the page holding this guest address "
              "read-only at Guide-press time and report the GUEST PC of the "
              "first write to it, then unprotect and carry on. Built because "
              "three static searches for the writer of the HUD state word "
              "81D43C50 all failed: the writer holds its base in a register "
              "loaded from memory and stores a computed value, so neither "
              "the address nor the value is visible in the image. Reads do "
              "not fault, so the constant polling of that word costs "
              "nothing. One shot - the first write disarms it. Note the "
              "whole 4 KB page is watched, so an unrelated neighbour being "
              "written first will spend the shot; the log prints the guest "
              "PC and the resulting value so that is visible rather than "
              "silent.",
              "GuideResearch");

DEFINE_uint32(guide_watch_write_hits, 1,
              "Phase 1097zh: how many writes guide_watch_write_addr catches "
              "before disarming. 1 is the original one-shot behaviour, which "
              "spent itself on whichever store came first - for the HUD "
              "state word that was the null branch storing 0, leaving the "
              "writes carrying 1, 2 and 4 unseen. The page is unprotected "
              "so the faulting store can retire and re-protected 2 ms later "
              "from a helper thread, so there is a small blind window: the "
              "hit numbers count writes SEEN, not writes that happened.",
              "GuideResearch");

DEFINE_bool(guide_rerun_sysapp_pass, false,
            "Phase 1097zq DIAGNOSTIC, not a fix: re-run xam's sys-app pass "
            "8177F588(81D42FD0,0,0,0) at Guide-press time, after hud has "
            "registered. The pass is the only one of the three Guide-app-"
            "record creators not gated behind a HUD state this xam cannot "
            "reach, and it normally runs three times inside the 293 ms "
            "window BEFORE hud registers, reading a zero handler every "
            "time. The argument is measured from the live call site at "
            "817800B8, not invented. Turning this on has the HOST DRIVE "
            "GUEST CODE, which is what the goal forbids and what removing "
            "produced this phase's best result - it exists only to make "
            "the claim falsifiable in one run.",
            "GuideResearch");

DEFINE_bool(guide_watch_write_early, false,
            "Phase 1097zs: arm guide_watch_write_addr right after xam is "
            "loaded instead of at Guide-press time. The press-time arming "
            "cannot see xam's own boot, which is the window that decides "
            "whether a word like the Guide app record pointer [81D426C8] is "
            "written and later cleared, or never written at all.",
            "GuideResearch");

// 1099z17559-2: real kernel behaviour, off by default only so the 17489 setup
// (input through the automation workaround) is unchanged.
DEFINE_bool(kernel_xinputd, false,
            "Implement the kernel controller driver contract for LLE xam "
            "(xboxkrnl_xinputd.cc): keep the callback from "
            "DrvSetUserBindingCallback, bind each present host input slot "
            "through it (DrvBindToUser), serve XInputdReadState/"
            "GetCapabilities/GetDeviceStats from the host InputSystem, and "
            "deliver the Guide button through DrvXenonButtonPressed as the "
            "17489 kernel's RGC driver does. Needed on retail systems, whose "
            "XAutomation exports are stubs.",
            "Kernel");

// 1099z17559-3: diagnostic only. Addresses come from the command line, so the
// host carries no build-specific address.
DEFINE_bool(xui_glyph_2x, true,
            "HOST-SIDE enhancement, not console behaviour: bake XUI text "
            "(dashboard and Guide, every system 6770-17559) at the integer "
            "render scale (min of draw_resolution_scale_x/_y, as picked by "
            "upscale_to_window) with 1x layout, so text is as sharp as the "
            "rest of the upscaled frame. No effect at scale 1. Unlisted "
            "builds are left unchanged.",
            "Guide");
DEFINE_int32(xui_glyph_scale, 0,
             "HOST-SIDE: XUI text scale for xui_glyph_2x. 0 = follow the "
             "render scale (min of draw_resolution_scale_x/_y); 2-8 = force "
             "that scale.",
             "Guide");
DEFINE_double(xui_glyph_dpi, 0.0,
              "HOST-SIDE enhancement, not console behaviour: replace the DPI "
              "xam 17559's XUI text renderer stores at init (96.0, stfs "
              "f31,8(r31) at 8178E30C) so glyphs are rasterized at "
              "size*dpi/72 pixels. Changes text size as well as sharpness "
              "(metrics are not compensated). 0 = off.",
              "Guide");
DEFINE_string(trace_dash_lua, "",
              "Diagnostic: throw,sleep,yield hex pcs of dash.xex's luaD_throw, "
              "Lua Sleep and lua_yield for a build other than 17489 (17559: "
              "928E4A00,928F6DE0,928E47D0) - logs Lua errors and wait sites.",
              "Kernel");
DEFINE_string(trace_guest_list, "",
              "Diagnostic: pc:rN:headoff:nextoff:words - at guest pc walk the "
              "linked list at [rN+headoff] via [node+nextoff] and log each "
              "node's words (phase 1099z161).",
              "Kernel");
DEFINE_string(trace_guest_pcs, "",
              "Diagnostic: comma-separated hex guest addresses. Each logs its "
              "first 8 executions (r3-r6, lr, r1, [r1+0x70..0x7C], thread "
              "id) through a guest hook; 'addr:N' logs N executions. "
              "Hooks apply to code translated after start-up.",
              "Kernel");

// 1099z17559-4: real kernel behaviour, off by default so the 17489 setup (which
// uses guest_native_timers, a workaround) is unchanged.
DEFINE_bool(kernel_guest_timers, false,
            "Adopt guest KTIMERs (KeInitializeTimerEx/KeSetTimer(Ex)/"
            "KeCancelTimer) so they can be waited on, and run a timer's DPC as "
            "a DPC: DeferredRoutine(Dpc, DeferredContext, SystemArgument1, "
            "SystemArgument2) with all four arguments, on the kernel's DPC "
            "thread rather than as an APC on the thread that armed the timer "
            "(the guest_native_timers approximation). xam's task pool waits on "
            "a synchronization timer; without this every such wait fails.",
            "Kernel");

// 1099z17559-5: real kernel device, off by default so 17489 (whose devkit xam
// tolerates the failed open) is unchanged.
DEFINE_bool(kernel_device_auth, false,
            "Provide the kernel's \\Device\\DeviceAuth (accessory "
            "authentication). Retail xam opens it during UI start-up and "
            "KeBugChecks (0x29) when the open fails. Its request IOCTL "
            "(0x474000) completes only when an accessory needs "
            "authenticating; the controllers xboxkrnl_xinputd.cc presents "
            "are already authenticated, so the request stays pending.",
            "Kernel");

// 1099z167: diagnostic for "a game creates its save slot and never writes the
// data" (Fable III: only saveuid.bin appears). Logs every HLE XamContent entry
// point with its arguments and result, uncapped - the NtCreateFile log caps at
// 400 lines a session, which hides everything after the first minute.
DEFINE_bool(kernel_trace_xam_content, false,
            "Log every HLE XamContent call (create/open/close/flush and the "
            "content manager result) with root name, content type, file name, "
            "flags, disposition and status. Diagnostic only.",
            "Kernel");

// 1099z163: XeKeysConsoleSignatureVerification (ordinal 0x257) was an
// undefined extern returning 0, so xam treated every package signed by a real
// console as invalid - a Fable III save copied off the user's console read as
// "corrupted" while the same data inside an emulator-signed package loaded.
// Default ON: while this export was an undefined extern the guest saw r3 =
// its own first argument (a nonzero hash pointer = TRUE), so every signature
// was already being accepted. Returning FALSE here is a behaviour CHANGE that
// makes xam mark every profile "Corrupted Profile" (seen 2026-09-15).
DEFINE_bool(kernel_accept_console_signatures, true,
            "HOST-SIDE: XeKeysConsoleSignatureVerification accepts any console "
            "signature. The real kernel (8014C1E8) checks the console "
            "certificate against the master key and the signature against "
            "the certificate's public key; this build has neither the "
            "master key check nor other consoles' keys wired in, so content "
            "signed by a real console (saves, profiles) is taken as valid. "
            "The 'is this console's certificate' out parameter is still "
            "computed from the console ID.",
            "Kernel");

// 1099z17559-8
DEFINE_bool(kernel_boot_via_xam, false,
            "Load the boot title but do not start it: xam's own loader "
            "launches its boot title (retail 17559 does, 8169EAB0), and "
            "XexLoadExecutable adopts the loaded image. Unlike "
            "guide_xam_boot_launch nothing calls xam from the host. For 17489 "
            "devkit xam, whose loader does not launch a boot title, leave off.",
            "Kernel");

// 1099z17559-9
DEFINE_bool(kernel_boot_state_exports, false,
            "Implement DumpGetRawDumpInfo / HalFinalizePowerLossRecovery / "
            "HalGetNotedArgonErrors after the 17489 kernel (no dump partition, "
            "no power-loss recovery, no Argon errors -> 0). Off reproduces "
            "the undefined-extern result (r3 unchanged) that 17489 runs get; "
            "retail xam reads it as 'crash dump present' and launches "
            "ProcessDump.xex.",
            "Kernel");

// 1099z17559-11
DEFINE_bool(kernel_xex_load_keep_heap, false,
            "Do not Reset() the whole guest heap when an XEX image is loaded "
            "into it (xex_module.cc ReadImage). The reset freed the page "
            "table of every module already in that heap, so objects inside "
            "their images were refused as unmapped. Off keeps the upstream "
            "behaviour the 17489 setup was measured with.",
            "Kernel");

DEFINE_bool(kernel_system_flash_patches, true,
            "A system module (SYS: or the flash device) with a <name>.xexp "
            "beside it is loaded with that delta patch applied, as the console "
            "applies an update's $flash_*.xexp to the base 2.0.1888 flash "
            "files. Only acts when such a file exists (pre-2010 updates "
            "imported with a base firmware).",
            "Kernel");
DEFINE_bool(kernel_xex_module_handle_self, false,
            "XexGetModuleHandle with a name of (PSZ)-1 returns the module "
            "containing the caller's return address, as the 17489 kernel does "
            "(800A2B40). Old dashboards' dash.firstuse.xex (2.0.12625-15574) "
            "need it. Off keeps the upstream behaviour (reads -1 as a string).",
            "Kernel");

DEFINE_bool(kernel_xex_verify_headers, false,
            "XexVerifyImageHeaders with the 17489 kernel's bounds checks "
            "(signature not checked - HOST-SIDE). Off reproduces the "
            "undefined-extern result (r3 unchanged).",
            "Kernel");

// ---------------------------------------------------------------------------
// 1099z165: first-launch out-of-box experience (OOBE)
// ---------------------------------------------------------------------------
// MEASURED on retail 2.0.17559 (runs oobe_base / oobe_clear / oobe_drive):
// the dashboard's OOBE gate is bit 0x40 (DashboardInitialized) of
// XCONFIG_USER_RETAIL_FLAGS (category 3, setting 0x0C).
//   set   -> dash loads dashmain/hubui/hubapp/slots/... and shows Home.
//   clear -> dash loads its 'oobe' section (from L.dash.xex.oobe.xzp) and
//            dashnui.xex, and renders the "press the Guide button" screen;
//            pressing Guide gets xam's "Finish initial setup to enable the
//            Xbox Guide." Confirmed by looking at captured frames.
//
// Read out of the decompressed images (dash load base 0x92000000, xam
// 0x815F0000), which says the same thing:
//   dash 92182810  ExGetXConfigSetting(3, 0x0C, &flags, 4, &req)
//        92182820  rlwinm. r11,r11,0,25,25      ; mask 0x00000040
//        92182824  beq -> 92182838 li r11,1     ; BIT CLEAR => OOBE
//        9218283C  stw r11,0x158(r31)           ; the only runtime writer of
//                                               ; the dash's OOBE gate
//   dash 9217E91C  reads +0x158; only when set does it bl 92339588, which
//                  loads "oobeStrings.xus" out of package "oobe.xzp" and
//                  registers the OOBE scenes; the same test swaps ordinary
//                  scenes for their Oobe*.xur twins (922DDB74, 922CEF20,
//                  922E16A0).
// On the real console the bit is cleared by xam itself: 816E3510 reads
// XCONFIG_USER_LANGUAGE (3, 9) and, when it is 0 - a console that has never
// been set up - calls ExReadModifyWriteXConfigSettingUlong(3, 0x0C,
// 0xFFFFFFBF, 0) at 816E3564. Xenia's XConfig::SetDefaults writes a language
// AND the bit, so a fresh console never ran OOBE.
//
// Both flags are off by default, and the fresh-console one only ever runs
// when there is no xconfig.settings at all, so an existing console state is
// never touched.
DEFINE_bool(kernel_oobe_on_fresh_console, false,
            "On a console with no xconfig.settings yet (a fresh install), "
            "start with XCONFIG_USER_RETAIL_FLAGS bit 0x40 "
            "(DashboardInitialized) CLEAR so the dashboard runs its "
            "out-of-box experience on first launch, as a console out of the "
            "box does. Completing OOBE sets the bit and it never runs again. "
            "Has no effect once an xconfig.settings exists (phase 1099z165).",
            "Kernel");

DEFINE_bool(kernel_oobe_force, false,
            "Clear XCONFIG_USER_RETAIL_FLAGS bit 0x40 at every boot, even on "
            "a console that has already been set up, so the dashboard runs "
            "OOBE again. TEST-ONLY: the cleared value is in memory, but any "
            "later guest xconfig write persists the whole block, so point it "
            "at a scratch run folder, never the user's own (phase 1099z165).",
            "Kernel");
