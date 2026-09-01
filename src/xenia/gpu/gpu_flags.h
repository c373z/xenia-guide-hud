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
