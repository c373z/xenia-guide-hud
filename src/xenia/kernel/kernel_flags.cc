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
DEFINE_uint32(guide_xui_anim_init, 0x8174FDE0,
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
