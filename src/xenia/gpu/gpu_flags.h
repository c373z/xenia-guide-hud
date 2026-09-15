/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_GPU_FLAGS_H_
#define XENIA_GPU_GPU_FLAGS_H_
#include "xenia/base/cvar.h"

DECLARE_path(trace_gpu_prefix);
DECLARE_bool(trace_gpu_stream);

DECLARE_path(dump_shaders);

DECLARE_bool(vsync);
DECLARE_bool(guide_cp_probe);
DECLARE_bool(guide_fix_projection);
DECLARE_bool(guide_overlay_before_resolve);
DECLARE_bool(guide_syscmd_at_resolve);
DECLARE_bool(guide_present_surface);
DECLARE_int32(guide_present_stale_swaps);
DECLARE_bool(guide_present_pitch_width);
DECLARE_bool(guide_composite_blend);
DECLARE_bool(guide_show_alpha);
DECLARE_bool(guide_alpha_trace);
DECLARE_bool(guide_route_fetch15);
DECLARE_bool(guide_syscmd_at_swap);
DECLARE_bool(guide_syscmd_drain_all);
DECLARE_int32(guide_syscmd_idle_drain_ms);
DECLARE_int32(guide_refresh_hz);
DECLARE_bool(guide_overlay_test_quad);
DECLARE_bool(guide_marker_color);
DECLARE_bool(guide_marker_title);
DECLARE_bool(guide_clear_rt);
DECLARE_bool(guide_clear_rt_title);
DECLARE_bool(guide_clear_rt_pre);
DECLARE_bool(guide_d3d12_messages);
DECLARE_bool(guide_clear_rt_mid);
DECLARE_bool(guide_suppress_draws);
DECLARE_bool(guide_rebind_rt);
DECLARE_bool(guide_degenerate_quad);
DECLARE_bool(guide_overlay_repeat);
DECLARE_bool(guide_occlusion_query);
DECLARE_bool(guide_readback_rt);
DECLARE_bool(guide_overlay_restore_regs);
DECLARE_bool(guide_overlay_color_mode);
DECLARE_int32(guide_slide_ms);
DECLARE_bool(guide_cond_write_force);
DECLARE_bool(guide_atlas_invalidate);
DECLARE_bool(guide_readback_preclear);
DECLARE_bool(guide_invalidate_vertex);
DECLARE_bool(guide_quad_census);
DECLARE_bool(guide_overlay_quad_px);
DECLARE_bool(guide_overlay_skip_lut);
DECLARE_bool(guide_overlay_reset_state);
DECLARE_bool(guide_overlay_mask_off);
DECLARE_bool(guide_overlay_clip_disable);
DECLARE_bool(guide_overlay_vport_identity);
DECLARE_bool(guide_overlay_vte_passthru);
DECLARE_bool(guide_overlay_restore_context);
DECLARE_bool(guide_overlay_restore_surface);
DECLARE_uint32(guide_swap_log_every);
DECLARE_bool(guide_paint_marker);
DECLARE_bool(guide_resolve_after_draw);
DECLARE_bool(guide_verify_resolve);
DECLARE_bool(guide_force_opaque);
DECLARE_bool(guide_composite_frontbuffer);
DECLARE_bool(guide_replay_before_resolve);
DECLARE_bool(guide_replay_at_draw);

DECLARE_uint64(framerate_limit);

DECLARE_bool(gpu_allow_invalid_fetch_constants);

DECLARE_bool(non_seamless_cube_map);

DECLARE_bool(half_pixel_offset);

DECLARE_string(occlusion_query);

DECLARE_int32(occlusion_query_fake_lower_threshold);

DECLARE_int32(occlusion_query_fake_upper_threshold);

DECLARE_int32(occlusion_query_querybatch_range);

DECLARE_double(occlusion_query_saturation);

DECLARE_int32(anisotropic_override);

DECLARE_bool(disassemble_pm4);

DECLARE_bool(no_discard_stencil_in_transfer_pipelines);

DECLARE_bool(async_shader_compilation);

DECLARE_bool(gpu_3d_to_2d_texture);

DECLARE_bool(ac6_ground_fix);

DECLARE_bool(force_depth_clamp);

#define XE_GPU_FINE_GRAINED_DRAW_SCOPES 1

#endif  // XENIA_GPU_GPU_FLAGS_H_
