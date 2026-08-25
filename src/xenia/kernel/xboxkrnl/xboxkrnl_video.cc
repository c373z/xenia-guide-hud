/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_video.h"

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_rtl.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/xbox.h"

DEFINE_bool(interlaced, false, "Toggles interlaced mode.", "Video");
// BT.709 on modern monitors and TVs looks the closest to the Xbox 360 connected
// to an HDTV.
DEFINE_uint32(kernel_display_gamma_type, 2,
              "Display gamma type: 0 - linear, 1 - sRGB (CRT), 2 - BT.709 "
              "(HDTV), 3 - power specified via kernel_display_gamma_power.",
              "Kernel");
UPDATE_from_uint32(kernel_display_gamma_type, 2020, 12, 31, 13, 1);
DEFINE_double(kernel_display_gamma_power, 2.22222233,
              "Display gamma to use with kernel_display_gamma_type 3.",
              "Kernel");

static std::pair<uint32_t, uint32_t> CalculateScaledAspectRatio(
    uint32_t fb_x, uint32_t fb_y, bool forced_widescreen) {
  // Calculate the game's final aspect ratio as it would appear on a physical
  // TV.
  const auto res = xe::kernel::Resolution(fb_x, fb_y);

  uint32_t display_x = res.aspect_ratio().first;
  uint32_t display_y = res.aspect_ratio().second;

  if (forced_widescreen) {
    display_x = 16;
    display_y = 9;
  }

  uint32_t res_x = res.width_.get();
  uint32_t res_y = res.height_.get();

  uint32_t x_factor = std::gcd(fb_x, res_x);
  res_x /= x_factor;
  fb_x /= x_factor;
  uint32_t y_factor = std::gcd(fb_y, res_y);
  res_y /= y_factor;
  fb_y /= y_factor;

  display_x = display_x * res_x - display_x * (res_x - fb_x);
  display_y *= res_x;

  display_y = display_y * res_y - display_y * (res_y - fb_y);
  display_x *= res_y;

  uint32_t aspect_factor = std::gcd(display_x, display_y);
  display_x /= aspect_factor;
  display_y /= aspect_factor;

  XELOGI(
      "Hardware scaler: width ratio {}:{}, height ratio {}:{}, final aspect "
      "ratio {}:{}",
      fb_x, res_x, fb_y, res_y, display_x, display_y);

  return {display_x, display_y};
}

namespace xe {
namespace kernel {
namespace xboxkrnl {

bool IsWidescreen(KernelState* kernel_state, Resolution res) {
  if (res.is_widescreen()) {
    return true;
  }

  return kernel_state->xconfig()->ReadSetting<uint32_t>(
             XCONFIG_USER_CATEGORY, XCONFIG_USER_VIDEO_FLAGS) &
         X_VIDEO_FLAGS::Widescreen;
}

// Video standard only supports value from 1 to 3. PAL50 is not included here
// and PAL50 is converted to PAL with 50Hz in GetVideoRefreshRate.
X_AV_VIDEO_STANDARD GetVideoStandard(KernelState* kernel_state) {
  auto av_region = static_cast<X_AV_VIDEO_STANDARD>(
      (kernel_state->xconfig()->ReadSetting<uint32_t>(
           XCONFIG_SECURED_CATEGORY, XCONFIG_SECURED_AV_REGION) &
       0xFF00) >>
      8);

  if (av_region == X_AV_VIDEO_STANDARD::PAL_50) {
    av_region = X_AV_VIDEO_STANDARD::PAL;
  }

  if (av_region < X_AV_VIDEO_STANDARD::NTSCM ||
      av_region > X_AV_VIDEO_STANDARD::PAL_50) {
    return X_AV_VIDEO_STANDARD::NTSCM;
  }

  return av_region;
}

float GetVideoRefreshRate(KernelState* kernel_state) {
  const bool is_50Hz =
      (kernel_state->xconfig()->ReadSetting<uint32_t>(
           XCONFIG_SECURED_CATEGORY, XCONFIG_SECURED_AV_REGION) >>
       23) &
      0x1;

  return !is_50Hz ? 60.0f : 50.0f;
}

// https://web.archive.org/web/20150805074003/https://www.tweakoz.com/orkid/
// http://www.tweakoz.com/orkid/dox/d3/d52/xb360init_8cpp_source.html
// https://github.com/Free60Project/xenosfb/
// https://github.com/Free60Project/xenosfb/blob/master/src/xe.h
// https://github.com/gligli/libxemit
// https://web.archive.org/web/20090428095215/https://msdn.microsoft.com/en-us/library/bb313877.aspx
// https://web.archive.org/web/20100423054747/https://msdn.microsoft.com/en-us/library/bb313961.aspx
// https://web.archive.org/web/20100423054747/https://msdn.microsoft.com/en-us/library/bb313878.aspx
// https://web.archive.org/web/20090510235238/https://msdn.microsoft.com/en-us/library/bb313942.aspx
// https://svn.dd-wrt.com/browser/src/linux/universal/linux-3.8/drivers/gpu/drm/radeon/radeon_ring.c?rev=21595
// https://www.microsoft.com/en-za/download/details.aspx?id=5313 -- "Stripped
// Down Direct3D: Xbox 360 Command Buffer and Resource Management"

void VdGetCurrentDisplayGamma_entry(lpdword_t type_ptr, lpfloat_t power_ptr) {
  // 1 - sRGB.
  // 2 - TV (BT.709).
  // 3 - use the power written to *power_ptr.
  // Anything else - linear.
  // Used in D3D SetGammaRamp/SetPWLGamma to adjust the ramp for the display.
  *type_ptr = cvars::kernel_display_gamma_type;
  *power_ptr = float(cvars::kernel_display_gamma_power);
}
DECLARE_XBOXKRNL_EXPORT1(VdGetCurrentDisplayGamma, kVideo, kStub);

struct X_D3DPRIVATE_RECT {
  xe::be<uint32_t> x1;  // 0x0
  xe::be<uint32_t> y1;  // 0x4
  xe::be<uint32_t> x2;  // 0x8
  xe::be<uint32_t> y2;  // 0xC
};
static_assert_size(X_D3DPRIVATE_RECT, 0x10);

struct X_D3DFILTER_PARAMETERS {
  xe::be<float> nyquist;         // 0x0
  xe::be<float> flicker_filter;  // 0x4
  xe::be<float> beta;            // 0x8
};
static_assert_size(X_D3DFILTER_PARAMETERS, 0xC);

struct X_D3DPRIVATE_SCALER_PARAMETERS {
  X_D3DPRIVATE_RECT scaler_source_rect;                 // 0x0
  xe::be<uint32_t> scaled_output_width;                 // 0x10
  xe::be<uint32_t> scaled_output_height;                // 0x14
  xe::be<uint32_t> vertical_filter_type;                // 0x18
  X_D3DFILTER_PARAMETERS vertical_filter_parameters;    // 0x1C
  xe::be<uint32_t> horizontal_filter_type;              // 0x28
  X_D3DFILTER_PARAMETERS horizontal_filter_parameters;  // 0x2C
};
static_assert_size(X_D3DPRIVATE_SCALER_PARAMETERS, 0x38);

struct X_DISPLAY_INFO {
  xe::be<uint16_t> front_buffer_width;               // 0x0
  xe::be<uint16_t> front_buffer_height;              // 0x2
  uint8_t front_buffer_color_format;                 // 0x4
  uint8_t front_buffer_pixel_format;                 // 0x5
  X_D3DPRIVATE_SCALER_PARAMETERS scaler_parameters;  // 0x8
  xe::be<uint16_t> display_window_overscan_left;     // 0x40
  xe::be<uint16_t> display_window_overscan_top;      // 0x42
  xe::be<uint16_t> display_window_overscan_right;    // 0x44
  xe::be<uint16_t> display_window_overscan_bottom;   // 0x46
  xe::be<uint16_t> display_width;                    // 0x48
  xe::be<uint16_t> display_height;                   // 0x4A
  xe::be<float> display_refresh_rate;                // 0x4C
  xe::be<uint32_t> display_interlaced;               // 0x50
  uint8_t display_color_format;                      // 0x54
  xe::be<uint16_t> actual_display_width;             // 0x56
};
static_assert_size(X_DISPLAY_INFO, 0x58);

void VdGetCurrentDisplayInformation_entry(
    pointer_t<X_DISPLAY_INFO> display_info) {
  X_VIDEO_MODE mode;
  VdQueryVideoMode(&mode, false);

  display_info.Zero();
  display_info->front_buffer_width = (uint16_t)mode.display_width;
  display_info->front_buffer_height = (uint16_t)mode.display_height;

  display_info->scaler_parameters.scaler_source_rect.x2 = mode.display_width;
  display_info->scaler_parameters.scaler_source_rect.y2 = mode.display_height;
  display_info->scaler_parameters.scaled_output_width = mode.display_width;
  display_info->scaler_parameters.scaled_output_height = mode.display_height;
  display_info->scaler_parameters.horizontal_filter_type = 1;
  display_info->scaler_parameters.vertical_filter_type = 1;

  display_info->display_window_overscan_left = 320;
  display_info->display_window_overscan_top = 180;
  display_info->display_window_overscan_right = 320;
  display_info->display_window_overscan_bottom = 180;
  display_info->display_width = (uint16_t)mode.display_width;
  display_info->display_height = (uint16_t)mode.display_height;
  display_info->display_refresh_rate = mode.refresh_rate;
  display_info->display_interlaced = mode.is_interlaced;
  display_info->actual_display_width = (uint16_t)mode.display_width;
}
DECLARE_XBOXKRNL_EXPORT1(VdGetCurrentDisplayInformation, kVideo, kStub);

void VdQueryVideoMode(X_VIDEO_MODE* video_mode,
                      [[maybe_unused]] bool is_internal_resolution) {
  // TODO(benvanik): get info from actual display.
  std::memset(video_mode, 0, sizeof(X_VIDEO_MODE));

  // Later calculate if resolution is widescreen or not and apply flag
  // accordingly. Technically possible resolutions should depend on av_pack, but
  // we can ignore it.
  const auto resolution =
      Resolution(kernel_state()->xconfig()->ReadSetting<uint32_t>(
          XCONFIG_USER_CATEGORY, XCONFIG_USER_AV_COMPOSITE_SCREENSZ));

  const auto is_widescreen = IsWidescreen(kernel_state(), resolution);

  video_mode->display_width = resolution.width_.get();
  video_mode->display_height = resolution.height_.get();
  video_mode->is_interlaced = cvars::interlaced;
  video_mode->is_widescreen = is_widescreen;
  video_mode->is_hi_def = video_mode->display_width >= 0x500;
  video_mode->refresh_rate = GetVideoRefreshRate(kernel_state());
  video_mode->video_standard =
      static_cast<uint32_t>(GetVideoStandard(kernel_state()));
  video_mode->pixel_rate = 0x8A;
  video_mode->widescreen_flag = is_widescreen ? 0x01 : 0x03;
}

void VdQueryRealVideoMode_entry(pointer_t<X_VIDEO_MODE> video_mode) {
  VdQueryVideoMode(video_mode, true);
}
DECLARE_XBOXKRNL_EXPORT1(VdQueryRealVideoMode, kVideo, kStub);

void VdQueryVideoMode_entry(pointer_t<X_VIDEO_MODE> video_mode) {
  VdQueryVideoMode(video_mode, false);
}
DECLARE_XBOXKRNL_EXPORT1(VdQueryVideoMode, kVideo, kStub);

dword_result_t VdQueryVideoFlags_entry() {
  X_VIDEO_MODE mode;
  VdQueryVideoMode(&mode, false);

  uint32_t flags = 0;
  flags |= mode.is_widescreen ? 1 : 0;
  flags |= mode.display_width >= 1280 ? 2 : 0;
  flags |= mode.display_width >= 1920 ? 4 : 0;

  return flags;
}
DECLARE_XBOXKRNL_EXPORT1(VdQueryVideoFlags, kVideo, kStub);

dword_result_t VdSetDisplayMode_entry(dword_t flags) {
  // Often 0x40000000.

  // 0?ccf000 00000000 00000000 000000r0

  // r: 0x00000002 |     1
  // f: 0x08000000 |    27
  // c: 0x30000000 | 28-29
  // ?: 0x40000000 |    30

  // r: 1 = Resolution is 720x480 or 720x576
  // f: 1 = Texture format is k_2_10_10_10 or k_2_10_10_10_AS_16_16_16_16
  // c: Color space (0 = RGB, 1 = ?, 2 = ?)
  // ?: (always set?)

  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(VdSetDisplayMode, kVideo, kStub);

dword_result_t VdSetDisplayModeOverride_entry(dword_t width, dword_t height,
                                              double_t refresh_rate,
                                              unknown_t unk3, unknown_t unk4) {
  // refresh_rate = 0, 50, 59.9, etc.
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(VdSetDisplayModeOverride, kVideo, kStub);

dword_result_t VdInitializeEngines_entry(unknown_t unk0, function_t callback,
                                         lpvoid_t arg, lpdword_t pfp_ptr,
                                         lpdword_t me_ptr) {
  // r3 = 0x4F810000
  // r4 = function ptr (cleanup callback?)
  // r5 = function arg
  // r6 = PFP Microcode
  // r7 = ME Microcode
  return 1;
}
DECLARE_XBOXKRNL_EXPORT1(VdInitializeEngines, kVideo, kStub);

void VdShutdownEngines_entry() {
  // Ignored for now.
  // Games seem to call an Initialize/Shutdown pair to query info, then
  // re-initialize.
}
DECLARE_XBOXKRNL_EXPORT1(VdShutdownEngines, kVideo, kStub);

dword_result_t VdGetGraphicsAsicID_entry() {
  // Games compare for < 0x10 and do VdInitializeEDRAM, else other
  // (retrain/etc).
  return 0x11;
}
DECLARE_XBOXKRNL_EXPORT1(VdGetGraphicsAsicID, kVideo, kStub);

dword_result_t VdEnableDisableClockGating_entry(dword_t enabled) {
  // Ignored, as it really doesn't matter.
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(VdEnableDisableClockGating, kVideo, kStub);

void VdSetGraphicsInterruptCallback_entry(function_t callback,
                                          lpvoid_t user_data) {
  // callback takes 2 params
  // r3 = bool 0/1 - 0 is normal interrupt, 1 is some acquire/lock mumble
  // r4 = user_data (r4 of VdSetGraphicsInterruptCallback)
  auto graphics_system = kernel_state()->emulator()->graphics_system();
  graphics_system->SetInterruptCallback(callback, user_data);
}
DECLARE_XBOXKRNL_EXPORT1(VdSetGraphicsInterruptCallback, kVideo, kImplemented);

void VdInitializeRingBuffer_entry(lpvoid_t ptr, int_t size_log2) {
  // r3 = result of MmGetPhysicalAddress
  // r4 = log2(size)
  // Buffer pointers are from MmAllocatePhysicalMemory with WRITE_COMBINE.
  auto graphics_system = kernel_state()->emulator()->graphics_system();
  graphics_system->InitializeRingBuffer(ptr, size_log2);
}
DECLARE_XBOXKRNL_EXPORT1(VdInitializeRingBuffer, kVideo, kImplemented);

void VdEnableRingBufferRPtrWriteBack_entry(lpvoid_t ptr,
                                           int_t block_size_log2) {
  // r4 = log2(block size), 6, usually --- <=19
  auto graphics_system = kernel_state()->emulator()->graphics_system();
  graphics_system->EnableReadPointerWriteBack(ptr, block_size_log2);
}
DECLARE_XBOXKRNL_EXPORT1(VdEnableRingBufferRPtrWriteBack, kVideo, kImplemented);

void VdGetSystemCommandBuffer_entry(lpunknown_t p0_ptr, lpunknown_t p1_ptr) {
  p0_ptr.Zero(0x94);
  xe::store_and_swap<uint32_t>(p0_ptr, 0xBEEF0000);
  xe::store_and_swap<uint32_t>(p1_ptr, 0xBEEF0001);
}
DECLARE_XBOXKRNL_EXPORT1(VdGetSystemCommandBuffer, kVideo, kStub);

void VdSetSystemCommandBufferGpuIdentifierAddress_entry(lpunknown_t unk) {
  // r3 = 0x2B10(d3d?) + 8
}
DECLARE_XBOXKRNL_EXPORT1(VdSetSystemCommandBufferGpuIdentifierAddress, kVideo,
                         kStub);

// VdVerifyMEInitCommand
// r3
// r4 = 19
// no op?

dword_result_t VdInitializeScalerCommandBuffer_entry(
    dword_t scaler_source_xy,      // ((uint16_t)y << 16) | (uint16_t)x
    dword_t scaler_source_wh,      // ((uint16_t)h << 16) | (uint16_t)w
    dword_t scaled_output_xy,      // ((uint16_t)y << 16) | (uint16_t)x
    dword_t scaled_output_wh,      // ((uint16_t)h << 16) | (uint16_t)w
    dword_t front_buffer_wh,       // ((uint16_t)h << 16) | (uint16_t)w
    dword_t vertical_filter_type,  // 7?
    pointer_t<X_D3DFILTER_PARAMETERS> vertical_filter_params,    //
    dword_t horizontal_filter_type,                              // 7?
    pointer_t<X_D3DFILTER_PARAMETERS> horizontal_filter_params,  //
    lpvoid_t unk9,                                               //
    lpvoid_t dest_ptr,  // Points to the first 80000000h where the memcpy
                        // sources from.
    dword_t dest_count  // Count in words.
) {
  // We could fake the commands here, but I'm not sure the game checks for
  // anything but success (non-zero ret).
  // For now, we just fill it with NOPs.
  auto dest = dest_ptr.as_array<uint32_t>();
  for (size_t i = 0; i < dest_count; ++i) {
    dest[i] = 0x80000000;
  }

  uint32_t fb_x = (scaled_output_wh >> 16) & 0xFFFF;
  uint32_t fb_y = scaled_output_wh & 0xFFFF;
  const bool forced_widescreen =
      kernel_state()->xconfig()->ReadSetting<uint32_t>(
          XCONFIG_USER_CATEGORY, XCONFIG_USER_VIDEO_FLAGS) &
      X_VIDEO_FLAGS::Widescreen;

  auto aspect = CalculateScaledAspectRatio(fb_x, fb_y, forced_widescreen);

  auto graphics_system = kernel_state()->emulator()->graphics_system();
  graphics_system->SetScaledAspectRatio(aspect.first, aspect.second);

  return (uint32_t)dest_count;
}
DECLARE_XBOXKRNL_EXPORT2(VdInitializeScalerCommandBuffer, kVideo, kImplemented,
                         kSketchy);

struct BufferScaling {
  xe::be<uint16_t> fb_width;
  xe::be<uint16_t> fb_height;
  xe::be<uint16_t> bb_width;
  xe::be<uint16_t> bb_height;
};
void AppendParam(StringBuffer* string_buffer, pointer_t<BufferScaling> param) {
  string_buffer->AppendFormat(
      "{:08X}(scale {}x{} -> {}x{}))", param.guest_address(),
      uint16_t(param->bb_width), uint16_t(param->bb_height),
      uint16_t(param->fb_width), uint16_t(param->fb_height));
}

// Graphics notification routines. This is how the Guide reaches the screen:
// xam registers a renderer, the title calls the routines during its own
// frame, and xam's callback draws over the title's back buffer, which the
// title then swaps. Xenia previously had no registration function at all and
// a no-op for the call, so the title invoked the hook every frame and nothing
// happened. See research/FINDINGS.md phase 72.
struct GraphicsNotificationRoutine {
  uint32_t callback;
  uint32_t context;
  bool is_xam;
};
static xe::global_critical_region graphics_notification_region_;
static std::vector<GraphicsNotificationRoutine>* graphics_notification_routines_
    = nullptr;

static uint32_t guide_draw_fn_ = 0;
static uint32_t guide_draw_this_ = 0;

void SetGuideDrawHook(uint32_t fn, uint32_t self) {
  guide_draw_fn_ = fn;
  guide_draw_this_ = self;
}

static void RegisterGraphicsNotification(uint32_t callback, uint32_t context,
                                         bool is_xam) {
  auto global_lock = graphics_notification_region_.Acquire();
  if (!graphics_notification_routines_) {
    graphics_notification_routines_ =
        new std::vector<GraphicsNotificationRoutine>();
  }
  if (!callback) {
    return;
  }
  for (auto& r : *graphics_notification_routines_) {
    if (r.callback == callback && r.context == context) {
      return;
    }
  }
  graphics_notification_routines_->push_back({callback, context, is_xam});
  XELOGI("Vd{}RegisterGraphicsNotification: callback {:08X} context {:08X}",
         is_xam ? "Xam" : "", callback, context);
}

dword_result_t VdRegisterGraphicsNotification_entry(dword_t callback,
                                                    dword_t context,
                                                    dword_t unk2) {
  RegisterGraphicsNotification(callback, context, false);
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(VdRegisterGraphicsNotification, kVideo, kImplemented);

dword_result_t VdRegisterXamGraphicsNotification_entry(dword_t callback,
                                                       dword_t context,
                                                       dword_t unk2) {
  RegisterGraphicsNotification(callback, context, true);
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(VdRegisterXamGraphicsNotification, kVideo,
                         kImplemented);

dword_result_t VdCallGraphicsNotificationRoutines_entry(
    unknown_t unk0, pointer_t<BufferScaling> args_ptr) {
  assert_true(unk0 == 1);
  {
    static std::atomic<uint32_t> gcalls{0};
    uint32_t n = ++gcalls;
    if (n <= 3 || (n % 300) == 0) {
      XELOGI("VdCallGraphicsNotificationRoutines #{} (hook={:08X})", n,
             guide_draw_fn_);
    }
  }

  // Callbacks get 0, r3, r4 (per the original TODO here).
  std::vector<GraphicsNotificationRoutine> routines;
  {
    auto global_lock = graphics_notification_region_.Acquire();
    if (graphics_notification_routines_) {
      routines = *graphics_notification_routines_;
    }
  }
  auto* thread = XThread::GetCurrentThread();
  if (!thread) {
    return 0;
  }
  // The Guide draws here, on the title's render thread, inside the title's
  // frame - the only place the title's thread-affine D3D device may be used.
  if (guide_draw_fn_ && guide_draw_this_) {
    uint64_t gargs[] = {guide_draw_this_};
    kernel_state()->processor()->Execute(thread->thread_state(),
                                         guide_draw_fn_, gargs,
                                         xe::countof(gargs));
  }
  if (routines.empty()) {
    return 0;
  }
  for (auto& r : routines) {
    uint64_t args[] = {0, static_cast<uint32_t>(unk0),
                       args_ptr.guest_address()};
    kernel_state()->processor()->Execute(thread->thread_state(), r.callback,
                                         args, xe::countof(args));
  }
  return 0;
}
DECLARE_XBOXKRNL_EXPORT2(VdCallGraphicsNotificationRoutines, kVideo,
                         kImplemented, kSketchy);

dword_result_t VdIsHSIOTrainingSucceeded_entry() {
  // BOOL return value
  return 1;
}
DECLARE_XBOXKRNL_EXPORT1(VdIsHSIOTrainingSucceeded, kVideo, kStub);

dword_result_t VdPersistDisplay_entry(unknown_t unk0, lpdword_t unk1_ptr) {
  // unk1_ptr needs to be populated with a pointer passed to
  // MmFreePhysicalMemory(1, *unk1_ptr).
  if (unk1_ptr) {
    auto heap = kernel_memory()->LookupHeapByType(true, 16 * 1024);
    uint32_t unk1_value;
    heap->Alloc(64, 32, kMemoryAllocationReserve | kMemoryAllocationCommit,
                kMemoryProtectNoAccess, false, &unk1_value);
    *unk1_ptr = unk1_value;
  }

  return 1;
}
DECLARE_XBOXKRNL_EXPORT2(VdPersistDisplay, kVideo, kImplemented, kSketchy);

dword_result_t VdRetrainEDRAMWorker_entry(unknown_t unk0) { return 0; }
DECLARE_XBOXKRNL_EXPORT1(VdRetrainEDRAMWorker, kVideo, kStub);

DEFINE_int32(vd_retrain_edram_result, 0,
             "Value returned by VdRetrainEDRAM. Changes which loop the 17489 "
             "dashboard spins in: 0 keeps its main loop polling input and "
             "tiles, non-zero quiets the guest and spins the command "
             "processor instead. Neither presents a frame. Games do not use "
             "this path.",
             "Video");

dword_result_t VdRetrainEDRAM_entry(unknown_t unk0, unknown_t unk1,
                                    unknown_t unk2, unknown_t unk3,
                                    unknown_t unk4, unknown_t unk5) {
  return static_cast<uint32_t>(cvars::vd_retrain_edram_result);
}
DECLARE_XBOXKRNL_EXPORT2(VdRetrainEDRAM, kVideo, kStub, kHighFrequency);

// Selects studio (16-235) vs full (0-255) RGB output range. The host
// presenter always works in full range.
dword_result_t VdSetStudioRGBMode_entry(dword_t mode) { return 0; }
DECLARE_XBOXKRNL_EXPORT1(VdSetStudioRGBMode, kVideo, kStub);

static uint32_t guide_bs_hud_base_ = 0;
static uint32_t guide_bs_obj_ = 0;
static bool guide_bs_use_title_device_ = false;
static uint32_t guide_bs_skin_module_ = 0;
static std::atomic<bool> guide_bs_ready_{false};

bool GuideBootstrapReady() { return guide_bs_ready_; }
static std::atomic<bool> guide_bs_pending_{false};

void QueueGuideBootstrap(uint32_t hud_base, uint32_t guide_obj,
                         bool use_title_device, uint32_t skin_module) {
  guide_bs_hud_base_ = hud_base;
  guide_bs_obj_ = guide_obj;
  guide_bs_use_title_device_ = use_title_device;
  guide_bs_skin_module_ = skin_module;
  guide_bs_pending_ = true;
}

// Runs the XUI bootstrap on the title's render thread. Everything that touches
// the device must happen here: doing it from the Guide's own thread makes the
// guest D3D runtime refuse with "trying to use a D3D device object that is
// owned by a different thread".
static void RunGuideBootstrapOnTitleThread(XThread* thread) {
  auto* memory = kernel_state()->memory();
  auto rd = [&](uint32_t a) {
    return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(a));
  };
  auto* processor = kernel_state()->processor();
  auto* ts = thread->thread_state();

  if (guide_bs_use_title_device_) {
    uint32_t title_dev = rd(0x801E6FC4u);
    if (title_dev) {
      xe::store_and_swap<uint32_t>(memory->TranslateVirtual(0x81D43684u),
                                   title_dev);
      XELOGI("GuideBootstrap: xam device global -> title device {:08X}",
             title_dev);
    }
  }

  uint64_t a0[] = {0};
  uint64_t hr = processor->Execute(ts, 0x8178DC58u, a0, xe::countof(a0));
  XELOGI("GuideBootstrap: render host -> {:08X}, XUI ctx {:08X}, "
         "provider {:08X}",
         static_cast<uint32_t>(hr), rd(0x81D6C978u), rd(0x81D6D0ACu));

  {
    // The XUI resource provider is a static xam object; its vtable[1] is the
    // open-by-name call that is failing with 80300004.
    uint32_t prov = rd(0x81D6D0ACu);
    uint32_t pvt = prov ? rd(prov) : 0;
    XELOGI("GuideBootstrap: provider {:08X} vtable {:08X} [0]={:08X} "
           "[1]={:08X} [2]={:08X}",
           prov, pvt, pvt ? rd(pvt) : 0, pvt ? rd(pvt + 4) : 0,
           pvt ? rd(pvt + 8) : 0);
  }
  // Run xam's extra XUI class registrars here, AFTER the host has called
  // XuiInit. Running them before (lle_xam_xui_init) made XuiInit fail on the
  // duplicate "XuiElement" - see phase 67. Classes and cached resources share
  // the registry at 81D6D508, and a scene cannot instantiate a class that is
  // not in it.
  if (::cvars::guide_register_classes) {
    for (uint32_t reg : {0x817503E8u, 0x8199BE08u, 0x8176B2C8u}) {
      uint64_t rargs[] = {0};
      uint64_t rr = processor->Execute(ts, reg, rargs, xe::countof(rargs));
      XELOGI("GuideBootstrap: registrar {:08X} -> {:08X}", reg,
             static_cast<uint32_t>(rr));
    }
  }
  uint32_t dcp = memory->SystemHeapAlloc(16, 16);
  uint64_t a1[] = {dcp};
  uint64_t dr = processor->Execute(ts, 0x818FB038u, a1, xe::countof(a1));
  XELOGI("GuideBootstrap: XuiRenderCreateDC -> {:08X} dc={:08X}",
         static_cast<uint32_t>(dr), rd(dcp));

  uint32_t render_obj = guide_bs_obj_ + 16;
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(render_obj + 20), 1u);
  // Call the Guide object's own scene creator (vtable[27] = hud 913EB940)
  // rather than the bare init. It takes (this, a, b), stores a at [this+28],
  // calls the init itself with this+16, and then builds the scene via
  // XuiSceneCreate. Calling the init directly gives a root element with no
  // scene under it, which draws nothing.
  // hud reads [guide+4] as the module for XamBuildResourceLocator and its
  // constructor leaves it 0, which builds an empty locator. Setting it to
  // hud's own hmodule is load-bearing: without it the scene creator fails
  // D0000034, with it the failure moves past resource lookup to 80300004.
  // (The handle that reaches XexGetModuleSection comes from somewhere else
  // in hud - both values matter.)
  if (guide_bs_skin_module_) {
    xe::store_and_swap<uint32_t>(
        memory->TranslateVirtual(guide_bs_obj_ + 4), guide_bs_skin_module_);
    XELOGI("GuideBootstrap: [guide+4] = skin module {:08X}",
           guide_bs_skin_module_);
  }
  {
    // hud's init ends in a virtual call to the render sub-object's vtable[7]
    // (913ea924: lwz r10,0(r31) / lwz r11,28(r10) / bctrl). That call is where
    // it hangs once the extra XUI classes are registered.
    uint32_t rvt = rd(render_obj);
    XELOGI("GuideBootstrap: render obj {:08X} vtable {:08X} [7]={:08X} "
           "[1]={:08X}",
           render_obj, rvt, rvt ? rd(rvt + 28) : 0, rvt ? rd(rvt + 4) : 0);
  }
  // First call in that registration routine is xam's
  // GamerCardRegisterControls; read its thunk to get the real target.
  XELOGI("GuideBootstrap: thunk 913FEA04: {:08X} {:08X} {:08X} {:08X}",
         rd(0x913FEA04u), rd(0x913FEA08u), rd(0x913FEA0Cu), rd(0x913FEA10u));
  if (::cvars::guide_step_registrations) {
    // hud's init tail-calls 913F0DB0, a flat sequence of 56 registrations.
    // Driving them one at a time here shows exactly which one blocks, which a
    // single call into the whole routine cannot.
    static const uint32_t kRegs[] = {
        0x913FEA04u, 0x913EE1A0u, 0x913EF358u, 0x913EF3F0u, 0x913EF488u,
        0x913EF520u, 0x913EF5B8u, 0x913EFC18u, 0x913EDF10u, 0x913EDFA8u,
        0x913EE9A8u, 0x913F0BD8u, 0x913EE0C0u, 0x913F0C80u, 0x913EFE48u,
        0x913EFDB0u, 0x913EFEE0u, 0x913EFF78u, 0x913F0010u, 0x913F0568u,
        0x913F0600u, 0x913F0308u, 0x913F03A0u, 0x913F08F8u, 0x913F00A8u,
        0x913F0140u, 0x913F01D8u, 0x913F0270u, 0x913F0438u, 0x913F0698u,
        0x913F04D0u, 0x913F0730u, 0x913F07C8u, 0x913F0860u, 0x913EE5B8u,
        0x913F0D18u, 0x913F0AA8u, 0x913EF8A0u, 0x913F0B40u, 0x913EF6C8u,
        0x913F0990u, 0x913FE934u, 0x913ECE40u, 0x913ECD28u, 0x913EDA30u,
        0x913EFAE8u, 0x913EFB80u, 0x913EDB00u, 0x913EDB98u, 0x913EDC30u,
        0x913EDCC8u, 0x913EDDF8u, 0x913EDD60u, 0x913EF9B8u, 0x913EE910u,
        0x913EFA50u};
    for (size_t i = 0; i < xe::countof(kRegs); ++i) {
      XELOGI("GuideBootstrap: reg[{}] {:08X} calling", i, kRegs[i]);
      uint64_t ra[] = {0};
      uint64_t rr = processor->Execute(ts, kRegs[i], ra, xe::countof(ra));
      XELOGI("GuideBootstrap: reg[{}] {:08X} -> {:08X}", i, kRegs[i],
             static_cast<uint32_t>(rr));
    }
    XELOGI("GuideBootstrap: all registrations done");
  }
  uint32_t obj_vt = rd(guide_bs_obj_);
  uint32_t scene_fn = obj_vt ? rd(obj_vt + 27 * 4) : 0;
  if (::cvars::guide_init_only) {
    scene_fn = 0;  // fall through to the bare-init path below
  }
  if (::cvars::guide_scene_off_thread) {
    // Leave scene creation to the Guide's own thread; install the draw hook
    // and hand off. Holding the render thread through an async scene load
    // deadlocks it.
    if (::cvars::guide_install_draw_hook) {
      SetGuideDrawHook(guide_bs_hud_base_ + 0xAB28u, render_obj);
    } else {
      XELOGI("GuideBootstrap: draw hook NOT installed (test)");
    }
    guide_bs_ready_ = true;
    XELOGI("GuideBootstrap: device work done; scene creation handed off");
    return;
  }
  uint64_t ir = 0;
  if (scene_fn) {
    uint64_t a2[] = {guide_bs_obj_, 0, 0};
    ir = processor->Execute(ts, scene_fn, a2, xe::countof(a2));
    XELOGI("GuideBootstrap: scene creator {:08X} -> {:08X}", scene_fn,
           static_cast<uint32_t>(ir));
  } else {
    // Mirror what hud's scene creator sets before it calls the init:
    // [obj+28] = second arg, [obj+32] = 1 (render_obj+16),
    // [obj+64] = 1 (render_obj+48). If the hang follows these fields rather
    // than the call site, this direct call will hang too.
    if (::cvars::guide_preset_fields) {
      xe::store_and_swap<uint32_t>(
          memory->TranslateVirtual(guide_bs_obj_ + 28), 0u);
      xe::store_and_swap<uint32_t>(
          memory->TranslateVirtual(guide_bs_obj_ + 32), 1u);
      xe::store_and_swap<uint32_t>(
          memory->TranslateVirtual(guide_bs_obj_ + 64), 1u);
      XELOGI("GuideBootstrap: preset [obj+28,32,64] as the scene creator does");
    }
    uint64_t a2[] = {render_obj, 0};
    ir = processor->Execute(ts, guide_bs_hud_base_ + 0xA898u, a2,
                            xe::countof(a2));
  }
  XELOGI("GuideBootstrap: after init -> {:08X}  +8={:08X} +12={:08X}",
         static_cast<uint32_t>(ir), rd(render_obj + 8), rd(render_obj + 12));

  // hud's scene creator reads its skin/scene strings from these globals
  // (XuiSceneCreate gets [91400170] as the scene file). If they are null the
  // Guide has no content to build, which is what an empty root element means.
  XELOGI("GuideBootstrap: hud globals 91400168={:08X} 91400170={:08X} "
         "91400690={:08X}",
         rd(0x91400168u), rd(0x91400170u), rd(0x91400690u));
  // Scene loading stops at 80300004: xam's XUI resource-provider global
  // (81D6C978's neighbour at 81D6D0AC) is null. XuiInit copies it from
  // params[+8], but only when params[0] >= 0xC, and hud's init builds default
  // params {12, 0, 0} whenever its second argument is zero - which is what
  // hud's own scene creator and this bootstrap both pass. The host is meant to
  // supply a params struct carrying the provider: a C++ object whose vtable[1]
  // opens a resource by name (ghidra 81987F24). xam has a one-argument setter
  // for it at ghidra 81946AE0 / runtime 8193F8E0, so installing one is easy
  // once we know which object to install. Letting hud call XuiInit itself
  // does not help - its params are null too.
  SetGuideDrawHook(guide_bs_hud_base_ + 0xAB28u, render_obj);
  XELOGI("GuideBootstrap: draw hook installed on title thread");
}

void VdSwap_entry(
    lpvoid_t buffer_ptr,        // ptr into primary ringbuffer
    lpvoid_t fetch_ptr,         // frontbuffer Direct3D 9 texture header fetch
    lpunknown_t unk2,           // system writeback ptr
    lpunknown_t unk3,           // buffer from VdGetSystemCommandBuffer
    lpunknown_t unk4,           // from VdGetSystemCommandBuffer (0xBEEF0001)
    lpdword_t frontbuffer_ptr,  // ptr to frontbuffer address
    lpdword_t texture_format_ptr, lpdword_t color_space_ptr, lpdword_t width,
    lpdword_t height) {
  {
    // Which thread is presenting? The Guide renders through xam's D3D device
    // while the title renders through its own; if only one thread ever swaps,
    // the Guide's frames never reach the presenter.
    static std::atomic<uint32_t> swap_count{0};
    uint32_t n = ++swap_count;
    if (n <= 3 || (n % 300) == 0) {
      auto* th = XThread::GetCurrentThread();
      XELOGI("VdSwap #{} from thread '{}'", n, th ? th->name() : "<none>");
    }
  }
  // Composite the Guide here. The title's D3D device is thread-affine and
  // this runs on the thread that owns it, inside the title's frame and just
  // before its swap - which is where the Guide is drawn on hardware. The
  // title calls VdCallGraphicsNotificationRoutines only once at startup, so
  // that is not the per-frame path.
  if (guide_bs_pending_.exchange(false)) {
    auto* bth = XThread::GetCurrentThread();
    if (bth) {
      RunGuideBootstrapOnTitleThread(bth);
    }
  }
  if (guide_draw_fn_ && guide_draw_this_) {
    static thread_local bool in_guide_draw = false;
    auto* gth = XThread::GetCurrentThread();
    if (gth && !in_guide_draw) {
      in_guide_draw = true;
      uint64_t gargs[] = {guide_draw_this_};
      uint64_t gr = kernel_state()->processor()->Execute(
          gth->thread_state(), guide_draw_fn_, gargs, xe::countof(gargs));
      static std::atomic<uint32_t> gdraws{0};
      uint32_t gn = ++gdraws;
      if (gn <= 3 || (gn % 300) == 0) {
        XELOGI("Guide composite draw #{} -> {:08X}", gn,
               static_cast<uint32_t>(gr));
      }
      in_guide_draw = false;
    }
  }
  // All of these parameters are REQUIRED.
  assert(buffer_ptr);
  assert(fetch_ptr);
  assert(frontbuffer_ptr);
  assert(texture_format_ptr);
  assert(width);
  assert(height);

  namespace xenos = xe::gpu::xenos;

  xenos::xe_gpu_texture_fetch_t gpu_fetch;
  xe::copy_and_swap_32_unaligned(
      &gpu_fetch, reinterpret_cast<uint32_t*>(fetch_ptr.host_address()), 6);

  // The fetch constant passed is not a true GPU fetch constant, but rather, the
  // fetch constant stored in the Direct3D 9 texture header, which contains the
  // address in one of the virtual mappings of the physical memory rather than
  // the physical address itself. We're emulating swapping in the GPU subsystem,
  // which works with GPU memory addresses (physical addresses directly) from
  // proper fetch constants like ones used to bind textures to shaders, not CPU
  // MMU addresses, so translation from virtual to physical is needed.
  uint32_t frontbuffer_virtual_address = gpu_fetch.base_address << 12;
  assert_true(*frontbuffer_ptr == frontbuffer_virtual_address);
  uint32_t frontbuffer_physical_address =
      kernel_memory()->GetPhysicalAddress(frontbuffer_virtual_address);
  assert_true(frontbuffer_physical_address != UINT32_MAX);
  if (frontbuffer_physical_address == UINT32_MAX) {
    // Xenia-specific safety check.
    XELOGE("VdSwap: Invalid front buffer virtual address 0x{:08X}",
           frontbuffer_virtual_address);
    return;
  }
  gpu_fetch.base_address = frontbuffer_physical_address >> 12;
  XE_MAYBE_UNUSED
  auto texture_format = gpu::xenos::TextureFormat(texture_format_ptr.value());
  auto color_space = *color_space_ptr;
  assert_true(texture_format == gpu::xenos::TextureFormat::k_8_8_8_8 ||
              texture_format ==
                  gpu::xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16);
  assert_true(color_space == 0);  // RGB(0)
  assert_true(*width == 1 + gpu_fetch.size_2d.width);
  assert_true(*height == 1 + gpu_fetch.size_2d.height);

  // The caller seems to reserve 64 words (256b) in the primary ringbuffer
  // for this method to do what it needs. We just zero them out and send a
  // token value. It'd be nice to figure out what this is really doing so
  // that we could simulate it, though due to TCR I bet all games need to
  // use this method.
  buffer_ptr.Zero(64 * 4);

  uint32_t offset = 0;
  auto dwords = buffer_ptr.as_array<uint32_t>();

  // Write in the GPU texture fetch.
  dwords[offset++] =
      xenos::MakePacketType0(gpu::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 6);
  dwords[offset++] = gpu_fetch.dword_0;
  dwords[offset++] = gpu_fetch.dword_1;
  dwords[offset++] = gpu_fetch.dword_2;
  dwords[offset++] = gpu_fetch.dword_3;
  dwords[offset++] = gpu_fetch.dword_4;
  dwords[offset++] = gpu_fetch.dword_5;

  dwords[offset++] = xenos::MakePacketType3(xenos::PM4_XE_SWAP, 4);
  dwords[offset++] = xe::gpu::xenos::kSwapSignature;
  dwords[offset++] = frontbuffer_physical_address;

  dwords[offset++] = *width;
  dwords[offset++] = *height;

  // Fill the rest of the buffer with NOP packets.
  for (uint32_t i = offset; i < 64; i++) {
    dwords[i] = xenos::MakePacketType2();
  }
}
DECLARE_XBOXKRNL_EXPORT3(VdSwap, kVideo, kImplemented, kHighFrequency,
                         kImportant);

void RegisterVideoExports(xe::cpu::ExportResolver* export_resolver,
                          KernelState* kernel_state) {
  auto memory = kernel_state->memory();

  // Allocate single page that stores all pointers instead of separate pages.
  const uint32_t baseAllocation =
      memory->SystemHeapAlloc(40, 32, kSystemHeapPhysical);  // 40 bytes

  // VdGlobalDevice (4b)
  // Pointer to a global D3D device. Games only seem to set this, so we don't
  // have to do anything. We may want to read it back later, though.
  const uint32_t pVdGlobalDevice = 0x801E6FC4;
  export_resolver->SetVariableMapping("xboxkrnl.exe", ordinals::VdGlobalDevice,
                                      pVdGlobalDevice);
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(pVdGlobalDevice),
                               baseAllocation);

  // VdGlobalXamDevice (4b)
  // Pointer to the XAM D3D device, which we don't have.
  const uint32_t pVdGlobalXamDevice = 0x801E6FC8;
  export_resolver->SetVariableMapping(
      "xboxkrnl.exe", ordinals::VdGlobalXamDevice, pVdGlobalXamDevice);
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(pVdGlobalXamDevice), 0);

  // VdGpuClockInMHz (4b)
  // GPU clock. Xenos is 500MHz. Hope nothing is relying on this timing...
  const uint32_t pVdGpuClockInMHz = 0x801D0E94;
  export_resolver->SetVariableMapping("xboxkrnl.exe", ordinals::VdGpuClockInMHz,
                                      pVdGpuClockInMHz);
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(pVdGpuClockInMHz), 500);

  // VdHSIOCalibrationLock (28b)
  // CriticalSection.
  const uint32_t pVdHSIOCalibrationLock = 0x801D1210;
  export_resolver->SetVariableMapping(
      "xboxkrnl.exe", ordinals::VdHSIOCalibrationLock, pVdHSIOCalibrationLock);
  auto hsio_lock =
      memory->TranslateVirtual<X_RTL_CRITICAL_SECTION*>(pVdHSIOCalibrationLock);
  xeRtlInitializeCriticalSectionAndSpinCount(hsio_lock, pVdHSIOCalibrationLock,
                                             10000);
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
