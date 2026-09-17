/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/gpu_flags.h"

DEFINE_path(trace_gpu_prefix, "scratch/gpu/",
            "Prefix path for GPU trace files.", "GPU.Debug");
DEFINE_bool(trace_gpu_stream, false, "Trace all GPU packets.", "GPU.Debug");

DEFINE_path(
    dump_shaders, "",
    "For shader debugging, path to dump GPU shaders to as they are compiled.",
    "GPU.Debug");

DEFINE_bool(vsync, true, "Enable VSYNC.", "GPU");

DEFINE_uint64(framerate_limit, 60,
              "Maximum frames per second. Default 60. 0 = Unlimited frames "
              "(still 60 when VSYNC is enabled).",
              "GPU");
UPDATE_from_uint64(framerate_limit, 2024, 8, 31, 20, 60);

DEFINE_bool(
    gpu_allow_invalid_fetch_constants, true,
    "Allow texture and vertex fetch constants with invalid type - generally "
    "unsafe because the constant may contain completely invalid values, but "
    "may be used to bypass fetch constant type errors in certain games until "
    "the real reason why they're invalid is found.",
    "GPU");
DEFINE_bool(
    gpu_allow_invalid_upload_range, false,
    "Allows games to read data from pages that are marked as no access.",
    "GPU");

DEFINE_bool(
    non_seamless_cube_map, true,
    "Disable filtering between cube map faces near edges where possible "
    "(Vulkan with VK_EXT_non_seamless_cube_map) to reproduce the Direct3D 9 "
    "behavior.",
    "GPU.Debug");

// Extremely bright screen borders in 4D5307E6.
// Reading between texels with half-pixel offset in 58410954.
DEFINE_bool(
    half_pixel_offset, true,
    "Enable support of vertex half-pixel offset (D3D9 PA_SU_VTX_CNTL "
    "PIX_CENTER). Generally games are aware of the half-pixel offset, and "
    "having this enabled is the correct behavior (disabling this may "
    "significantly break post-processing in some games), but in certain games "
    "it might have been ignored, resulting in slight blurriness of UI "
    "textures, for instance, when they are read between texels rather than "
    "at texel centers, or the leftmost/topmost pixels may not be fully covered "
    "when MSAA is used with fullscreen passes.",
    "GPU.Debug");

DEFINE_int32(occlusion_query_fake_lower_threshold, 80,
             "Lower end of the fake sample count value written on "
             "EVENT_WRITE_ZPD when real occlusion queries are disabled.\n"
             "-1 writes nothing, resulting in some games that sit and hang.\n"
             "0 means the fake result stays fully occluded.",
             "GPU");
DEFINE_int32(occlusion_query_fake_upper_threshold, 100,
             "Upper end of the fake sample count value written on "
             "EVENT_WRITE_ZPD when real occlusion queries are disabled.\n"
             "Keep this higher than occlusion_query_fake_lower_threshold.\n"
             "Ignored if occlusion_query_fake_lower_threshold is -1.",
             "GPU");
DEFINE_int32(occlusion_query_querybatch_range, 0,
             "Range of fake sample count values to walk for titles using the "
             "D3D QueryBatch standard before wrapping back to "
             "occlusion_query_fake_lower_threshold.\n"
             "This shouldn't be changed from the default value of 0 (disabled) "
             "unless necessary for a specific title.",
             "GPU");
DEFINE_double(
    occlusion_query_saturation, 1.0,
    "Compress higher occlusion query sample counts before guest writeback.\n"
    "This can be useful if effects such as lens flares appear too strong.\n"
    "1.0 = default behavior\n"
    "0.0 = collapse all nonzero sample counts to 1\n"
    "Values around 0.90 are a good starting point for subtle tuning.",
    "GPU");

DEFINE_int32(anisotropic_override, -1,
             "Forces anisotropic filtering (AF) for eligible textures.\n"
             "Higher values keep textures sharper at oblique angles at the "
             "cost of GPU bandwidth, though most GPUs handle up to 16x fine.\n"
             "In rare cases, forcing AF can introduce visual artifacts.\n"
             " -1 = No override\n"
             "  0 = Disable anisotropic filtering\n"
             "  1 = Force 1x anisotropic filtering\n"
             "  2 = Force 2x anisotropic filtering\n"
             "  3 = Force 4x anisotropic filtering\n"
             "  4 = Force 8x anisotropic filtering\n"
             "  5 = Force 16x anisotropic filtering",
             "GPU");

DEFINE_bool(no_discard_stencil_in_transfer_pipelines, false,
            "Skip stencil bit discard in render target transfer pipelines. "
            "May improve performance on some GPUs.",
            "GPU.Debug");

DEFINE_bool(gpu_3d_to_2d_texture, true,
            "Handle shaders that sample 3D textures as 2D by creating a 2D "
            "texture from slice 0 of the guest memory.",
            "GPU");

DEFINE_bool(
    async_shader_compilation, true,
    "Compile shaders and create pipelines asynchronously in background "
    "threads. "
    "Eliminates shader compilation stutter but may cause brief rendering "
    "artifacts while pipelines are being created. When disabled, pipelines are "
    "created synchronously which causes stutter but no visual artifacts.",
    "GPU");

DEFINE_bool(
    ac6_ground_fix, false,
    "This fixes(hide) issues with black ground in AC6. Use only in AC6. "
    "Might cause issues in other titles.",
    "HACKS");

DEFINE_bool(
    force_depth_clamp, false,
    "Use host depth clamping instead of near and far plane clipping when "
    "guest clipping is enabled. X/Y/W clipping is unaffected. On Vulkan, "
    "this requires depthClamp support.",
    "GPU");

// Phase 522/523: the Guide's geometry reaches the primary EDRAM target, but the
// per-frame ordering is `title renders -> RESOLVE -> Guide draws -> SWAP`, so it
// is written after the displayed pixels were copied out and is cleared unseen.
// This issues a second resolve after the Guide has drawn, immediately before the
// swap, reusing the copy registers the title's own resolve just used.
DEFINE_bool(guide_cp_probe, false,
            "Log command-processor internals: worker heartbeat, primary "
            "buffer entry/exit, the stall wait, swaps and pending functions. "
            "Phase 617: these were added unconditionally while diagnosing the "
            "ring handover; the wait path alone emits three lines per entry, "
            "which floods a normal run. The ring-change fixes they were "
            "written to investigate are NOT gated by this - only the logging.",
            "GPU");

DEFINE_bool(guide_resolve_after_draw, false,
            "Issue a second EDRAM resolve after the Guide's draws and before "
            "the swap, so its geometry reaches the front buffer.",
            "GPU");

// Phase 524: checksum the resolve destination either side of the Guide's extra
// resolve, to establish that it changes the image rather than assuming it does.
// Needs --readback_resolve=full so the resolve reaches guest memory.
DEFINE_bool(guide_verify_resolve, false,
            "Checksum the resolve destination before and after the Guide's "
            "extra resolve and report how many frames it changes.",
            "GPU");

// Phase 525: force the Guide's own draws to opaque blending, to separate "the
// geometry covers nothing" from "the geometry is drawn fully transparent".
DEFINE_bool(guide_force_opaque, false,
            "Override RB_BLENDCONTROL0 to src=One dst=Zero for draws issued "
            "inside the Guide's render call.",
            "GPU");

// Phase 532: capture the Guide's contribution to the front buffer as a diff and
// re-apply it after the title's resolve, immediately before the swap. Needs
// --readback_resolve=full and --guide_resolve_after_draw.
DEFINE_bool(guide_composite_frontbuffer, false,
            "Re-apply the Guide's resolved pixels on top of the finished frame "
            "so they survive the title's own resolve.",
            "GPU");

// Phase 533: replay the Guide's recorded ring range immediately before the
// title's resolve, so its geometry is in EDRAM when the frame is copied out.
DEFINE_bool(guide_replay_before_resolve, false,
            "Re-execute the Guide's draw burst just before the title's resolve.",
            "GPU");

// Phase 554: replay the Guide's buffer at the first title draw of a frame,
// inside an active render pass, rather than inside IssueCopy.
DEFINE_bool(guide_replay_at_draw, false,
            "Replay the Guide's captured buffer at the frame's first title "
            "draw instead of during the resolve.",
            "GPU");

// Phase 728: every layer of the draw path reports correct and no pixels change.
// Before chasing the resolve further, establish whether guest memory at the
// swap's frontbuffer_ptr reaches the display at all.
DEFINE_bool(guide_fix_projection, false,
            "Replace the Guide's projection constants c4/c5 when they are "
            "non-finite. Phase 953: the Guide builds them as 2/w and -2/h "
            "from a screen size of zero, giving infinity, so every vertex "
            "leaves the shader non-finite and covers nothing.",
            "GPU");
// Phase 1098p: where to execute the system command buffer xam submits. Swap
// time dispatches real draws (measured +2/+3 per exec) but changes nothing on
// screen - phase 946's comment in IssueCopy says why: only the placement
// immediately before the title's resolve leaves the pixels in EDRAM when the
// copy to the displayed image runs.
// Phase 1098s: execute EVERY queued submit instead of only the most recent.
// Phase 1098zd: ON by default. It faulted when first tried (1098s) ONLY because
// the guest was recycling its three buffers under the host; with the slot
// contract (guide_syscmd_slot_contract) the backlog stays at or below three and
// every queued pointer still describes the bytes that were in it - measured 0
// host faults across every run since. And it is REQUIRED: executing only the
// newest submit drops the stream that carries the Guide's resolving draw, so
// 1FA50000 is never resolved and the Guide never reaches the screen.
// Default 0 (phase 1099z123): at 60 it measured 60 presents/s over a 30 fps
// Sonic, but the Guide's dim pass samples the title front buffer (fetch 15)
// while the title is mid-frame, and Sonic reuses that memory for a smaller
// intermediate resolve - the refresh showed that as garbage in the top-left.
// Needs a snapshot of the finished title frame before it can default on.
DEFINE_int32(guide_refresh_hz, 0,
             "HOST-SIDE FIX, not present in real hardware: while the Guide is "
             "up, re-present the title's last frame with the Guide redrawn at "
             "this rate when the title presents slower, so a low-framerate "
             "game does not cap the Guide. 0 = Guide follows the title's "
             "swaps. Phase 1099z114.",
             "Guide");
DEFINE_int32(guide_syscmd_idle_drain_ms, 100,
             "When the ring is idle and no swap/resolve has drained xam's "
             "system command buffer for this many ms, drain it anyway (the "
             "console GPU does not wait for a title to present). 0 = off. "
             "Phase 1099z97.",
             "Guide");
DEFINE_bool(guide_syscmd_drain_all, true,
            "Execute every queued system-command-buffer submit, not just the "
            "most recent.",
            "Guide");

// Phase 1098zc: present the GUIDE's own resolved surface instead of the title's
// swap texture. This is a DIAGNOSTIC, not the finished composite: it replaces
// rather than blends, and it exists to answer a question no instrument has been
// able to answer - does that surface contain the Guide? If it shows the blade
// over a dimmed dashboard, xam composites the title itself and presenting it IS
// the correct routing; if it shows the blade on black, a blend is needed.
// Phase 1098ze: ALPHA BLEND the Guide over the title's frame instead of
// replacing it - what the console's display hardware does. The captured image
// from 1098zc settles that xam does not composite the title into its own
// surface, so the blend has to happen here. Falls back to replacement if the
// composite objects cannot be built.
DEFINE_bool(guide_show_alpha, false,
            "Diagnostic: draw the Guide surface alpha channel as grey.",
            "Guide");
// Phase 1099g: 1099a's "alpha is 255 everywhere" histogrammed the PRESENTED
// capture, whose alpha the diagnostic shader forces to 1, and only at a settled
// frame. This reads back the Guide's own texture every swap and logs how its
// alpha changes, plus the guest's clear and per-draw blend/constant state.
// Phase 1099h: the real VdSwap (xboxkrnl 0x25B, 80090080), when a system
// command buffer is pending, writes PM4 0005485A - texture fetch 15 := the
// title's front-buffer fetch header (base translated to physical, clamp X/Y =
// ClampToEdge, mag/min = Linear) - and THEN an INDIRECT_BUFFER to the Guide's
// PM4. xam's first draw blits fetch 15 x its dim ramp. Replicate that prefix.
DEFINE_bool(guide_route_fetch15, true,
            "Bind the title's front buffer to texture fetch 15 before running "
            "the Guide's system command buffer, as the kernel's VdSwap does.",
            "Guide");
// Phase 1099j: WHERE the Guide's system command buffer runs. The real VdSwap
// emits it into the ring at swap, after the title's resolve, with that swap's
// front buffer in fetch 15. Draining at the title's resolve instead (1098p)
// binds the previous swap's buffer - measured one frame behind, 900/900.
DEFINE_bool(guide_syscmd_at_swap, true,
            "Run the Guide's system command buffer at swap (as VdSwap does) "
            "instead of at the title's resolve.",
            "Guide");
DEFINE_bool(guide_overlay_without_enable, true,
            "HOST-SIDE: show the display overlay plane while xam keeps "
            "resolving the overlay surface even if D1OVL_ENABLE was never "
            "written (xam writes ENABLE only from its HUD thread, and the "
            "toast is rendered on another thread in this emulator).",
            "Guide");
DEFINE_bool(guide_display_overlay, true,
            "Emulate the display overlay plane (D1OVL registers) xam uses for "
            "notification toasts, and present the full Guide surface only "
            "when the system command buffer descriptor names one (+0x08).",
            "Guide");
DEFINE_bool(guide_present_scaled_resolve, true,
            "Present the Guide surface from its resolution-scaled resolve when "
            "draw_resolution_scale > 1, as the title's swap texture is.",
            "Guide");
DEFINE_bool(guide_text_trace, false,
            "Diagnostic: log each distinct draw in the Guide's stream (bound "
            "textures, render target width, viewport, first vertices) and "
            "dump each bound texture once as guide_tex_*.bin next to the exe.",
            "Guide");
DEFINE_bool(guide_text_trace_title, false,
            "With guide_text_trace: also trace the running title's own draws "
            "(e.g. the dashboard), dumping textures up to 1024x1024.",
            "Guide");
DEFINE_bool(guide_alpha_trace, false,
            "Trace the Guide surface alpha over time: GPU readback of the "
            "Guide texture each swap, guest clear registers at its resolves, "
            "and blend/pixel-constant state of its first draws.",
            "Guide");
DEFINE_bool(guide_composite_blend, true,
            "Blend the Guide's surface over the title's frame rather than "
            "replacing it.",
            "Guide");
// Phase 1099z177: faithful - the real VdSwap substitutes the scanout address
// with descriptor +0x08 (xboxkrnl 17489 800F8EF4..800F8F28), so that surface
// is the primary plane and its alpha is not blended with anything.
DEFINE_bool(guide_scanout_opaque, true,
            "When xam's system command buffer descriptor names the Guide "
            "surface (+0x08), show it opaque (scanned out in place of the "
            "title's frame) instead of blending by its alpha. Old xam "
            "(2.0.6770-8955) leaves alpha < 1 in opaque panel pixels.",
            "Guide");
DEFINE_bool(guide_present_pitch_width, false,
            "Use RB_COPY_DEST_PITCH as the Guide texture's width instead of the "
            "descriptor's logical width. Only to A/B the 1098zi width change.",
            "Guide");
DEFINE_int32(guide_present_stale_swaps, 4,
             "Present the Guide's surface only if the guest resolved it within "
             "this many swaps - it resolves every frame while the Guide is up "
             "and stops when it closes.",
             "Guide");
DEFINE_bool(guide_present_surface, true,
            "Route the Guide's own resolved surface to the screen while the "
            "guest is producing it. Still REPLACES rather than alpha-blends "
            "over the title - see the note above.",
            "Guide");

DEFINE_bool(guide_syscmd_at_resolve, true,
            "Execute the guest's system command buffer immediately before the "
            "title's resolve instead of at swap.",
            "Guide");

DEFINE_bool(guide_overlay_before_resolve, false,
            "Execute the published Guide stream immediately before the title's "
            "own resolve, rather than at the swap. Phase 946: the title's "
            "resolve is the step that moves EDRAM to the displayed image, so "
            "drawing just before it is the one placement where the pixels "
            "cannot be stranded.",
            "GPU");
DEFINE_bool(guide_overlay_test_quad, false,
            "Overwrite the Guide's vertex data with a full-screen quad in NDC "
            "before its draws. Phase 939: everything that could stop a "
            "fragment has been read and is correct, so the question is whether "
            "ANY draw injected at this point can put a pixel on screen.",
            "GPU");
// Phase 969: every reading so far covers what a draw declares; none covers
// how large the resulting primitive is. The first draw's quad is 323x1 pixels
// after the model transform (964) - one pixel tall - and a sub-pixel primitive
// rasterises to nothing however correct its state is.
DEFINE_bool(guide_marker_title, false,
            "Apply the same marker patch to the TITLE's draws instead of the "
            "Guide's. Phase 974: a negative from guide_marker_color is only "
            "worth as much as evidence that the patch can paint at all, and "
            "nothing has ever shown that it reaches the GPU.",
            "GuideResearch");
DEFINE_bool(guide_marker_color, false,
            "Force the Guide's fragments to opaque magenta - pixel constants "
            "c0*c1 = (1,0,1,1), src One / dst Zero, alpha test off. Phase 972: "
            "every diff instrument here is either masked over the region the "
            "blade lands in or fighting a 9% animation noise floor; an exact "
            "colour needs no reference frame.",
            "GuideResearch");
DEFINE_bool(guide_overlay_repeat, false,
            "Keep the published Guide stream armed after executing it, so it "
            "runs on every swap instead of once. Phase 980: the burst fires "
            "exactly ONCE in a session of ~390 frames, while every capture in "
            "this log is taken on a fixed wall-clock timer - so a blade that "
            "rendered perfectly for one frame would have been photographed 10 "
            "seconds later and recorded as 0 pixels.",
            "GuideResearch");
DEFINE_bool(guide_degenerate_quad, false,
            "Collapse the Guide's vertex data to a single point, so its draws "
            "still run - and still trigger Xenia's ownership transfers - but "
            "cover no pixels. Phase 1002: guide_suppress_draws skips IssueDraw "
            "entirely, which also skips Update() and its transfer draws, so it "
            "cannot tell the Guide's fragments from Xenia's. This can.",
            "GuideResearch");
DEFINE_bool(guide_rebind_rt, false,
            "Invalidate the command list's render target binding before every "
            "Guide draw, so OMSetRenderTargets is re-issued. Phase 1000: the "
            "host render target census reports ZERO binds across the burst, "
            "so the draws inherit whatever binding the command list already "
            "had - while ClearRenderTargetView takes an explicit handle and is "
            "unaffected, which is exactly the asymmetry being seen.",
            "GuideResearch");
DEFINE_bool(guide_suppress_draws, false,
            "Execute the Guide's stream but skip IssueDraw for its draws. "
            "Phase 999: the occlusion count is taken around the whole burst, "
            "which also contains Xenia's own ownership-transfer draws, and "
            "985's 6.7x scaling with the geometry does not separate them - "
            "enlarging the quads also enlarges the transfers. This is the "
            "control that does.",
            "GuideResearch");
DEFINE_bool(guide_clear_rt_mid, false,
            "Clear the bound colour target to magenta once, in the MIDDLE of "
            "the burst, between two of the Guide's draws. Phase 996: clears "
            "immediately before and immediately after the burst both reach the "
            "display, and the draws in between do not - so this separates "
            "'the draws are special' from 'the interval is special'.",
            "GuideResearch");
DEFINE_bool(guide_d3d12_messages, false,
            "Drain the Direct3D 12 info queue into the log around the Guide's "
            "burst, with the storage filter cleared. Phase 995: Xenia "
            "configures the info queue but never reads it, and its deny list "
            "silences RENDER_TARGET_FORMAT_MISMATCH_PIPELINE_STATE and "
            "RENDERTARGETVIEW_NOT_SET - the two messages that would explain "
            "fragments being produced and no pixel written.",
            "GuideResearch");
DEFINE_bool(guide_clear_rt_pre, false,
            "Clear the bound colour target to magenta BEFORE the burst rather "
            "than after. Phase 987: a clear after the burst survives to the "
            "display (982) and the draws do not (983), so the question is "
            "whether the Guide's own stream - which ends in ~125 kCopy-mode "
            "resolves - wipes the target between the two.",
            "GuideResearch");
DEFINE_bool(guide_clear_rt_title, false,
            "Clear the bound colour target to magenta on every TITLE draw. "
            "Phase 979: a clear that does not reach the screen from the "
            "Guide's burst proves nothing until a clear is shown to reach the "
            "screen at all.",
            "GuideResearch");
DEFINE_bool(guide_clear_rt, false,
            "Clear the render target the Guide's draws are bound to, to "
            "magenta, immediately after the burst. Phase 977: the occlusion "
            "query says fragments ARE produced, so the question is whether "
            "this render target is the one that reaches the display at all.",
            "GuideResearch");
DEFINE_bool(guide_readback_rt, false,
            "Copy the bound colour target into a readback buffer INSIDE the "
            "command list - once right after the pre-burst clear (positive "
            "control, needs guide_clear_rt_pre) and once right after the "
            "burst that emitted draws - then count marker pixels on the CPU. "
            "Phase 1010: every visibility reading so far went through the "
            "resolve, the swap, the presenter and a timed screenshot; this "
            "reads the render target itself on the burst frame.",
            "GuideResearch");
DEFINE_bool(guide_readback_preclear, false,
            "Also read the colour target back right after the pre-burst clear, "
            "BEFORE the burst. Phase 1010: the GPU wait this needs sits between "
            "the paint publishing its stream and the burst parsing it, and the "
            "run that had it parsed 416 draws with zeroed constants instead of "
            "541 - so it is off by default and the burst readback's green "
            "background is the positive control instead.",
            "GuideResearch");
DEFINE_bool(guide_overlay_restore_regs, false,
            "Snapshot the whole register file before the before-resolve burst "
            "and put every changed register back afterwards through "
            "WriteRegister, so the title's resolve that follows runs with the "
            "title's own state. Phase 1011: the Guide's stream ends in copy-"
            "mode resolves that leave RB_COPY_DEST_BASE/PITCH/INFO at zero, "
            "so the title's resolve right after the burst failed "
            "('Unsupported resolve vertex buffer format') and the frame the "
            "blade was drawn into was never copied to a front buffer. The "
            "scratch, coherency and DC_LUT registers are skipped because "
            "writing them has side effects.",
            "GuideResearch");
DEFINE_bool(guide_occlusion_query, false,
            "Wrap the Guide's burst in a D3D12 occlusion query and report how "
            "many samples passed. Phase 976: every other instrument in this "
            "investigation reports what Xenia submits; this one reports what "
            "the rasteriser produced.",
            "GuideResearch");
DEFINE_bool(guide_invalidate_vertex, false,
            "Mark each Guide vertex buffer range CPU-modified before its draw, "
            "so SharedMemory re-uploads it. Phase 975: every vertex reading in "
            "this log is a CPU-side read through TranslatePhysical, and none "
            "shows what the GPU actually fetched.",
            "GuideResearch");
DEFINE_bool(guide_quad_census, false,
            "Measure the post-model-transform extent of every quad in the "
            "Guide's burst and report how many are thinner than a pixel.",
            "GuideResearch");
DEFINE_bool(guide_overlay_quad_px, false,
            "Overwrite the Guide's vertex data with a large quad expressed in "
            "the Guide's own pixel space, solving the model transform in "
            "c0/c1 so the result lands on screen. Phase 969: replaces "
            "guide_overlay_test_quad, whose NDC quad ignored that transform "
            "and was never a valid control (939, 950).",
            "GuideResearch");
DEFINE_bool(guide_overlay_skip_lut, false,
            "Skip DC_LUT gamma ramp register writes while executing the "
            "Guide's stream. Phase 936: those writes, not the draws, are what "
            "blacks the display - Xenia applies the ramp, and the replayed "
            "entries overwrite the title's.",
            "GPU");

// Phase 1047: the burst runs inside the title's IssueCopy, with the title's
// RB_MODECONTROL already in copy mode; once text paints, its quad-list draws
// reach IssueDraw in that mode and are treated as resolves (every one fails
// with "Unsupported resolve vertex buffer format" and the failed state then
// breaks the title's own resolve for the rest of the run). Put the burst in
// colour mode; guide_overlay_restore_regs puts the title's value back.
DEFINE_bool(guide_overlay_color_mode, false,
            "Set RB_MODECONTROL edram_mode to colour/depth for the "
            "before-resolve burst.",
            "GPU");

// Phase 1052b: the Guide's slide-in. The burst replays the same stream every
// frame, so a per-frame x offset on the Guide's viewport animates the whole
// blade without touching XUI (XuiElementSetPosition on the scene hung).
DEFINE_int32(guide_slide_ms, 0,
             "Slide the Guide in from the left over this many milliseconds, "
             "starting at the first burst; 0 off.",
             "GPU");

DEFINE_bool(guide_atlas_invalidate, false,
            "Phase 1054: before every Guide burst, mark the shared-memory "
            "ranges of the paint's 8-bit textures (the glyph atlases) as "
            "modified by the CPU, so the texture cache re-uploads them. The "
            "atlas in memory holds every glyph the screen lacks.",
            "GPU");
DEFINE_bool(guide_cond_write_force, false,
            "Phase 1054: apply every COND_WRITE in the Guide's burst whether "
            "or not its poll condition holds (XUI writes its glyph atlas "
            "through 768 of them per paint; a replayed stream cannot satisfy "
            "a fence it never wrote).",
            "GPU");
DEFINE_bool(guide_overlay_reset_state, false,
            "Reset the host binding trackers after the Guide's stream runs, "
            "as BeginSubmission does. Phase 931: nothing currently restores "
            "them, so the title's next frame re-binds only what it believes "
            "changed.",
            "GPU");
DEFINE_bool(guide_overlay_mask_off, false,
            "Force RB_COLOR_MASK to 0 for the Guide's draws, so they execute "
            "fully but write no colour. Phase 929: if the frame is still black "
            "with every colour write masked off, the damage is not caused by "
            "the draws writing pixels.",
            "GPU");
DEFINE_bool(guide_overlay_clip_disable, false,
            "Set PA_CL_CLIP_CNTL.clip_disable for the Guide's draws, as the "
            "title has it. Phase 927: that bit selects the huge-viewport path "
            "in GetHostViewportInfo, where ndc_scale becomes 2/extent with a "
            "unit scale - a window-to-NDC conversion, which is what the "
            "Guide's pixel-space shader output needs.",
            "GPU");
DEFINE_bool(guide_overlay_vport_identity, false,
            "Patch the Guide's viewport to identity (scale 1, offset 0) "
            "instead of 640/-360. Phase 924: VTE=0x43F sets both the scale "
            "enables and vtx_xy_fmt - vertices already in screen space - which "
            "is only consistent with an identity viewport transform.",
            "GPU");
DEFINE_bool(guide_overlay_vte_passthru, false,
            "Force VTE_CNTL to the title's 0x300 for the Guide's draws, "
            "disabling the viewport scale/offset transform. Phase 911: the "
            "Guide's vertices are pixel-sized (a 323x1 quad), so the transform "
            "being enabled scales them off screen rather than being merely "
            "unconfigured.",
            "GPU");
DEFINE_bool(guide_overlay_restore_context, false,
            "Restore the title's whole RB/PA register block (0x2000-0x25FF) "
            "before executing the Guide's stream. Phase 894: patching "
            "individual registers found three real defects and none was "
            "sufficient, and the list has no end condition.",
            "GPU");
DEFINE_bool(guide_overlay_restore_surface, false,
            "Restore RB_SURFACE_INFO/RB_COLOR_INFO/RB_DEPTH_INFO from the "
            "title's last resolve before executing the Guide's stream. Phase "
            "892: the geometry draws with SURFACE_INFO=0 - pitch zero - "
            "because the state preamble is not in the executed range.",
            "GPU");
DEFINE_uint32(guide_swap_log_every, 600,
              "Log one line every N swaps, with a millisecond timestamp. "
              "Phase 880: at the fixed 600 this is far too coarse to see the "
              "swap rate change at the 10-12s transition where the display "
              "goes black. Set to 1 to time every swap.",
              "GPU");
DEFINE_bool(guide_paint_marker, false,
            "Fill a block of guest memory at the swap's frontbuffer_ptr with a "
            "solid colour just before presenting. If it appears, guest memory "
            "is what gets displayed and the Guide's resolve should be visible; "
            "if it does not, the presenter never reads that buffer and the "
            "whole resolve-destination line of investigation is moot.",
            "GPU");
