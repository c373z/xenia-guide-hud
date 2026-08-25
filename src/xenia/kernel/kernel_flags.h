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
DECLARE_bool(lle_xam_trace_loader);
DECLARE_bool(lle_xam_app_host);
DECLARE_uint32(xbox_hardware_info_flags);
DECLARE_bool(guide_skip_bkgnd_transition);
DECLARE_bool(system_root_early);
DECLARE_bool(guide_spoof_ui_thread);
DECLARE_uint32(guide_xui_anim_init);
DECLARE_bool(guide_install_draw_hook);
DECLARE_bool(guide_step_scene);
DECLARE_bool(guide_scene_off_thread);
DECLARE_bool(guide_preset_fields);
DECLARE_bool(guide_init_only);
DECLARE_bool(guide_step_registrations);
DECLARE_bool(guide_register_classes);
DECLARE_string(guide_skin_path);
DECLARE_bool(guide_bootstrap_on_title_thread);
DECLARE_bool(guide_use_title_device);
DECLARE_bool(guide_create_scene);
DECLARE_bool(guide_create_xam_device);
DECLARE_bool(guide_call_render_host);
DECLARE_bool(guide_call_xuiinit);
DECLARE_bool(guide_force_render_gate);
DECLARE_bool(lle_guide_draw);
DECLARE_string(lle_xam_scope);
DECLARE_bool(lle_xam_fake_app_fe);
DECLARE_bool(lle_xam_sysapp_init);
DECLARE_bool(lle_xam_xui_init);
DECLARE_bool(lle_xam_heap0_alias);
DECLARE_bool(lle_xam_heap_init);
DECLARE_bool(lle_xam_heap_patch);
DECLARE_bool(lle_xam_appid_sentinel);
DECLARE_bool(lle_show_guide);
DECLARE_int32(guide_subcommand);
DECLARE_string(guide_hud_path);
DECLARE_uint32(guide_message);

#endif  // XENIA_KERNEL_KERNEL_FLAGS_H_
