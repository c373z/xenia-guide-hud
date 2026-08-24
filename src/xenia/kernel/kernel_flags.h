/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_KERNEL_FLAGS_H_
#define XENIA_KERNEL_KERNEL_FLAGS_H_
#include "xenia/base/cvar.h"

DECLARE_bool(headless);
DECLARE_bool(log_high_frequency_kernel_calls);
DECLARE_string(lle_xam);
DECLARE_bool(lle_xam_heap0_alias);
DECLARE_bool(lle_xam_heap_init);
DECLARE_bool(lle_xam_heap_patch);
DECLARE_bool(lle_xam_appid_sentinel);
DECLARE_bool(lle_show_guide);
DECLARE_int32(guide_subcommand);
DECLARE_string(guide_hud_path);
DECLARE_uint32(guide_message);

#endif  // XENIA_KERNEL_KERNEL_FLAGS_H_
