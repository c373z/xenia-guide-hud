/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <map>
#include <mutex>
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_video.h"

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/cpu/function.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xmodule.h"
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

// Set while the Guide draw is executing, so kernel entries can tell
// whether a call came from the Guide or from the title.
thread_local bool in_guide_draw_scope = false;
// Set while xam's D3D device creation runs, to see which kernel
// video calls that path makes.
thread_local bool in_xam_createdevice_scope = false;
uint32_t guide_syscmdbuf_ptr_ = 0;
uint32_t guide_syscmdbuf_size_ = 0;


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

void VdGetSystemCommandBuffer_entry(lpunknown_t p0_ptr, lpunknown_t p1_ptr,
                                    const ppc_context_t& context) {
  {
    // Who is actually calling this 600,000 times? A back-edge analysis
    // named a loop that runs 36 times, so ask the caller directly
    // instead of reasoning about control flow.
    static std::atomic<uint32_t> lrn{0};
    static std::mutex lrmtx;
    static std::map<uint32_t, uint32_t> lrs;
    uint32_t n = ++lrn;
    uint32_t lr = static_cast<uint32_t>(context->lr);
    {
      std::lock_guard<std::mutex> g(lrmtx);
      lrs[lr]++;
      if (n == 1 || n == 200000) {
        // Walk the guest back chain, as the crash reporter does: [sp] is the
        // caller's frame and its return address is [caller_sp - 8]. The LR
        // alone only names the innermost caller, and that one turned out to
        // be called from somewhere else in a loop.
        auto* um = kernel_state()->memory();
        uint32_t sp = static_cast<uint32_t>(context->r[1]);
        std::string bt;
        for (int f = 0; f < 10 && sp; ++f) {
          uint32_t caller_sp =
              xe::load_and_swap<uint32_t>(um->TranslateVirtual(sp));
          if (caller_sp <= sp || caller_sp - sp > 0x10000) break;
          uint32_t ra =
              xe::load_and_swap<uint32_t>(um->TranslateVirtual(caller_sp - 8));
          if (ra < 0x81000000u || ra >= 0x93000000u) break;
          bt += fmt::format("{:08X} ", ra);
          sp = caller_sp;
        }
        XELOGI("SysCmdBufCaller #{}: unwind {}", n, bt);
      }
      if (n == 200000 || n == 600000) {
        for (auto& kv : lrs) {
          XELOGI("SysCmdBufCaller: lr {:08X} x{}", kv.first, kv.second);
        }
      }
    }
  }
  {
    // Is this mechanism used at all? The Guide's drawing is expected to reach
    // the GPU through the system command buffer, and this stub hands back two
    // magic constants instead of one.
    static std::atomic<uint32_t> n{0};
    uint32_t c = ++n;
    if (in_guide_draw_scope || in_xam_createdevice_scope || c <= 5 ||
        (c % 2000) == 0) {
      auto* th = XThread::GetCurrentThread();
      XELOGI("VdGetSystemCommandBuffer #{}{} from '{}' p0={:08X} p1={:08X}", c,
             in_guide_draw_scope      ? " [GUIDE DRAW]"
             : in_xam_createdevice_scope ? " [XAM CREATEDEVICE]"
                                         : "",
             th ? th->name() : std::string("<none>"), p0_ptr.guest_address(),
             p1_ptr.guest_address());
    }
  }
  p0_ptr.Zero(0x94);
  xe::store_and_swap<uint32_t>(p0_ptr, 0xBEEF0000);
  xe::store_and_swap<uint32_t>(p1_ptr, 0xBEEF0001);
  if (::cvars::guide_syscmdbuf_buffer_kb > 0) {
    static uint32_t buf = 0, buf_size = 0;
    if (!buf) {
      buf_size = static_cast<uint32_t>(::cvars::guide_syscmdbuf_buffer_kb) * 1024;
      buf = kernel_state()->memory()->SystemHeapAlloc(buf_size, 4096);
      if (buf) {
        std::memset(kernel_state()->memory()->TranslateVirtual(buf), 0,
                    buf_size);
        XELOGI("VdGetSystemCommandBuffer: handing guest buffer {:08X} size {}",
               buf, buf_size);
      }
    }
    if (buf) {
      auto* b = reinterpret_cast<uint8_t*>(p0_ptr.host_address());
      xe::store_and_swap<uint32_t>(b + 0x04, buf);
      xe::store_and_swap<uint32_t>(b + 0x08, buf_size);
      guide_syscmdbuf_ptr_ = buf;
      guide_syscmdbuf_size_ = buf_size;
      // Under the forced-wait livelock the guest asks for this buffer tens of
      // thousands of times a second. Sample rarely: has it ever written into
      // what we handed it? The earlier answer was no, but that was measured in
      // the stable configuration where the guest was not retrying at all.
      static std::atomic<uint32_t> acq{0};
      uint32_t an = ++acq;
      if (an == 1 || (an % 50000) == 0) {
        auto* w = reinterpret_cast<const uint32_t*>(
            kernel_state()->memory()->TranslateVirtual(buf));
        uint32_t nz = 0;
        for (uint32_t i = 0; i < buf_size / 4; ++i) {
          if (w[i]) ++nz;
        }
        XELOGI("SysCmdBufAcq #{}: guest has written {} non-zero words into "
               "the buffer we handed it",
               an, nz);
      }
    }
  }
  if (::cvars::guide_syscmdbuf_fields) {
    // 819FE138 reads p0+30 and p0+34 and compares them against 0x500 and
    // 0x5BE. They are the only fields of the 0x94-byte descriptor with an
    // observed reader.
    auto* base = reinterpret_cast<uint8_t*>(p0_ptr.host_address());
    xe::store_and_swap<uint32_t>(base + 0x30, 0x500);
    xe::store_and_swap<uint32_t>(base + 0x34, 0x5BE);
    static std::atomic<uint32_t> once{0};
    if (once++ == 0) {
      XELOGI("VdGetSystemCommandBuffer: filled p0+30=0x500 p0+34=0x5BE");
    }
  }
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

uint32_t guide_prev_device_ = 0;
static uint32_t guide_draw_fn_ = 0;
static uint32_t guide_cmdbuf_base_ = 0;
static uint32_t guide_cmdbuf_size_ = 0;
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

static gpu::CommandProcessor::GuideRing saved_ring_{};
static bool saved_ring_valid_ = false;

void GuideSaveTitleRing() {
  auto* gs = kernel_state()->emulator()->graphics_system();
  if (!gs || !gs->command_processor()) {
    return;
  }
  saved_ring_ = gs->command_processor()->GuideRingSave();
  saved_ring_valid_ = true;
  XELOGI("GuideRing: saved title ring ptr={:08X} size={:08X} wb={:08X}",
         saved_ring_.ptr, saved_ring_.size, saved_ring_.wb);
}

void GuideRestoreTitleRing() {
  if (!saved_ring_valid_) {
    return;
  }
  auto* gs = kernel_state()->emulator()->graphics_system();
  if (!gs || !gs->command_processor()) {
    return;
  }
  uint32_t p = 0, s = 0, w = 0;
  gs->command_processor()->GuideRingState(&p, &s, &w);
  if (p == saved_ring_.ptr) {
    return;  // already the title's ring
  }
  saved_ring_valid_ = false;
  gs->command_processor()->GuideRingRestore(saved_ring_);
  XELOGI("GuideRing: restored title ring ptr {:08X}->{:08X} "
         "size {:08X}->{:08X}",
         p, saved_ring_.ptr, s, saved_ring_.size);
}

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
  // guide_bootstrap_before_device queues this ahead of the device
  // creation, because the mode-1 creator never returns. That means
  // VdGlobalXamDevice can still be null when the first swap picks the
  // bootstrap up, and the guide_use_bound_device redirect then no-ops
  // against a null device. Re-arm and retry on the next swap instead of
  // blocking the title's render thread waiting for it.
  if (::cvars::guide_use_bound_device && !rd(0x801E6FC8u)) {
    static uint32_t waits = 0;
    if (++waits <= 600) {
      guide_bs_pending_ = true;
      if (waits == 1 || waits % 120 == 0) {
        XELOGI("GuideBootstrap: deferring, VdGlobalXamDevice still null "
               "(swap {})", waits);
      }
      return;
    }
    XELOGW("GuideBootstrap: VdGlobalXamDevice never appeared after {} "
           "swaps; proceeding without the redirect", waits);
  }

  if (guide_bs_use_title_device_) {
    uint32_t title_dev = rd(0x801E6FC4u);
    if (title_dev) {
      xe::store_and_swap<uint32_t>(memory->TranslateVirtual(0x81D43684u),
                                   title_dev);
      XELOGI("GuideBootstrap: xam device global -> title device {:08X}",
             title_dev);
    }
  }

  {
    // xam's render host first checks that the caller is the thread recorded at
    // 0x81D42520, comparing it against [r13+256]. Log both: if the recorded
    // slot is zero, xam never registered a UI thread and the check can never
    // pass no matter which thread we use.
    uint32_t recorded = rd(0x81D42520u);
    uint32_t r13 = static_cast<uint32_t>(ts->context()->r[13]);
    uint32_t current = r13 ? rd(r13 + 256) : 0;
    XELOGI("GuideBootstrap: xam UI thread recorded={:08X} current={:08X} "
           "(r13={:08X})",
           recorded, current, r13);
  }
  if (::cvars::guide_xui_anim_init) {
    // 8174FDE0 is the only writer of the CHUDBkgndScene singleton slot at
    // 81D3F924, and refs.py finds it referenced nowhere in xam at all - no
    // direct call, no stored pointer, no register-formed address - so it is
    // invoked from outside the module. The cvar naming it has existed all
    // along with an accurate description and was never wired to anything.
    // Calling the real initialiser is strictly better than the zeroed stub
    // guide_skip_bkgnd_transition installs.
    uint32_t before = rd(0x81D3F924u);
    uint64_t ia[] = {0};
    uint64_t ir = processor->Execute(ts, ::cvars::guide_xui_anim_init, ia,
                                     xe::countof(ia));
    XELOGI("GuideBootstrap: XUI anim init {:08X} -> {:08X}; [81D3F924] "
           "{:08X} -> {:08X}",
           static_cast<uint32_t>(::cvars::guide_xui_anim_init),
           static_cast<uint32_t>(ir), before, rd(0x81D3F924u));
  }
  if (::cvars::guide_skip_bkgnd_transition) {
    // Do NOT patch guest code here - writing to a code page faults the host.
    // Instead give the CHUDBkgndScene singleton slot a zeroed object so
    // PlayTransition's dereference lands on readable memory. This is a probe,
    // not a fix: the object has no vtable and no state, so it only answers
    // whether anything downstream of the transition can still run.
    uint32_t cur = rd(0x81D3F924u);
    if (!cur) {
      uint32_t stub = memory->SystemHeapAlloc(0x400, 16);
      if (stub) {
        std::memset(memory->TranslateVirtual(stub), 0, 0x400);
        xe::store_and_swap<uint32_t>(memory->TranslateVirtual(0x81D3F924u),
                                     stub);
        XELOGI("GuideBootstrap: CHUDBkgndScene slot 81D3F924 = stub {:08X}",
               stub);
      }
    }
  }
  uint32_t saved_ui_thread = 0;
  bool spoofed = false;
  if (::cvars::guide_spoof_ui_thread) {
    uint32_t r13 = static_cast<uint32_t>(ts->context()->r[13]);
    uint32_t current = r13 ? rd(r13 + 256) : 0;
    if (current) {
      saved_ui_thread = rd(0x81D42520u);
      xe::store_and_swap<uint32_t>(memory->TranslateVirtual(0x81D42520u),
                                   current);
      spoofed = true;
      XELOGI("GuideBootstrap: spoofing xam UI thread {:08X} -> {:08X}",
             saved_ui_thread, current);
    }
  }
  if (::cvars::guide_use_bound_device) {
    uint32_t bound = rd(0x801E6FC8u);
    uint32_t cur = rd(0x81D43684u);
    if (bound && bound != cur) {
      xe::store_and_swap<uint32_t>(memory->TranslateVirtual(0x81D43684u),
                                   bound);
      guide_prev_device_ = cur;  // keep the displaced device visible
      XELOGI("GuideBootstrap: xam device global {:08X} (RT0={:08X}) -> "
             "{:08X} (RT0={:08X})",
             cur, cur ? rd(cur + 0x32A0u) : 0, bound, rd(bound + 0x32A0u));
    } else {
      XELOGI("GuideBootstrap: device global unchanged ({:08X}, bound={:08X})",
             cur, bound);
    }
  }
  uint64_t a0[] = {0};
  uint64_t hr = processor->Execute(ts, 0x8178DC58u, a0, xe::countof(a0));
  if (spoofed) {
    xe::store_and_swap<uint32_t>(memory->TranslateVirtual(0x81D42520u),
                                 saved_ui_thread);
    XELOGI("GuideBootstrap: restored xam UI thread {:08X}", saved_ui_thread);
  }
  XELOGI("GuideBootstrap: render host -> {:08X}, XUI ctx {:08X}, "
         "provider {:08X}",
         static_cast<uint32_t>(hr), rd(0x81D6C978u), rd(0x81D6D0ACu));

  {
    // Read-only: the null-render flag as the render host left it. It is
    // computed, not a constant, so what it depends on is worth probing - the
    // hardware-info word is the cheapest input to vary.
    uint32_t c0 = rd(0x81D6C978u);
    XELOGI("GuideBootstrap: null-render flag [ctx+1C] = {:08X} "
           "(hw info word = {:08X})",
           c0 ? rd(c0 + 0x1Cu) : 0xFFFFFFFFu,
           rd(rd(0x815F048Cu)));
  }
  if (::cvars::guide_clear_null_render) {
    uint32_t ctx = rd(0x81D6C978u);
    if (ctx) {
      uint32_t before = rd(ctx + 0x1Cu);
      xe::store_and_swap<uint32_t>(memory->TranslateVirtual(ctx + 0x1Cu), 0);
      XELOGI("GuideBootstrap: XUI ctx {:08X} [1C] {:08X} -> 00000000 "
             "(null-render flag cleared at source)",
             ctx, before);
    }
  }

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
  if (::cvars::guide_register_all_classes) {
    // xam has 39 per-class registrars; the registry otherwise ends up
    // with only 16 entries. A scene asking for an unregistered control
    // class is exactly what E_FAIL from XuiSceneCreate looks like.
    static const uint32_t kRegs[] = {
        0x8194F3B8u, 0x8194F860u, 0x8194F950u, 0x8194FAC0u,
        0x8194FBE8u, 0x8194FCD8u, 0x8194FDC8u, 0x8194FEB0u,
        0x8194FFA0u, 0x81950090u, 0x81950178u, 0x81950268u,
        0x81950350u, 0x819504C0u, 0x819505B0u, 0x819506A0u,
        0x819507D0u, 0x819508C0u, 0x819509B0u, 0x81950AA0u,
        0x81950B88u, 0x81950C78u, 0x81950D68u, 0x81950E58u,
        0x81950F48u, 0x81951038u, 0x81951128u, 0x81951218u,
        0x81951308u, 0x819513F8u, 0x81952428u, 0x819524D0u,
        0x81952580u, 0x81952628u, 0x81953298u, 0x81953338u,
        0x819533E8u, 0x819536B0u, 0x81970290u,
    };
    // No arguments (each builds its own descriptor) and no HRESULT, so a
    // non-zero return is not a failure. Log values instead of scoring them.
    std::string rets;
    for (uint32_t ra : kRegs) {
      uint64_t aa[] = {0};
      uint64_t rr2 = processor->Execute(ts, ra, aa, xe::countof(aa));
      rets += fmt::format("{:08X} ", static_cast<uint32_t>(rr2));
    }
    XELOGI("GuideBootstrap: called {} class registrars, returns: {}",
           uint32_t(xe::countof(kRegs)), rets);
  }
  if (::cvars::guide_register_classes) {
    for (uint32_t reg : {0x817503E8u, 0x8199BE08u, 0x8176B2C8u}) {
      uint64_t rargs[] = {0};
      uint64_t rr = processor->Execute(ts, reg, rargs, xe::countof(rargs));
      XELOGI("GuideBootstrap: registrar {:08X} -> {:08X}", reg,
             static_cast<uint32_t>(rr));
    }
  }
  uint32_t dcp = memory->SystemHeapAlloc(16, 16);
  uint64_t dr = 0;
  if (::cvars::guide_bootstrap_create_dc) {
    uint64_t a1[] = {dcp};
    dr = processor->Execute(ts, 0x818FB038u, a1, xe::countof(a1));
  } else {
    XELOGI("GuideBootstrap: skipping our own XuiRenderCreateDC");
  }
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
    XELOGI("GuideBootstrap: guide object {:08X}, [guide+4] = skin module "
           "{:08X}",
           guide_bs_obj_, guide_bs_skin_module_);
    // hud's string-table load builds its locator from a container string at
    // 913E1B24 and a resource pointer at [91400160]. Read them out of live
    // guest memory - the extracted hud image has relocations applied, so
    // file-offset arithmetic for its .rdata is not trustworthy.
    {
      auto wide_at = [&](uint32_t addr) {
        std::string out;
        auto* hp = memory->LookupHeap(addr);
        if (!hp || hp->QueryRangeAccess(addr, addr + 1) ==
                       xe::memory::PageAccess::kNoAccess) {
          return std::string("(unmapped)");
        }
        for (uint32_t i = 0; i < 64; ++i) {
          uint16_t c = xe::load_and_swap<uint16_t>(
              memory->TranslateVirtual(addr + i * 2));
          if (!c) break;
          out += (c >= 0x20 && c < 0x7F) ? char(c) : '?';
        }
        return out;
      };
      uint32_t res_ptr = rd(0x91400160u);
      XELOGI("GuideBootstrap: hud container@913E1B24 = \"{}\"; "
             "[91400160] = {:08X} -> \"{}\"",
             wide_at(0x913E1B24u), res_ptr,
             res_ptr ? wide_at(res_ptr) : std::string("(null)"));
    }
  }
  // hud loads its own string table into [guide+0x4E8] and the load fails
  // silently - the assign helper ignores the return value - so the field stays
  // null and hud later copies from a null string (crash at 913FA204 in a
  // UTF-16 copy loop). Calling XuiLoadStringTableFromFile ourselves with a
  // locator built from the skin module succeeds where hud's own attempt does
  // not, so supply the table directly.
  if (guide_bs_obj_ && guide_bs_skin_module_) {
    auto xm_st = kernel_state()->GetModule("xam.xex", true);
    uint32_t lst = xm_st ? xm_st->GetProcAddressByOrdinal(0x342) : 0;
    uint32_t tloc = memory->SystemHeapAlloc(256, 16);
    uint32_t tout = memory->SystemHeapAlloc(16, 16);
    if (lst && tloc && tout) {
      std::memset(memory->TranslateVirtual(tloc), 0, 256);
      std::memset(memory->TranslateVirtual(tout), 0, 16);
      std::string l =
          fmt::format("section://{:08X},hud#strings.xus", guide_bs_skin_module_);
      for (size_t w = 0; w < l.size(); ++w) {
        xe::store_and_swap<uint16_t>(
            memory->TranslateVirtual(tloc + uint32_t(w) * 2), uint16_t(l[w]));
      }
      xe::store_and_swap<uint16_t>(
          memory->TranslateVirtual(tloc + uint32_t(l.size()) * 2), 0);
      uint64_t la[] = {tloc, tout};
      uint64_t st_r = processor->Execute(ts, lst, la, xe::countof(la));
      uint32_t table = rd(tout);
      if (!st_r && table) {
        xe::store_and_swap<uint32_t>(
            memory->TranslateVirtual(guide_bs_obj_ + 0x4E8), table);
      }
      XELOGI("GuideBootstrap: string table \"{}\" -> {:08X}, table {:08X}, "
             "[guide+4E8] now {:08X}",
             l, static_cast<uint32_t>(st_r), table,
             rd(guide_bs_obj_ + 0x4E8));
    }
  }
  if (::cvars::guide_static_locator) {
    // The dynamic locator builder is handed [guide+8] as its module and
    // that field is 0, giving "section://@0,...". Selecting the static
    // builder makes it use [guide+4], which is set just above.
    xe::store_and_swap<uint32_t>(
        memory->TranslateVirtual(guide_bs_obj_ + 8), 0xFFFFFFFFu);
    XELOGI("GuideBootstrap: [guide+8] = FFFFFFFF (static locator path)");
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
    // The third argument is an OUT pointer: on success the creator stores the
    // scene handle through it (stw r11,0(r29) at 913EBA18, r29 = r5). Passing
    // 0 made it store through null once the earlier crash was cleared.
    uint32_t scene_out = memory->SystemHeapAlloc(16, 16);
    if (scene_out) {
      std::memset(memory->TranslateVirtual(scene_out), 0, 16);
    }
    uint64_t a2[] = {guide_bs_obj_, 0, scene_out};
    ir = processor->Execute(ts, scene_fn, a2, xe::countof(a2));
    XELOGI("GuideBootstrap: scene creator {:08X} -> {:08X}, scene={:08X}",
           scene_fn, static_cast<uint32_t>(ir),
           scene_out ? rd(scene_out) : 0);
    // A scene HANDLE is not evidence of scene CONTENT. Resolve it with
    // XuiObjectFromHandle (xam ordinal 0x346) and dump the object head:
    // if the render tree has no children there is nothing to draw, which
    // would explain an emitter that runs every frame and emits nothing.
    uint32_t scene_h = scene_out ? rd(scene_out) : 0;
    if (scene_h) {
      uint32_t obj_out = memory->SystemHeapAlloc(16, 16);
      if (obj_out) {
        std::memset(memory->TranslateVirtual(obj_out), 0, 16);
        uint64_t oa[] = {scene_h, obj_out};
        uint64_t orr = processor->Execute(ts, 0x81942938u, oa,
                                          xe::countof(oa));
        uint32_t sobj = rd(obj_out);
      {
        // The XUI class registry, read AFTER the scene has loaded. The
        // bootstrap logs it as all zeros at hud-load time; if it is still
        // empty here then the .xur instantiated its elements against no
        // registered classes, which is what a "labelHeading" that has an
        // id and a position but no visual and no text would look like.
        // 48 words, not 16: a short fixed window could not show whether
        // extra classes were registered.
        std::string reg;
        uint32_t reg_n = 0;
        for (uint32_t w = 0; w < 48; ++w) {
          uint32_t rv = rd(0x81D6D508u + w * 4);
          if (rv) ++reg_n;
          reg += fmt::format("{:08X} ", rv);
        }
        XELOGI("GuideScene: XUI registry ({} non-null of 48): {}", reg_n,
               reg);
        std::string reg2;
        for (uint32_t w = 0; w < 8; ++w) {
          reg2 += fmt::format("{:08X} ", rd(0x81D6D0D8u + w * 4));
        }
        XELOGI("GuideScene: table @81D6D0D8: {}", reg2);
        // The lookup at runtime 8194A4E0 - whose null result is what makes
        // XuiSceneCreate return E_FAIL - bounds-checks a class index against
        // the word at [81D6D0D8 + 0x420] = 81D6D4F8 and then indexes a table
        // from the same base. Log the count and the entries either side of
        // it, since a class index at or past the count is exactly the
        // "registered classes" failure this would produce.
        uint32_t cls_count = rd(0x81D6D4F8u);
        std::string tail;
        for (uint32_t w = 0; w < 12; ++w) {
          tail += fmt::format("{:08X}:{:08X} ", 0x81D6D4E0u + w * 4,
                              rd(0x81D6D4E0u + w * 4));
        }
        // hud returns E_FAIL from XuiSceneCreate when
        // XamNotifyCreateListener (ordinal 0x28A) hands back 0, and the HLE
        // shim is never called, so the guest implementation is the one that
        // runs. Report where it lives so it can be read.
        {
          auto xmn = kernel_state()->GetModule("xam.xex", true);
          uint32_t nfn = xmn ? xmn->GetProcAddressByOrdinal(0x28A) : 0;
          XELOGI("GuideScene: xam ordinal 28A (XamNotifyCreateListener) -> "
                 "{:08X}",
                 nfn);
          // hud's string table is loaded by XuiLoadStringTableFromFile with a
          // locator from XamBuildDynamicResourceLocator; when that load fails
          // the table stays null and hud dereferences it. Report where those
          // live so they can be read.
          for (auto ord : {0x342u, 0x31Eu, 0x31Bu}) {
            XELOGI("GuideScene: xam ordinal {:03X} -> {:08X}", ord,
                   xmn ? xmn->GetProcAddressByOrdinal(ord) : 0);
          }
          // The listener creation ends in ObCreateObject with the object type
          // descriptor at 81D22460, and ObCreateObject calls that
          // descriptor's allocate_proc. 81D22460 sits inside the
          // 81D14000-81D5F000 range that is zero at load, so dump it: a null
          // allocate_proc would explain the whole failure.
          std::string ot;
          for (uint32_t w = 0; w < 12; ++w) {
            ot += fmt::format("{:08X} ", rd(0x81D22460u + w * 4));
          }
          XELOGI("GuideScene: object type @81D22460: {}", ot);
        }
        XELOGI("GuideScene: class index bound [81D6D4F8] = {} ({:08X}); "
               "neighbourhood {}",
               cls_count, cls_count, tail);
        // XuiInit's normal (null-params) path consults [81D6D0A4], which
        // sits next to the resource provider at [81D6D0AC]. Read the
        // whole neighbourhood after the scene has loaded - this is the
        // state XUI uses to find visuals, so an empty slot here would be
        // a fact rather than another theory.
        std::string xg;
        for (uint32_t a = 0x81D6D090u; a <= 0x81D6D0C0u; a += 4) {
          xg += fmt::format("{:08X}:{:08X} ", a, rd(a));
        }
        XELOGI("GuideScene: XUI globals {}", xg);
        if (!::cvars::guide_scene_override.empty()) {
          auto xm2 = kernel_state()->GetModule("xam.xex", true);
          uint32_t sc2 = xm2 ? xm2->GetProcAddressByOrdinal(0x357) : 0;
          uint32_t glc2 = xm2 ? xm2->GetProcAddressByOrdinal(0x32F) : 0;
          // Build the same locator hud uses and ask for a named scene.
          auto wide = [&](uint32_t buf, const std::string& t) {
            for (size_t w = 0; w < t.size(); ++w) {
              xe::store_and_swap<uint16_t>(
                  memory->TranslateVirtual(buf + uint32_t(w) * 2),
                  uint16_t(t[w]));
            }
            xe::store_and_swap<uint16_t>(
                memory->TranslateVirtual(buf + uint32_t(t.size()) * 2), 0);
          };
          uint32_t loc = memory->SystemHeapAlloc(256, 16);
          uint32_t nmb = memory->SystemHeapAlloc(256, 16);
          uint32_t out2 = memory->SystemHeapAlloc(16, 16);
          // hud's own string-table load fails and leaves [guide+0x4E8] null,
          // and every input to it has checked out. The locator string itself
          // has only ever been inferred, so call XuiLoadStringTableFromFile
          // (ordinal 0x342) directly with a locator we control: success here
          // means hud's locator is the problem, failure means the loader is.
          {
            uint32_t lst = xm2 ? xm2->GetProcAddressByOrdinal(0x342) : 0;
            uint32_t tloc = memory->SystemHeapAlloc(256, 16);
            uint32_t tout = memory->SystemHeapAlloc(16, 16);
            if (lst && tloc && tout) {
              for (const char* form :
                   {"section://{:08X},hud#strings.xus",
                    "section://{:X},hud#strings.xus",
                    "section://{:08X},hud#Strings.xus"}) {
                std::memset(memory->TranslateVirtual(tloc), 0, 256);
                std::memset(memory->TranslateVirtual(tout), 0, 16);
                std::string l = fmt::format(fmt::runtime(form),
                                            guide_bs_skin_module_);
                wide(tloc, l);
                uint64_t la[] = {tloc, tout};
                uint64_t lr2 = processor->Execute(ts, lst, la,
                                                  xe::countof(la));
                XELOGI("GuideScene: XuiLoadStringTableFromFile(\"{}\") -> "
                       "{:08X}, table {:08X}",
                       l, static_cast<uint32_t>(lr2), rd(tout));
              }
            } else {
              XELOGW("GuideScene: no ordinal 342 / alloc for string table "
                     "probe");
            }
          }
          std::string spec2 = ::cvars::guide_scene_override;
          size_t sp = 0;
          while (sp <= spec2.size()) {
            size_t cm = spec2.find(',', sp);
            std::string one = spec2.substr(
                sp, cm == std::string::npos ? std::string::npos
                                            : cm - sp);
            if (!one.empty()) {
          if (loc && nmb && out2 && sc2) {
            std::memset(memory->TranslateVirtual(loc), 0, 256);
            std::memset(memory->TranslateVirtual(nmb), 0, 256);
            std::memset(memory->TranslateVirtual(out2), 0, 16);
            wide(loc, fmt::format("section://{:08X},hud#strings.xus",
                                  guide_bs_skin_module_));
            wide(nmb, one);
            uint64_t sa[] = {loc, nmb, 0ull, out2};
            uint64_t sr2 = processor->Execute(ts, sc2, sa,
                                              xe::countof(sa));
            uint32_t nsc = rd(out2);
            XELOGI("GuideScene: override XuiSceneCreate(\"{}\") -> "
                   "{:08X}, scene {:08X}",
                   one,
                   static_cast<uint32_t>(sr2), nsc);
            if (nsc && glc2) {
              uint32_t co2 = memory->SystemHeapAlloc(16, 16);
              if (co2) {
                std::memset(memory->TranslateVirtual(co2), 0, 16);
                uint64_t ca2[] = {nsc, co2};
                processor->Execute(ts, glc2, ca2, xe::countof(ca2));
                XELOGI("GuideScene: override last child {:08X}",
                       rd(co2));
                uint32_t okid = rd(co2);
                // Does an element of a scene that loads properly get a
                // visual? The claim that nothing anywhere has one was
                // only ever tested on the near-empty upsell page.
                std::string oid;
                uint32_t ovr = 0xFFFFFFFFu, ovis = 0;
                uint32_t gid2 = xm2 ? xm2->GetProcAddressByOrdinal(0x32E)
                                    : 0;
                uint32_t gvi2 = xm2 ? xm2->GetProcAddressByOrdinal(0x395)
                                    : 0;
                if (okid && gid2) {
                  std::memset(memory->TranslateVirtual(co2), 0, 16);
                  uint64_t q1[] = {okid, co2};
                  processor->Execute(ts, gid2, q1, xe::countof(q1));
                  uint32_t sp3 = rd(co2);
                  if (sp3 > 0x1000u) {
                    for (uint32_t w = 0; w < 40; ++w) {
                      uint16_t c3 = xe::load_and_swap<uint16_t>(
                          memory->TranslateVirtual(sp3 + w * 2));
                      if (!c3) break;
                      oid += (c3 >= 0x20 && c3 < 0x7F) ? char(c3) : '?';
                    }
                  }
                }
                if (okid && gvi2) {
                  std::memset(memory->TranslateVirtual(co2), 0, 16);
                  uint64_t q2[] = {okid, co2};
                  ovr = uint32_t(processor->Execute(ts, gvi2, q2,
                                                    xe::countof(q2)));
                  ovis = rd(co2);
                }
                XELOGI("GuideScene: override child {:08X} \"{}\" "
                       "visual -> {:08X}: {:08X}",
                       okid, oid, ovr, ovis);
              }
            }
          }

            }
            if (cm == std::string::npos) break;
            sp = cm + 1;
          }
        }
      }
        XELOGI("GuideScene: XuiObjectFromHandle({:08X}) -> {:08X}, "
               "object {:08X}",
               scene_h, static_cast<uint32_t>(orr), sobj);
        if (sobj) {
          std::string head;
          for (uint32_t w = 0; w < 24; ++w) {
            head += fmt::format("{:02X}:{:08X} ", w * 4, rd(sobj + w * 4));
          }
          XELOGI("GuideScene: object head {}", head);
        }
      // Ask XUI itself whether the scene has any children, rather than
      // guessing from a memory dump: XuiElementGetLastChild is ordinal
      // 0x32F. A scene that loaded visual content has children; an empty
      // one does not, and that is the difference between a render pass
      // that emits draws and one that emits nothing.
      auto xmod = kernel_state()->GetModule("xam.xex", true);
      uint32_t glc = xmod ? xmod->GetProcAddressByOrdinal(0x32F) : 0;
      if (glc) {
        uint32_t child_out = memory->SystemHeapAlloc(16, 16);
        if (child_out) {
          std::memset(memory->TranslateVirtual(child_out), 0, 16);
          uint64_t ca[] = {scene_h, child_out};
          uint64_t cr = processor->Execute(ts, glc, ca, xe::countof(ca));
          uint32_t kid = rd(child_out);
          XELOGI("GuideScene: XuiElementGetLastChild({:08X}) -> {:08X}, "
                 "child {:08X}",
                 scene_h, static_cast<uint32_t>(cr), kid);
          // Walk down: 00010000 is the root container and the navigated
          // scene is its child, so the scene's OWN children are what the
          // .xur populates. Two more levels tells us whether the visual
          // content is really there.
          for (int depth = 0; depth < 3 && kid; ++depth) {
            std::memset(memory->TranslateVirtual(child_out), 0, 16);
            uint64_t ka[] = {kid, child_out};
            uint64_t kr = processor->Execute(ts, glc, ka,
                                            xe::countof(ka));
            uint32_t next = rd(child_out);
            XELOGI("GuideScene: depth {} child of {:08X} -> {:08X}, "
                   "{:08X}",
                   depth + 1, kid, static_cast<uint32_t>(kr), next);
            // Geometry and identity of each element: a populated tree
            // whose elements have zero bounds renders nothing, and that
            // is indistinguishable from "no content" unless measured.
            uint32_t gid = xmod ? xmod->GetProcAddressByOrdinal(0x32E) : 0;
            uint32_t gpos = xmod ? xmod->GetProcAddressByOrdinal(0x3DF) : 0;
            uint32_t buf3 = memory->SystemHeapAlloc(32, 16);
            if (buf3 && gpos) {
              std::memset(memory->TranslateVirtual(buf3), 0, 32);
              uint64_t pa[] = {kid, buf3};
              uint64_t pr = processor->Execute(ts, gpos, pa,
                                              xe::countof(pa));
              XELOGI("GuideScene:   {:08X} GetPosition -> {:08X}: "
                     "{:08X} {:08X} {:08X} {:08X}",
                     kid, static_cast<uint32_t>(pr), rd(buf3),
                     rd(buf3 + 4), rd(buf3 + 8), rd(buf3 + 12));
            }
            if (buf3 && gid) {
              std::memset(memory->TranslateVirtual(buf3), 0, 32);
              uint64_t ia3[] = {kid, buf3};
              uint64_t ir3 = processor->Execute(ts, gid, ia3,
                                               xe::countof(ia3));
              // GetId hands back a wide string naming the element; that
              // says what type it is, which decides whether GetVisual
              // failing is meaningful or just a type mismatch.
              uint32_t idp = rd(buf3);
              std::string ids;
              if (idp > 0x1000u) {
                for (uint32_t w = 0; w < 64; ++w) {
                  uint16_t ch = xe::load_and_swap<uint16_t>(
                      memory->TranslateVirtual(idp + w * 2));
                  if (!ch) break;
                  ids += (ch >= 0x20 && ch < 0x7F) ? char(ch) : '?';
                }
              }
              XELOGI("GuideScene:   {:08X} GetId -> {:08X}: {:08X} "
                     "\"{}\"",
                     kid, static_cast<uint32_t>(ir3), idp, ids);
              // Resolve THIS element (not just the root, which was all
              // that was dumped before) and show its vtable: a real
              // control has one, a bare element instantiated for want of
              // a class would look different.
              uint32_t eo = memory->SystemHeapAlloc(16, 16);
              uint32_t gof = xmod ? xmod->GetProcAddressByOrdinal(0x346)
                                  : 0;
              if (eo && gof) {
                std::memset(memory->TranslateVirtual(eo), 0, 16);
                uint64_t ea[] = {kid, eo};
                processor->Execute(ts, gof, ea, xe::countof(ea));
                uint32_t eobj = rd(eo);
                std::string eh;
                for (uint32_t w = 0; w < 12; ++w) {
                  eh += fmt::format("{:02X}:{:08X} ", w * 4,
                                    rd(eobj + w * 4));
                }
                XELOGI("GuideScene:   {:08X} object {:08X} head {}", kid,
                       eobj, eh);
                // The scene object references more children than
                // GetLastChild reports ([+0x0C], [+0x10]); walk those
                // too, with their ids, since they are referenced by a
                // real object with a real vtable.
                if (rd(eobj) > 0x90000000u) {
                  uint32_t gv2 =
                      xmod ? xmod->GetProcAddressByOrdinal(0x395) : 0;
                  for (uint32_t co = 0x0C; co <= 0x10; co += 4) {
                    uint32_t ch = rd(eobj + co);
                    if (!ch) continue;
                    uint32_t io2 = memory->SystemHeapAlloc(16, 16);
                    if (!io2) continue;
                    std::memset(memory->TranslateVirtual(io2), 0, 16);
                    uint64_t ia4[] = {ch, io2};
                    processor->Execute(ts, gid, ia4, xe::countof(ia4));
                    uint32_t nm = rd(io2);
                    std::string ns;
                    if (nm > 0x1000u) {
                      for (uint32_t w = 0; w < 48; ++w) {
                        uint16_t c2 = xe::load_and_swap<uint16_t>(
                            memory->TranslateVirtual(nm + w * 2));
                        if (!c2) break;
                        ns += (c2 >= 0x20 && c2 < 0x7F) ? char(c2) : '?';
                      }
                    }
                    std::memset(memory->TranslateVirtual(io2), 0, 16);
                    uint64_t va4[] = {ch, io2};
                    uint64_t vr4 = processor->Execute(ts, gv2, va4,
                                                     xe::countof(va4));
                    XELOGI("GuideScene:     child [+{:X}] {:08X} "
                           "\"{}\" visual -> {:08X}: {:08X}",
                           co, ch, ns, static_cast<uint32_t>(vr4),
                           rd(io2));
                  }
                }
              }
            }
            // An element carries geometry, but what actually rasterises
            // is its attached visual. A tree that lays out correctly and
            // draws nothing is exactly what missing visuals look like.
            uint32_t gvis = xmod ? xmod->GetProcAddressByOrdinal(0x395)
                                 : 0;
            if (buf3 && gvis) {
              std::memset(memory->TranslateVirtual(buf3), 0, 32);
              uint64_t va[] = {kid, buf3};
              uint64_t vr = processor->Execute(ts, gvis, va,
                                              xe::countof(va));
              XELOGI("GuideScene:   {:08X} GetVisual -> {:08X}: {:08X}",
                     kid, static_cast<uint32_t>(vr), rd(buf3));
            }
            if (::cvars::guide_inject_label_text) {
              uint32_t stf = xmod ? xmod->GetProcAddressByOrdinal(0x363)
                                  : 0;
              uint32_t str = memory->SystemHeapAlloc(64, 16);
              if (stf && str) {
                static const char16_t kTxt[] = u"XENIA GUIDE TEST";
                std::memset(memory->TranslateVirtual(str), 0, 64);
                for (uint32_t w = 0; kTxt[w]; ++w) {
                  xe::store_and_swap<uint16_t>(
                      memory->TranslateVirtual(str + w * 2),
                      uint16_t(kTxt[w]));
                }
                uint64_t ta[] = {kid, str};
                uint64_t tr = processor->Execute(ts, stf, ta,
                                                xe::countof(ta));
                XELOGI("GuideScene:   {:08X} SetText -> {:08X}", kid,
                       static_cast<uint32_t>(tr));
              }
            }
            kid = next;
          }
        }
      } else {
        XELOGW("GuideScene: could not resolve XuiElementGetLastChild");
      }
      }
    }
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
  {
    // What is the render DC actually bound to? If it holds no surface, the
    // draw calls are no-ops by construction rather than by submission.
    uint32_t dc = rd(render_obj + 12);
    XELOGI("GuideBootstrap: DC {:08X} contents:", dc);
    for (int i = 0; i < 6 && dc; ++i) {
      XELOGI("  dc[{:02X}] = {:08X} {:08X} {:08X} {:08X}", i * 16,
             rd(dc + i * 16), rd(dc + i * 16 + 4), rd(dc + i * 16 + 8),
             rd(dc + i * 16 + 12));
    }
    // XuiRenderPresent tail-calls dc->vtable[21] (runtime 818F9290), which
    // reads three fields before it will present anything:
    //   [dc+11C] == 0        -> bail out with E_UNEXPECTED
    //   [dc+1CC] == 0        -> assert (twi 31,r0,19)
    //   [dc+134] != 0        -> return S_OK having presented NOTHING
    // Only [dc+134] == 0 reaches the real present, which tail-calls
    // [dc+1CC]->vtable[24]. A 00000000 return from the composite draw is
    // therefore NOT evidence that anything was presented.
    XELOGI("GuideBootstrap: DC present gates: [11C]={:08X} [134]={:08X} "
           "[1CC]={:08X}",
           rd(dc + 0x11Cu), rd(dc + 0x134u), rd(dc + 0x1CCu));
    {
      // Which vtable the DC actually has, and the render-begin slots.
      // 818F82F0 dispatches slot 20 (vtable+0x50) with the surface it
      // was handed; slots 18/19/21 are begin/end/present. Read them
      // rather than trusting the static vtable at file VA 81640684 -
      // the crash unwind puts slot 20's callee below that address.
      uint32_t dcv = dc ? rd(dc) : 0;
      if (dcv) {
        XELOGI("GuideBootstrap: DC {:08X} vtable {:08X} [0C]={:08X} "
               "slot18={:08X} slot19={:08X} slot20={:08X} slot21={:08X}",
               dc, dcv, rd(dc + 0x0Cu), rd(dcv + 18 * 4),
               rd(dcv + 19 * 4), rd(dcv + 20 * 4), rd(dcv + 21 * 4));
      } else {
        XELOGI("GuideBootstrap: DC {:08X} has no vtable", dc);
      }
    }
    {
      // Present (819DE94C) reads [dev+0x32A0], falls back to
      // [dev+0x32B0], then dereferences +0x24 of it as a fetch-constant
      // descriptor. Both null is the crash. Dump the slots on xam's own
      // device and on the title's so it is visible which, if either,
      // actually carries a surface.
      uint32_t xam_dev = rd(0x81D43684u);
      uint32_t ttl_dev = rd(0x801E6FC4u);
      for (auto& e : {std::make_pair("xam", xam_dev),
                      std::make_pair("title", ttl_dev)}) {
        if (!e.second) {
          XELOGI("GuideBootstrap: {} device is null", e.first);
          continue;
        }
        XELOGI("GuideBootstrap: {} device {:08X}: [32A0]={:08X} "
               "[32B0]={:08X} [3F74]={:08X}",
               e.first, e.second, rd(e.second + 0x32A0u),
               rd(e.second + 0x32B0u), rd(e.second + 0x3F74u));
        {
          // Packet emission (81A015B8) asserts the caller's cursor
          // equals [dev+0x2B4C], reserves N words by advancing it, and
          // records the reservation at [dev+0x2B54]. On the mode-2
          // device the cursor is 0, so the first packet word stores to
          // guest address 4. Dump the region on both devices to see what
          // a device with a real ring buffer carries here.
          std::string cb;
          for (uint32_t o = 0x2B40; o <= 0x2B60; o += 4) {
            cb += fmt::format("+{:X}={:08X} ", o, rd(e.second + o));
          }
          XELOGI("GuideBootstrap: {} device cmdbuf {}", e.first, cb);
          // Does this device reference a ring buffer at all? The rings
          // observed live in physical memory (title 1FAE2000, xam mode-1
          // 1D686000). A device with no such pointer has nowhere to emit
          // a draw stream, which would explain why even a Clear produces
          // no packets in the small buffer at [dev+0x30].
          std::string rings;
          for (uint32_t o = 0; o < 0x8000u; o += 4) {
            uint32_t v = rd(e.second + o);
            if (v >= 0x10000000u && v < 0x20000000u && (v & 0xFFF) == 0) {
              rings += fmt::format("+{:X}={:08X} ", o, v);
            }
          }
          XELOGI("GuideBootstrap: {} device ring-like pointers: {}",
                 e.first, rings.empty() ? "none" : rings);
        }
        // Both surface slots are null even on a healthy device, so look
        // for a real surface object anywhere in the device: guest heap
        // pointers whose word 0 passes the same validity test the setter
        // at 819F38C8 applies (bit 30 clear; "!" marks ones that fail).
        // A render target is a GPU fetch-constant descriptor: 819DE94C
        // reads [surface+0x24] and unpacks it with
        //   rlwinm r10,r9,14,18,31 ; rlwinm r9,r9,29,17,31
        // i.e. width = (rotl(v,14) & 0x3FFF) + 1, height = (rotl(v,29) &
        // 0x7FFF) + 1. Decode every candidate the same way; the title's real
        // RT should come out at its actual resolution. Validate readability
        // first - an uncommitted pointer faults host-side and kills the
        // thread with no guest crash report.
        auto rotl32 = [](uint32_t v, uint32_t n) {
          return (v << n) | (v >> (32 - n));
        };
        auto readable = [&](uint32_t a) {
          auto* hp = memory->LookupHeap(a);
          return hp && hp->QueryRangeAccess(a, a + 0x28u) !=
                           xe::memory::PageAccess::kNoAccess;
        };
        std::string cands;
        for (uint32_t o = 0; o < 0x4000; o += 4) {
          uint32_t v = rd(e.second + o);
          if (v < 0x40000000u || v >= 0x50000000u || !readable(v)) {
            continue;
          }
          uint32_t w0 = rd(v), fc = rd(v + 0x24u);
          uint32_t w = (rotl32(fc, 14) & 0x3FFFu) + 1;
          uint32_t h = (rotl32(fc, 29) & 0x7FFFu) + 1;
          if (w >= 64 && w <= 4096 && h >= 64 && h <= 4096) {
            cands += fmt::format("+{:X}={:08X}(w0={:08X} fc={:08X} {}x{}) ", o,
                                 v, w0, fc, w, h);
          }
        }
        XELOGI("GuideBootstrap: {} device surface-shaped candidates: {}",
               e.first, cands.empty() ? "none" : cands);
      }
    }
  }
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
  // Composite the Guide here. The title's D3D device is thread-affine and
  // this runs on the thread that owns it, inside the title's frame and just
  // before its swap - which is where the Guide is drawn on hardware. The
  // title calls VdCallGraphicsNotificationRoutines only once at startup, so
  // that is not the per-frame path.
  {
    // Draws dispatched per swap, sampled. The Guide's XUI writes GPU state but
    // dispatches no draws through the buffers we submit; if it draws at all it
    // must be through the title's own ring buffer, which this counter also
    // sees. Comparing a run with the button pressed against one without is the
    // test.
    {
      // slot 20 (818FDDE8) clears the render target, which present and
      // clear both reach via [dev+0x32A0] ? : [dev+0x32B0]. Both read
      // null when the bootstrap samples them, but that sampling happens
      // inside VdSwap - after the title has finished its frame. Track
      // whether they are EVER non-null, and at which swap, to tell
      // "never bound" apart from "unbound by the time we look".
      static uint32_t rt_seen = 0;
      if (rt_seen < 3) {
        auto* m = kernel_state()->memory();
        auto rdw2 = [m](uint32_t a) {
          return a ? xe::load_and_swap<uint32_t>(m->TranslateVirtual(a))
                   : 0u;
        };
        uint32_t td = rdw2(0x801E6FC4u);
        uint32_t a = rdw2(td + 0x32A0u), b = rdw2(td + 0x32B0u);
        if (a || b) {
          ++rt_seen;
          XELOGI("SwapRT: title device {:08X} has a render target at "
                 "swap-entry: [32A0]={:08X} [32B0]={:08X}",
                 td, a, b);
        }
      }
    }
    static std::atomic<uint32_t> swaps{0};
    uint32_t sn = ++swaps;
    if ((sn % 200) == 0) {
      auto* gsd = kernel_state()->emulator()->graphics_system();
      if (gsd && gsd->command_processor()) {
        XELOGI("SwapDraws: swap #{} cumulative draws {}", sn,
               gsd->command_processor()->guide_draw_count_);
      }
    }
  }
  if (guide_bs_pending_.exchange(false)) {
    auto* bth = XThread::GetCurrentThread();
    if (bth) {
      RunGuideBootstrapOnTitleThread(bth);
    }
  }
  {
    // VdSwap's second argument is the frontbuffer D3D9 texture header fetch -
    // a descriptor for the buffer already on screen, and the only surface-like
    // thing the Guide path is handed for free.
    static bool fetch_once = false;
    if (!fetch_once && guide_draw_fn_) {
      fetch_once = true;
      auto* fm = kernel_state()->memory();
      uint32_t fp = fetch_ptr.guest_address();
      XELOGI("VdSwap fetch_ptr={:08X}", fp);
      if (fp) {
        for (uint32_t i = 0; i < 8; ++i) {
          XELOGI("  fetch[{:02X}] = {:08X}", i * 4,
                 xe::load_and_swap<uint32_t>(fm->TranslateVirtual(fp + i * 4)));
        }
      }
    }
  }
  if (guide_draw_fn_ && guide_draw_this_) {
    static thread_local bool in_guide_draw = false;
    auto* gth = XThread::GetCurrentThread();
    if (gth && !in_guide_draw) {
      in_guide_draw = true;
      uint64_t gargs[] = {guide_draw_this_};
      {
        // Log BEFORE the draw: with guide_force_real_present the draw faults,
        // so anything logged after Execute never appears.
        static bool once = false;
        if (!once) {
          once = true;
          auto* pm = kernel_state()->memory();
          auto prd = [pm](uint32_t a) {
            return xe::load_and_swap<uint32_t>(pm->TranslateVirtual(a));
          };
          uint32_t pdc = prd(guide_draw_this_ + 12);
          uint32_t pdev = pdc ? prd(pdc + 0x1CCu) : 0;
          // 818FDE98 sets [dc+134] = [param+1C] and [dc+1CC] = [param+8],
          // and stores the parameter object itself at [dc+1C8]. Read it back
          // so the inheritance is measured rather than assumed.
          uint32_t pparam = pdc ? prd(pdc + 0x1C8u) : 0;
          XELOGI("Guide pre-draw: dc={:08X} [134]={:08X} dev={:08X} "
                 "param=[1C8]={:08X} [param+1C]={:08X} [param+8]={:08X}",
                 pdc, pdc ? prd(pdc + 0x134u) : 0, pdev, pparam,
                 pparam ? prd(pparam + 0x1Cu) : 0,
                 pparam ? prd(pparam + 8u) : 0);
          if (pdev) {
            XELOGI("Guide pre-draw: dev [32A0]={:08X} [32B0]={:08X}",
                   prd(pdev + 0x32A0u), prd(pdev + 0x32B0u));
            // NOTE on SetRenderTarget's surface check (819FA3E4): it is
            //   rlwinm. r11,w0,0,1,1   ; isolate bit 30, Rc=1 so CR0 is set
            //   beq     cr0,+8         ; skip the trap when the bit is ZERO
            //   twi     31,r0,25
            // so the assert fires when bit 30 is SET - a valid surface has it
            // CLEAR. That is the opposite of what an earlier note here said,
            // and it makes the bit useless as a search filter: almost any
            // object passes. Scanning on it matched function pointers
            // (819E9750, whose first word is the 7D8802A6 of "mfspr r12,8").
            // The real constraint is the packed fetch constant at +0x24, so a
            // surface has to be recognised by that, not by the type bit.
          }
          // The title's device (VdGlobalDevice, 801E6FC4) is a working device
          // of the same class. Diffing it against xam's says which fields the
          // system boot would have filled in, without having to guess at D3D
          // internals. Only report offsets where they differ and at least one
          // side is non-zero.
          // Name every device object in play, with its render-target slots,
          // in THIS run - so the crash dump's r31 can be matched to one of
          // them. Comparing addresses across runs is worthless: the guest heap
          // is not stable between sessions.
          for (auto& e : {std::pair<const char*, uint32_t>{"xam dev [81D43684]",
                                                           prd(0x81D43684u)},
                          {"VdGlobalXamDevice [801E6FC8]", prd(0x801E6FC8u)},
                          {"VdGlobalDevice [801E6FC4]", prd(0x801E6FC4u)},
                          {"dc wrapper [dc+1CC]", pdev},
                          {"displaced device (pre-redirect)",
                           guide_prev_device_},
                          // 8191B418 does "lwz r3,12(r31)" and hands THAT to
                          // 819FEB78, so the device the present path actually
                          // uses is [wrapper+12] - not the 81D43684 global the
                          // redirect changes.
                          {"PRESENT PATH [wrapper+12]",
                           pdev ? prd(pdev + 12u) : 0}}) {
            if (!e.second) {
              XELOGI("  device {}: <null>", e.first);
              continue;
            }
            // +3F74 is the resource the present path loads into r6 at
            // 819FEC24 and passes down as the sixth argument that faults.
            // 81A0FA80 writes it, and that is the same function whose
            // SetRenderTarget call binds RT0.
            XELOGI("  device {} = {:08X}  RT0={:08X} RT1={:08X} depth={:08X} "
                   "[3F74]={:08X}",
                   e.first, e.second, prd(e.second + 0x32A0u),
                   prd(e.second + 0x32A4u), prd(e.second + 0x32B0u),
                   prd(e.second + 0x3F74u));
          }
          uint32_t tdev = prd(0x801E6FC4u);
          uint32_t xdev = prd(0x801E6FC8u);
          XELOGI("Device diff: title={:08X} xam={:08X}", tdev, xdev);
          if (tdev && xdev) {
            // The same value patterns show up in xam's device at exactly +80
            // from the title's, so the two are different struct layouts (dash
            // and xam link different D3D builds) and offsets are NOT portable
            // between them. Dump the region around xam's render-target fields
            // on both, side by side, rather than diffing.
            for (uint32_t off = 0x3280; off <= 0x32C0; off += 4) {
              XELOGI("  +{:04X}: title={:08X} xam={:08X}", off, prd(tdev + off),
                     prd(xdev + off));
            }
            // 819F4C00 compares each RT slot against [dev+3F78] and the
            // depth slot against [dev+3F70] - those are the device's own
            // "default" surfaces. If they are real objects they are the
            // cheapest thing to bind, and SetRenderTarget's validation says
            // exactly what "real" means: bit 30 set in word 0.
            for (uint32_t off : {0x3F70u, 0x3F78u}) {
              uint32_t sp = prd(xdev + off);
              XELOGI("  default surface [{:04X}] = {:08X}", off, sp);
              if (sp) {
                XELOGI("    w0={:08X} (bit30={}) +24={:08X}", prd(sp),
                       (prd(sp) & 0x40000000u) ? "set" : "CLEAR",
                       prd(sp + 0x24u));
              }
            }
          }
        }
      }
      if (::cvars::guide_use_bound_device) {
        // Doing this once at bootstrap is racy: VdGlobalXamDevice is sometimes
        // still 0 when the bootstrap runs, and then no redirect happens at all
        // - which is what makes some runs fault at 819DE94C and others at
        // 819F5EC4. Patch the pointer the present path actually reads,
        // [wrapper+12] (8191B418 does "lwz r3,12(r31)"), every frame until it
        // agrees. That is race-free.
        auto* wm2 = kernel_state()->memory();
        auto w2 = [wm2](uint32_t a) {
          return xe::load_and_swap<uint32_t>(wm2->TranslateVirtual(a));
        };
        uint32_t wdc = w2(guide_draw_this_ + 12);
        uint32_t wrap = wdc ? w2(wdc + 0x1CCu) : 0;
        uint32_t boundd = w2(0x801E6FC8u);
        if (wrap && boundd && w2(wrap + 12u) != boundd) {
          static uint32_t reported = 0;
          uint32_t was = w2(wrap + 12u);
          xe::store_and_swap<uint32_t>(wm2->TranslateVirtual(wrap + 12u),
                                       boundd);
          if (reported++ < 3) {
            XELOGI("Guide: present-path device [wrapper {:08X} +12] {:08X} -> "
                   "{:08X} (RT0={:08X} fb={:08X})",
                   wrap, was, boundd, w2(boundd + 0x32A0u),
                   w2(boundd + 0x3F74u));
          }
        }
      }
      if (::cvars::guide_device_begin || ::cvars::guide_device_init_fn) {
        static bool dbg_done = false;
        if (!dbg_done) {
          dbg_done = true;
          auto* dm = kernel_state()->memory();
          auto d2 = [dm](uint32_t a) {
            return a ? xe::load_and_swap<uint32_t>(
                           dm->TranslateVirtual(a)) : 0u;
          };
          uint32_t ddc = d2(guide_draw_this_ + 12);
          uint32_t dwrap = ddc ? d2(ddc + 0x1CCu) : 0;
          uint32_t ddev = dwrap ? d2(dwrap + 12u) : 0;
          auto* dth = XThread::GetCurrentThread();
          if (ddev && dth) {
            uint32_t dfn = ::cvars::guide_device_init_fn
                               ? uint32_t(::cvars::guide_device_init_fn)
                               : 0x81A0F858u;
            uint64_t dargs[] = {ddev, 0ull};
            uint64_t dres = kernel_state()->processor()->Execute(
                dth->thread_state(), dfn, dargs, xe::countof(dargs));
            XELOGI("Guide: device init {:08X}(dev {:08X}) -> {:08X}; "
                   "[2B10]={:08X} [2B4C]={:08X} RT0={:08X} [3F74]={:08X}",
                   dfn, ddev, static_cast<uint32_t>(dres),
                   d2(ddev + 0x2B10u),
                   d2(ddev + 0x2B4Cu), d2(ddev + 0x32A0u),
                   d2(ddev + 0x3F74u));
          } else {
            XELOGW("Guide: device begin skipped (dev={:08X})", ddev);
          }
        }
      }
      if (::cvars::guide_bind_cmdbuf_kb > 0) {
        static uint32_t cbuf = 0;
        if (!cbuf) {
          auto* cm = kernel_state()->memory();
          auto c2 = [cm](uint32_t a) {
            return a ? xe::load_and_swap<uint32_t>(
                           cm->TranslateVirtual(a)) : 0u;
          };
          uint32_t cdc = c2(guide_draw_this_ + 12);
          uint32_t cwrap = cdc ? c2(cdc + 0x1CCu) : 0;
          uint32_t cdev = cwrap ? c2(cwrap + 12u) : 0;
          uint32_t csize =
              uint32_t(::cvars::guide_bind_cmdbuf_kb) * 1024u;
          if (cdev && !c2(cdev + 0x2B4Cu)) {
            cbuf = cm->SystemHeapAlloc(csize, 4096);
            if (cbuf) {
              std::memset(cm->TranslateVirtual(cbuf), 0, csize);
              // Go through xam's own setter rather than writing the
              // cursor: 81A01358(dev, buffer, words) sets base +2B48,
              // cursor +2B4C = buffer-4 (emission does +4 before each
              // store), limit +2B50 and stride +2B58 together, and
              // asserts the cursor is currently 0. Writing +2B4C alone
              // leaves the other three inconsistent and does not hold.
              auto* cth = XThread::GetCurrentThread();
              uint64_t cargs[] = {cdev, cbuf, csize / 4};
              uint64_t cres =
                  cth ? kernel_state()->processor()->Execute(
                            cth->thread_state(), 0x81A01358u, cargs,
                            xe::countof(cargs))
                      : 0;
              guide_cmdbuf_base_ = cbuf;
              guide_cmdbuf_size_ = csize;
              XELOGI("Guide: cmdbuf init {:08X}(dev {:08X}, {:08X}, {}) "
                     "-> {:08X}; base={:08X} cursor={:08X} limit={:08X}",
                     0x81A01358u, cdev, cbuf, csize / 4,
                     static_cast<uint32_t>(cres), c2(cdev + 0x2B48u),
                     c2(cdev + 0x2B4Cu), c2(cdev + 0x2B50u));
            }
          } else {
            XELOGW("Guide: cmdbuf bind skipped (dev={:08X} cursor={:08X})",
                   cdev, cdev ? c2(cdev + 0x2B4Cu) : 0);
          }
        }
      }
      if (::cvars::guide_fake_ring) {
        static bool fr_done = false;
        if (!fr_done) {
          auto* fm3 = kernel_state()->memory();
          auto f3 = [fm3](uint32_t a) {
            return a ? xe::load_and_swap<uint32_t>(
                           fm3->TranslateVirtual(a)) : 0u;
          };
          uint32_t fdc3 = guide_draw_this_ ? f3(guide_draw_this_ + 12) : 0;
          uint32_t fw3 = fdc3 ? f3(fdc3 + 0x1CCu) : 0;
          uint32_t fdev = fw3 ? f3(fw3 + 12u) : 0;
          if (!fdev) fdev = f3(0x81D43684u);
          if (fdev && !f3(fdev + 0x3B64u)) {
            fr_done = true;
            uint32_t p1 = fm3->SystemHeapAlloc(0x1000, 0x1000,
                                               kSystemHeapPhysical);
            uint32_t p2 = fm3->SystemHeapAlloc(0x1000, 0x1000,
                                               kSystemHeapPhysical);
            if (p1 && p2) {
              std::memset(fm3->TranslateVirtual(p1), 0, 0x1000);
              std::memset(fm3->TranslateVirtual(p2), 0, 0x1000);
              xe::store_and_swap<uint32_t>(
                  fm3->TranslateVirtual(fdev + 0x3B64u), p1);
              xe::store_and_swap<uint32_t>(
                  fm3->TranslateVirtual(fdev + 0x3DC4u), p2);
              XELOGI("Guide: fake ring on {:08X}: [3B64]={:08X} "
                     "[3DC4]={:08X}",
                     fdev, p1, p2);
            }
          }
        }
      }
      if (::cvars::guide_bind_title_rt) {
        // Must run BEFORE guide_fake_front_buffer, which clones RT0
        // into +3F74 and does nothing while RT0 is null.
        static bool rt_done = false;
        if (!rt_done) {
          auto* rm = kernel_state()->memory();
          auto r2 = [rm](uint32_t a) {
            return xe::load_and_swap<uint32_t>(rm->TranslateVirtual(a));
          };
          // Reach the device through the DC chain when our draw hook is
          // driving, and through xam's device globals when it is not.
          // Without the fallback this only ever ran under our own
          // bootstrap, so the xam-driven path - the one that matters -
          // never got a render target bound at all.
          uint32_t rdc = guide_draw_this_ ? r2(guide_draw_this_ + 12) : 0;
          uint32_t rwrap = rdc ? r2(rdc + 0x1CCu) : 0;
          uint32_t rdev = rwrap ? r2(rwrap + 12u) : 0;
          if (!rdev) {
            rdev = r2(0x81D43684u);
            if (!rdev) rdev = r2(0x801E6FC8u);
          }
          uint32_t tdev = r2(0x801E6FC4u);
          // Find the title's render target by SHAPE, not by a fixed
          // offset. 0x3AC4 was measured on one game, but every title
          // links its own D3D build with its own device layout - on
          // another title that offset held 0x0D and SetRenderTarget
          // faulted dereferencing it. A surface is a GPU fetch constant:
          // [p+0x24] unpacks as width = (rotl(v,14) & 0x3FFF) + 1 and
          // height = (rotl(v,29) & 0x7FFF) + 1, and the setter itself
          // requires bit 30 of word 0 to be clear.
          uint32_t surf = 0;
          if (tdev) {
            X_VIDEO_MODE vm;
            VdQueryVideoMode(&vm, false);
            uint32_t disp_w = uint32_t(vm.display_width);
            uint32_t disp_h = uint32_t(vm.display_height);
            uint32_t alt = 0, alt_off = 0, alt_w = 0, alt_h = 0;
            auto rot = [](uint32_t v, uint32_t n) {
              return (v << n) | (v >> (32 - n));
            };
            uint32_t seen = 0;
            for (uint32_t off = 0; off < 0x10000u && !surf; off += 4) {
              uint32_t cand = r2(tdev + off);
              if (cand < 0x40000000u || cand >= 0x50000000u) continue;
              auto* hp = rm->LookupHeap(cand);
              if (!hp || hp->QueryRangeAccess(cand, cand + 0x28u) ==
                             xe::memory::PageAccess::kNoAccess) {
                continue;
              }
              ++seen;
              uint32_t w0 = r2(cand), fc = r2(cand + 0x24u);
              uint32_t wd = (rot(fc, 14) & 0x3FFFu) + 1;
              uint32_t ht = (rot(fc, 29) & 0x7FFFu) + 1;
              // Range checks alone are not enough: on one title a field
              // at +0x565C decoded to 1153x609 and passed, which is not a
              // real render resolution. Require the surface to match the
              // display mode Xenia already knows, so a coincidental shape
              // cannot masquerade as the render target.
              if (w0 & 0x40000000u) continue;
              // Prefer an exact match against the display mode. Many
              // titles render at a lower internal resolution and upscale,
              // so keep the best plausible candidate as a fallback rather
              // than binding nothing at all - but say which one it was.
              if (wd == disp_w && ht == disp_h) {
                surf = cand;
                XELOGI("Guide: title RT at [dev+{:X}] = {:08X} ({}x{}), "
                       "matches display mode",
                       off, cand, wd, ht);
              } else if (!alt && wd >= 256 && wd <= 4096 &&
                         ht >= 256 && ht <= 4096) {
                alt = cand;
                alt_off = off;
                alt_w = wd;
                alt_h = ht;
              }
            }
            if (!surf && alt) {
              surf = alt;
              XELOGW("Guide: no exact display-mode match; using "
                     "[dev+{:X}] = {:08X} ({}x{}) against display "
                     "{}x{} - the title may render at a lower "
                     "internal resolution, or this may be wrong",
                     alt_off, alt, alt_w, alt_h, disp_w, disp_h);
            } else if (!surf) {
              XELOGW("Guide: no title RT found in {:08X} ({} "
                     "pointer candidates examined)",
                     tdev, seen);
            }
          }
          if (rdev && surf && !r2(rdev + 0x32A0u)) {
            rt_done = true;
            // Writing [dev+0x32A0] by hand does not stick: 819F4C00
            // walks RT0..RT3 and calls SetRenderTarget(dev, i, NULL) for
            // any slot not equal to [dev+0x3F78], the device's own
            // default target. Register the surface as that default AND
            // bind it through xam's real setter (819F31A8), so it goes
            // through the bookkeeping xam checks rather than around it.
            xe::store_and_swap<uint32_t>(
                rm->TranslateVirtual(rdev + 0x3F78u), surf);
            auto* bth = XThread::GetCurrentThread();
            uint64_t sargs[] = {rdev, 0ull, surf};
            uint64_t sres =
                bth ? kernel_state()->processor()->Execute(
                          bth->thread_state(), 0x819F31A8u, sargs,
                          xe::countof(sargs))
                    : 0;
            XELOGI("Guide: SetRenderTarget(dev {:08X}, 0, {:08X} "
                   "fc={:08X}) -> {:08X}; RT0 now {:08X} [3F78]={:08X}",
                   rdev, surf, r2(surf + 0x24u),
                   static_cast<uint32_t>(sres), r2(rdev + 0x32A0u),
                   r2(rdev + 0x3F78u));
            XELOGI("Guide: bound title RT {:08X} (fc={:08X}) as RT0 on "
                   "device {:08X}",
                   surf, r2(surf + 0x24u), rdev);
          } else if (!rdev || !surf) {
            static uint32_t warned = 0;
            if (warned++ < 3) {
              XELOGW("Guide: cannot bind title RT (dev={:08X} "
                     "surf={:08X})", rdev, surf);
            }
          }
        }
      }
      if (::cvars::guide_fake_front_buffer) {
        static bool fb_done = false;
        if (!fb_done) {
          auto* fm2 = kernel_state()->memory();
          auto f2 = [fm2](uint32_t a) {
            return xe::load_and_swap<uint32_t>(fm2->TranslateVirtual(a));
          };
          uint32_t fdc2 = f2(guide_draw_this_ + 12);
          uint32_t wrap2 = fdc2 ? f2(fdc2 + 0x1CCu) : 0;
          uint32_t dv2 = wrap2 ? f2(wrap2 + 12u) : 0;
          uint32_t rt = dv2 ? f2(dv2 + 0x32A0u) : 0;
          if (dv2 && rt && !f2(dv2 + 0x3F74u)) {
            fb_done = true;
            // Dump the template before cloning it. The emitter reads six
            // consecutive words from +0x1C - a fetch constant - so whether the
            // colour surface even carries one there decides if it can stand in
            // for a front buffer at all.
            {
              std::string row;
              for (uint32_t k = 0; k < 8; ++k) {
                row += fmt::format("{:08X} ", f2(rt + 0x1Cu + k * 4));
              }
              XELOGI("Guide: RT0 surface {:08X} words +1C..+38: {}", rt, row);
              std::string row0;
              for (uint32_t k = 0; k < 8; ++k) {
                row0 += fmt::format("{:08X} ", f2(rt + k * 4));
              }
              XELOGI("Guide: RT0 surface {:08X} words +00..+1C: {}", rt, row0);
            }
            uint32_t clone = fm2->SystemHeapAlloc(0x100, 16);
            if (clone) {
              uint32_t shift =
                  static_cast<uint32_t>(::cvars::guide_front_buffer_shift);
              std::memcpy(fm2->TranslateVirtual(clone),
                          fm2->TranslateVirtual(rt + shift), 0x100 - shift);
              if (shift) {
                XELOGI("Guide: front buffer cloned from RT0+{} so the fetch "
                       "constant lands at +0x1C", shift);
              }
              if (::cvars::guide_front_buffer_format >= 0) {
                uint32_t f = f2(clone + 0x20u);
                uint32_t nf = (f & ~0x3Fu) |
                              (static_cast<uint32_t>(
                                   ::cvars::guide_front_buffer_format) & 0x3Fu);
                xe::store_and_swap<uint32_t>(
                    fm2->TranslateVirtual(clone + 0x20u), nf);
                XELOGI("Guide: front buffer format [+20] {:08X} -> {:08X}", f,
                       nf);
              }
              xe::store_and_swap<uint32_t>(
                  fm2->TranslateVirtual(dv2 + 0x3F74u), clone);
              XELOGI("Guide: front buffer [dev {:08X} +3F74] = clone {:08X} "
                     "of RT0 {:08X}",
                     dv2, clone, rt);
            }
          }
        }
      }
      if (::cvars::guide_bind_depth_copy) {
        static bool depth_done = false;
        if (!depth_done) {
          depth_done = true;
          auto* dm = kernel_state()->memory();
          auto drd = [dm](uint32_t a) {
            return xe::load_and_swap<uint32_t>(dm->TranslateVirtual(a));
          };
          uint32_t ddev = drd(0x81D43684u);
          uint32_t rt0 = ddev ? drd(ddev + 0x32A0u) : 0;
          uint32_t dep = ddev ? drd(ddev + 0x32B0u) : 0;
          if (ddev && rt0 && !dep) {
            uint32_t clone = dm->SystemHeapAlloc(0x100, 16);
            if (clone) {
              std::memcpy(dm->TranslateVirtual(clone),
                          dm->TranslateVirtual(rt0), 0x100);
              uint64_t dargs[] = {ddev, clone};
              uint64_t dr = kernel_state()->processor()->Execute(
                  gth->thread_state(), 0x819F38C8u, dargs,
                  xe::countof(dargs));
              XELOGI("Guide: SetDepthStencilSurface(dev {:08X}, clone {:08X} "
                     "of RT0 {:08X}) -> {:08X}; depth now {:08X}",
                     ddev, clone, rt0, static_cast<uint32_t>(dr),
                     drd(ddev + 0x32B0u));
            }
          } else {
            XELOGI("Guide: depth bind skipped (dev={:08X} RT0={:08X} "
                   "depth={:08X})",
                   ddev, rt0, dep);
          }
        }
      }
      if (::cvars::guide_force_real_present) {
        auto* fm = kernel_state()->memory();
        uint32_t fdc = xe::load_and_swap<uint32_t>(
            fm->TranslateVirtual(guide_draw_this_ + 12));
        if (fdc) {
          xe::store_and_swap<uint32_t>(fm->TranslateVirtual(fdc + 0x134u), 0);
        }
      }
      static std::vector<uint32_t> pre_sums;
      // 0x30000000-0x50000000 was 512MB and hashing it forced commit of a
      // vast amount of untouched guest address space - the run died in the
      // snapshot before logging it. Cover the two ranges the Guide's objects
      // are actually observed in instead: xam's heap allocations around
      // 0x3018xxxx and the device/DC objects around 0x4088xxxx-0x409Bxxxx.
      // Two windows. The 408xxxxx one is where the Guide's device, DC and
      // wrapper objects live. The FE4xxxxx one is where xam's own VdSwap
      // arguments point - its front buffer (FE03E284) and its writeback
      // (FE474008) - and was the gap left by the previous scan.
      struct DiffRange { uint32_t lo, hi; };
      static const DiffRange kRanges[] = {{0x40800000u, 0x40A00000u},
                                          {0xFE000000u, 0xFE500000u}};
      const uint32_t kBlk = 0x10000u;
      uint32_t kTotalBlocks = 0;
      for (auto& r : kRanges) kTotalBlocks += (r.hi - r.lo) / kBlk;
      if (::cvars::guide_diff_draw_writes && pre_sums.empty()) {
        auto* mmv = kernel_state()->memory();
        pre_sums.reserve(kTotalBlocks);
        for (auto& r : kRanges) {
          for (uint32_t a = r.lo; a < r.hi; a += kBlk) {
            uint32_t sum = 0;
            auto* hp = mmv->TranslateVirtual(a);
            if (hp) {
              auto* w = reinterpret_cast<const uint32_t*>(hp);
              for (uint32_t i = 0; i < kBlk / 4; ++i) sum += w[i];
            }
            pre_sums.push_back(sum);
          }
        }
        XELOGI("DrawDiff: snapshot of {} blocks taken", pre_sums.size());
      }
      static std::vector<uint32_t> wd_pre;
      const uint32_t kWdLo = 0xFE030000u, kWdHi = 0xFE050000u;
      if (::cvars::guide_word_diff && wd_pre.empty()) {
        auto* wm3 = kernel_state()->memory();
        wd_pre.resize((kWdHi - kWdLo) / 4);
        for (uint32_t i = 0; i < wd_pre.size(); ++i) {
          wd_pre[i] = xe::load_and_swap<uint32_t>(
              wm3->TranslateVirtual(kWdLo + i * 4));
        }
      }
      // Draws dispatched by the guest's own draw call, measured across the
      // Execute itself. The per-swap sampler cannot see this in the deep
      // configuration, where the composite draw happens after the title
      // thread has stopped swapping.
      uint32_t gd_before = 0;
      {
        auto* gsx = kernel_state()->emulator()->graphics_system();
        if (gsx && gsx->command_processor()) {
          gd_before = gsx->command_processor()->guide_draw_count_;
        }
      }
      {
        // The emitter reads RT0 as [dev+0x32A0 + r17*4] with r17 = 0 and
        // faults on null, even though guide_bind_title_rt set it and
        // guide_fake_front_buffer successfully cloned from it. Sample it
        // here, immediately before the draw, so "reset between bind and
        // use" is measured rather than inferred.
        static uint32_t rtl = 0;
        if (rtl < 4) {
          ++rtl;
          auto* qm = kernel_state()->memory();
          auto q = [qm](uint32_t a) {
            return a ? xe::load_and_swap<uint32_t>(
                           qm->TranslateVirtual(a)) : 0u;
          };
          uint32_t qdc = q(guide_draw_this_ + 12);
          uint32_t qw = qdc ? q(qdc + 0x1CCu) : 0;
          uint32_t qd = qw ? q(qw + 12u) : 0;
          XELOGI("GuidePreDraw: device {:08X} RT0={:08X} RT1={:08X} "
                 "depth={:08X} [3F74]={:08X}",
                 qd, q(qd + 0x32A0u), q(qd + 0x32A4u), q(qd + 0x32B0u),
                 q(qd + 0x3F74u));
        }
      }
      // --- second rendering context: begin ---
      // The title owns the only GPU ring, so xam gets a command buffer of
      // its own instead. Begin it through xam's own routine every frame:
      // 81A01358 asserts the cursor is 0 on entry (a paired begin/end
      // protocol), which is why setting it up once never survived.
      uint32_t sc_dev = 0, sc_begin_cursor = 0;
      if (::cvars::guide_second_context_kb > 0) {
        auto* sm = kernel_state()->memory();
        auto sr = [sm](uint32_t a) {
          return a ? xe::load_and_swap<uint32_t>(sm->TranslateVirtual(a))
                   : 0u;
        };
        uint32_t sdc = sr(guide_draw_this_ + 12);
        uint32_t swrap = sdc ? sr(sdc + 0x1CCu) : 0;
        sc_dev = swrap ? sr(swrap + 12u) : 0;
        if (sc_dev) {
          if (!guide_cmdbuf_base_) {
            guide_cmdbuf_size_ =
                uint32_t(::cvars::guide_second_context_kb) * 1024u;
            guide_cmdbuf_base_ =
                sm->SystemHeapAlloc(guide_cmdbuf_size_, 4096);
            XELOGI("GuideCtx2: buffer {:08X} +{} bytes",
                   guide_cmdbuf_base_, guide_cmdbuf_size_);
          }
          if (guide_cmdbuf_base_) {
            std::memset(sm->TranslateVirtual(guide_cmdbuf_base_), 0,
                        guide_cmdbuf_size_);
            // Do NOT call the begin ourselves. xam has its own begin
            // (81A041F0) which derives the whole command buffer from
            // [dev+0x30]: cursor = ptr, base = ptr+4, limit = ptr+160.
            // That field is uninitialised here (it reads 5), so xam
            // rebuilds the buffer at a garbage address and overwrites
            // anything we set up. Give it a real pointer instead and let
            // xam derive the rest through its own path.
            xe::store_and_swap<uint32_t>(
                sm->TranslateVirtual(sc_dev + 0x30u), guide_cmdbuf_base_);
            sc_begin_cursor = guide_cmdbuf_base_;
            static uint32_t sc_b = 0;
            if (++sc_b <= 3) {
              XELOGI("GuideCtx2 begin: dev={:08X} cursor={:08X} "
                     "base={:08X} limit={:08X}",
                     sc_dev, sc_begin_cursor, sr(sc_dev + 0x2B48u),
                     sr(sc_dev + 0x2B50u));
            }
          }
        }
      }
      in_guide_draw_scope = true;
      uint64_t gr = kernel_state()->processor()->Execute(
          gth->thread_state(), guide_draw_fn_, gargs, xe::countof(gargs));
      in_guide_draw_scope = false;
      // --- second rendering context: submit ---
      // The begin left the cursor at buffer-4 and every emitted word
      // advances it by 4, so the distance from where the begin left it
      // is exactly how much the Guide wrote. Submit that directly -
      // this runs inside the title's frame, before its swap, so with
      // guide_bind_title_rt the Guide lands in the buffer about to be
      // presented.
      if (::cvars::guide_second_context_kb > 0 && sc_dev &&
          guide_cmdbuf_base_) {
        auto* sm2 = kernel_state()->memory();
        auto sd = [sm2](uint32_t a) {
          return xe::load_and_swap<uint32_t>(sm2->TranslateVirtual(a));
        };
        // xam allocates its OWN command buffer and puts it in [dev+0x30]
        // (our pointer there gets overwritten), and the cursor is cleared
        // by the end before we can read it. So inspect the buffer
        // contents directly: find the last non-zero word in the window
        // xam set up, which is exactly what it wrote this frame.
        uint32_t xbuf = sd(sc_dev + 0x30u);
        uint32_t xlim = sd(sc_dev + 0x2B50u);
        uint32_t span = (xlim > xbuf && xlim - xbuf < 0x4000u)
                            ? (xlim - xbuf) / 4
                            : 40u;
        uint32_t words = 0;
        if (xbuf) {
          for (uint32_t w = 0; w < span; ++w) {
            if (sd(xbuf + w * 4)) words = w + 1;
          }
        }
        static uint32_t sc_n = 0;
        ++sc_n;
        if (words) {
          auto* gs3 = kernel_state()->emulator()->graphics_system();
          if (gs3 && gs3->command_processor()) {
            uint32_t before = gs3->command_processor()->guide_draw_count_;
            gs3->command_processor()->ExecuteGuestBufferUnsafe(xbuf, words);
            uint32_t after = gs3->command_processor()->guide_draw_count_;
            if (sc_n <= 3 || sc_n % 300 == 0) {
              std::string dump;
              for (uint32_t w = 0; w < 12 && w < span; ++w) {
                dump += fmt::format("{:08X} ", sd(xbuf + w * 4));
              }
              XELOGI("GuideCtx2 #{}: submitted {} words from {:08X}, "
                     "GPU draws +{}; buf: {}",
                     sc_n, words, xbuf, after - before, dump);
            }
          }
        } else if (sc_n <= 3 || sc_n % 300 == 0) {
          XELOGI("GuideCtx2 #{}: xam buffer {:08X} empty (span {} words, "
                 "limit {:08X})",
                 sc_n, xbuf, span, xlim);
        }
      }
      if (::cvars::guide_restore_title_ring) {
        // The Guide has emitted its packets into xam's ring; hand
        // the GPU back the title's so it can present again.
        GuideRestoreTitleRing();
      }
      if (::cvars::guide_diff_draw_writes && !pre_sums.empty()) {
        auto* mmv = kernel_state()->memory();
        uint32_t changed = 0, idx = 0, shown = 0;
        for (auto& r : kRanges) for (uint32_t a = r.lo; a < r.hi; a += kBlk, ++idx) {
          uint32_t sum = 0, pm4 = 0;
          auto* hp = mmv->TranslateVirtual(a);
          if (hp) {
            auto* w = reinterpret_cast<const uint32_t*>(hp);
            for (uint32_t i = 0; i < kBlk / 4; ++i) {
              sum += w[i];
              uint32_t v = xe::byte_swap(w[i]);
              if ((v & 0xC0000000u) == 0xC0000000u) ++pm4;
            }
          }
          if (sum != pre_sums[idx]) {
            ++changed;
            if (shown < 16) {
              XELOGI("DrawDiff: block {:08X} changed, {} type-3-looking words",
                     a, pm4);
              ++shown;
              // The bitmask count is not evidence. Dump the bytes so packet
              // headers can be walked for self-consistency offline.
              if (hp) {
                auto path = std::filesystem::path("dump_" +
                                                  fmt::format("{:08X}", a) +
                                                  ".bin");
                FILE* f = fopen(path.string().c_str(), "wb");
                if (f) {
                  fwrite(hp, 1, kBlk, f);
                  fclose(f);
                }
              }
            }
          }
        }
        XELOGI("DrawDiff: {} of {} blocks changed across the draw", changed,
               pre_sums.size());
        pre_sums.clear();
      }
      if (::cvars::guide_execute_command_stream) {
        // Control: a buffer of zeros must dispatch no draws. If it does, the
        // counter measures something other than what it claims and every
        // number beside it is void.
        static bool ctrl_done = false;
        auto* gsc = kernel_state()->emulator()->graphics_system();
        if (!ctrl_done && gsc && gsc->command_processor()) {
          ctrl_done = true;
          uint32_t z = kernel_state()->memory()->SystemHeapAlloc(4096, 4096);
          if (z) {
            std::memset(kernel_state()->memory()->TranslateVirtual(z), 0, 4096);
            uint32_t b0 = gsc->command_processor()->guide_draw_count_;
            gsc->command_processor()->ExecuteGuestBufferUnsafe(z, 512);
            XELOGI("GuideExec CONTROL: 512 zero words -> {} draws (must be 0)",
                   gsc->command_processor()->guide_draw_count_ - b0);
          }
        }
      }
      if (::cvars::guide_word_diff && !wd_pre.empty()) {
        auto* wm3 = kernel_state()->memory();
        uint32_t runs = 0, total = 0, i = 0;
        while (i < wd_pre.size()) {
          uint32_t now = xe::load_and_swap<uint32_t>(
              wm3->TranslateVirtual(kWdLo + i * 4));
          if (now == wd_pre[i]) { ++i; continue; }
          uint32_t start = i, gap = 0;
          while (i < wd_pre.size() && gap < 16) {
            uint32_t v = xe::load_and_swap<uint32_t>(
                wm3->TranslateVirtual(kWdLo + i * 4));
            if (v == wd_pre[i]) ++gap; else gap = 0;
            ++i;
          }
          uint32_t len = (i - gap) - start;
          total += len;
          // Execute the runs the draw actually wrote. The extent comes from
          // the diff, not from walking headers: the largest run begins at
          // FE03E284, which is exactly the pointer xam hands to VdSwap, while
          // the header walker had guessed FE038000 and captured a fragment.
          if (::cvars::guide_execute_command_stream && len >= 32) {
            auto* gs2 = kernel_state()->emulator()->graphics_system();
            if (gs2 && gs2->command_processor()) {
              // Did the command processor actually interpret this, or merely
              // consume it? If the packets execute, the register file moves.
              // A screenshot cannot answer that - it shows scanout, not what
              // the GPU did.
              // Checksum the WHOLE register file. An earlier version sampled
              // eight registers picked by hand and reported "UNCHANGED" for
              // runs whose very first words are type-0 writes to register
              // 0x0A31 - which was not among the eight. A probe that cannot
              // see the writes it is looking for reports absence either way.
              auto* rf = gs2->register_file();
              uint32_t before = 0, changed_regs = 0;
              static std::vector<uint32_t> reg_snap;
              if (rf) {
                reg_snap.assign(rf->values,
                                rf->values + gpu::RegisterFile::kRegisterCount);
                for (uint32_t v : reg_snap) before += v;
              }
              uint32_t draws_before =
                  gs2->command_processor()->guide_draw_count_;
              gs2->command_processor()->ExecuteGuestBufferUnsafe(
                  kWdLo + start * 4, len);
              uint32_t after = 0;
              if (rf) {
                for (uint32_t i = 0; i < gpu::RegisterFile::kRegisterCount;
                     ++i) {
                  after += rf->values[i];
                  if (rf->values[i] != reg_snap[i]) ++changed_regs;
                }
              }
              // Log the head of each run next to its result. If the runs are
              // real packets that merely start at the wrong offset, the one
              // the processor reacts to should look structurally different
              // from the ones it ignores.
              auto* hm = kernel_state()->memory();
              std::string head;
              for (uint32_t k = 0; k < 6 && k < len; ++k) {
                head += fmt::format("{:08X} ",
                                    xe::load_and_swap<uint32_t>(
                                        hm->TranslateVirtual(kWdLo +
                                                             (start + k) * 4)));
              }
              uint32_t draws_now = gs2->command_processor()->guide_draw_count_;
              XELOGI("GuideExec: run {:08X} +{} words; {} regs, {} DRAWS "
                     "| head {}",
                     kWdLo + start * 4, len, changed_regs,
                     draws_now - draws_before, head);
            }
          }
          if (runs < 12) {
            XELOGI("WordDiff: run {:08X} .. {:08X}  ({} words)",
                   kWdLo + start * 4, kWdLo + (start + len) * 4, len);
          }
          ++runs;
        }
        XELOGI("WordDiff: {} runs, {} words changed", runs, total);
        wd_pre.clear();
      }
      if (::cvars::guide_execute_command_stream) {
        static bool ran = false;
        if (!ran) {
          ran = true;
          auto* em = kernel_state()->memory();
          auto rdc = [em](uint32_t a) {
            return xe::load_and_swap<uint32_t>(em->TranslateVirtual(a));
          };
          // Walk from a block base to the first plausible type-3 header, then
          // follow the chain while it stays self-consistent. Opcode 0x45
          // COND_WRITE and 0x22 DRAW_INDX dominate what the Guide writes.
          for (uint32_t base : {0xFE030000u, 0xFE040000u}) {
            uint32_t start = 0, words = 0;
            for (uint32_t i = 0; i < 0x4000; ++i) {
              uint32_t w = rdc(base + i * 4);
              if ((w >> 30) == 3) {
                uint32_t cnt = ((w >> 16) & 0x3FFF) + 1;
                if (cnt <= 64) { start = base + i * 4; break; }
              }
            }
            if (!start) continue;
            uint32_t i = (start - base) / 4;
            while (i < 0x4000) {
              uint32_t w = rdc(base + i * 4);
              if (w == 0x80000000u) { i++; words++; continue; }
              if ((w >> 30) != 3) break;
              uint32_t cnt = ((w >> 16) & 0x3FFF) + 1;
              if (cnt > 64 || i + 1 + cnt > 0x4000) break;
              i += 1 + cnt;
              words += 1 + cnt;
            }
            XELOGI("GuideExec: {:08X} chain starts {:08X}, {} words",
                   base, start, words);
            if (words > 8) {
              auto* gs = kernel_state()->emulator()->graphics_system();
              if (gs && gs->command_processor()) {
                gs->command_processor()->ExecuteGuestBufferUnsafe(start, words);
                XELOGI("GuideExec: submitted {:08X} +{} words", start, words);
              }
            }
          }
        }
      }
      {
        auto* gsx = kernel_state()->emulator()->graphics_system();
        if (gsx && gsx->command_processor()) {
          uint32_t gd_after = gsx->command_processor()->guide_draw_count_;
          static uint32_t reported = 0;
          if (reported < 5) {
            ++reported;
            XELOGI("GuideDrawGPU: the guest draw dispatched {} GPU draws",
                   gd_after - gd_before);
          }
        }
      }
      if (::cvars::guide_coverage_fn) {
        static bool cov_done = false;
        if (!cov_done) {
          cov_done = true;
          auto* f = kernel_state()->processor()->LookupFunction(
              ::cvars::guide_coverage_fn);
          auto* gf = f ? dynamic_cast<cpu::GuestFunction*>(f) : nullptr;
          if (gf && gf->trace_data().is_valid()) {
            auto& td = gf->trace_data();
            auto* counts =
                reinterpret_cast<uint64_t*>(td.instruction_execute_counts());
            uint32_t n = td.instruction_count(), executed = 0, last = 0;
            for (uint32_t i = 0; i < n; ++i) {
              if (counts[i]) {
                ++executed;
                last = td.start_address() + i * 4;
              }
            }
            XELOGI("Coverage {:08X}: {} of {} instructions executed, furthest "
                   "reached {:08X} (+0x{:X})",
                   ::cvars::guide_coverage_fn, executed, n, last,
                   last - td.start_address());
          } else {
            XELOGI("Coverage {:08X}: no trace data (need "
                   "trace_function_coverage and trace_function_data_path)",
                   ::cvars::guide_coverage_fn);
          }
        }
      }
      static std::atomic<uint32_t> gdraws{0};
      uint32_t gn = ++gdraws;
      if (gn <= 3 || (gn % 300) == 0) {
        // hud's draw (913EAB28) ends with "li r3,0" AFTER the call to
        // XuiRenderPresent, so it discards Present's HRESULT - this return
        // value is 0 whether or not anything was presented. The DC the draw
        // uses is render_obj+12, which is NOT the pointer XuiRenderCreateDC
        // handed back. Read the present gates off the right object.
        auto* mem = kernel_state()->memory();
        auto rdw = [mem](uint32_t a) {
          return xe::load_and_swap<uint32_t>(mem->TranslateVirtual(a));
        };
        uint32_t ddc = rdw(guide_draw_this_ + 12);
        // Did the guest write anything into the buffer we handed it? PM4
        // type-3 packets start 0xC0......, so their presence is checkable
        // rather than a matter of opinion.
        if (guide_syscmdbuf_ptr_) {
          uint32_t nonzero = 0, pm4 = 0, first = 0;
          for (uint32_t i = 0; i < guide_syscmdbuf_size_ / 4 && i < 4096; ++i) {
            uint32_t v = rdw(guide_syscmdbuf_ptr_ + i * 4);
            if (v) {
              if (!nonzero) first = v;
              ++nonzero;
            }
            if ((v & 0xC0000000u) == 0xC0000000u) ++pm4;
          }
          XELOGI("SysCmdBuf {:08X}: {} non-zero words, {} type-3 headers, "
                 "first={:08X}",
                 guide_syscmdbuf_ptr_, nonzero, pm4, first);
        }
        uint32_t dev = ddc ? rdw(ddc + 0x1CCu) : 0;
        XELOGI("Guide composite draw #{} -> {:08X}; draw dc={:08X} "
               "[11C]={:08X} [134]={:08X} [1CC]={:08X}",
               gn, static_cast<uint32_t>(gr), ddc, ddc ? rdw(ddc + 0x11Cu) : 0,
               ddc ? rdw(ddc + 0x134u) : 0, dev);
        // The real present path (819DE94C) picks a surface as
        //   r11 = [dev+32A0] ? [dev+32A0] : [dev+32B0]
        // and immediately does lwz r9,36(r11). Both null => null deref at
        // guest 0x24, which is the crash seen with guide_force_real_present.
        if (dev) {
          XELOGI("Guide device {:08X}: [32A0]={:08X} [32B0]={:08X}", dev,
                 rdw(dev + 0x32A0u), rdw(dev + 0x32B0u));
        }
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
