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
DECLARE_uint32(guide_xam_ui_startup);
DECLARE_bool(guest_native_timers);
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
DECLARE_bool(guide_fake_gpu_writeback);
DECLARE_int32(guide_stall_probe_seconds);
DECLARE_bool(guide_create_primary_device);
DECLARE_bool(guide_system_process_type);
DECLARE_bool(guide_call_boot_entry);
DECLARE_int32(guide_force_cmdbuf_complete);
DECLARE_bool(guide_word_diff);
DECLARE_bool(guide_execute_command_stream);
DECLARE_bool(guide_diff_draw_writes);
DECLARE_int32(guide_syscmdbuf_buffer_kb);
DECLARE_bool(guide_syscmdbuf_fields);
DECLARE_string(guide_system_root);
DECLARE_int32(guide_auto_press_seconds);
DECLARE_uint32(guide_force_obj14);
DECLARE_bool(guide_patch_null_render);
DECLARE_uint32(guide_coverage_fn);
DECLARE_int32(guide_front_buffer_shift);
DECLARE_int32(guide_front_buffer_format);
DECLARE_bool(guide_fake_front_buffer);
DECLARE_bool(guide_bootstrap_create_dc);
DECLARE_bool(guide_watch_null_render);
DECLARE_bool(guide_watch_front_buffer);
DECLARE_int32(guide_probe_threads_seconds);
DECLARE_string(guide_trace_stores);
DECLARE_uint32(guide_trace_pump);
DECLARE_int32(guide_xam_button_api);
DECLARE_bool(guide_patch_cmdbuf_reset);
DECLARE_bool(guide_inject_label_text);
DECLARE_bool(guide_register_all_classes);
DECLARE_string(guide_scene_override);
DECLARE_bool(guide_static_locator);
DECLARE_bool(guide_fake_ring);
DECLARE_int32(guide_second_context_kb);
DECLARE_int32(guide_bind_cmdbuf_kb);
DECLARE_uint32(guide_device_init_fn);
DECLARE_bool(guide_device_begin);
DECLARE_bool(guide_restore_title_ring);
DECLARE_bool(guide_bind_title_rt);
DECLARE_bool(guide_bind_depth_copy);
DECLARE_bool(guide_use_bound_device);
DECLARE_bool(guide_trace_setrendertarget);
DECLARE_bool(guide_bootstrap_before_device);
DECLARE_bool(guide_clear_null_render);
DECLARE_bool(guide_force_real_present);
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
DECLARE_bool(guide_reuse_xui_ctx);
DECLARE_bool(lle_xam_skin_init);
DECLARE_bool(lle_xam_heap_patch);
DECLARE_bool(lle_xam_appid_sentinel);
DECLARE_bool(lle_show_guide);
DECLARE_int32(guide_subcommand);
DECLARE_string(guide_hud_path);
DECLARE_uint32(guide_message);

#endif  // XENIA_KERNEL_KERNEL_FLAGS_H_
