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
            "before opening the Guide. They register the built-in XUI classes "
            "and have no callers inside xam, so nothing drives them here.",
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
