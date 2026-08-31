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

// Phase 593: ordinal 0x1D8 is in the export table but had no implementation,
// so xam's import of it resolved to nothing. It is the call by which the
// system command buffer is registered, and phase 592 showed the Guide's
// buffer is never handed to the command processor. Log it to establish
// whether xam calls it at all, and with what.
void VdSetSystemCommandBuffer_entry(dword_t r3, dword_t r4) {
  static std::atomic<uint32_t> once{0};
  if (once++ < 4) {
    XELOGI("VdSetSystemCommandBuffer(r3={:08X}, r4={:08X})", uint32_t(r3),
           uint32_t(r4));
  }
}
DECLARE_XBOXKRNL_EXPORT1(VdSetSystemCommandBuffer, kVideo, kStub);

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
// The device's own reservation window, sampled either side of the composite
// draw. 81A042E0 allocates packet space from [dev+0x30] and advances it; that
// is where the draw emitter 819F5D18 writes, and it is NOT the 81A01358 block
// that guide_bind_cmdbuf_kb repoints. Bracketing the cursor gives exactly the
// words one draw emitted, with no diffing and no guessing where the stream
// starts.
// Ordered checkpoints through the pre-draw path. With the ownership claim
// the run wedges before the composite draw is even reached, and the last
// line logged was a device survey - which narrows it to a stretch of code
// with several guest calls in it. Naming each block as it is entered says
// which one, instead of another round of disassembly and inference.
static bool ckpt_on = false;
static uint32_t guide_claim_dev_ = 0;
static uint32_t guide_claim_prev_ = 0;
static std::atomic<void*> guide_stall_thread_{nullptr};
// Recovered 2026-08-29: accessors lost when the refactor incident destroyed
// this file; the storage above survived the replay but these did not.
void* GuideStallThread() { return guide_stall_thread_.load(); }
void GuidePublishStallThread(void* h) { guide_stall_thread_ = h; }
static uint32_t guide_first_visual_node_ = 0;
static uint32_t guide_resv_dev_ = 0;
static uint32_t g_draw_entry_reserve = 0;

// Resolve the guest thread xam recorded as its XUI render thread.
//
// 81778BB8 is six instructions: r10 = [r13+256] (current thread), r11 =
// [0x81D42520] (recorded render thread), return r10 == r11. It is called at
// the entry of 46 functions across the render subsystem and every call site
// traps when it returns 0 - including 81792928, the only route to 819FF7A0,
// which is what sets [dev+16344] and clears the 0x20 latch that makes the
// vertex allocator refuse.
//
// Measured: the slot holds 3002A010 and the bootstrap runs on 30052010, so
// the gate has always failed. Executing against the recorded thread's
// ThreadState makes r13 - and therefore the comparison - come out right,
// which satisfies the gate the way xam means it instead of writing the slot.
static xe::cpu::ThreadState* GuideRenderThreadState() {
  auto* ks = kernel_state();
  if (!ks || !ks->memory()) {
    return nullptr;
  }
  if (!XamAddrInImage(XamUiThreadSlot(), 4)) {
    return nullptr;  // dashroot-only slot; not present on this build
  }
  uint32_t want =
      xe::load_and_swap<uint32_t>(ks->memory()->TranslateVirtual(XamUiThreadSlot()));
  if (!want) {
    return nullptr;
  }
  static bool logged = false;
  for (uint32_t id : ks->GetAllThreadIDs()) {
    auto th = ks->GetThreadByID(id);
    if (th && th->guest_object() == want) {
      if (!logged) {
        logged = true;
        // Name it once. Which xam thread this turns out to be decides the
        // next move: if it is a thread whose proc is 81794BC8 then that loop
        // is what should have called 819FF7A0 on its own, and the question
        // becomes why it is not running rather than which state to borrow.
        XELOGI("GuideRenderThread: guest={:08X} id={} name='{}'", want, id,
               th->thread_name());
      }
      return th->thread_state();
    }
  }
  if (!logged) {
    logged = true;
    XELOGW("GuideRenderThread: [81D42520]={:08X} matches no live XThread", want);
  }
  return nullptr;
}
static uint32_t guide_resv_pre_ = 0;
static uint32_t guide_cmdbuf_base_ = 0;
static uint32_t guide_boot_dc_ = 0;
static uint32_t guide_cmdbuf_size_ = 0;
static uint32_t guide_draw_this_ = 0;

static uint32_t g_xam_lo = 0;
static uint32_t g_xam_hi = 0;
uint32_t XamUiThreadSlot();
uint32_t XamDeviceSlot();
// hud's XUI-init and render entry points, located by signature in the
// emulator and published here. dashroot +0xA898/+0xAB28, retail +0x98F8/
// +0x9B88; the draw-hook install below used the dashroot offsets, which is
// why retail installed no hook and drew nothing.
static uint32_t g_hud_xuiinit = 0;
static uint32_t g_hud_render = 0;
void SetHudEntries(uint32_t xuiinit, uint32_t render) {
  g_hud_xuiinit = xuiinit;
  g_hud_render = render;
  XELOGI("SetHudEntries: xuiinit {:08X} render {:08X}", xuiinit, render);
}
void SetXamImageExtent(uint32_t lo, uint32_t hi) {
  g_xam_lo = lo;
  g_xam_hi = hi;
  // Resolve eagerly. Lazily, these only fire on paths a given build happens to
  // reach - retail never called XamDeviceSlot at all, so it went unvalidated
  // while looking fine. Resolving here means every run reports both, on both
  // builds, whether or not anything later uses them.
  XamUiThreadSlot();
  XamDeviceSlot();
  XamRenderHost();
  XamXuiCtxSlot();
  XamProviderSlot();
}
bool XamAddrInImage(uint32_t addr, uint32_t len) {
  return g_xam_lo && addr >= g_xam_lo && addr + len <= g_xam_hi;
}

// The slot holding xam's recorded UI thread. dashroot puts it at 81D42520;
// retail 17559 at 81AA09F0. Both are read by the same six-instruction identity
// check - lwz rX,256(r13) (current thread), load the slot, subtract, cntlzw,
// rlwinm ..,27,31,31 - so find that shape and decode the address out of its
// lis/addi/lwz triple rather than hardcoding either value.
static uint32_t g_ui_thread_slot = 0;
static bool g_ui_thread_slot_done = false;
// xam's device global. dashroot puts it at 81D43684, retail 17559 at
// 81AA1B14. Both are the out-parameter of the same device-creation function,
// whose head is the XboxHardwareInfo bit-0x200 gate; the slot address is built
// by a lis/addi pair 0x58 bytes in. Find the function by masked signature and
// decode the pair, rather than hardcoding either address.
static uint32_t g_xam_dev_slot = 0;
static bool g_xam_dev_slot_done = false;
// xam's XUI render host. dashroot 8178DC58, retail 17559 816C8328. The two
// are the same function except retail has no thread-identity assertion, which
// is why a whole-prologue signature and a role match both failed (phases
// 429-430). Anchor on what they share: the standard prologue with a 240-byte
// frame, then within a few instructions memset(r1+100, 0, 120).
static uint32_t g_render_host = 0;
static bool g_render_host_done = false;
// The XUI context pointer slot. dashroot 81D6C978, retail 17559 81AC7DE4.
// No shape-based scan finds it: its dashroot accessor has no counterpart in
// retail, and dashroot's copy carries CALLCAP marker nops that shift every
// offset (phase 433). Positional anchor instead - in BOTH builds the render
// host calls a ctx-reading helper immediately before it calls XuiInit, and
// that helper's first global load is the slot.
static uint32_t g_xui_ctx_slot = 0;
static uint32_t g_xui_createdc = 0;  // the helper itself, not just its slot
static bool g_xui_ctx_slot_done = false;
uint32_t XamXuiCreateDC() { return g_xui_createdc; }
// The XUI resource-provider slot. dashroot 81D6D0AC, retail 17559 81AC82FC.
// Both are the SECOND global store performed by XuiInit (export ordinal
// 0x340), and in both builds the second and third stores are 0xC apart - a
// structural corroboration that the ordering is the same object, not a
// coincidence of position.
static uint32_t g_provider_slot = 0;
static bool g_provider_slot_done = false;
// Is the loaded xam the dashroot build these hardcoded tables came from?
// Some blocks are lists of dozens of build-specific function addresses (the
// 39 XUI class registrars, the device-creation entries) where converting each
// by signature is not practical. Those addresses all fall inside retail's
// image too, so a range check cannot reject them - the resolved structures
// can. If the device and UI-thread slots are dashroot's, the tables apply.
bool XamIsDashrootLayout() {
  return XamDeviceSlot() == 0x81D43684u && XamUiThreadSlot() == 0x81D42520u;
}

// Phase 440 left 22 hardcoded dashroot addresses that this research code calls
// outright. Four have now been caught one crash at a time (817503E8,
// 81A01358, XuiObjectFromHandle, 818FB2B8). Rewriting the 39 call sites is
// what destroyed this file, so instead each constant is wrapped in place:
// GuideConst(0x818FB038u) is a token swap, which cannot reorder anything.
//
// On a non-dashroot build it returns the address of a `blr` inside the loaded
// xam image - a real, executable, already-mapped instruction that returns
// immediately - so the call is harmless instead of jumping into an unrelated
// function. Returning 0 would just move the fault to address 0.
static uint32_t g_nop_fn = 0;
uint32_t GuideNopFn() {
  if (g_nop_fn) return g_nop_fn;
  if (!kernel_state() || !g_xam_lo) return 0;
  uint32_t lo = g_xam_lo, hi = g_xam_hi;
  auto* mem = kernel_state()->memory();
  for (uint32_t a = lo; a && a + 4 <= hi; a += 4) {
    if (xe::load_and_swap<uint32_t>(mem->TranslateVirtual(a)) == 0x4E800020u) {
      g_nop_fn = a;
      XELOGI("GuideConst: using blr at {:08X} as the no-op for this build", a);
      break;
    }
  }
  return g_nop_fn;
}

// Phase 519: [81D6C9C8] holds the stand-in at bootstrap entry and reads zero by
// XuiRenderCreateDC, so one of the early bootstrap calls clears it wholesale -
// no store to it exists anywhere in the image. Installing once is therefore not
// enough; re-establish it wherever it is found null.
static void GuideEnsureStandin(const char* tag) {
  if (!::cvars::guide_patch_skin_dispatch) return;
  auto* m = kernel_state()->memory();
  uint32_t cur = xe::load_and_swap<uint32_t>(m->TranslateVirtual(0x81D6C9C8u));
  if (cur) {
    XELOGI("Standin @{}: already {:08X}", tag, cur);
    return;
  }
  uint32_t blk = m->SystemHeapAlloc(0x80, 16);
  uint32_t nopfn = GuideNopFn();
  if (!blk || !nopfn) {
    XELOGW("Standin @{}: cannot build (blk={:08X} nop={:08X})", tag, blk, nopfn);
    return;
  }
  std::memset(m->TranslateVirtual(blk), 0, 0x80);
  uint32_t vt = blk + 0x40u;
  xe::store_and_swap<uint32_t>(m->TranslateVirtual(blk), vt);
  for (uint32_t sl = 0; sl < 16; ++sl) {
    xe::store_and_swap<uint32_t>(m->TranslateVirtual(vt + sl * 4u), nopfn);
  }
  xe::store_and_swap<uint32_t>(m->TranslateVirtual(0x81D6C9C8u), blk);
  XELOGI("Standin @{}: installed {:08X}", tag, blk);
}

uint32_t GuideConst(uint32_t addr) {
  if (XamIsDashrootLayout()) return addr;
  static uint32_t reported[64] = {};
  static size_t reported_n = 0;
  bool seen = false;
  for (size_t i = 0; i < reported_n; ++i) {
    if (reported[i] == addr) { seen = true; break; }
  }
  if (!seen) {
    if (reported_n < xe::countof(reported)) reported[reported_n++] = addr;
    XELOGW("GuideConst: {:08X} is a dashroot address - substituting a no-op",
           addr);
  }
  uint32_t nop = GuideNopFn();
  return nop ? nop : addr;
}

// Phase 509: a fixed 1280x720 surface needs 0x2AA blocks and the pool typically
// has fewer free, so 819E7528 fails its fit check at 819E75EC and the Guide ends
// up with no render target at all (phase 508). Fall back through smaller sizes
// and take the first that fits - 852x480 matches the measured element bounds and
// is what this picks in practice.
//
// This is a workaround, not the fix: a smaller target means the Guide renders at
// the wrong resolution. It exists so the open question from phase 506 - whether
// the missing render target is what stops the elements emitting - can be tested.
// Phase 517: patch one guest instruction, with the protect/verify/log dance the
// RT-unbind patch needed. The xam text pages are mapped read-only and the guest
// heap's Protect() does not lift that, so this goes at the host page directly;
// a bare store faults at the host mapping and kills the run before logging,
// which reads exactly like "the patch code never executed".
// Phase 567: resolve a XUI handle to its object through the generation-checked
// table at 81D6D0D8. Phases 558-566 traced every remaining failure to this
// harness installing handles where hud and xam expect object pointers; the fix
// has to be applied at every install, not one, which is why phase 559's
// single-site change moved the fault instead of clearing it.
uint32_t GuideResolveHandle(uint32_t handle) {
  if (!handle) return 0;
  auto* m = kernel_state()->memory();
  auto r = [m](uint32_t a) {
    return xe::load_and_swap<uint32_t>(m->TranslateVirtual(a));
  };
  uint32_t idx = handle & 0xFFFFu, tag = handle >> 16;
  uint32_t bucket = r(0x81D6D0D8u + (idx >> 8) * 4u);
  if (!bucket) return 0;
  uint32_t entry = bucket + (idx & 0xFFu) * 8u;
  return (r(entry) == tag) ? r(entry + 4u) : 0;
}

bool GuidePatchWord(uint32_t addr, uint32_t expect, uint32_t value,
                    const char* name) {
  auto* pm = kernel_state()->memory();
  uint8_t* hp = pm->TranslateVirtual(addr);
  xe::memory::PageAccess old_access = xe::memory::PageAccess::kNoAccess;
  void* page = reinterpret_cast<void*>(
      reinterpret_cast<uintptr_t>(hp) & ~0xFFFull);
  if (!xe::memory::Protect(page, 0x1000, xe::memory::PageAccess::kReadWrite,
                           &old_access)) {
    XELOGW("{}: cannot unprotect {:08X}", name, addr);
    return false;
  }
  uint32_t was = xe::load_and_swap<uint32_t>(hp);
  bool ok = (was == expect);
  if (ok) {
    xe::store_and_swap<uint32_t>(hp, value);
    XELOGI("{}: {:08X} {:08X} -> {:08X} (readback {:08X})", name, addr, was,
           value, xe::load_and_swap<uint32_t>(hp));
  } else {
    XELOGW("{}: {:08X} reads {:08X}, expected {:08X} - not patching", name,
           addr, was, expect);
  }
  xe::memory::Protect(page, 0x1000, old_access, nullptr);
  return ok;
}

static uint32_t GuideMakeSurface(xe::cpu::Processor* proc,
                                 xe::cpu::ThreadState* ts) {
  static const uint32_t kSizes[][2] = {{1280, 720}, {852, 480}, {640, 480},
                                       {640, 640},  {512, 512}, {256, 256}};
  for (const auto& wh : kSizes) {
    uint64_t ca[] = {wh[0], wh[1], 0x18280186u, 0, 0};
    uint32_t s = static_cast<uint32_t>(
        proc->Execute(ts, GuideConst(0x819E7528u), ca, xe::countof(ca)));
    if (s) {
      XELOGI("GuideMakeSurface: {}x{} -> {:08X}", wh[0], wh[1], s);
      return s;
    }
    XELOGW("GuideMakeSurface: {}x{} did not fit", wh[0], wh[1]);
  }
  return 0;
}

// Phase 588: bind a render target on an arbitrary device. DrawSurfBind
// already does exactly this, but only for the device the draw hook happens to
// hold; the device hud's render is driven against is a different one, reached
// as [[bootDC+0x1CC]+0x0C], and nothing has ever bound a surface on it.
// `ts` is a xe::cpu::ThreadState* (kept as void* so this header does not have
// to pull in the cpu headers, matching GuidePublishStallThread).
static void EmitGuideCoverageOnce();  // defined below

// Phase 589: the readbacks all sit at the end of something - the paint loop,
// or 900 swaps in. A guest crash reaches neither, so coverage went silent in
// exactly the runs where the path mattered most. Callable from the crash
// handler.
void GuideEmitCoverageNow() { EmitGuideCoverageOnce(); }

uint32_t GuideBindDeviceRt(uint32_t dev, void* ts) {
  if (!dev || !ts) {
    return 0;
  }
  auto* mem = kernel_state()->memory();
  auto rdv = [&](uint32_t a) {
    return xe::load_and_swap<uint32_t>(mem->TranslateVirtual(a));
  };
  uint32_t existing = rdv(dev + 0x32B0u);
  if (existing) {
    return existing;
  }
  uint32_t surf = GuideMakeSurface(kernel_state()->processor(),
                                   static_cast<xe::cpu::ThreadState*>(ts));
  if (!surf) {
    XELOGW("GuideBindDeviceRt: surface creation returned 0 for dev {:08X}",
           dev);
    return 0;
  }
  for (uint32_t off : {0x32A0u, 0x32B0u, 0x3F70u, 0x3F74u, 0x3F78u}) {
    xe::store_and_swap<uint32_t>(mem->TranslateVirtual(dev + off), surf);
  }
  XELOGI("GuideBindDeviceRt: dev={:08X} surf={:08X} -> [32A0]={:08X} "
         "[32B0]={:08X} [3F78]={:08X}",
         dev, surf, rdv(dev + 0x32A0u), rdv(dev + 0x32B0u),
         rdv(dev + 0x3F78u));
  return surf;
}

// Phase 589: the same command-buffer setup the draw hook performs, but for an
// arbitrary device. 81A015B8 emits a packet word with `stw r10, 0(r11)` where
// r11 = [cursor]+4, so a zero cursor stores to address 4 - the 81A01638 fault.
// The hook's version (guide_bind_cmdbuf_kb) computes its device by this exact
// chain but keys off guide_draw_this_ and only runs from a draw that is not
// firing here, so this device never got a buffer.
uint32_t GuideBindDeviceCmdbuf(uint32_t dev, void* ts, uint32_t kb) {
  if (!dev || !ts || !kb) {
    return 0;
  }
  auto* mem = kernel_state()->memory();
  auto rdv = [&](uint32_t a) {
    return xe::load_and_swap<uint32_t>(mem->TranslateVirtual(a));
  };
  if (rdv(dev + 0x2B4Cu)) {
    return rdv(dev + 0x2B48u);  // already bound
  }
  uint32_t csize = kb * 1024u;
  static uint32_t cbuf = 0;
  if (!cbuf) {
    // Phase 592: this used the default (virtual) heap and got 0x3008F000,
    // while every buffer xam uses itself sits at 0x40xxxxxx. A command
    // buffer has to be memory the GPU can fetch from, so allocate physical.
    cbuf = mem->SystemHeapAlloc(csize, 4096, kSystemHeapPhysical);
    if (cbuf) {
      std::memset(mem->TranslateVirtual(cbuf), 0, csize);
    }
  }
  if (!cbuf) {
    XELOGW("GuideBindDeviceCmdbuf: allocation of {} bytes failed", csize);
    return 0;
  }
  // Go through xam's own setter: it sets base +2B48, cursor +2B4C = buf-4
  // (emission does +4 before each store), limit +2B50 and stride +2B58
  // together, and asserts the cursor is currently 0.
  uint64_t cargs[] = {dev, cbuf, csize / 4};
  kernel_state()->processor()->Execute(
      static_cast<xe::cpu::ThreadState*>(ts), GuideConst(0x81A01358u), cargs,
      xe::countof(cargs));
  // 81A042E0, the reservation that seeds the emitter's cursor, allocates from
  // [dev+0x30]/[dev+0x34] - a different pair from the block above, and the
  // fit test fails if they are not widened too.
  xe::store_and_swap<uint32_t>(mem->TranslateVirtual(dev + 0x30u), cbuf);
  xe::store_and_swap<uint32_t>(mem->TranslateVirtual(dev + 0x34u),
                               cbuf + csize);
  XELOGI("GuideBindDeviceCmdbuf: dev={:08X} buf={:08X} +2B48={:08X} "
         "+2B4C={:08X} +2B50={:08X} [30]={:08X} [34]={:08X}",
         dev, cbuf, rdv(dev + 0x2B48u), rdv(dev + 0x2B4Cu),
         rdv(dev + 0x2B50u), rdv(dev + 0x30u), rdv(dev + 0x34u));
  return cbuf;
}

uint32_t XamProviderSlot() {
  if (g_provider_slot_done) {
    return g_provider_slot;
  }
  g_provider_slot_done = true;
  auto* mem = kernel_state() ? kernel_state()->memory() : nullptr;
  auto xam = kernel_state() ? kernel_state()->GetModule("xam.xex", true)
                            : nullptr;
  uint32_t xui_init = xam ? xam->GetProcAddressByOrdinal(0x340) : 0;
  if (!mem || !xui_init) {
    return 0;
  }
  uint32_t hi = 0, rt = 0, seen = 0;
  for (uint32_t off = 0; off < 0x400; off += 4) {
    uint32_t w =
        xe::load_and_swap<uint32_t>(mem->TranslateVirtual(xui_init + off));
    if ((w >> 26) == 15 && ((w >> 16) & 31) == 0) {
      hi = w & 0xFFFF;
      rt = (w >> 21) & 31;
    } else if (hi && (w >> 26) == 36 && ((w >> 16) & 31) == rt) {
      int32_t lo = static_cast<int16_t>(w & 0xFFFF);
      uint32_t slot = (hi << 16) + lo;
      if (++seen == 2) {
        g_provider_slot = slot;
        XELOGI("XamProviderSlot: XuiInit {:08X} store #2 -> slot {:08X}",
               xui_init, slot);
        return g_provider_slot;
      }
    }
  }
  XELOGW("XamProviderSlot: not resolved on this build");
  return 0;
}

uint32_t XamXuiCtxSlot() {
  if (g_xui_ctx_slot_done) {
    return g_xui_ctx_slot;
  }
  g_xui_ctx_slot_done = true;
  auto* mem = kernel_state() ? kernel_state()->memory() : nullptr;
  uint32_t host = XamRenderHost();
  auto xam = kernel_state() ? kernel_state()->GetModule("xam.xex", true)
                            : nullptr;
  uint32_t xui_init = xam ? xam->GetProcAddressByOrdinal(0x340) : 0;
  if (!mem || !host || !xui_init) {
    XELOGW("XamXuiCtxSlot: need render host and XuiInit; have {:08X}/{:08X}",
           host, xui_init);
    return 0;
  }
  // Walk the host's calls; remember the previous one; stop at XuiInit.
  uint32_t prev = 0;
  for (uint32_t off = 0; off < 0x200; off += 4) {
    uint32_t w = xe::load_and_swap<uint32_t>(mem->TranslateVirtual(host + off));
    if ((w >> 26) != 18 || !(w & 1)) continue;
    int32_t li = (w >> 2) & 0xFFFFFF;
    if (li & 0x800000) li -= 0x1000000;
    uint32_t tgt = host + off + (li << 2);
    if (tgt == xui_init) {
      if (!prev) break;
      g_xui_createdc = prev;
      // First lis/lwz global read inside that helper is the slot.
      uint32_t hi = 0, rt = 0;
      for (uint32_t o2 = 0; o2 < 0x80; o2 += 4) {
        uint32_t v =
            xe::load_and_swap<uint32_t>(mem->TranslateVirtual(prev + o2));
        if ((v >> 26) == 15 && ((v >> 16) & 31) == 0) {
          hi = v & 0xFFFF;
          rt = (v >> 21) & 31;
        } else if (hi && (v >> 26) == 32 && ((v >> 16) & 31) == rt) {
          int32_t lo = static_cast<int16_t>(v & 0xFFFF);
          g_xui_ctx_slot = (hi << 16) + lo;
          XELOGI("XamXuiCtxSlot: helper {:08X} -> slot {:08X}", prev,
                 g_xui_ctx_slot);
          return g_xui_ctx_slot;
        }
      }
      break;
    }
    prev = tgt;
  }
  XELOGW("XamXuiCtxSlot: not resolved on this build");
  return 0;
}

uint32_t XamRenderHost() {
  if (g_render_host_done) {
    return g_render_host;
  }
  g_render_host_done = true;
  auto* mem = kernel_state() ? kernel_state()->memory() : nullptr;
  if (!mem || !g_xam_lo) {
    return 0;
  }
  for (uint32_t a = g_xam_lo; a + 48 <= g_xam_hi; a += 4) {
    uint32_t w[12];
    for (int k = 0; k < 12; ++k) {
      w[k] = xe::load_and_swap<uint32_t>(mem->TranslateVirtual(a + k * 4));
    }
    if (w[0] != 0x7D8802A6u || w[1] != 0x9181FFF8u || w[2] != 0xFBE1FFF0u) {
      continue;
    }
    if ((w[3] >> 26) != 37 || (w[3] & 0xFFFF) != 0xFF10u) {
      continue;  // stwu r1,-240(r1)
    }
    for (int j = 4; j < 10; ++j) {
      if (w[j] == 0x38A00078u && w[j + 1] == 0x38800000u &&
          (w[j + 2] & 0xFFFF0000u) == 0x38610000u &&
          (w[j + 2] & 0xFFFFu) == 100u) {
        g_render_host = a;
        XELOGI("XamRenderHost: located at {:08X}", a);
        return g_render_host;
      }
    }
  }
  XELOGW("XamRenderHost: not found in this xam image");
  return 0;
}

uint32_t XamDeviceSlot() {
  if (g_xam_dev_slot_done) {
    return g_xam_dev_slot;
  }
  g_xam_dev_slot_done = true;
  auto* mem = kernel_state() ? kernel_state()->memory() : nullptr;
  if (!mem || !g_xam_lo) {
    return 0;
  }
  // lis rX,0x815F / lwz rX,imm(rX) / lwz rX,0(rX) / rlwinm rX,rX,0,22,22
  for (uint32_t a = g_xam_lo; a + 0x60 <= g_xam_hi; a += 4) {
    uint32_t w0 = xe::load_and_swap<uint32_t>(mem->TranslateVirtual(a + 0x0C));
    if ((w0 >> 26) != 15 || ((w0 >> 16) & 31) != 0 || (w0 & 0xFFFF) != 0x815F) {
      continue;
    }
    uint32_t w3 = xe::load_and_swap<uint32_t>(mem->TranslateVirtual(a + 0x18));
    if ((w3 >> 26) != 21 || ((w3 >> 11) & 31) != 0 || ((w3 >> 6) & 31) != 22 ||
        ((w3 >> 1) & 31) != 22) {
      continue;  // not the rlwinm rX,rX,0,22,22 bit-0x200 test
    }
    uint32_t hi = xe::load_and_swap<uint32_t>(mem->TranslateVirtual(a + 0x54));
    uint32_t lo = xe::load_and_swap<uint32_t>(mem->TranslateVirtual(a + 0x58));
    if ((hi >> 26) == 15 && (lo >> 26) == 14) {
      g_xam_dev_slot = ((hi & 0xFFFF) << 16) + (lo & 0xFFFF);
      XELOGI("XamDeviceSlot: creator at {:08X} -> slot {:08X}", a,
             g_xam_dev_slot);
      return g_xam_dev_slot;
    }
  }
  XELOGW("XamDeviceSlot: device creator not found in this xam image");
  return 0;
}

uint32_t XamUiThreadSlot() {
  if (g_ui_thread_slot_done) {
    return g_ui_thread_slot;
  }
  g_ui_thread_slot_done = true;
  auto* mem = kernel_state() ? kernel_state()->memory() : nullptr;
  if (!mem || !g_xam_lo) {
    return 0;
  }
  for (uint32_t a = g_xam_lo; a + 32 <= g_xam_hi; a += 4) {
    uint32_t w[8];
    for (int k = 0; k < 8; ++k) {
      w[k] = xe::load_and_swap<uint32_t>(mem->TranslateVirtual(a + k * 4));
    }
    bool cur = false, clz = false, norm = false;
    for (int k = 0; k < 8; ++k) {
      if (k < 3 && (w[k] >> 26) == 32 && ((w[k] >> 16) & 31) == 13 &&
          (w[k] & 0xFFFF) == 256) {
        cur = true;
      }
      if ((w[k] >> 26) == 31 && ((w[k] >> 1) & 0x3FF) == 26) clz = true;
      if ((w[k] >> 26) == 21 && ((w[k] >> 11) & 31) == 27 &&
          ((w[k] >> 6) & 31) == 31 && ((w[k] >> 1) & 31) == 31) {
        norm = true;
      }
    }
    if (!cur || !clz || !norm) continue;
    uint32_t hi = 0, lo1 = 0, lo2 = 0;
    bool hi_ok = false, lo1_ok = false, lo2_ok = false;
    for (int k = 0; k < 8; ++k) {
      if ((w[k] >> 26) == 15 && ((w[k] >> 16) & 31) == 0 && !hi_ok) {
        hi = w[k] & 0xFFFF;
        hi_ok = true;
      } else if ((w[k] >> 26) == 14 && hi_ok && !lo1_ok) {
        lo1 = w[k] & 0xFFFF;
        lo1_ok = true;
      } else if ((w[k] >> 26) == 32 && ((w[k] >> 16) & 31) != 13 && !lo2_ok) {
        lo2 = w[k] & 0xFFFF;
        lo2_ok = true;
      }
    }
    if (hi_ok && lo1_ok && lo2_ok) {
      g_ui_thread_slot = (hi << 16) + lo1 + lo2;
      XELOGI("XamUiThreadSlot: identity check at {:08X} -> slot {:08X}", a,
             g_ui_thread_slot);
      return g_ui_thread_slot;
    }
  }
  XELOGW("XamUiThreadSlot: identity check not found in this xam image");
  return 0;
}

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
static uint32_t guide_title_surface_ = 0;
// Set from the 81A0FE48 breakpoint: the device mode 1 sets up, which is never
// published to any global and cannot be found by signature scan.
static std::atomic<uint32_t> guide_mode1_device_{0};
void GuideSetMode1Device(uint32_t dev) { guide_mode1_device_.store(dev); }

// 819F4D28 arg5 (r7). See the header for why this is captured there rather
// than at 81A0FE48: that function only runs on the mode-1 path, so a
// breakpoint on it never fires in the mode-2 configuration that needs the
// value.
static std::atomic<uint32_t> guide_devcreate_arg5_{0};
void GuideSetDevCreateArg5(uint32_t ptr) { guide_devcreate_arg5_.store(ptr); }
uint32_t GuideGetDevCreateArg5() { return guide_devcreate_arg5_.load(); }
static uint32_t guide_bs_scene_ = 0;
static std::atomic<bool> guide_bs_ready_{false};

uint32_t GuideBootDc() { return guide_boot_dc_; }

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
  auto rd = [&](uint32_t a) -> uint32_t {
    // This bootstrap is full of dashroot-derived xam constants. On a build
    // whose image ends lower they are unmapped and reading them host-faults,
    // so reject 0x81xxxxxx addresses outside the loaded image. Guarding the
    // reader rather than each call site: patching individual sites missed the
    // same constant three times.
    if (a >= 0x81000000u && a < 0x82000000u && !XamAddrInImage(a, 4)) {
      return uint32_t(0);
    }
    return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(a));
  };
  // Same bound for the writes below - a store to an unmapped constant faults
  // just as readily as a load.
  auto wr = [&](uint32_t a, uint32_t v) {
    if (a >= 0x81000000u && a < 0x82000000u && !XamAddrInImage(a, 4)) {
      return false;
    }
    xe::store_and_swap<uint32_t>(memory->TranslateVirtual(a), v);
    return true;
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
      wr(XamDeviceSlot(), title_dev);
      XELOGI("GuideBootstrap: xam device global -> title device {:08X}",
             title_dev);
    }
  }

  {
    // xam's render host first checks that the caller is the thread recorded at
    // 0x81D42520, comparing it against [r13+256]. Log both: if the recorded
    // slot is zero, xam never registered a UI thread and the check can never
    // pass no matter which thread we use.
    uint32_t recorded = rd(XamUiThreadSlot());
    uint32_t r13 = static_cast<uint32_t>(ts->context()->r[13]);
    uint32_t current = r13 ? rd(r13 + 256) : 0;
    // Phase 518: [81D6C9C8] is null again by the time the bootstrap runs, even
    // though a stand-in was installed after skin init - the object is released
    // and the global cleared in between, so the fix has to be re-applied on
    // this thread rather than once at init time.
    if (::cvars::guide_patch_skin_dispatch) {
      auto* sm = kernel_state()->memory();
      uint32_t cur = xe::load_and_swap<uint32_t>(
          sm->TranslateVirtual(0x81D6C9C8u));
      if (!cur) {
        uint32_t blk = sm->SystemHeapAlloc(0x80, 16);
        uint32_t nopfn = GuideNopFn();
        if (blk && nopfn) {
          std::memset(sm->TranslateVirtual(blk), 0, 0x80);
          uint32_t vt = blk + 0x40u;
          xe::store_and_swap<uint32_t>(sm->TranslateVirtual(blk), vt);
          for (uint32_t sl = 0; sl < 16; ++sl) {
            xe::store_and_swap<uint32_t>(sm->TranslateVirtual(vt + sl * 4u),
                                         nopfn);
          }
          xe::store_and_swap<uint32_t>(sm->TranslateVirtual(0x81D6C9C8u), blk);
          XELOGI("GuideBootstrap: re-installed [81D6C9C8] stand-in {:08X}", blk);
        }
      } else {
        XELOGI("GuideBootstrap: [81D6C9C8] already {:08X}", cur);
      }
      // The crash at 819138F0 reports r3=0 while this global reads non-null a
      // few lines earlier, and nothing in the image writes it - only one
      // instruction anywhere materialises its base. That combination is
      // impossible if the offline xam.bin matches the loaded image, so check
      // the assumption every phase since 461 has rested on: dump the actual
      // guest bytes and compare with what research/ppcdis.py decodes.
      {
        std::string got;
        for (uint32_t a = 0x819138CCu; a <= 0x819138F4u; a += 4) {
          got += fmt::format("{:08X} ", xe::load_and_swap<uint32_t>(
                                            sm->TranslateVirtual(a)));
        }
        XELOGI("ImageCheck 819138CC: {}", got);
        XELOGI("ImageCheck expected: 3D6081D7 FC20F890 3B5F0028 3B2BC9C5 "
               "38E00000 7F48D378 7F86E378 38810060 80790003 81630000 "
               "816B000C");
      }
    }
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
        if (wr(0x81D3F924u, stub)) {
          XELOGI("GuideBootstrap: CHUDBkgndScene slot 81D3F924 = stub {:08X}",
                 stub);
        } else {
          XELOGW("GuideBootstrap: CHUDBkgndScene slot 81D3F924 is not in this "
                 "xam image - not written");
        }
      }
    }
  }
  uint32_t saved_ui_thread = 0;
  bool spoofed = false;
  if (::cvars::guide_spoof_ui_thread) {
    uint32_t r13 = static_cast<uint32_t>(ts->context()->r[13]);
    uint32_t current = r13 ? rd(r13 + 256) : 0;
    if (current) {
      saved_ui_thread = rd(XamUiThreadSlot());
      wr(XamUiThreadSlot(), current);
      spoofed = true;
      XELOGI("GuideBootstrap: spoofing xam UI thread {:08X} -> {:08X}",
             saved_ui_thread, current);
    }
  }
  if (::cvars::guide_use_bound_device) {
    uint32_t bound = rd(0x801E6FC8u);
    uint32_t cur = rd(XamDeviceSlot());
    if (bound && bound != cur) {
      wr(XamDeviceSlot(), bound);
      guide_prev_device_ = cur;  // keep the displaced device visible
      XELOGI("GuideBootstrap: xam device global {:08X} (RT0={:08X}) -> "
             "{:08X} (RT0={:08X})",
             cur, cur ? rd(cur + 0x32A0u) : 0, bound, rd(bound + 0x32A0u));
    } else {
      XELOGI("GuideBootstrap: device global unchanged ({:08X}, bound={:08X})",
             cur, bound);
    }
  }
  // 8178DC58 is xam's render-host init. It builds a XUI context and stores
  // it at 81D6C978 - and when one is already there it builds a *second* one
  // and frees the first. Anything holding the old pointer is then dangling,
  // which is what killed the run: 81D6C978 went 40877DC0 -> 408BCA60 here,
  // and a device context created earlier kept [dc+0x1C8] = 40877DC0. By the
  // time it called through it the block had been reused for the wide string
  // "XuiScene", so CTR took 006E0065 - the "ne" - and the fetch faulted.
  uint32_t live_ctx = rd(XamXuiCtxSlot());
  uint64_t hr = 0;
  if (::cvars::guide_reuse_xui_ctx && live_ctx) {
    XELOGI(
        "GuideBootstrap: render host skipped, reusing live XUI ctx {:08X}",
        live_ctx);
  } else {
    uint64_t a0[] = {0};
    // 8178DC58 is dashroot's XUI render host. Unlike the seven structures
    // resolved so far, it does NOT signature-match in retail 17559 - not at
    // 14, 12, 10, 8 or 6 instructions, even with frame immediates masked - so
    // that function is restructured there, not merely relocated. Executing
    // dashroot's address on retail landed in unrelated code and crashed in
    // __restgprlr_14 with lr=0.
    //
    // A role-based search (functions that both call the thread-identity check
    // and read the device slot) gives exactly one retail candidate, 816C7D00,
    // but the same search yields FIVE on dashroot, so it does not identify
    // the analogue uniquely. Executing an unconfirmed address is what caused
    // this whole family of bugs, so skip rather than guess.
    uint32_t rhost = XamRenderHost();
    // Phase 597: coverage says this function executes 80 of 80 instructions,
    // which by the disassembly means it returns 0 - but it returns 8000FFFF.
    // One of the two is wrong. Dump the running bytes of its tail and compare
    // against the image being disassembled before trusting either.
    if (rhost) {
      std::string tw;
      for (uint32_t w = 0; w < 10; ++w) {
        tw += fmt::format("{:08X}:{:08X} ", rhost + 0x108u + w * 4,
                          rd(rhost + 0x108u + w * 4));
      }
      XELOGI("GuideRHostBytes: {}", tw);
    }
    if (!rhost) {
      XELOGW("GuideBootstrap: XUI render host not located on this build - "
             "skipping the call");
      hr = 0x80004005u;
    } else {
      hr = processor->Execute(ts, rhost, a0, xe::countof(a0));
    }
  }
  if (spoofed) {
    wr(XamUiThreadSlot(), saved_ui_thread);
    XELOGI("GuideBootstrap: restored xam UI thread {:08X}", saved_ui_thread);
  }
  XELOGI("GuideBootstrap: render host -> {:08X}, XUI ctx {:08X}, "
         "provider {:08X}",
         static_cast<uint32_t>(hr), rd(XamXuiCtxSlot()), rd(XamProviderSlot()));
  {
    // 81901EAC crashes calling through [ctx+0x0C]: the skin-init path does
    // lwz r11,0x1c8(r31) / lwz r11,0xc(r11) / mtctr / bctrl, and CTR held
    // UTF-16 text ("en"). It null-checks the slot but cannot tell text from
    // code. Dump the context head so the slot's real content is visible.
    uint32_t cx = rd(XamXuiCtxSlot());
    if (cx) {
      std::string w;
      for (uint32_t q = 0; q < 8; ++q) {
        w += fmt::format("+{:X}:{:08X} ", q * 4, rd(cx + q * 4u));
      }
      XELOGI("XuiCtxHead: ctx={:08X} {}", cx, w);
    }
  }

  {
    // Read-only: the null-render flag as the render host left it. It is
    // computed, not a constant, so what it depends on is worth probing - the
    // hardware-info word is the cheapest input to vary.
    uint32_t c0 = rd(XamXuiCtxSlot());
    XELOGI("GuideBootstrap: null-render flag [ctx+1C] = {:08X} "
           "(hw info word = {:08X})",
           c0 ? rd(c0 + 0x1Cu) : 0xFFFFFFFFu,
           rd(rd(0x815F048Cu)));
  }
  if (::cvars::guide_clear_null_render) {
    uint32_t ctx = rd(XamXuiCtxSlot());
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
    uint32_t prov = rd(XamProviderSlot());
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
  if (::cvars::guide_register_all_classes && !XamIsDashrootLayout()) {
    XELOGW("GuideBootstrap: the 39 XUI class registrars are dashroot "
           "addresses; on this build they are different functions - skipping "
           "(this is why retail crashed before installing the draw hook)");
  }
  if (::cvars::guide_register_all_classes && XamIsDashrootLayout()) {
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
  // These three are dashroot addresses. On retail 17559, 817503E8 is an
  // unrelated function and executing it is what crashed at 817503E4 - the
  // crash PC matched the constant four bytes in, which is the tell.
  if (::cvars::guide_register_classes && !XamIsDashrootLayout()) {
    XELOGW(
        "GuideBootstrap: the 3 XUI class registrars are dashroot addresses; "
        "on this build they are different functions - skipping");
  } else if (::cvars::guide_register_classes) {
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
    // 818FB038 is dashroot's XuiRenderCreateDC helper. It is the same
    // function the ctx-slot resolver already locates (the call immediately
    // before XuiInit in the render host); on retail it is 8177DAC8. Executing
    // the dashroot address there crashed inside xam's cleanup loop.
    uint32_t createdc = XamXuiCreateDC();
    if (!createdc) createdc = 0x818FB038u;
    XELOGI("GuideBootstrap: XuiRenderCreateDC fn={:08X}", createdc);
    dr = processor->Execute(ts, createdc, a1, xe::countof(a1));
  } else {
    XELOGI("GuideBootstrap: skipping our own XuiRenderCreateDC");
  }
  XELOGI("GuideBootstrap: XuiRenderCreateDC -> {:08X} dc={:08X}",
         static_cast<uint32_t>(dr), rd(dcp));
  GuideEnsureStandin("createdc");
  // Remember our DC. It is the only one in the process whose [+0x1C8]/[+0x1CC]
  // are populated - 81900E70 (the constructor) writes [+0x134] and nothing
  // else, and only 818FDE98 writes those two, reached solely through a vtable
  // dispatch that our bootstrap never performs on hud's own DC. hud's DC is
  // therefore constructed with [+0x1C8] holding whatever the allocation
  // contained, and 81901E40 bctrls through [[dc+0x1C8]+0x0C] into it.
  guide_boot_dc_ = rd(dcp);
  // Phase 576: the dispatcher path calls [DC_vtable + 0x104] and CTR was zero
  // (575). Log the vtable and the slots around 0x104 - if the table is a xam
  // address its real contents are in xam.bin, and a null here means our DC was
  // built from the wrong table rather than that the method does not exist.
  if (guide_boot_dc_) {
    uint32_t vt = rd(guide_boot_dc_);
    std::string sl;
    for (uint32_t off = 0xF8u; off <= 0x110u; off += 4) {
      sl += fmt::format("+{:X}:{:08X} ", off, vt ? rd(vt + off) : 0);
    }
    XELOGI("GuideDCVtable: dc={:08X} vtable={:08X} | {}", guide_boot_dc_, vt,
           sl);
  }

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
  GuideEnsureStandin("string_table");
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
  GuideEnsureStandin("render_obj");
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
      SetGuideDrawHook((g_hud_render ? g_hud_render : guide_bs_hud_base_ + 0xAB28u), render_obj);
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
    if (scene_out) guide_bs_scene_ = rd(scene_out);
    XELOGI("GuideBootstrap: scene creator {:08X} -> {:08X}, scene={:08X}",
           scene_fn, static_cast<uint32_t>(ir),
           scene_out ? rd(scene_out) : 0);
    // Create a scene AND NAVIGATE TO IT, the way hud does.
    //
    // hud never calls the bare creator through a bl - it is vtable dispatch
    // only. Its own path is 913EB7D8(this, parent):
    //   r3 = [this+0x508]                    current scene handle
    //   bl XuiHandleIsValid
    //   if valid: XuiSceneNavigateBack(...)
    //   913EB508(this, parent, L"Status.xur", 0)
    // and 913EB508 is XuiSceneCreate immediately followed by
    // XuiSceneNavigateForward(parent, 0, newScene, transition). Creating
    // without navigating leaves the scene out of whatever the renderer walks,
    // which is the shape of the problem: a tree that lays out correctly and
    // emits no geometry.
    //
    // The scene path comes from hud's own global at hud+0x201E8, which holds
    // a pointer to L"Status.xur" - the Guide's entry screen. Read at runtime
    // from the loaded image rather than hardcoded, since the constant is only
    // meaningful relative to hud's base.
    if (::cvars::guide_navigate_scene && guide_bs_obj_) {
      uint32_t hud_base = guide_bs_hud_base_;
      uint32_t path_ptr = hud_base ? rd(hud_base + 0x201E8u) : 0;
      // The object 913EB7D8 wants is a SINGLETON, not hud's main object.
      // 913E9580 loads it as `lwz r30, 1684(r10)` with r10 = 0x91400000, i.e.
      // [hud+0x20694], and dispatches on [this+0x1F0] to one of four navigate
      // variants; case 1 is `913EB7D8(r30, [this+8])`. Passing guide_bs_obj_
      // was a guess that the create half happened to accept.
      // Build the scene by hand and PARENT IT before navigating.
      //
      // 913EB508 does XuiSceneCreate and XuiSceneNavigateForward back to back,
      // and the scene it creates has no parent, so the navigate rejects it
      // with 8030000B (XuiElementGetParent returns null for both the new and
      // the current scene). There is no way to attach anything in between, so
      // the four steps are performed directly:
      //
      //   XamBuildResourceLocator([obj+4], L"hud", 0, buf, 128)
      //   XuiSceneCreate(locator, L"Status.xur", 0, &newScene)
      //   XuiElementAddChild(currentScene, newScene)
      //   XuiSceneNavigateForward(currentScene, 0, newScene, 0)
      //
      // Only the NEW scene needs a parent: NavigateForward checks it first
      // (8193C31C) and proceeds if either scene has one, which avoids having
      // to invent a host above our existing scene.
      if (::cvars::guide_navigate_manual) {
        auto xmg = kernel_state()->GetModule("xam.xex", true);
        uint32_t f_loc = xmg ? xmg->GetProcAddressByOrdinal(0x31B) : 0;
        uint32_t f_scn = xmg ? xmg->GetProcAddressByOrdinal(0x357) : 0;
        uint32_t f_add = xmg ? xmg->GetProcAddressByOrdinal(0x328) : 0;
        uint32_t f_nav = xmg ? xmg->GetProcAddressByOrdinal(0x35A) : 0;
        uint32_t buf_loc = memory->SystemHeapAlloc(512, 16);
        uint32_t buf_out = memory->SystemHeapAlloc(16, 16);
        uint32_t obj4 = rd(guide_bs_obj_ + 4u);
        uint32_t s_hud = hud_base + 0x1B24u;   // L"hud"
        uint32_t s_path = hud_base ? rd(hud_base + 0x201E8u) : 0;
        {
          // The scene loaded is whatever s_path points at. Phase 283 wants a
          // DIFFERENT scene (GuideMain.xur, the blade itself, vs the
          // Status.xur loading screen), so log the string to learn the format
          // a replacement must follow.
          std::string sp;
          if (s_path) {
            for (uint32_t c = 0; c < 96; ++c) {
              uint32_t addr = s_path + c * 2u;
              uint16_t ch = xe::load_and_swap<uint16_t>(
                  memory->TranslateVirtual(addr));
              if (!ch) break;
              sp += (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
            }
          }
          XELOGI("GuideNavM: scene path ptr={:08X} = L\"{}\"", s_path, sp);
          // Phase 556: the comment above has said since phase 283 that this
          // wants GuideMain.xur rather than Status.xur, and the logging was
          // added to learn the format - but the substitution was never written.
          // So every run since has navigated to the loading screen, which is
          // why the paint chain is a spinner and why thirty-five phases of
          // rendering work found every component correct and nothing to show.
          //
          // Build the replacement in the same format: UTF-16, byte-swapped to
          // match the reads above, NUL-terminated.
          if (!::cvars::guide_scene_name.empty()) {
            static uint32_t s_override = 0;
            if (!s_override) {
              s_override = memory->SystemHeapAlloc(256, 16);
              if (s_override) {
                const std::string& nm = ::cvars::guide_scene_name;
                for (size_t c = 0; c < nm.size() && c < 100; ++c) {
                  xe::store_and_swap<uint16_t>(
                      memory->TranslateVirtual(s_override +
                                               uint32_t(c) * 2u),
                      static_cast<uint16_t>(nm[c]));
                }
                xe::store_and_swap<uint16_t>(
                    memory->TranslateVirtual(s_override +
                                             uint32_t(nm.size()) * 2u),
                    0);
              }
            }
            if (s_override) {
              XELOGI("GuideNavM: substituting scene path {:08X} -> {:08X} "
                     "L\"{}\"",
                     s_path, s_override, ::cvars::guide_scene_name);
              s_path = s_override;
            }
          }
        }
        if (f_loc && f_scn && f_add && f_nav && buf_loc && buf_out && s_path) {
          std::memset(memory->TranslateVirtual(buf_loc), 0, 512);
          std::memset(memory->TranslateVirtual(buf_out), 0, 16);
          uint64_t la[] = {obj4, s_hud, 0, buf_loc, 128};
          uint32_t lr = uint32_t(
              processor->Execute(ts, f_loc, la, xe::countof(la)));
          uint64_t sa[] = {buf_loc, s_path, 0, buf_out};
          uint32_t sr = uint32_t(
              processor->Execute(ts, f_scn, sa, xe::countof(sa)));
          uint32_t new_scene = rd(buf_out);
          XELOGI("GuideNavM: locator -> {:08X}; SceneCreate -> {:08X}, "
                 "scene={:08X}",
                 lr, sr, new_scene);
          if (new_scene) {
            uint64_t ca[] = {guide_bs_scene_, new_scene};
            uint32_t cr = uint32_t(
                processor->Execute(ts, f_add, ca, xe::countof(ca)));
            uint64_t va[] = {guide_bs_scene_, 0, new_scene, 0};
            uint32_t vr = uint32_t(
                processor->Execute(ts, f_nav, va, xe::countof(va)));
            XELOGI("GuideNavM: AddChild({:08X},{:08X}) -> {:08X}; "
                   "NavigateForward -> {:08X}",
                   guide_bs_scene_, new_scene, cr, vr);
            // Navigating does not repoint the draw. hud's composite draw uses
            // [this+8] as its root, which still holds the ORIGINAL scene, so
            // a successful navigate to Status.xur changes what XUI considers
            // current without changing what gets drawn. [this+8] is
            // guide_bs_obj_+0x18, the draw object being the bootstrap object
            // plus 0x10.
            if (!vr && new_scene) {
              // Phase 559: 818FB110 takes an object POINTER - it null-checks
              // and dereferences a vtable, returning E_INVALIDARG for null, and
              // never resolves a handle. On the dispatcher path hud passes this
              // field straight to it, so a handle here faults. Store the
              // resolved object instead when asked.
              uint32_t stored = new_scene;
              if (::cvars::guide_draw_root_object) {
                uint32_t idx = new_scene & 0xFFFFu, tg = new_scene >> 16;
                uint32_t bkt = rd(0x81D6D0D8u + (idx >> 8) * 4u);
                uint32_t ent = bkt ? bkt + (idx & 0xFFu) * 8u : 0;
                if (ent && rd(ent) == tg) {
                  stored = rd(ent + 4u);
                  XELOGI("GuideNavM: draw root as object {:08X} (handle {:08X})",
                         stored, new_scene);
                } else {
                  XELOGW("GuideNavM: handle {:08X} does not resolve; storing it",
                         new_scene);
                }
              }
              xe::store_and_swap<uint32_t>(
                  memory->TranslateVirtual(guide_bs_obj_ + 0x18u), stored);
              guide_bs_scene_ = new_scene;
              XELOGI("GuideNavM: draw root [this+8] -> {:08X} (Status.xur)",
                     new_scene);
            }
          }
        } else {
          XELOGW("GuideNavM: skipped (loc={:08X} scn={:08X} add={:08X} "
                 "nav={:08X} path={:08X})",
                 f_loc, f_scn, f_add, f_nav, s_path);
        }
      }
      uint32_t nav_obj = hud_base ? rd(hud_base + 0x20694u) : 0;
      XELOGI("GuideNav: singleton [hud+0x20694] = {:08X}", nav_obj);
      // Phase 299: resolve the xam exports the tab loader depends on, so
      // they can be disassembled. 0x3BA is XuiTabSceneGetCurrentTab -
      // the comparison at 913E9250 that keeps every tab from activating.
      {
        auto xm = kernel_state()->GetModule("xam.xex", true);
        if (xm) {
          XELOGI("GuideNav: xam exports: XuiTabSceneGetCurrentTab(3BA)={:08X} "
                 "XuiElementInitUserFocus(334)={:08X} "
                 "XuiElementGetUserFocus(332)={:08X}",
                 xm->GetProcAddressByOrdinal(0x3BA),
                 xm->GetProcAddressByOrdinal(0x334),
                 xm->GetProcAddressByOrdinal(0x332));
        }
      }
      // hud has TWO navigate entries. 913EB7D8 (called below) is hardcoded
      // to [9140:01E8] = L"Status.xur". The real dispatcher is 913EC6D0,
      // which selects by state at [navObj+76]:
      //   7 / 3 / 5 / 6 / 8 -> specific scenes
      //   0 or 4            -> GuideMain.xur ([9140:014C]) unless 913FE294()
      //   anything else     -> E_FAIL 0x80004005
      // So this word decides whether the Guide can reach its blade.
      if (nav_obj) {
        XELOGI("GuideNav: state [+72]={} [+76]={} [+88]={} [+240]={} "
               "(76 in {{0,4}} selects GuideMain.xur)",
               rd(nav_obj + 72u), rd(nav_obj + 76u), rd(nav_obj + 88u),
               rd(nav_obj + 240u));
      }
      if (hud_base && path_ptr && nav_obj) {
        // Call hud's own entry, 913EB7D8(navObj, parentScene), rather than
        // the inner helper: it checks [navObj+0x508] with XuiHandleIsValid and
        // navigates back from the current scene first, which is the sequence
        // hud performs and the inner helper assumes has happened.
        // Phase 559: dump the hud code around the unwind frames so it can be
        // disassembled offline. The hud module is not in xam.bin, so this is
        // the only way to read it - the same approach the ImageCheck probe used.
        {
          // Phase 574: 913EA9EC calls a hud thunk at 913FE7D4. Resolving it
          // says which xam function hud is really invoking, and therefore
          // whether that function takes a handle or an object - which is the
          // question three phases of value-substitution failed to settle.
          // Phase 581: 913EA898 branches on [navObj+0x14] and then calls
        // vtable[7] (+0x1C) and vtable[9] (+0x24), forwarding whichever fails.
        // Dump those so the failing method is a named address rather than an
        // inference.
        {
          // Phase 582: 913EA898's `this` is navObj+0x10 (581), so its vtable
          // is [navObj+0x10] and the field it branches on is [navObj+0x24].
          uint32_t sub = nav_obj + 0x10u;
          uint32_t svt = rd(sub);
          XELOGI("GuideNavVT: sub={:08X} vtable={:08X} | [+1C]={:08X} "
                 "[+24]={:08X} [+04]={:08X} | [sub+14]=[navObj+24]={:08X} "
                 "[sub+0C]={:08X}",
                 sub, svt, svt ? rd(svt + 0x1Cu) : 0,
                 svt ? rd(svt + 0x24u) : 0, svt ? rd(svt + 4u) : 0,
                 rd(sub + 0x14u), rd(sub + 0x0Cu));
        }
        // Phase 579: the dispatcher now completes and returns 8000FFFF, so
          // dump 913EC6D0 to find which check produces it. Note the documented
          // "anything else -> 0x80004005" is a different constant, so this is a
          // failure further in, not the state check.
          // Phase 580: 913EA898 is the function whose negative return the
          // dispatcher forwards as 8000FFFF. Every phase since 557 has looked
          // at its callers and callees through crash addresses without reading
          // it.
          // Phase 581: vtable[7] and vtable[9] are the same address,
          // 913EACA8 - the shape of a shared stub. Dump it.
          // Phase 584: read the WHOLE dispatcher this time. Phase 579 dumped
          // 0x80 bytes, stopped at the first exit-shaped branch, and four
          // phases followed from assuming the error came from there.
          for (uint32_t base : {0x913EC750u, 0x913EC7D0u, 0x913EC850u,
                                0x913EC8D0u, 0x913EC950u, 0x913EC9D0u}) {
            std::string hx;
            for (uint32_t a = base; a < base + 0x80u; a += 4) {
              hx += fmt::format("{:08X} ", rd(a));
            }
            XELOGI("HudCode {:08X}: {}", base, hx);
          }
        }
        // Phase 558: the dispatcher faults dereferencing 00010135, a XUI
        // handle, which is what the generation-checked table returns on a miss.
        // Resolve the scene handle through that table by hand and report the
        // tag comparison, so "the scene is not registered" is measured rather
        // than assumed. Layout from phases 458-472: bucket = [base+(idx>>8)*4],
        // entry = bucket + (idx & 0xFF)*8, entry[0] = tag, entry[1] = object,
        // tag = handle >> 16.
        {
          uint32_t hnd = guide_bs_scene_;
          uint32_t idx = hnd & 0xFFFFu, tag = hnd >> 16;
          uint32_t bucket = rd(0x81D6D0D8u + (idx >> 8) * 4u);
          uint32_t entry = bucket ? bucket + (idx & 0xFFu) * 8u : 0;
          XELOGI("GuideHandle: scene {:08X} idx={:04X} tag={:04X} | "
                 "bucket={:08X} entry={:08X} stored_tag={:08X} obj={:08X} -> {}",
                 hnd, idx, tag, bucket, entry, entry ? rd(entry) : 0,
                 entry ? rd(entry + 4u) : 0,
                 (entry && rd(entry) == tag) ? "resolves" : "MISS");
        }
        // Phase 557: [navObj+76] is 1 in every run. Per the dispatch documented
        // above, 913EC6D0 selects GuideMain.xur only for 0 or 4, specific
        // scenes for 7/3/5/6/8, and returns E_FAIL for anything else - so 1
        // selects nothing, and the hardcoded Status entry is the only path that
        // ever runs. Set the state and call the real dispatcher instead.
        if (::cvars::guide_nav_state >= 0) {
          uint32_t want = static_cast<uint32_t>(::cvars::guide_nav_state);
          XELOGI("GuideNav: [navObj+76] {} -> {} and dispatching via 913EC6D0",
                 rd(nav_obj + 76u), want);
          xe::store_and_swap<uint32_t>(memory->TranslateVirtual(nav_obj + 76u),
                                       want);
        }
        // Phase 572: the re-point added in 571 runs after the dispatcher
        // returns, and the fault is inside it - so the dispatcher sees whatever
        // the field held on entry. If that is a handle, fix it before the call
        // rather than after.
        if (::cvars::guide_draw_root_object) {
          uint32_t cur_root = rd(guide_bs_obj_ + 0x18u);
          uint32_t as_obj = GuideResolveHandle(cur_root);
          XELOGI("GuideNav: pre-dispatch draw root {:08X} resolves to {:08X}",
                 cur_root, as_obj);
          // Phase 573: [as_obj+0] read back the handle rather than a vtable
          // (572), so this structure is not the object 818FB110 wants. Dump its
          // first words: a vtable pointer is a code address (81xxxxxx), so
          // whichever field holds one names the real object or the way to it.
          if (as_obj) {
            std::string w;
            for (uint32_t k = 0; k < 16; ++k) {
              w += fmt::format("+{:X}:{:08X} ", k * 4, rd(as_obj + k * 4u));
            }
            XELOGI("HandleObj {:08X}: {}", as_obj, w);
            // 818FB110 wants an object whose +0 is a vtable, i.e. a code
            // address in the 81xxxxxx range. Check each pointer field for that
            // shape rather than guessing which one is the object.
            for (uint32_t off : {0x08u, 0x18u, 0x20u, 0x3Cu}) {
              uint32_t cand = rd(as_obj + off);
              uint32_t first = cand ? rd(cand) : 0;
              XELOGI("HandleCand +{:X} = {:08X}, [it+0]={:08X} {}", off, cand,
                     first,
                     (first >= 0x81000000u && first < 0x82000000u)
                         ? "<- vtable-shaped"
                         : "");
            }
          }
          if (as_obj) {
            xe::store_and_swap<uint32_t>(
                memory->TranslateVirtual(guide_bs_obj_ + 0x18u), as_obj);
          }
        }
        // Phase 572: the second argument is the parent scene, and this passes
        // guide_bs_scene_ - a handle straight from hud's scene creator. The
        // draw-root field was not the source of the faulting handle; this is
        // the other place the same value is handed on.
        // Phase 575: this second argument was assumed to be a parent scene,
        // taken from 913EB7D8's signature. The dispatcher is a different
        // function and hud caches this value at [this+0xC] - the field it then
        // releases through 818FB110, the render-DC release (phase 574). The
        // fault address confirms it: [ [r31+0xC] + 0 ] was 000100A9, and the
        // structure with that at +0 is 40881894, which is exactly what phase
        // 572 passed here. So the parameter is a DC, and every scene value
        // tried in phases 559-573 was the wrong kind of thing.
        uint32_t parent_arg = guide_bs_scene_;
        if (::cvars::guide_nav_state >= 0 && guide_boot_dc_) {
          // Phase 576: the vtable slot is populated at creation
          // (+104:818FA168), so a null CTR at the call means the DC's vtable
          // pointer changed in between. Read it here, at the point of use.
          uint32_t vt_now = rd(guide_boot_dc_);
          XELOGI("GuideNav: passing DC {:08X} as arg2 instead of scene {:08X} "
                 "| [dc+0]={:08X} [vt+104]={:08X}",
                 guide_boot_dc_, guide_bs_scene_, vt_now,
                 vt_now ? rd(vt_now + 0x104u) : 0);
          // The vtable target 818FA168 reads [DC+0x1CC] and dereferences it at
          // 818FA188, which faults at 0 if the field is null. This file claims
          // our DC is the only one with [+0x1C8]/[+0x1CC] populated - check it
          // here rather than trusting the note.
          XELOGI("GuideNav: DC [+1C8]={:08X} [+1CC]={:08X} [+134]={:08X}",
                 rd(guide_boot_dc_ + 0x1C8u), rd(guide_boot_dc_ + 0x1CCu),
                 rd(guide_boot_dc_ + 0x134u));
          // Phase 577: walk the chain 818FA168 takes, one step at a time, so
          // the failing dereference is identified rather than guessed:
          //   [DC+0x1CC] -> w ; [w+0] -> vt ; [vt+0x10] -> fn ; bctrl fn
          {
            uint32_t w = rd(guide_boot_dc_ + 0x1CCu);
            uint32_t wvt = w ? rd(w) : 0;
            uint32_t fn = wvt ? rd(wvt + 0x10u) : 0;
            XELOGI("GuideNav: chain [DC+1CC]={:08X} -> [w+0]={:08X} -> "
                   "[vt+10]={:08X} {}",
                   w, wvt, fn,
                   (!w || !wvt || !fn) ? "<- BREAKS HERE" : "complete");
          }
          // Phase 578: at the fault r31 is our DC and r11/ctr are zero, so the
          // vtable read returned null during the call although it was valid
          // before it. hud's sequence is release-then-store: passing a live DC
          // as arg2 makes it release ours and then call through the corpse.
          // Passing null takes the `beq` that skips the release, leaving hud to
          // create its own.
          parent_arg = ::cvars::guide_nav_dc_null ? 0u : guide_boot_dc_;
        } else if (::cvars::guide_draw_root_object) {
          uint32_t po_ = GuideResolveHandle(guide_bs_scene_);
          if (po_) parent_arg = po_;
        }
        uint64_t na[] = {nav_obj, parent_arg};
        uint32_t nav_entry =
            (::cvars::guide_nav_state >= 0) ? 0xC6D0u : 0xB7D8u;
        // Phase 585: coverage located the failure precisely. 913EC6D0 does
        // `addi r3, r3, 0x10` then `bl 913EA898`, and 913EA898 dispatches
        // through the sub-object's vtable: slot 0x1C at 913EA938, then slot
        // 0x24 at 913EA968. Branch counts show slot 0x24 returning a negative
        // HRESULT on one of three invocations, and 913EC6D0 forwards it
        // unchanged as 8000FFFF. Name the slots so the failing method can be
        // traced in turn.
        if (::cvars::guide_nav_state >= 0) {
          uint32_t sub = nav_obj + 0x10u;
          uint32_t vt = rd(sub);
          XELOGI("GuideNavVT: sub={:08X} vtable={:08X} [+04]={:08X} "
                 "[+1C]={:08X} [+24]={:08X}",
                 sub, vt, vt ? rd(vt + 0x04u) : 0u, vt ? rd(vt + 0x1Cu) : 0u,
                 vt ? rd(vt + 0x24u) : 0u);
          // 913EABE0 fails when [this+8] != 0 and when [this+0x10] == 0.
          // Coverage says the second condition always held and the first is
          // what tripped, so dump the head of the sub-object to see what
          // occupies the slot.
          {
            std::string sh;
            for (uint32_t w = 0; w < 8; ++w) {
              sh += fmt::format("{:02X}:{:08X} ", w * 4, rd(sub + w * 4));
            }
            XELOGI("GuideNavSub: {:08X} {}", sub, sh);
          }
          if (::cvars::guide_nav_clear_slot) {
            uint32_t prev = rd(sub + 8u);
            xe::store_and_swap<uint32_t>(memory->TranslateVirtual(sub + 8u),
                                         0u);
            XELOGI("GuideNavClear: [sub+8] {:08X} -> {:08X}", prev,
                   rd(sub + 8u));
          }
        }
        uint64_t nr = processor->Execute(ts, hud_base + nav_entry, na,
                                         xe::countof(na));
        XELOGI("GuideNav: hud+{:04X}(navObj {:08X}, parent scene {:08X}) -> "
               "{:08X}; [navObj+500]={:08X} [navObj+508]={:08X}",
               nav_entry, nav_obj, guide_bs_scene_,
               static_cast<uint32_t>(nr),
               rd(nav_obj + 0x500u), rd(nav_obj + 0x508u));
        // hud's own path now succeeds: giving the manually created scene a
        // parent first satisfies NavigateForward's check inside 913EB508, so
        // 913EB7D8 builds AND navigates to a properly initialised Status
        // scene and records it at [navObj+0x508]. That scene came through
        // hud's construction path, unlike the phase 192 one, so it is the
        // fair comparison phase 215 said was missing.
        uint32_t hud_scene = rd(nav_obj + 0x508u);
        // 913EB7D8 is hardcoded to Status.xur (phase 284), so this re-point
        // always lands on the loading screen and silently discards a scene
        // chosen with guide_scene_name. If the caller named a scene, that is
        // an explicit choice - leave the draw root where GuideNavM put it.
        // Phase 571: this guard was added because 913EB7D8 always lands on
        // Status, so re-pointing would discard a scene chosen by name. With the
        // real dispatcher (guide_nav_state >= 0) hud_scene IS the requested
        // scene, and refusing to re-point leaves the draw root on the bootstrap
        // scene - which is what rt2 reads, so the paint has been walking the old
        // tree while the blade sat loaded and unreferenced.
        if (!nr && hud_scene && ::cvars::guide_nav_state >= 0) {
          uint32_t st = hud_scene;
          if (::cvars::guide_draw_root_object) {
            uint32_t o = GuideResolveHandle(hud_scene);
            if (o) st = o;
          }
          xe::store_and_swap<uint32_t>(
              memory->TranslateVirtual(guide_bs_obj_ + 0x18u), st);
          guide_bs_scene_ = hud_scene;
          XELOGI("GuideNav: draw root -> {:08X} (dispatcher scene {:08X})", st,
                 hud_scene);
        } else if (!nr && hud_scene && !::cvars::guide_scene_name.empty()) {
          XELOGI("GuideNav: keeping guide_scene_name draw root; NOT "
                 "re-pointing to hud-built Status {:08X}", hud_scene);
        } else if (!nr && hud_scene) {
          // Phase 567: this is the branch that runs when guide_navigate_manual
          // is off - the configuration phase 559 was testing - so the fix
          // applied there never executed.
          uint32_t hud_stored = hud_scene;
          if (::cvars::guide_draw_root_object) {
            uint32_t ho = GuideResolveHandle(hud_scene);
            if (ho) {
              hud_stored = ho;
              XELOGI("GuideNav: draw root as object {:08X} (handle {:08X})", ho,
                     hud_scene);
            }
          }
          xe::store_and_swap<uint32_t>(
              memory->TranslateVirtual(guide_bs_obj_ + 0x18u), hud_stored);
          guide_bs_scene_ = hud_scene;
          XELOGI("GuideNav: draw root [this+8] -> {:08X} (hud-built Status)",
                 hud_scene);
        }
      } else {
        XELOGW("GuideNav: skipped (hud_base={:08X} path_ptr={:08X} "
               "nav_obj={:08X})",
               hud_base, path_ptr, nav_obj);
      }
    }
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
        // XuiObjectFromHandle is xam ordinal 0x346. 81942938 is only its
        // dashroot address; resolve the export the way XuiInit is resolved.
        auto xm_oh = kernel_state()->GetModule("xam.xex", true);
        uint32_t objfh = xm_oh ? xm_oh->GetProcAddressByOrdinal(0x346) : 0u;
        if (!objfh) objfh = 0x81942938u;
        XELOGI("GuideBootstrap: XuiObjectFromHandle fn={:08X}", objfh);
        uint64_t orr = processor->Execute(ts, objfh, oa, xe::countof(oa));
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
          // XuiControlGetVisual (81935B60) resolves the visual by calling
          // 81943378(control, [81D6CDDC]) and returns 0x803000xx when that
          // comes back null. [81D6CDDC] is the visual class it searches for;
          // if the global itself is null the lookup can never succeed for any
          // control, which would explain the result being global.
          // xam's XUI functions announce themselves with printf-style
          // traces, but the sink at 817FED60 bails when [r13+0x2B4] is zero -
          // a per-thread gate - which is why a whole run has 31 DbgPrint
          // lines and none from XUI. Point it at a zeroed buffer rather than
          // a bare 1, in case anything downstream dereferences it.
          if (const char* want_trace = std::getenv("XENIA_XAM_TRACE")) {
            (void)want_trace;
            uint32_t r13 = static_cast<uint32_t>(ts->context()->r[13]);
            uint32_t gate = r13 + 0x2B4u;
            uint32_t prev = rd(gate);
            if (!prev) {
              uint32_t blk = memory->SystemHeapAlloc(256, 16);
              if (blk) {
                std::memset(memory->TranslateVirtual(blk), 0, 256);
                xe::store_and_swap<uint32_t>(memory->TranslateVirtual(gate),
                                             blk);
              }
            }
            XELOGI("GuideScene: xam trace gate [r13+2B4] (r13={:08X}) was "
                   "{:08X}, now {:08X}",
                   r13, prev, rd(gate));
          }
          // The whole class-descriptor table around 81D6CDDC. XuiTextElement
          // SetText (81937310) is 29 instructions: it loads [81D6CDCC], calls
          // 81930FE8(handle, class) to resolve the handle AND check its class,
          // and returns 80300016 verbatim when that comes back null. Every
          // SetText we have tried returns exactly that, including on
          // labelHeading, which really is a text element - so either the
          // handle does not resolve or the class slot is empty. A null slot
          // would also break every other class-checked dispatch, which is the
          // kind of thing that makes a render walk emit nothing.
          {
            auto* km = kernel_state()->memory();
            std::string tbl;
            for (uint32_t a = 0x81D6CDA0u; a < 0x81D6CE00u; a += 4) {
              tbl += fmt::format("{:08X}:{:08X} ", a,
                                 xe::load_and_swap<uint32_t>(
                                     km->TranslateVirtual(a)));
            }
            XELOGI("GuideScene: class table {}", tbl);
          }
          XELOGI("GuideScene: visual class global [81D6CDDC] = {:08X}",
                 rd(0x81D6CDDCu));
          // Check whether computing .rdata file offsets as runtime + 0x7200
          // is actually wrong, or whether the operand simply points at a
          // shared string suffix. Read the same address from guest memory.
          {
            std::string a1;
            auto* hp = memory->LookupHeap(0x816462C4u);
            if (hp && hp->QueryRangeAccess(0x816462C4u, 0x816462C4u + 0x3Fu) !=
                          xe::memory::PageAccess::kNoAccess) {
              for (uint32_t i = 0; i < 48; ++i) {
                uint8_t c = xe::load_and_swap<uint8_t>(
                    memory->TranslateVirtual(0x816462C4u + i));
                if (!c) break;
                a1 += (c >= 0x20 && c < 0x7F) ? char(c) : '?';
              }
            } else {
              a1 = "(unmapped)";
            }
            XELOGI("GuideScene: string at runtime 816462C4 = \"{}\"", a1);
          }
          // 818FAEE0(obj) returns E_INVALIDARG when obj is null and only
          // otherwise dispatches obj->vtable[19] - which on the wrapper's
          // vtable (81640680) is 8191B250, the draw method that never runs.
          // Its four bl callers all never execute either, so hud calls it as
          // an export and passes null. Resolve the render ordinals to find
          // which export it is; the .edata parse gives nonsense so use
          // Xenia's own module lookup.
          // 0x35A XuiSceneNavigateForward, 0x39E XuiSceneInterruptTransitions,
          // 0x35E/0x35D the transition players. hud imports all of them and
          // the bootstrap calls none: the scene is created and never
          // navigated to, which would leave it out of the render list while
          // still laying out correctly - exactly the observed split.
          for (auto ord : {0x35Au, 0x39Eu, 0x35Eu, 0x35Du,
                           0x342u, 0x31Eu, 0x31Bu, 0x395u, 0x359u, 0x38Au,
                           0x35Fu, 0x350u, 0x336u, 0x34Bu, 0x34Fu, 0x357u,
                           0x363u, 0x3DFu, 0x37Eu, 0x346u}) {
            XELOGI("GuideScene: xam ordinal {:03X} -> {:08X}", ord,
                   xmn ? xmn->GetProcAddressByOrdinal(ord) : 0);
          }
          // hud calls XuiRenderGetBackBufferSize (ordinal 0x350) - its thunk
          // 913FE8A4 is entered - and lays the scene out against whatever it
          // returns. Our Guide device is synthetic, so if it reports 0x0 the
          // layout collapses and nothing rasterises, which would fit an
          // otherwise-intact pipeline emitting nothing. Call it here and read
          // the answer rather than assuming the device reports a sane size.
          {
            uint32_t bbs = xmn ? xmn->GetProcAddressByOrdinal(0x350u) : 0;
            uint32_t dc_now = rd(dcp);
            // XuiRenderEnd is ordinal 0x34F = 818FAEE0, and it IS the draw
            // dispatcher: null-check arg1, then call arg1->vtable[19]
            // (lwz r11,0x4C(r11) / mtctr / bctrl). Slot 19 was matched
            // against the WRAPPER vtable 81640680 earlier and gave 8191B250 -
            // but XuiRenderEnd takes the DC, which has its own vtable. Print
            // the DC's, so the right slot 19 is read rather than assumed.
            {
              uint32_t dvt = dc_now ? rd(dc_now) : 0;
              XELOGI("GuideScene: DC {:08X} vtable {:08X} [19]={:08X} "
                     "[11]={:08X} [20]={:08X}",
                     dc_now, dvt, dvt ? rd(dvt + 19 * 4) : 0,
                     dvt ? rd(dvt + 11 * 4) : 0, dvt ? rd(dvt + 20 * 4) : 0);
            }
            uint32_t obuf = memory->SystemHeapAlloc(32, 16);
            if (bbs && obuf && dc_now) {
              std::memset(memory->TranslateVirtual(obuf), 0, 32);
              uint64_t ba[] = {dc_now, obuf, obuf + 4};
              uint64_t br = processor->Execute(ts, bbs, ba, xe::countof(ba));
              XELOGI("GuideScene: GetBackBufferSize(dc {:08X}) -> {:08X}; "
                     "w={} h={} (raw {:08X} {:08X})",
                     dc_now, static_cast<uint32_t>(br), rd(obuf),
                     rd(obuf + 4), rd(obuf), rd(obuf + 4));
            }
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
                // Navigate to the scene before inspecting it. Visuals are
                // attached explicitly (XuiControlAttachVisual), and nothing
                // in this project ever navigates - it only creates - so a
                // created-but-never-current scene plausibly has no visuals by
                // construction. NavigateFirst(a, scene, transition) touches
                // its first argument only when the transition byte is 0xFD
                // (checked at 81943334), so passing 0 there is safe; the
                // scene handle must be non-null, which it is here.
                {
                  uint32_t nav = xm2 ? xm2->GetProcAddressByOrdinal(0x359)
                                     : 0;
                  if (nav && nsc) {
                    // Host argument: NavigateFirst wants either a scene that
                    // already has a parent or an explicit host, and returns
                    // 8030000B given neither. The bootstrap's own scene is a
                    // real XUI handle, which is what 81938240 needs to
                    // resolve it (it answers 80300016 otherwise).
                    uint64_t na[] = {guide_bs_scene_, nsc, 0ull};
                    uint32_t nr = uint32_t(processor->Execute(
                        ts, nav, na, xe::countof(na)));
                    XELOGI("GuideScene: NavigateFirst(host {:08X}, {:08X}, 0)"
                           " -> {:08X}",
                           guide_bs_scene_, nsc, nr);
                  }
                }
                // Query elements by id. GetLastChild only follows one
                // branch, and the scene has far more objects than that path
                // reaches; these ids come from GuideMain's own STRN section.
                {
                  uint32_t gcbi = xm2 ? xm2->GetProcAddressByOrdinal(0x32A)
                                      : 0;
                  uint32_t idb = memory->SystemHeapAlloc(128, 16);
                  if (gcbi && idb) {
                    for (const char* want :
                         {// GuideMain
                          "Blade_Center", "txt_Games", "Label_Head",
                          "ringOfLight_Group", "btnB", "imgHeadsetBattery",
                          "Header", "Tab1", "Blade3",
                          // Options / Status - controls in scenes that have
                          // always loaded, as a control for whether missing
                          // visuals are specific to GuideMain or global
                          "btnOnlineStatus", "btnA", "backBtn", "artPanel",
                          "graphic_metapane", "txtMessage"}) {
                      std::memset(memory->TranslateVirtual(idb), 0, 128);
                      std::memset(memory->TranslateVirtual(co2), 0, 16);
                      wide(idb, want);
                      uint64_t ca[] = {nsc, idb, co2};
                      uint32_t cr = uint32_t(processor->Execute(
                          ts, gcbi, ca, xe::countof(ca)));
                      uint32_t child = rd(co2);
                      uint32_t vr2 = 0xFFFFFFFFu, vis2 = 0;
                      if (child && gvi2) {
                        std::memset(memory->TranslateVirtual(co2), 0, 16);
                        uint64_t va2[] = {child, co2};
                        vr2 = uint32_t(processor->Execute(ts, gvi2, va2,
                                                          xe::countof(va2)));
                        vis2 = rd(co2);
                      }
                      XELOGI("GuideScene:   byId \"{}\" -> {:08X} handle "
                             "{:08X}; visual -> {:08X}: {:08X}",
                             want, cr, child, vr2, vis2);
                      // A control attaches its visual when it receives
                      // message 9 (the compare chain at 8196BD34 branches
                      // there). The id sits at [msg+4] - both XuiSendMessage's
                      // own validation and the dispatcher read that offset -
                      // and [msg+8] is an output slot the dispatcher clears.
                      // If sending it produces a visual, nothing is delivering
                      // message 9; if not, the gate at 81938BC8 is the target.
                      // Name the visual class XUI is looking for. The
                      // handler derives its key as 8193B370([obj+0]) - runtime
                      // 81934170 - and hands it to XuiVisualCreateInstance as
                      // a wide string, so doing the same here says exactly
                      // which visual class is missing.
                      if (child) {
                        uint32_t ofh = xm2
                                           ? xm2->GetProcAddressByOrdinal(0x346)
                                           : 0;
                        if (ofh) {
                          std::memset(memory->TranslateVirtual(co2), 0, 16);
                          uint64_t oa[] = {child, co2};
                          processor->Execute(ts, ofh, oa, xe::countof(oa));
                          uint32_t obj = rd(co2);
                          uint32_t cls = obj ? rd(obj) : 0;
                          if (cls) {
                            uint64_t ka[] = {cls};
                            uint32_t nm = uint32_t(processor->Execute(
                                ts, GuideConst(0x81934170u), ka, xe::countof(ka)));
                            std::string cname;
                            auto* nh = nm ? memory->LookupHeap(nm) : nullptr;
                            if (nh && nh->QueryRangeAccess(nm, nm + 0x3Fu) !=
                                          xe::memory::PageAccess::kNoAccess) {
                              for (uint32_t i = 0; i < 48; ++i) {
                                uint16_t c = xe::load_and_swap<uint16_t>(
                                    memory->TranslateVirtual(nm + i * 2));
                                if (!c) break;
                                cname += (c >= 0x20 && c < 0x7F) ? char(c)
                                                                 : '?';
                              }
                            }
                            XELOGI("GuideScene:     obj {:08X} class {:08X} "
                                   "visual class name -> {:08X} \"{}\"",
                                   obj, cls, nm, cname);
                          }
                        }
                      }
                      if (child && !vis2 && vr2 == 0x80300017u) {
                        uint32_t sm = xm2
                                          ? xm2->GetProcAddressByOrdinal(0x35F)
                                          : 0;
                        uint32_t msg = memory->SystemHeapAlloc(32, 16);
                        if (sm && msg) {
                          // Layout matches XUI's XUIMessage: dwSize at +0,
                          // dwMessage at +4, bHandled at +8. Sending with
                          // size 0 was rejected with 80300026, so try the
                          // plausible sizes rather than assume one.
                          uint32_t mr = 0;
                          for (uint32_t sz : {0x0Cu, 0x10u, 0x18u, 0x20u}) {
                            std::memset(memory->TranslateVirtual(msg), 0, 32);
                            xe::store_and_swap<uint32_t>(
                                memory->TranslateVirtual(msg), sz);
                            xe::store_and_swap<uint32_t>(
                                memory->TranslateVirtual(msg + 4), 9u);
                            uint64_t ma[] = {child, msg};
                            mr = uint32_t(processor->Execute(
                                ts, sm, ma, xe::countof(ma)));
                            XELOGI("GuideScene:     msg9 size {:02X} -> {:08X}",
                                   sz, mr);
                            if (!mr) break;
                          }
                          std::memset(memory->TranslateVirtual(co2), 0, 16);
                          uint64_t rq[] = {child, co2};
                          uint32_t vr3 = uint32_t(processor->Execute(
                              ts, gvi2, rq, xe::countof(rq)));
                          XELOGI("GuideScene:     msg9 -> {:08X}; visual now "
                                 "{:08X}: {:08X}",
                                 mr, vr3, rd(co2));
                        }
                      }
                    }
                  }
                }
                // Walk down the tree with repeated GetLastChild. hud imports
                // no sibling accessor, but recursing the last child is enough
                // to show whether the scene has real depth and where visuals
                // start or stop appearing.
                if (gid2 && gvi2) {
                  uint32_t node = rd(co2);
                  for (int depth = 0; depth < 12 && node; ++depth) {
                    std::string nid;
                    std::memset(memory->TranslateVirtual(co2), 0, 16);
                    uint64_t qi[] = {node, co2};
                    processor->Execute(ts, gid2, qi, xe::countof(qi));
                    uint32_t sp4 = rd(co2);
                    if (sp4 > 0x1000u) {
                      for (uint32_t w = 0; w < 40; ++w) {
                        uint16_t c4 = xe::load_and_swap<uint16_t>(
                            memory->TranslateVirtual(sp4 + w * 2));
                        if (!c4) break;
                        nid += (c4 >= 0x20 && c4 < 0x7F) ? char(c4) : '?';
                      }
                    }
                    std::memset(memory->TranslateVirtual(co2), 0, 16);
                    uint64_t qv[] = {node, co2};
                    uint32_t vr = uint32_t(processor->Execute(
                        ts, gvi2, qv, xe::countof(qv)));
                    uint32_t vis = rd(co2);
                    XELOGI("GuideScene:   depth {} node {:08X} id \"{}\" "
                           "visual -> {:08X}: {:08X}",
                           depth, node, nid, vr, vis);
                    std::memset(memory->TranslateVirtual(co2), 0, 16);
                    uint64_t qc[] = {node, co2};
                    processor->Execute(ts, glc2, qc, xe::countof(qc));
                    uint32_t next = rd(co2);
                    if (next == node) break;
                    node = next;
                  }
                }

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
            // Visual on an element of the scene that is actually being
            // composite-drawn. Querying scenes built ad hoc by
            // guide_scene_override says nothing: those are never attached to
            // anything that renders, so their controls have no reason to hold
            // a visual.
            {
              uint32_t gvi3 = xmod ? xmod->GetProcAddressByOrdinal(0x395) : 0;
              uint32_t vb = memory->SystemHeapAlloc(16, 16);
              if (gvi3 && vb && kid) {
                std::memset(memory->TranslateVirtual(vb), 0, 16);
                uint64_t vq[] = {kid, vb};
                uint32_t vrr = uint32_t(
                    processor->Execute(ts, gvi3, vq, xe::countof(vq)));
                XELOGI("GuideScene: depth {} node {:08X} visual -> {:08X}: "
                       "{:08X}  (bootstrap scene)",
                       depth + 1, kid, vrr, rd(vb));
                // Remember the shallowest node that actually HAS a visual.
                // The observed tree is root 000100A9 -> 0001012D -> 000100B1
                // ("scnInfoUpsellLive") -> 000100E2 ("labelHeading"), and
                // 0001012D - the only child of the root, with no id, no
                // position and no visual (8030000A) - is the node the render
                // walk would have to descend THROUGH to reach any content.
                // Layout descends unconditionally, which is why the tree lays
                // out correctly; a render that skips a visual-less node and
                // does not recurse would emit nothing, which is exactly what
                // ResvWalk measures.
                if (!vrr && rd(vb) && !guide_first_visual_node_) {
                  guide_first_visual_node_ = kid;
                }
              }
            }
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
            // WHAT IS THIS ELEMENT? 81930FE8(handle, class) is the cast
            // helper every typed XUI accessor funnels through - SetText loads
            // [81D6CDCC] and calls it, then returns 80300016 when it comes
            // back null. The class slot is populated (408878F0), so the
            // failure is a type mismatch, not a missing class. Probing the
            // handle against every slot in the table names the element's
            // actual class instead of guessing from its id.
            if (::cvars::guide_probe_class && kid) {
              uint32_t cast = 0x81930FE8u;
              std::string got;
              for (uint32_t ca = 0x81D6CDC8u; ca <= 0x81D6CDFCu; ca += 4) {
                uint32_t cls = rd(ca);
                if (!cls) continue;
                uint64_t qa[] = {kid, cls};
                uint32_t qr = uint32_t(
                    processor->Execute(ts, cast, qa, xe::countof(qa)));
                if (qr) {
                  got += fmt::format("[{:08X}]={:08X}->{:08X} ", ca, cls, qr);
                }
              }
              XELOGI("GuideClass: {:08X} matches {}", kid,
                     got.empty() ? std::string("NOTHING") : got);
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
    ir = processor->Execute(ts, (g_hud_xuiinit ? g_hud_xuiinit : guide_bs_hud_base_ + 0xA898u), a2,
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
        // 81901EAC bctrls through [[dc+0x1C8]+0x0C] and lands on 4089B0B0 -
        // data, not code. The guest only asserts the slot is non-null, so a
        // wrong pointer passes. Logged here rather than pre-draw because the
        // crash happens during hud's XUI work, before any composite draw.
        {
          uint32_t prm = rd(dc + 0x1C8u);
          XELOGI("GuideBootstrap: param=[dc+1C8]={:08X} vt=[param+00]={:08X} "
                 "cb=[param+0C]={:08X} [param+08]={:08X} [param+1C]={:08X} "
                 "[dc+1CC]={:08X}",
                 prm, prm ? rd(prm) : 0, prm ? rd(prm + 0x0Cu) : 0,
                 prm ? rd(prm + 8u) : 0, prm ? rd(prm + 0x1Cu) : 0,
                 rd(dc + 0x1CCu));
        }
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
      uint32_t xam_dev = rd(XamDeviceSlot());
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
  // Print both arguments. guide_patch_null_render yields 0 composite draws
  // where borrow-only yields 2100+, with the hook reported installed in both
  // - so the suspicion is render_obj coming back null here, which would make
  // the `guide_draw_fn_ && guide_draw_this_` test in the notification path
  // silently false. "Installed" logged without values cannot distinguish that
  // from a notification that never fires.
  // Phase 502: 819F4C00 is the sole surviving producer of a NULL RT slot.
  //
  // A complete sweep of the image (research/slotscan.py) shows the RT slots are
  // never written by a D-form store and never by the indexed path - all seven
  // 0xCA8 sites are lwzx. The only writer is the setter 819F31A8, and of its 11
  // callers exactly two pass a literal NULL surface: 819F4C44 (here) and
  // 81A0F21C (the teardown, which phase 500 showed does not run). So every
  // 408070C0 -> 00000000 transition on this device comes through this compare:
  //
  //   819F4C28  r11 = [dev + 0x32A0 + idx*4]   slot
  //   819F4C2C  r10 = [dev + 0x3F78]           RT default
  //   819F4C34  beq -> skip                    matches default, leave alone
  //   819F4C38  r5  = 0                        else unbind: SetRenderTarget(NULL)
  //
  // Phase 500 eliminated this by observing slot == default at the paint site,
  // but the compare that matters runs later - after the Guide binds its own RT,
  // which makes the slot differ and drives it to NULL. Testing a precondition at
  // the wrong moment, the same error phase 494 made about paths.
  //
  // Forcing the branch unconditional stops the unbind. If the chain above is
  // right the 819F5F60 fault must disappear; if it survives unchanged with the
  // readback confirming the patch landed, suspect the function was already
  // translated and the JIT is running the pre-patch body.
  // Phase 517: 81901EAC is a bctrl through [[obj+0x1C8]+0x0C], and that slot
  // holds 006E0065 - UTF-16 "en" - so the field points at locale text rather
  // than an interface table. lle_xam_skin_init calls 81795548 directly and that
  // function "has no callers inside xam", so it runs without whatever
  // initialises the field.
  //
  // The site already has a clean failure path: 81901E88 `bne` skips the call
  // when the slot is null and 81901E8C returns 0x80004005 (E_FAIL). Turning the
  // bne into a nop takes that path unconditionally, which answers whether skin
  // initialisation can complete - and still register visuals - without this one
  // dispatch. If it can, the visuals and the draw hook stop being mutually
  // exclusive (phase 516).
  if (::cvars::guide_patch_skin_dispatch && XamIsDashrootLayout()) {
    GuidePatchWord(0x81901E88u, 0x409A0010u, 0x60000000u, "SkinDispatchPatch");
  }
  // Phase 546: 819E01E0 converts a CPU pointer to a GPU address and returns 0
  // for the Guide's vertex buffer (phase 545). Its input has been inferred from
  // the arithmetic, never observed, and the two candidate values imply different
  // upstream faults. Patch the final `add r31, r11, r10` to `mr r31, r3` so the
  // function returns its own argument: the fetch constant then carries the raw
  // input pointer, which is already logged. This deliberately breaks the
  // conversion - it is a diagnostic, not a fix.
  if (::cvars::guide_patch_addr_passthru && XamIsDashrootLayout()) {
    GuidePatchWord(0x819E0218u, 0x7FEB5214u, 0x7C7F1B78u, "AddrPassthruPatch");
  }
  // 819DE934 is `bne cr6, 0x819dea30`, guarding a block that loads the
  // device's current render target ([dev+0x32A0]), falls back to
  // [dev+0x32B0], and dereferences +0x24 of it. Both slots are null on the
  // device hud's render is handed, so the fallback yields null and 819DE94C
  // faults. Forcing the branch takes the same exit the function already uses
  // whenever r30 != 0.
  // (Applied from the paint loop in emulator.cc instead: this bootstrap
  // runs AFTER hud's render has already faulted - the patch landed at log
  // line 3259 and the crash was at 3131.)
  if (::cvars::guide_patch_rt_unbind && XamIsDashrootLayout()) {
    auto* pm = kernel_state()->memory();
    uint32_t site = 0x819F4C34u;
    // The xam text pages are mapped read-only; the first attempt at this took a
    // host fault at 1819F4C34 (the host mapping of the guest address) and killed
    // the run before the store landed, which reads exactly like "the patch code
    // never executed". Unprotect first.
    // The guest heap's Protect() did not make the host mapping writable (the
    // second attempt faulted at the same host address with no log line), so go
    // at the host page directly.
    uint8_t* hp = pm->TranslateVirtual(site);
    xe::memory::PageAccess old_access = xe::memory::PageAccess::kNoAccess;
    bool unprot = xe::memory::Protect(
        reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hp) & ~0xFFFull),
        0x1000, xe::memory::PageAccess::kReadWrite, &old_access);
    XELOGI("RtUnbindPatch: host={} unprotect={}", static_cast<void*>(hp),
           unprot ? "ok" : "FAILED");
    if (!unprot) {
      XELOGW("RtUnbindPatch: cannot unprotect - skipping patch");
    } else {
    uint32_t was = xe::load_and_swap<uint32_t>(pm->TranslateVirtual(site));
    if (was == 0x419A0014u) {
      xe::store_and_swap<uint32_t>(pm->TranslateVirtual(site), 0x48000014u);
      XELOGI("RtUnbindPatch: {:08X} {:08X} -> {:08X} (readback {:08X})", site,
             was, 0x48000014u,
             xe::load_and_swap<uint32_t>(pm->TranslateVirtual(site)));
    } else {
      XELOGW("RtUnbindPatch: {:08X} reads {:08X}, expected 419A0014 - not "
             "patching", site, was);
    }
    xe::memory::Protect(
        reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hp) & ~0xFFFull),
        0x1000, old_access, nullptr);
    }
  }
  SetGuideDrawHook((g_hud_render ? g_hud_render : guide_bs_hud_base_ + 0xAB28u), render_obj);
  XELOGI("GuideBootstrap: draw hook args fn={:08X} self={:08X}",
         (g_hud_render ? g_hud_render : guide_bs_hud_base_ + 0xAB28u), render_obj);
  XELOGI("GuideBootstrap: draw hook installed on title thread");
}

// Phase 377: the coverage readback used to live only inside the composite-
// draw hook. With the command-buffer fixes enabled that hook stops firing,
// so the instrument this investigation depends on went silent exactly in
// the configurations that needed measuring. Extracted here so it can also
// be driven from the swap path.
static void EmitGuideCoverageOnce() {
  if (::cvars::guide_coverage_fn) {
    static bool cov_done = false;
    if (!cov_done) {
      auto* f = kernel_state()->processor()->LookupFunction(
          ::cvars::guide_coverage_fn);
      auto* gf = f ? dynamic_cast<cpu::GuestFunction*>(f) : nullptr;
      // Phase 591: only spend the one-shot on a readback that has data.
      // Driven from the paint loop, the first call lands before the function
      // under study has ever run, and burning the one-shot there reported
      // "never translated" and then stayed silent for the rest of the run -
      // which reads exactly like "the function is never called".
      if (!gf || !gf->trace_data().is_valid()) {
        static bool warned = false;
        if (!warned) {
          warned = true;
          XELOGI("Coverage {:08X}: not yet translated/valid - will retry",
                 uint32_t(::cvars::guide_coverage_fn));
        }
        return;
      }
      cov_done = true;
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
        // counts[0] is the entry instruction's execution count, i.e. the
        // number of times the function was CALLED. Reporting it turns the
        // instrument from "was this reached" into "how often" - which is
        // what distinguishes "painted once" from "painted per element".
        XELOGI("Coverage {:08X}: {} of {} instructions executed, calls={}, "
               "furthest reached {:08X} (+0x{:X})",
               ::cvars::guide_coverage_fn, executed, n,
               n ? counts[0] : 0, last, last - td.start_address());
        // Report the NOT-executed spans. A function that runs to its last
        // instruction while skipping 40% of its body has taken a set of
        // branches, and the untaken ones are where an alternative path -
        // such as one that submits geometry - would sit. The totals cannot
        // show that; the gaps can.
        std::string gaps;
        uint32_t gap_start = 0;
        bool in_gap = false;
        uint32_t shown = 0;
        for (uint32_t i = 0; i <= n; ++i) {
          bool zero = (i < n) && (counts[i] == 0);
          if (zero && !in_gap) {
            in_gap = true;
            gap_start = i;
          } else if (!zero && in_gap) {
            in_gap = false;
            uint32_t len = i - gap_start;
            if (len >= 4 && shown < 14) {
              gaps += fmt::format("{:08X}+{} ",
                                  td.start_address() + gap_start * 4, len);
              ++shown;
            }
          }
        }
        XELOGI("Coverage {:08X}: unexecuted spans (>=4 insns): {}",
               ::cvars::guide_coverage_fn,
               gaps.empty() ? std::string("none") : gaps);
      } else {
        // "lookup=FOUND" means nothing on its own: Processor::LookupFunction
        // DECLARES the function when it is absent, so it returns non-null for
        // any valid guest address - including one that never executed. Reading
        // it as "was called" produced two confident false positives (81A0FE48
        // in phase 381, 819FF7A0 in phase 385) before the contradiction with
        // the device state gave it away.
        //
        // The status field is the real signal. kDeclared is set by the lookup
        // itself; kDefined is set only by DemandFunction after the function is
        // actually translated, and translation happens on the demand path when
        // a call resolves. So kDefined => it ran, kDeclared => it did not.
        const char* st = "?";
        if (f) {
          using SS = xe::cpu::Symbol::Status;
          SS ss = f->status();
          st = ss == SS::kNew        ? "NEW"
             : ss == SS::kDeclared   ? "DECLARED (never translated -> never called)"
             : ss == SS::kDefined    ? "DEFINED (translated -> was called)"
             : ss == SS::kFailed     ? "FAILED"
                                     : "?";
        }
        XELOGI("Coverage {:08X}: status={} guest={} trace_valid={}",
               ::cvars::guide_coverage_fn, st, gf ? "yes" : "no",
               (gf && gf->trace_data().is_valid()) ? "yes" : "no");
        XELOGI("Coverage {:08X}: no trace data (need "
               "trace_function_coverage and trace_function_data_path)",
               ::cvars::guide_coverage_fn);
      }
    }
  }
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
  // Phase 377: drive the coverage readback from the swap path too, so it
  // reports in configurations where the composite-draw hook does not fire
  // (e.g. with guide_clear_cmd_overflow). One-shot inside the function.
  {
    static uint32_t cov_frames = 0;
    // Phase 585: was 2000 swaps. covrun kills at 60s and the title swaps at
    // ~30fps, so the one-shot landed ~6s AFTER every run ended and the
    // instrument never reported - the readback in emulator.cc sits behind a
    // 6000-frame (~96s) loop and is equally unreachable. 900 swaps is ~30s:
    // well after the 10s auto-press, well before the kill.
    if (::cvars::guide_coverage_fn && ++cov_frames == 900) {
      EmitGuideCoverageOnce();
    }
  }
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
      // Clear the re-entry guard on scope exit, not by assignment.
      //
      // Phase 250: the Guide draw path faults on the title thread, and when it
      // does before the trailing `in_guide_draw = false`, the flag stays set
      // and the hook is disabled for the rest of the session - which is why
      // identical flags produced paintframe=32 in one run and 0 in the next,
      // and why an unknown number of "forcing X changed nothing" results may
      // have been runs where X never ran. A scope guard cannot be skipped by
      // a fault that unwinds.
      struct GuideDrawScope {
        bool* flag;
        explicit GuideDrawScope(bool* f) : flag(f) { *flag = true; }
        ~GuideDrawScope() { *flag = false; }
      } guide_draw_scope(&in_guide_draw);
      {
        // Arm the checkpoints at hook ENTRY - they are all upstream of the
        // draw bracket, so arming them there would be too late to say
        // anything about the first pass, which is the only one that matters.
        static uint32_t hook_n = 0;
        ckpt_on = hook_n++ < 3;
      }
      // Repoint the draw root at the shallowest element that owns a visual.
      // hud hands [guide+8] to the layout and render walk. In the observed
      // tree that is the root whose only child, 0001012D, has no id, no
      // position and no visual - so a render that requires a visual to
      // descend gets no further, while layout (which does not) reports the
      // whole tree laid out correctly. Pointing the root at 000100B1
      // ("scnInfoUpsellLive") skips the empty node. If the render emits
      // packets after this, that node was the blocker; if it still emits
      // nothing, the tree is not what is stopping it.
      // The scene creator writes its handle through an out-pointer. We pass
      // our own scratch buffer, so the handle lands there and never reaches
      // [guide+8] - which is what hud's draw uses as its root. Measured:
      // [guide+8] was 00000000 at every draw. hud itself would have passed
      // &this->field8; storing the handle after the fact is the same result.
      // Write to the DRAW object, not the bootstrap object. Both of these
      // previously stored into guide_bs_obj_+8, which the object dump showed
      // is a field on the OUTER object that nothing reads - the draw's `this`
      // is guide_bs_obj_+0x10, so its root lives at bs+0x18. Their measured
      // "no change" result was therefore about a field nobody looks at, and
      // the repoint hypothesis was never actually tested.
      //
      // Worth testing because the class probe found that 0001012D - the only
      // child of the scene root, and the node every render walk must pass
      // through - matches ONLY the base class descriptor [81D6CDC8]. Its
      // siblings deeper in the tree match the visual class [81D6CDDC] as well.
      // A walk that needs the visual class to recurse stops there, which is
      // what emitting no geometry from a healthy tree looks like.
      if (::cvars::guide_root_to_visual && guide_draw_this_ &&
          guide_first_visual_node_) {
        auto* rm2 = kernel_state()->memory();
        uint32_t cur_root = xe::load_and_swap<uint32_t>(
            rm2->TranslateVirtual(guide_draw_this_ + 8u));
        if (cur_root != guide_first_visual_node_) {
          xe::store_and_swap<uint32_t>(
              rm2->TranslateVirtual(guide_draw_this_ + 8u),
              guide_first_visual_node_);
          XELOGI("GuideRoot: [this+8] {:08X} -> {:08X}", cur_root,
                 guide_first_visual_node_);
        }
      }
      // Claim the D3D device for the thread that is about to draw with it.
      //
      // xam's 819F4348 is a thread-ownership guard, and it does not degrade
      // gracefully:
      //   819F4444  lwz   r11, 11016(r31)   [device+0x2B08] = owner thread
      //   819F4454  bl    817F6C30          current thread id
      //   819F4458  cmplw r31, r3
      //   819F445C  beq   -> return         match: proceed
      //   819F4474  bl    81A050A8          print the warning
      //   819F4478  .long 0x0FE00019        unconditional TRAP
      //
      // The device is created on the Guide button dispatch thread and every
      // draw is issued from the title's render thread inside VdSwap, so this
      // fires on every frame - it is the "current thread (0x10) ... owned by a
      // different thread (0x6)" message that appeared 400,000 times in a 25s
      // log and was deduplicated as noise.
      //
      // 817F6C30 is xam's own "current thread id", so calling it here on the
      // drawing thread and storing the result gives the guard the answer it
      // is looking for, in its own terms, rather than faking the comparison.
      if (::cvars::guide_claim_device_thread && guide_draw_this_) {
        auto* cm2 = kernel_state()->memory();
        auto crd = [cm2](uint32_t a) {
          return a ? xe::load_and_swap<uint32_t>(cm2->TranslateVirtual(a)) : 0u;
        };
        uint32_t cdc2 = crd(guide_draw_this_ + 0x0Cu);
        uint32_t cwr2 = crd(cdc2 + 0x1CCu);
        uint32_t cdv2 = crd(cwr2 + 0x0Cu);
        if (cdv2) {
          uint32_t tid = uint32_t(kernel_state()->processor()->Execute(
              gth->thread_state(), GuideConst(0x817F6C30u), nullptr, 0));
          uint32_t owner = crd(cdv2 + 0x2B08u);
          if (tid && owner != tid) {
            xe::store_and_swap<uint32_t>(
                cm2->TranslateVirtual(cdv2 + 0x2B08u), tid);
            // Remember what we displaced so it can be put back after the
            // draw. Taking ownership permanently is not a fix, it is a theft:
            // thread 0x6 owns this device legitimately and its own guarded
            // calls - every D3D entry point checks - would then fail the same
            // way ours did. With ignore_trap_instructions on they do not stop,
            // they continue past a trap with whatever state they had, which
            // is a good description of an emulator that wedges.
            guide_claim_dev_ = cdv2;
            guide_claim_prev_ = owner;
            static uint32_t claim_logs = 0;
            if (claim_logs++ < 3) {
              XELOGI("GuideClaim: device {:08X} owner {:08X} -> this thread "
                     "{:08X} (restored after the draw)",
                     cdv2, owner, tid);
            }
          }
        }
      }
      // The whole Guide object at draw entry. [guide+8] being null was found
      // one field at a time; dumping the head answers "which of hud's fields
      // are actually populated" in a single line instead of a cvar per field.
      {
        static uint32_t obj_dumps = 0;
        if (obj_dumps++ < 2 && guide_draw_this_) {
          auto* om = kernel_state()->memory();
          std::string oh;
          for (uint32_t w = 0; w < 16; ++w) {
            oh += fmt::format("{:02X}:{:08X} ", w * 4,
                              xe::load_and_swap<uint32_t>(
                                  om->TranslateVirtual(guide_draw_this_ +
                                                       w * 4)));
          }
          XELOGI("GuideObj: this={:08X} {}", guide_draw_this_, oh);
          // The DC the draw actually renders into, which is not the one the
          // scene walk reports. +0x134 is the null-render flag that makes
          // XuiRenderPresent return S_OK without presenting and makes
          // XuiRenderBegin skip dc->vtable[20]; +0x1C8/+0x1CC are the wrapper
          // and callback that 81901E40 dereferences. If the draw's DC is a
          // different object from the bootstrap's, then every measurement of
          // "[dc+134]=0" so far was taken on a DC the draw does not use.
          uint32_t ddc = xe::load_and_swap<uint32_t>(
              om->TranslateVirtual(guide_draw_this_ + 0x0Cu));
          if (ddc) {
            auto ord = [om](uint32_t a) {
              return xe::load_and_swap<uint32_t>(om->TranslateVirtual(a));
            };
            XELOGI("GuideDC: draw dc={:08X} vtable={:08X} [134]={:08X} "
                   "[1C8]={:08X} [1CC]={:08X} [1D0]={:08X}",
                   ddc, ord(ddc), ord(ddc + 0x134u), ord(ddc + 0x1C8u),
                   ord(ddc + 0x1CCu), ord(ddc + 0x1D0u));
            uint32_t wrap = ord(ddc + 0x1CCu);
            if (wrap) {
              XELOGI("GuideDC: wrapper {:08X} [00]={:08X} [0C]={:08X} "
                     "(device) [10]={:08X}",
                     wrap, ord(wrap), ord(wrap + 0x0Cu), ord(wrap + 0x10u));
            }
          }
          if (guide_bs_obj_ && guide_bs_obj_ != guide_draw_this_) {
            std::string bh;
            for (uint32_t w = 0; w < 16; ++w) {
              bh += fmt::format("{:02X}:{:08X} ", w * 4,
                                xe::load_and_swap<uint32_t>(
                                    om->TranslateVirtual(guide_bs_obj_ +
                                                         w * 4)));
            }
      if (ckpt_on) XELOGI("GuideCk: render_begin_block");
            XELOGI("GuideObj: bs  ={:08X} {}", guide_bs_obj_, bh);
          }
        }
      }
      // Call the DC's own render-begin, dc->vtable[20] (818FDDE8), directly
      // before the composite draw. Disassembled it is:
      //   traps if [dc+0x1CC] (the wrapper) is null
      //   bctrl [[wrapper]+44]  -> Clear, flags 15, colour from [81601EF0]
      //   bctrl [[wrapper]+20]
      // A Clear with all flags set has to emit packets on every frame. The
      // reservation cursor moves on draw #1 only, so either hud never reaches
      // this call or the wrapper's clear emits nothing. Calling it ourselves
      // separates those: if the cursor advances, the render path works and
      // hud is not invoking it; if it does not, the wrapper is the problem.
      if (::cvars::guide_call_render_begin && guide_draw_this_) {
        auto* bm = kernel_state()->memory();
        auto brd = [bm](uint32_t a) {
          return a ? xe::load_and_swap<uint32_t>(bm->TranslateVirtual(a)) : 0u;
        };
        uint32_t bdc = brd(guide_draw_this_ + 0x0Cu);
        uint32_t bvt = brd(bdc);
        uint32_t bfn = bvt ? brd(bvt + 20u * 4u) : 0;
        static uint32_t rb_logs = 0;
        if (bdc && bfn && brd(bdc + 0x1CCu)) {
          // Publish the device BEFORE calling, not after. The capture in
          // CmdbufAtDraw sits further down the hook, so when this call hangs
          // it never runs, GuideResvDevice() stays 0, and anything keyed off
          // it - like the command-buffer completion forcer - silently targets
          // only VdGlobalXamDevice. That produced a clean-looking negative
          // for a treatment that was never applied to the right device.
          guide_resv_dev_ = brd(brd(bdc + 0x1CCu) + 0x0Cu);
          // Publish this thread so a host watchdog can sample its guest PC
          // while it is stuck. Four candidate stall sites have now been
          // proposed from disassembly and each test either failed to apply or
          // targeted a loop the thread turns out not to be in. Reading the
          // program counter is the only way to stop guessing.
          guide_stall_thread_ = gth->thread() ? gth->thread()->native_handle()
                                              : nullptr;
          uint64_t ba[] = {bdc, 0};
          // Which ThreadState this runs against decides whether the whole
          // render subsystem is reachable: every function below this call
          // re-checks 81778BB8 against [r13+256] and traps on mismatch.
          xe::cpu::ThreadState* rts =
              ::cvars::guide_render_thread ? GuideRenderThreadState() : nullptr;
          uint64_t br = kernel_state()->processor()->Execute(
              rts ? rts : gth->thread_state(), bfn, ba, xe::countof(ba));
          if (rb_logs++ < 3) {
            XELOGI("GuideRenderBegin: {:08X}(dc {:08X}, 0) -> {:08X} [thread={}]",
                   bfn, bdc, static_cast<uint32_t>(br),
                   rts ? "xam render thread"
                       : (::cvars::guide_render_thread ? "BOOTSTRAP (render "
                                                         "thread not resolved)"
                                                       : "bootstrap"));
          }
        } else if (rb_logs++ < 3) {
          XELOGW("GuideRenderBegin: skipped (dc={:08X} vt={:08X} fn={:08X} "
                 "wrapper={:08X})",
                 bdc, bvt, bfn, brd(bdc + 0x1CCu));
        }
      }
      // Publish the drawing thread unconditionally, not only when the
      // render-begin experiment runs. With guide_claim_device_thread the
      // composite draw itself can now block, and the watchdog needs a handle
      // to sample regardless of which call is the one that parks.
      guide_stall_thread_ = gth->thread() ? gth->thread()->native_handle()
                                          : nullptr;
      {
        // Name the thread being published. Several experiments this session
        // measured the wrong object and produced confident answers about it,
        // so record the OS thread id here and have the watchdog print the id
        // it actually samples - if they differ, nothing the watchdog says is
        // about this draw.
        static bool once_pub = false;
        if (!once_pub) {
          once_pub = true;
          XELOGI("GuidePublish: draw thread handle={} os_tid={} guest='{}'",
                 guide_stall_thread_.load(),
                 guide_stall_thread_
                     ? GetThreadId(reinterpret_cast<HANDLE>(
                           guide_stall_thread_.load()))
                     : 0,
                 gth->thread_name());
        }
      }
      uint64_t gargs[] = {guide_draw_this_};
      if (::cvars::guide_predraw_surfaces) {
        // Bind through xam's OWN setters, on the title thread, once, before
        // the first draw. Raw stores here suppress the draw entirely: the
        // setters do bookkeeping around the store (819F4348 first, more
        // after) that a bare store_and_swap skips. Doing it at button time
        // loses to 819F4C00, which unbinds by calling these same two
        // functions with null (819F4C44 / 819F4C70).
        // Every draw, not once. 819F4C00 clears these between the button
        // press and the draw, so a one-shot bind is already gone by draw
        // time - which is exactly the [dev+0x32B0]==0 that faults at
        // 819F5F60 (lwz r11,0x32B0(r31) / lhz r11,24(r11)).
        static uint32_t bindn = 0;
        ++bindn;
        {
          auto* sm = kernel_state()->memory();
          auto srd = [sm](uint32_t a) {
            return xe::load_and_swap<uint32_t>(sm->TranslateVirtual(a));
          };
          auto* sts = gth->thread_state();
          auto* proc = kernel_state()->processor();
          uint32_t sdc = srd(guide_draw_this_ + 12);
          uint32_t swrap = sdc ? srd(sdc + 0x1CCu) : 0;
          uint32_t sdev = swrap ? srd(swrap + 0x0Cu) : 0;
          if (sdev) {
            // This block runs per frame and used to create two fresh surfaces
            // each time without ever freeing them, which walks the pool down:
            // the first call got 1280x720, the next fell back to 852x480, then
            // 640x480, and so on. That exhaustion is the harness's own doing and
            // would make any later "did not fit" reading meaningless. Create
            // once and reuse.
            static uint32_t cached_predraw_surf = 0;
            static uint32_t cached_predraw_fb = 0;
            if (!cached_predraw_surf) {
              cached_predraw_surf = GuideMakeSurface(proc, sts);
            }
            if (!cached_predraw_fb) {
              cached_predraw_fb = GuideMakeSurface(proc, sts);
            }
            uint32_t surf = cached_predraw_surf;
            uint32_t fb = cached_predraw_fb;
            if (surf) {
              uint64_t ra[] = {sdev, 0, surf};
              proc->Execute(sts, GuideConst(0x819F31A8u), ra, xe::countof(ra));
              uint64_t da[] = {sdev, surf};
              proc->Execute(sts, GuideConst(0x819F38C8u), da, xe::countof(da));
            }
              // 819F4C00 does not null the RT slots - it restores them from
              // the device's own defaults at [dev+0x3F78] (RT) and
              // [dev+0x3F70] (depth). When those are null the restore is what
              // writes the null that faults at 819F5F60. Binding the defaults
              // means the unbind-all leaves a real surface behind - which is
              // why re-binding more often could never have worked.
              xe::store_and_swap<uint32_t>(
                  sm->TranslateVirtual(sdev + 0x3F78u), surf);
              // Same asymmetry as [3F78] in phase 454: 819F4C00 restores the
              // depth slot from [dev+0x3F70], which was left null - the log
              // there read "defaults [3F78]=408C4B90 [3F70]=00000000". With
              // skin init on, the crash lands at 819F5EC4, the second address
              // predraw is documented to make survivable.
              xe::store_and_swap<uint32_t>(
                  sm->TranslateVirtual(sdev + 0x3F70u), surf);
              XELOGI("PreDrawBind: defaults [3F78]={:08X} [3F70]={:08X}",
                     srd(sdev + 0x3F78u), srd(sdev + 0x3F70u));
            // 819FEB78 - which takes the device in r3 - traps outright unless
            // [dev+0x3F74] and [dev+0x3F78] are both non-null:
            //   lwz r11,0x3f74(r31) / bne / twui   and the same for 0x3f78.
            // Under skin init the second 819E7528 returns 0, so 3F74 was left
            // null and that assert is reachable. Fall back to the surface we
            // do have rather than leaving it zero.
            uint32_t front = fb ? fb : surf;
            if (front) {
              xe::store_and_swap<uint32_t>(
                  sm->TranslateVirtual(sdev + 0x3F74u), front);

            }
            XELOGI("PreDrawBind: dev={:08X} rt={:08X} fb={:08X} -> "
                   "[32A0]={:08X} [32B0]={:08X} [3F74]={:08X}",
                   sdev, surf, fb, srd(sdev + 0x32A0u), srd(sdev + 0x32B0u),
                   srd(sdev + 0x3F74u));
          }
        }
      }
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
          // [dc+0x1CC] is the WRAPPER, not the device - the device is
          // [wrapper+0x0C]. This read the wrapper and called it "dev" for
          // many phases, so "Guide pre-draw: dev [32A0]=00000060" was
          // reporting a word at wrapper+0x32A0 that means nothing, directly
          // contradicting the PreDrawBind line above it which correctly
          // showed [32A0]=40AE2160 on device 40870D00. The surfaces were
          // bound all along; only the report was wrong.
          uint32_t pwrap = pdc ? prd(pdc + 0x1CCu) : 0;
          uint32_t pdev = pwrap ? prd(pwrap + 0x0Cu) : 0;
          // 818FDE98 sets [dc+134] = [param+1C] and [dc+1CC] = [param+8],
          // and stores the parameter object itself at [dc+1C8]. Read it back
          // so the inheritance is measured rather than assumed.
          uint32_t pparam = pdc ? prd(pdc + 0x1C8u) : 0;
          XELOGI("Guide pre-draw: dc={:08X} [134]={:08X} dev={:08X} "
                 "param=[1C8]={:08X} [param+1C]={:08X} [param+8]={:08X}",
                 pdc, pdc ? prd(pdc + 0x134u) : 0, pdev, pparam,
                 pparam ? prd(pparam + 0x1Cu) : 0,
                 pparam ? prd(pparam + 8u) : 0);
          // 81901EAC does bctrl through [[dc+0x1C8]+0x0C] and lands on
          // 4089B0B0 - data, not code. The guest asserts only that the slot is
          // non-null, so a wrong pointer passes. If [param+00] looks like an
          // 81xxxxxx vtable the object is right and the slot is stale; if not,
          // [dc+0x1C8] is holding the wrong object entirely.
          XELOGI("Guide pre-draw: param vt=[param+00]={:08X} "
                 "cb=[param+0C]={:08X} [dc+1CC]={:08X}",
                 pparam ? prd(pparam) : 0, pparam ? prd(pparam + 0x0Cu) : 0,
                 pdc ? prd(pdc + 0x1CCu) : 0);
          if (ckpt_on) XELOGI("GuideCk: bind_cmdbuf");
          if (pdev) {
            XELOGI("Guide pre-draw: wrapper={:08X} dev={:08X} [32A0]={:08X} "
                   "[32B0]={:08X} [3F74]={:08X}",
                   pwrap, pdev, prd(pdev + 0x32A0u), prd(pdev + 0x32B0u),
                   prd(pdev + 0x3F74u));
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
                                                           prd(XamDeviceSlot())},
                          {"VdGlobalXamDevice [801E6FC8]", prd(0x801E6FC8u)},
                          {"VdGlobalDevice [801E6FC4]", prd(0x801E6FC4u)},
                          {"dc wrapper [dc+1CC]", pdev},
                          {"displaced device (pre-redirect)",
                           guide_prev_device_},
                          // 8191B418 does "lwz r3,12(r31)" and hands THAT to
                          // 819FEB78, so the device the present path actually
                          // uses is [wrapper+12] - not the 81D43684 global the
                          // redirect changes.
                          // pwrap, NOT pdev. This entry predates the phase
                          // 183 fix that redefined pdev from the wrapper to
                          // the device; left as pdev it reads [device+12] -
                          // an arbitrary field - and the loop body then
                          // dereferences that as a device pointer, which is
                          // the exe+EEE3F host fault. [wrapper+12] is the
                          // device, so this now agrees with pdev by
                          // construction rather than by accident.
                          {"PRESENT PATH [wrapper+12]",
                           pwrap ? prd(pwrap + 12u) : 0}}) {
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
      if (ckpt_on) XELOGI("GuideCk: predraw_surfaces");
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
            if (ckpt_on) XELOGI("GuideCk: call_819E7528_surface");
      if (::cvars::guide_bind_cmdbuf_kb > 0) {
        // Re-init the 81A01358 block on EVERY draw whose cursor is cold, not
        // once. The one-shot this replaces is exactly what limited emission to
        // the first frame: the log order is unambiguous - block cold at draw
        // #1, init runs, 237 words emitted; block cold again at draw #2, no
        // init, zero words; and so on for 3600 draws. Nothing zeroes the
        // block - it is simply never set again, because `if (!cbuf)` fired
        // once. The buffer allocation stays one-shot; only the binding
        // repeats. 81A01358 asserts the cursor is currently zero, which is
        // precisely the state at draw entry, so re-calling it is the
        // precondition it wants rather than a workaround.
        static uint32_t cbuf = 0;
            if (ckpt_on) XELOGI("GuideCk: call_819F31A8_rt");
        {
          auto* cm = kernel_state()->memory();
          auto c2 = [cm](uint32_t a) {
            return a ? xe::load_and_swap<uint32_t>(
                           cm->TranslateVirtual(a)) : 0u;
          };
            if (ckpt_on) XELOGI("GuideCk: call_819F38C8_depth");
          uint32_t cdc = c2(guide_draw_this_ + 12);
          uint32_t cwrap = cdc ? c2(cdc + 0x1CCu) : 0;
          uint32_t cdev = cwrap ? c2(cwrap + 12u) : 0;
          uint32_t csize =
              uint32_t(::cvars::guide_bind_cmdbuf_kb) * 1024u;
          if (cdev && !c2(cdev + 0x2B4Cu)) {
            bool fresh = false;
            if (!cbuf) {
              cbuf = cm->SystemHeapAlloc(csize, 4096);
              fresh = true;
            }
            if (cbuf) {
              // Clear on allocation only. Zeroing 512KB on each of 3600 draws
              // is 1.8GB of memset for no benefit - the emitter overwrites
              // what it uses, and the walkers read only as far as the cursor
              // advanced.
              if (fresh) {
                std::memset(cm->TranslateVirtual(cbuf), 0, csize);
              }
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
                            cth->thread_state(), GuideConst(0x81A01358u), cargs,
                            xe::countof(cargs))
                      : 0;
              guide_cmdbuf_base_ = cbuf;
              guide_cmdbuf_size_ = csize;
              // THE FIELDS THAT ACTUALLY MATTER. 81A042E0, the reservation
              // that seeds the emitter's cursor, allocates from [dev+0x30]
              // (current) and [dev+0x34] (end) - not the +0x2B48/2B4C/2B50
              // block 81A01358 sets. Measured at draw entry those were
              // cur=408881E0 end=40889424, i.e. 4676 bytes, while the Guide's
              // first reservation asks for 2309 words = 9236 bytes. The fit
              // test at 81A0432C fails, the slow path runs, and the reserve
              // returns 0 - which is the zero cursor the emitter faults on.
            if (ckpt_on) XELOGI("GuideCk: survey_start");
              // Point them at the buffer we just allocated.
              xe::store_and_swap<uint32_t>(
                  cm->TranslateVirtual(cdev + 0x30u), cbuf);
              xe::store_and_swap<uint32_t>(
                  cm->TranslateVirtual(cdev + 0x34u), cbuf + csize);
              if (ckpt_on) XELOGI("GuideCk: sv_reads");
              static uint32_t bind_logs = 0;
              if (bind_logs++ < 3)
              XELOGI("Guide: reserve window widened -> cur[30]={:08X} "
                     "end[34]={:08X} ({} bytes)",
                     c2(cdev + 0x30u), c2(cdev + 0x34u), csize);
              if (bind_logs < 4)
              XELOGI("Guide: cmdbuf init {:08X}(dev {:08X}, {:08X}, {}) "
                     "-> {:08X}; base={:08X} cursor={:08X} limit={:08X}",
                     0x81A01358u, cdev, cbuf, csize / 4,
                     static_cast<uint32_t>(cres), c2(cdev + 0x2B48u),
                     c2(cdev + 0x2B4Cu), c2(cdev + 0x2B50u));
            }
          } else {
          if (ckpt_on) XELOGI("GuideCk: sv_firstlog");
            static uint32_t skip_logs = 0;
            if (skip_logs++ < 3)
            XELOGW("Guide: cmdbuf bind skipped (dev={:08X} cursor={:08X})",
                   cdev, cdev ? c2(cdev + 0x2B4Cu) : 0);
          }
        }
        // Re-widen on EVERY draw, not just the first. The one-shot path above
        // gets draw #1 through - it completes with [dc+134]=0 and both
        // surfaces bound, the first time the real render path has run - but by
        // draw #2 the device has restored its own ~4.6 KB window (cur[30] back
        // in the 40888xxx range) and 81A042E0 fails again. Only intervene when
        // the window is actually too small, so the device keeps its own when
        // it is adequate.
        if (guide_cmdbuf_base_) {
          if (ckpt_on) XELOGI("GuideCk: sv_pdev");
          auto* wm = kernel_state()->memory();
          auto w2 = [wm](uint32_t a) {
            return a ? xe::load_and_swap<uint32_t>(wm->TranslateVirtual(a))
                     : 0u;
          };
          uint32_t wdc = w2(guide_draw_this_ + 12);
          uint32_t wwrap = wdc ? w2(wdc + 0x1CCu) : 0;
          uint32_t wdev = wwrap ? w2(wwrap + 12u) : 0;
          if (wdev) {
            uint32_t wcur = w2(wdev + 0x30u);
            uint32_t wend = w2(wdev + 0x34u);
            if (wcur < guide_cmdbuf_base_ ||
                wcur >= guide_cmdbuf_base_ + guide_cmdbuf_size_ ||
                wend - wcur < 0x8000u) {
              xe::store_and_swap<uint32_t>(wm->TranslateVirtual(wdev + 0x30u),
                                           guide_cmdbuf_base_);
              xe::store_and_swap<uint32_t>(
                  wm->TranslateVirtual(wdev + 0x34u),
                  guide_cmdbuf_base_ + guide_cmdbuf_size_);
              static std::atomic<uint32_t> wn{0};
              uint32_t wi = ++wn;
              if (wi <= 3) {
                XELOGI("Guide: re-widened #{} (was cur={:08X} end={:08X})", wi,
                       wcur, wend);
              }
            }
          if (ckpt_on) XELOGI("GuideCk: sv_entryloop");
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
          if (!fdev) fdev = f3(XamDeviceSlot());
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
              if (ckpt_on) XELOGI("GuideCk: sv_devdiff");
                     if (ckpt_on) XELOGI("GuideCk: sv_diffblock");
            }
          }
        }
      }
      if (::cvars::guide_bind_title_rt) {
        // Must run BEFORE guide_fake_front_buffer, which clones RT0
        // into +3F74 and does nothing while RT0 is null.
        static bool rt_done = false;
        {
          static uint32_t seen_rt = 0;
          if (seen_rt++ < 3) {
            XELOGI("Guide: bind_title_rt block entered (rt_done={})",
                   rt_done);
          }
        }
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
      if (ckpt_on) XELOGI("GuideCk: use_bound_device");
          if (!rdev) {
            rdev = r2(XamDeviceSlot());
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
      if (ckpt_on) XELOGI("GuideCk: fake_ring");
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
          // RT0 counts as already bound only if it looks like a real
          // surface. Testing "non-zero" alone silently skipped the bind
          // whenever the slot held junk - it reads 00000060 under the
          // bootstrap, which is not a pointer, and the present then
          // dereferences it as one.
          uint32_t cur_rt0 = r2(rdev + 0x32A0u);
          bool rt0_plausible = false;
          if (cur_rt0 >= 0x10000u) {
            auto* rh = rm->LookupHeap(cur_rt0);
            rt0_plausible =
                rh && rh->QueryRangeAccess(cur_rt0, cur_rt0 + 0x27u) !=
                          xe::memory::PageAccess::kNoAccess;
          }
          {
            // Remember the surface: the bind below targets the device
            // reached through the wrapper, which is NOT the device the
            // present uses ([dc+0x1CC]). The draw path binds it there too.
            if (surf) guide_title_surface_ = surf;
      if (ckpt_on) XELOGI("GuideCk: bind_title_rt");
            static uint32_t rtlog = 0;
            if (rtlog++ < 3) {
              XELOGI("Guide: RT bind sees dev {:08X} RT0 {:08X} "
                     "(plausible={}) surf {:08X}",
                     rdev, cur_rt0, rt0_plausible, surf);
            }
          }
          if (cur_rt0 && !rt0_plausible) {
            XELOGW("Guide: RT0 holds {:08X}, not a surface - rebinding",
                   cur_rt0);
          }
          if (rdev && surf && !rt0_plausible) {
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
                          bth->thread_state(), GuideConst(0x819F31A8u), sargs,
                          xe::countof(sargs))
                    : 0;
            XELOGI("Guide: SetRenderTarget(dev {:08X}, 0, {:08X} "
                   "fc={:08X}) -> {:08X}; RT0 now {:08X} [3F78]={:08X} "
                   "[3F74]={:08X}",
                   rdev, surf, r2(surf + 0x24u),
                   static_cast<uint32_t>(sres), r2(rdev + 0x32A0u),
                   r2(rdev + 0x3F78u), r2(rdev + 0x3F74u));
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
          uint32_t ddev = drd(XamDeviceSlot());
          uint32_t rt0 = ddev ? drd(ddev + 0x32A0u) : 0;
          uint32_t dep = ddev ? drd(ddev + 0x32B0u) : 0;
          if (ddev && rt0 && !dep) {
            uint32_t clone = dm->SystemHeapAlloc(0x100, 16);
            if (clone) {
              std::memcpy(dm->TranslateVirtual(clone),
                          dm->TranslateVirtual(rt0), 0x100);
              uint64_t dargs[] = {ddev, clone};
              uint64_t dr = kernel_state()->processor()->Execute(
                  gth->thread_state(), GuideConst(0x819F38C8u), dargs,
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
      if (ckpt_on) XELOGI("GuideCk: fake_front_buffer");
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
      // FE030000..FE050000 is where the draw wrote when the device used its
      // own reservation window. Since guide_bind_cmdbuf_kb repoints
      // [dev+0x30]/[dev+0x34] at our allocation, the packets now land there
      // instead - guide_diff_draw_writes shows ~12k changed words in the heap
      // blocks and nothing at all in FE03xxxx. Watch the buffer we actually
      // gave the device, else the word diff finds nothing to extract or run.
      const uint32_t kWdLo =
          guide_cmdbuf_base_ ? guide_cmdbuf_base_ : 0xFE030000u;
      const uint32_t kWdHi =
          guide_cmdbuf_base_ ? (guide_cmdbuf_base_ + guide_cmdbuf_size_)
                             : 0xFE050000u;
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
      if (ckpt_on) XELOGI("GuideCk: bind_depth_copy");
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
                if (ckpt_on) XELOGI("GuideCk: force_real_present");
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
      // --- front-buffer setup, BEFORE the draw ---
      // The guide_force_front_buffer block further down is unreachable: it
      // sits after this Execute, and the draw it is meant to enable faults
      // inside the emitter, so the guest thread dies and control never
      // returns to it. Run the same work here, with the other setup cvars.
      //
      // 81A0FE48 allocates [dev+0x2B10] (81A0A290(0x80,5,2) at 81A0FEF4) and
      // performs the setup that fills [dev+0x3F74]. Its second argument is a
      // 124-byte block; mode 2 hardcodes `li r7,0` at 8178F7BC so there is
      // none to pass through and it must be synthesized. 8178E9F0 memsets the
      // block and then writes only the fields below, so zero-plus-these is
      // faithful. 640x480 is that function's own fallback branch
      // (li r10,640 / li r11,480 at 8178EAE4/8178EAEC).
      if (::cvars::guide_force_front_buffer) {
        static bool pre_fb_done = false;
        if (!pre_fb_done) {
          auto* fm = kernel_state()->memory();
          auto fr2 = [fm](uint32_t a) {
            return a ? xe::load_and_swap<uint32_t>(fm->TranslateVirtual(a))
                     : 0u;
          };
          uint32_t fdc = fr2(guide_draw_this_ + 12);
          uint32_t fwrap = fdc ? fr2(fdc + 0x1CCu) : 0;
          uint32_t fdev = fwrap ? fr2(fwrap + 0x0Cu) : 0;
          // xam refused the call with "current thread (0x16) is trying to use
          // a D3D device object that is owned by a different thread (0x6)".
          // [dev+0x2B08] is where that owner is recorded - 81A03D40 asserts
          // it equals 817F6C30(), xam's current-thread query. Read both and
          // compare, so "the hook is on the wrong thread" is separated from
          // "the device was created in another phase".
          uint32_t owner = fdev ? fr2(fdev + 0x2B08u) : 0;
          uint64_t cur = kernel_state()->processor()->Execute(
              gth->thread_state(), GuideConst(0x817F6C30u), nullptr, 0);
          XELOGI("Guide: pre-draw front-buffer gate: dc={:08X} wrap={:08X} "
                 "dev={:08X} [3F74]={:08X} [2B10]={:08X} owner[2B08]={:08X} "
                 "817F6C30()={:08X} match={}",
                 fdc, fwrap, fdev, fdev ? fr2(fdev + 0x3F74u) : 0,
                 fdev ? fr2(fdev + 0x2B10u) : 0, owner,
                 static_cast<uint32_t>(cur),
                 owner == static_cast<uint32_t>(cur) ? "YES" : "NO");
          if (fdev && !fr2(fdev + 0x3F74u)) {
            pre_fb_done = true;
            // EXPERIMENT, not a fix: xam refuses device calls whose caller is
            // not the creating thread recorded at [dev+0x2B08]. Forge that
            // field to the calling thread for the duration of the call, to
            // confirm in one run that thread ownership is the only thing
            // standing between us and 81A0FE48 completing. If this works the
            // real change is to create the device on this thread instead.
            uint32_t saved_owner = fr2(fdev + 0x2B08u);
            if (saved_owner != static_cast<uint32_t>(cur)) {
              xe::store_and_swap<uint32_t>(fm->TranslateVirtual(fdev + 0x2B08u),
                                           static_cast<uint32_t>(cur));
              XELOGI("Guide: [dev+2B08] {:08X} -> {:08X} (ownership forged "
                     "for the setup call)",
                     saved_owner, static_cast<uint32_t>(cur));
            }
            uint32_t blk = fm->SystemHeapAlloc(0x7C, 16);
            if (blk) {
              std::memset(fm->TranslateVirtual(blk), 0, 0x7C);
              auto wr = [&](uint32_t off, uint32_t v) {
                xe::store_and_swap<uint32_t>(fm->TranslateVirtual(blk + off),
                                             v);
              };
              // Verbatim from a real mode-1 block, captured by dumping arg5 at
              // the 819F4D28 breakpoint during a --guide_create_primary_device
              // run (DevCreateBlock @7113F190). Every non-zero word is
              // reproduced; everything else stays zeroed by the memset above.
              //
              // Two things this corrected: 640x480 IS the right size here (the
              // "minimum frame buffer of 1280x720" validator line is a
              // certification advisory, not a rejection - the real block
              // triggers it too), and +0x40/+0x70/+0x74 are populated, which
              // earlier guesses left at zero.
      if (ckpt_on) XELOGI("GuideCk: second_context");
              wr(0x00, 0x00000280);  // 640  width
              wr(0x04, 0x000001E0);  // 480  height
              wr(0x08, 0x28280186);
              wr(0x34, 0x80000000);
              wr(0x3C, 0x00000001);
              wr(0x40, 0x24900106);
              wr(0x4C, 0x00001000);  // 4096 - read via +0x48 by 81A0FE48
              wr(0x54, 0x00010000);
              wr(0x68, 0x00000280);  // 640
              wr(0x6C, 0x000001E0);  // 480
              wr(0x70, 0x00000780);  // 1920
              wr(0x74, 0x00000438);  // 1080
              // Bracket the call. The JIT demand-compiles 81A0FE48 and its
              // callees, so it is definitely entered, but the result line has
              // never printed - which leaves "faults inside" and "never
              // returns" indistinguishable. Log both sides.
              XELOGI("Guide: calling 81A0FE48(dev {:08X}, blk {:08X}) ...",
                     fdev, blk);
              uint64_t fa[] = {fdev, blk};
              uint64_t fres = kernel_state()->processor()->Execute(
                  gth->thread_state(), GuideConst(0x81A0FE48u), fa, xe::countof(fa));
              XELOGI("Guide: 81A0FE48 RETURNED {:08X}",
                     static_cast<uint32_t>(fres));
              XELOGI("Guide: pre-draw 81A0FE48(dev {:08X}, blk {:08X}) -> "
                     "{:08X}; [3F74]={:08X} [2B10]={:08X} [30]={:08X} "
                     "[34]={:08X}",
                     fdev, blk, static_cast<uint32_t>(fres),
                     fr2(fdev + 0x3F74u), fr2(fdev + 0x2B10u),
                     fr2(fdev + 0x30u), fr2(fdev + 0x34u));
            }
          }
        }
      }
      // Bracket the draw itself. With guide_claim_device_thread the run
      // wedges and no "Guide composite draw" line is ever printed, but that
      // line sits a long way below this call behind several blocks - so its
      // absence does not establish that the guest call is where the thread
      // is stuck. One log either side settles it.
      // Run the composite draw's own sequence and READ the results it throws
      // away. 913EAB28 ends in `li r3, 0`, so it reports S_OK no matter what
      // XuiSendMessage, XuiBubbleMessage, XuiRenderEnd or XuiRenderPresent
      // returned - which is why its return value has never carried any signal.
      // Drawing happens by sending a render message down the element tree, so
      // the interesting HRESULTs are the two message passes.
      if (::cvars::guide_trace_draw && guide_draw_this_) {
        static bool traced = false;
        if (!traced) {
          traced = true;
          auto* tm = kernel_state()->memory();
          auto trd = [tm](uint32_t a) {
            return a ? xe::load_and_swap<uint32_t>(tm->TranslateVirtual(a))
                     : 0u;
          };
          auto xmt = kernel_state()->GetModule("xam.xex", true);
          auto ord = [&](uint32_t o) {
            return xmt ? xmt->GetProcAddressByOrdinal(o) : 0u;
          };
          uint32_t f_begin = ord(0x34B), f_layout = ord(0x82A);
          uint32_t f_send = ord(0x35F), f_bubble = ord(0x322);
          uint32_t f_end = ord(0x34F), f_present = ord(0x353);
          uint32_t dc = trd(guide_draw_this_ + 12u);
          uint32_t root = trd(guide_draw_this_ + 8u);
          uint32_t msgb = tm->SystemHeapAlloc(256, 16);
          uint32_t payb = tm->SystemHeapAlloc(256, 16);
          if (f_begin && f_send && f_bubble && dc && root && msgb && payb) {
            std::memset(tm->TranslateVirtual(msgb), 0, 256);
            std::memset(tm->TranslateVirtual(payb), 0, 256);
            auto* pr = kernel_state()->processor();
            auto* tsx = gth->thread_state();
            auto call = [&](uint32_t fn, std::initializer_list<uint64_t> a) {
              std::vector<uint64_t> v(a);
              return fn ? uint32_t(pr->Execute(tsx, fn, v.data(), v.size()))
                        : 0xDEADu;
            };
            uint32_t hb = call(f_begin, {dc, 0xFF000000});
            uint32_t hl = call(f_layout, {root});
            call(guide_bs_hud_base_ + 0xA888u, {msgb, payb, dc, 0xFFFFFFFFull, 1});
            // [msg+8] is the handled flag: 81949F50 zeroes it on entry and
            // aborts the chain walk as soon as a handler sets it. Reading it
            // after the send distinguishes "the walk was stopped early" from
            // "the chain only had two links", which phase 198's object diff
            // could not tell apart.
            auto msgw = [&](uint32_t off) {
              return xe::load_and_swap<uint32_t>(
                  tm->TranslateVirtual(msgb + off));
            };
            // Resolve the handle the way XuiSendMessage does and read the
            // chain node it dispatches through. 8194A51C..8194A574:
            //   table = 0x81D70000 - 12072 = 81D6D0D8
            //   idx   = handle & 0xFFFF, must be < [table+1056]
            //   sub   = [table + (idx >> 6) * 4]
            //   entry = sub + (idx & 0x3F) * 8
            //   generation check: [entry+0] == (handle >> 16) & 0xFFFF
            //   object = [entry+4]
            // This is NOT what 81931040 returns - on the scene object that
            // pointer has float bounds at +0x1C, where the dispatcher expects
            // a handler pointer, so the two must be different structures.
            {
              const uint32_t kTab = 0x81D6D0D8u;
              uint32_t idx = root & 0xFFFFu;
              uint32_t gen = (root >> 16) & 0xFFFFu;
              uint32_t cnt = trd(kTab + 1056u);
              // rlwinm r9,r10,26,6,29 rotates right 6 then masks bits 2..25,
              // which clears the low two bits of (idx>>6) - i.e. (idx>>8)*4,
              // 256 handles per sub-table. rlwinm r11,r10,3,21,28 is idx*8
              // masked to 0x7F8, i.e. (idx & 0xFF)*8. Both were decoded wrong
              // twice before; worked out from the mask bit numbering rather
              // than guessed a third time.
              uint32_t sub = (idx < cnt) ? trd(kTab + (idx >> 8) * 4u) : 0;
              uint32_t ent = sub ? (sub + (idx & 0xFFu) * 8u) : 0;
              uint32_t egen = ent ? trd(ent) : 0;
              uint32_t node = (ent && egen == gen) ? trd(ent + 4u) : 0;
              XELOGI("GuideChain: handle {:08X} idx={} gen={:04X} cnt={} "
                     "sub={:08X} ent={:08X} egen={:04X} node={:08X}",
                     root, idx, gen, cnt, sub, ent, egen, node);
              // The rlwinm masks above were decoded wrong - the entry found
              // had [+0]=10000001 where the code compares against the
              // generation 0001. Scan the sub-tables instead: the entry we
              // want is the one whose [+4] is an object we already know, and
              // finding it empirically also reveals the real stride.
              // Scan each sub-table's full extent word by word. The four
              // pointers are 0x810 apart, so an 8-byte stride over 64 entries
              // covers a fraction of it - the guess was too small as well as
              // misaligned. Look for any word equal to an object we know and
              // report where it sits; the layout follows from that rather
              // than from re-deriving the rlwinm masks a third time.
              for (uint32_t st = 0; st < 4; ++st) {
                uint32_t sb = trd(kTab + st * 4u);
                if (!sb) continue;
                for (uint32_t off = 0; off < 0x810u; off += 4) {
                  uint32_t ov = trd(sb + off);
                  if (ov == 0x407E94F0u || ov == 0x407E95F0u ||
                      ov == 0x40802A80u || ov == 0x407ECB00u) {
                    XELOGI("GuideChain: FOUND sub[{}]={:08X} +{:04X} = {:08X}"
                           "  prev={:08X} next={:08X}",
                           st, sb, off, ov, trd(sb + off - 4u),
                           trd(sb + off + 4u));
                  }
                }
              }
              uint32_t hops = 0;
              for (uint32_t o = node; o && hops < 8; o = trd(o + 8u), ++hops) {
                XELOGI("GuideChain:  [{}] node={:08X} this=[+0]={:08X} "
                       "cls=[+4]={:08X} next=[+8]={:08X} "
                       "handler=[+1C]={:08X} ctx=[+20]={:08X}",
                       hops, o, trd(o), trd(o + 4u), trd(o + 8u),
                       trd(o + 0x1Cu), trd(o + 0x20u));
              }
            }
            // Verify the gate empirically and call the paint directly.
            //
            // Phase 204 concluded from flag values that 81954468 returns 1 and
            // 81968890 therefore runs. That is a reading, not a measurement.
            // Call both against chain node 1's context - the element object
            // that carries the bounds - and bracket the device's reservation
            // cursor around the paint. If the cursor moves, the paint emits
            // and something upstream is not reaching it; if it does not, the
            // paint itself is where the geometry goes missing.
            {
              uint32_t node1 = 0, ctx1 = 0;
              {
                const uint32_t kT = 0x81D6D0D8u;
                uint32_t ix = root & 0xFFFFu;
                uint32_t sb2 = trd(kT + (ix >> 8) * 4u);
                uint32_t en2 = sb2 ? (sb2 + (ix & 0xFFu) * 8u) : 0;
                uint32_t n0 = en2 ? trd(en2 + 4u) : 0;
                node1 = n0 ? trd(n0 + 8u) : 0;
                ctx1 = node1 ? trd(node1 + 0x20u) : 0;
              }
              uint32_t dev = guide_resv_dev_;
              uint32_t cur0 = dev ? trd(dev + 0x30u) : 0;
              uint32_t gate = ctx1 ? call(0x81954468u, {ctx1}) : 0xDEADu;
              uint32_t pr2 = (ctx1 && gate == 1)
                                 ? call(0x81968890u, {ctx1, msgb})
                                 : 0xDEADu;
              uint32_t cur1 = dev ? trd(dev + 0x30u) : 0;
              XELOGI("GuidePaint: node1={:08X} ctx1={:08X} gate 81954468->"
                     "{:08X} paint 81968890->{:08X}; cursor {:08X}->{:08X} "
                     "({} words)",
                     node1, ctx1, gate, pr2, cur0, cur1,
                     (cur1 > cur0) ? (cur1 - cur0) / 4 : 0);
              // Call the paint's four children in isolation, each with the
              // cursor bracketed, to find which one should emit and does not.
              // Signatures come from 81968890's body:
              //   81966C60(obj, payload, &buf)
              //   81963A38(obj, payload)
              //   81963608(obj, payload, &buf)
              //   81957598(obj, msg)
              if (ctx1) {
                uint32_t payload = msgw(16);
                uint32_t sbuf = tm->SystemHeapAlloc(64, 16);
                if (sbuf) std::memset(tm->TranslateVirtual(sbuf), 0, 64);
                struct Sub { uint32_t fn; const char* name; int argc; };
                const Sub subs[] = {{0x81966C60u, "81966C60", 3},
                                    {0x81963A38u, "81963A38", 2},
                                    {0x81963608u, "81963608", 3},
                                    {0x81957598u, "81957598", 2}};
                for (const auto& sb3 : subs) {
                  uint32_t c0 = dev ? trd(dev + 0x30u) : 0;
                  uint32_t rr;
                  if (sb3.argc == 3) {
                    rr = call(sb3.fn, {ctx1, payload, sbuf});
                  } else if (sb3.fn == 0x81957598u) {
                    rr = call(sb3.fn, {ctx1, msgb});
                  } else {
                    rr = call(sb3.fn, {ctx1, payload});
                  }
                  uint32_t c1 = dev ? trd(dev + 0x30u) : 0;
                  XELOGI("GuidePaint:   {} -> {:08X}; cursor {:08X}->{:08X} "
                         "({} words)",
                         sb3.name, rr, c0, c1,
                         (c1 > c0) ? (c1 - c0) / 4 : 0);
                }
              }
            }
            // Do the elements have anything to draw?
            //
            // Every structural check in the render path now passes and nothing
            // emits, so the remaining candidate is content. An element
            // rasterises its visual; walk down the tree, ask each node for its
            // visual, resolve that through 81931040 and dump it. An element
            // whose visual does not resolve, or resolves to an object with no
            // geometry, has nothing to paint and a correct paint emits nothing.
            {
              uint32_t f_last2 = ord(0x32F), f_vis = ord(0x395);
              uint32_t ob2 = tm->SystemHeapAlloc(16, 16);
              uint32_t h = root;
              for (int depth = 0; depth < 4 && h && f_last2 && f_vis && ob2;
                   ++depth) {
                std::memset(tm->TranslateVirtual(ob2), 0, 16);
                uint32_t vr2 = call(f_vis, {h, ob2});
                uint32_t vh = trd(ob2);
                uint32_t vobj = 0;
                if (vh) {
                  vobj = call(0x81931040u, {vh});
                }
                // Dump the visual and flag anything pointing into hud's
                // resource section (91401000-91429E9D, 167581b) or into the
                // guest heap. A visual with no reference to either has nothing
                // to rasterise, which is what "full pipeline state, zero
                // draws" looks like from the GPU side.
                std::string vd;
                if (vobj) {
                  for (uint32_t i = 0; i < 32; ++i) {
                    uint32_t wv = trd(vobj + i * 4);
                    const char* tag = "";
                    // Narrow the heap tag. 0x40000000-0x4FFFFFFF also covers
                    // ordinary float bit patterns - 44550000 is 852.0f - and
                    // tagging those as pointers made the phase 211 dump
                    // misleading. The guest heap objects in play all sit in
                    // 0x40000000-0x41000000.
                    if (wv >= 0x91401000u && wv <= 0x91429E9Du) {
                      tag = "*RES";
                    } else if (wv >= 0x40000000u && wv < 0x41000000u) {
                      tag = "*HEAP";
                    }
                    vd += fmt::format("{:02X}:{:08X}{} ", i * 4, wv, tag);
                  }
                }
                // Also report the render forwarding collections at each
                // depth. 8195AA00 descends by iterating {ptr,count} at
                // [obj+12]/[obj+16] and [obj+24]/[obj+28]; if those are empty
                // at the top, the message never reaches the elements that do
                // have visuals - which are at depth 2 and 3.
                uint32_t eo_pub = 0, eo_int = call(0x81931040u, {h});
                {
                  uint32_t f_o = ord(0x346);
                  if (f_o && ob2) {
                    std::memset(tm->TranslateVirtual(ob2), 0, 16);
                    uint64_t oa2[] = {h, ob2};
                    pr->Execute(tsx, f_o, oa2, xe::countof(oa2));
                    eo_pub = trd(ob2);
                  }
                }
                // Follow the visual's two unexplained pointers.
                if (vobj) {
                  for (uint32_t which : {0x04u, 0x10u}) {
                    uint32_t sp2 = trd(vobj + which);
                    if (!sp2) continue;
                    std::string sd;
                    for (uint32_t i = 0; i < 24; ++i) {
                      uint32_t wv = trd(sp2 + i * 4);
                      const char* tg = "";
                      if (wv >= 0x91401000u && wv <= 0x91429E9Du) tg = "*RES";
                      else if (wv >= 0x40000000u && wv < 0x41000000u) tg = "*H";
                      sd += fmt::format("{:02X}:{:08X}{} ", i * 4, wv, tg);
                    }
                    XELOGI("GuideVisual:   depth {} vobj+{:02X} -> {:08X}: {}",
                           depth, which, sp2, sd);
                    // The +0x10 object is element-shaped with its own bounds
                    // (287x350 at 425,61) - a real sub-element. Paint it and
                    // see whether IT emits draws, which the element above it
                    // does not.
                    if (which == 0x10u) {
                      uint32_t q0 = guide_resv_dev_
                                        ? trd(guide_resv_dev_ + 0x30u) : 0;
                      uint32_t gq = call(0x81954468u, {sp2});
                      uint32_t pq = call(0x81968890u, {sp2, msgb});
                      uint32_t q1 = guide_resv_dev_
                                        ? trd(guide_resv_dev_ + 0x30u) : 0;
                      XELOGI("GuideVisual:   SUBPAINT {:08X} gate={:08X} "
                             "paint={:08X} cursor {:08X}->{:08X} ({} words)",
                             sp2, gq, pq, q0, q1,
                             (q1 > q0) ? (q1 - q0) / 4 : 0);
                      // Decode what it wrote. Phase 210's 2881-word stream was
                      // state only; whether this one carries DRAW_INDX decides
                      // whether the geometry exists and is merely unreachable,
                      // or is never produced at all.
                      if (q1 > q0 && (q1 - q0) < 0x40000u) {
                        uint32_t nw2 = (q1 - q0) / 4;
                        uint32_t cts[128] = {0};
                        uint32_t a0 = 0, a2 = 0, pk2 = 0, ov2 = 0, jw = 0;
                        std::string f2;
                        while (jw < nw2) {
                          uint32_t wd = trd(q0 + jw * 4);
                          uint32_t ty = wd >> 30;
                          uint32_t cn = ((wd >> 16) & 0x3FFF) + 1;
                          if (ty == 3) {
                            uint32_t op = (wd >> 8) & 0x7F;
                            cts[op]++; ++pk2;
                            if (pk2 <= 20) f2 += fmt::format("{:02X}x{} ", op, cn);
                            jw += 1 + cn;
                          } else if (ty == 0) { ++a0; jw += 1 + cn; }
                          else if (ty == 2) { ++a2; ++jw; }
                          else { jw += 2; }
                          if (jw > nw2) { ++ov2; break; }
                        }
                        std::string h2;
                        for (uint32_t o = 0; o < 128; ++o)
                          if (cts[o]) h2 += fmt::format("{:02X}:{} ", o, cts[o]);
                        XELOGI("GuideVisual:   SUBWALK {} words -> {} type3, "
                               "{} type0, {} type2, overrun={}",
                               nw2, pk2, a0, a2, ov2);
                        XELOGI("GuideVisual:   SUBWALK opcodes {}", h2);
                        XELOGI("GuideVisual:   SUBWALK first {}", f2);
                        XELOGI("GuideVisual:   SUBWALK DRAW_INDX(22)={} "
                               "DRAW_INDX_2(36)={}",
                               cts[0x22], cts[0x36]);
                        // Second, independent count. The walker overran with
                        // ~2000 of 2430 words undecoded, so its zero is about
                        // the prefix only. The command processor parses the
                        // whole range and counts draws itself; two methods
                        // agreeing is what made the phase 210 result safe.
                        auto* gsx = kernel_state()->emulator()->graphics_system();
                        if (gsx && gsx->command_processor()) {
                          auto* cpx = gsx->command_processor();
                          uint32_t before2 = cpx->guide_draw_count_;
                          cpx->ExecuteGuestBufferVirtualUnsafe(q0, nw2);
                          XELOGI("GuideVisual:   SUBEXEC executed {} words at "
                                 "{:08X}; GPU draws +{}",
                                 nw2, q0, cpx->guide_draw_count_ - before2);
                        }
                      }
                    }
                  }
                }
                XELOGI("GuideVisual: depth {} elem {:08X} GetVisual->{:08X} "
                       "vh={:08X} vobj={:08X} {}",
                       depth, h, vr2, vh, vobj,
                       vd.empty() ? std::string("(unresolved)") : vd);
                XELOGI("GuideVisual:   coll pub {:08X} [12]={:08X} [16]={} "
                       "[24]={:08X} [28]={} | int {:08X} [12]={:08X} [16]={}",
                       eo_pub, eo_pub ? trd(eo_pub + 12u) : 0,
                       eo_pub ? trd(eo_pub + 16u) : 0,
                       eo_pub ? trd(eo_pub + 24u) : 0,
                       eo_pub ? trd(eo_pub + 28u) : 0,
                       eo_int, eo_int ? trd(eo_int + 12u) : 0,
                       eo_int ? trd(eo_int + 16u) : 0);
                // If this element HAS a visual, paint it directly. Phases
                // 205-206 painted the depth 0/1 object, which has none, so
                // "emits nothing" was the correct result for the wrong
                // element. These are the ones with something to draw.
                if (vh && eo_int) {
                  // `dev` was in scope here before the replay; it comes from
                  // the same place the block above uses.
                  uint32_t dev = guide_resv_dev_;
                  uint32_t pc0 = dev ? trd(dev + 0x30u) : 0;
                  uint32_t g2 = call(0x81954468u, {eo_int});
                  uint32_t p2 = call(0x81968890u, {eo_int, msgb});
                  uint32_t pc1 = dev ? trd(dev + 0x30u) : 0;
                  // Report the three things the gate actually tests, so a
                  // closed gate can be attributed rather than guessed:
                  //   [obj+0xB4] bit 0 set, bit 30 clear, [obj+0x24] > 0.0
                  uint32_t fl = trd(eo_int + 0xB4u);
                  uint32_t op = trd(eo_int + 0x24u);
                  float opf;
                  std::memcpy(&opf, &op, 4);
                  XELOGI("GuideVisual:   PAINT depth {} obj {:08X} gate={:08X}"
                         " paint={:08X} cursor {:08X}->{:08X} ({} words)",
                         depth, eo_int, g2, p2, pc0, pc1,
                         (pc1 > pc0) ? (pc1 - pc0) / 4 : 0);
                  XELOGI("GuideVisual:     gate inputs [B4]={:08X} bit0={} "
                         "bit30={} [24]={:08X} ({}f)",
                         fl, fl & 1u, (fl >> 30) & 1u, op, opf);
                }
                std::memset(tm->TranslateVirtual(ob2), 0, 16);
                call(f_last2, {h, ob2});
                h = trd(ob2);
              }
            }
            std::string mpre;
            for (uint32_t i = 0; i < 8; ++i) {
              mpre += fmt::format("{:02X}:{:08X} ", i * 4, msgw(i * 4));
            }
            uint32_t hs = call(f_send, {root, msgb});
            std::string mpost;
            for (uint32_t i = 0; i < 8; ++i) {
              mpost += fmt::format("{:02X}:{:08X} ", i * 4, msgw(i * 4));
            }
            XELOGI("GuideTrace: msg before send: {}", mpre);
            XELOGI("GuideTrace: msg after  send: {}  handled[+8]={:08X}",
                   mpost, msgw(8));
            call(guide_bs_hud_base_ + 0xA890u, {msgb, payb, dc, 0xFFFFFFFFull, 1});
            uint32_t hu = call(f_bubble, {root, msgb});
            uint32_t he = call(f_end, {dc});
            uint32_t hp = call(f_present, {dc, 0, 0, 0});
            XELOGI("GuideTrace: dc={:08X} root={:08X} | RenderBegin={:08X} "
                   "LayoutTree={:08X} SendMessage={:08X} BubbleMessage={:08X} "
                   "RenderEnd={:08X} Present={:08X}",
                   dc, root, hb, hl, hs, hu, he, hp);
          } else {
            XELOGW("GuideTrace: skipped (begin={:08X} send={:08X} "
                   "bubble={:08X} dc={:08X} root={:08X})",
                   f_begin, f_send, f_bubble, dc, root);
          }
        }
      }
      // Give the root scene a size.
      //
      // XuiElementSetBounds (ordinal 0x336, 81932110) takes its width and
      // height as FLOATS in f1/f2, which Processor::Execute cannot pass - it
      // only fills GPRs - so the field is written directly instead. The object
      // dumps show where it lives: labelHeading's object carries
      // `08:BF800000 0C:BF800000`, i.e. -1.0/-1.0 meaning auto-size, while the
      // scene object 407E95F0 carries `08:00000000 0C:00000000`. A root
      // element with 0x0 bounds clips its whole subtree away, which is what an
      // XUI pipeline that returns S_OK at every stage and emits no geometry
      // looks like.
      if (::cvars::guide_scene_bounds_w > 0 && guide_draw_this_) {
        static bool bounds_done = false;
        if (!bounds_done) {
          bounds_done = true;
          auto* bm = kernel_state()->memory();
          auto xmb = kernel_state()->GetModule("xam.xex", true);
          uint32_t f_obj = xmb ? xmb->GetProcAddressByOrdinal(0x346) : 0;
          uint32_t root = xe::load_and_swap<uint32_t>(
              bm->TranslateVirtual(guide_draw_this_ + 8u));
          uint32_t ob = bm->SystemHeapAlloc(16, 16);
          if (f_obj && root && ob) {
            std::memset(bm->TranslateVirtual(ob), 0, 16);
            uint64_t oa[] = {root, ob};
            kernel_state()->processor()->Execute(gth->thread_state(), f_obj,
                                                 oa, xe::countof(oa));
            uint32_t so = xe::load_and_swap<uint32_t>(bm->TranslateVirtual(ob));
            if (so) {
              // Call the real setter with real float arguments.
              //
              // Processor::Execute fills r3..r10 and then calls the no-arg
              // overload, which touches only r1 and lr - it never writes the
              // FPRs. So f1/f2 can simply be set on the context beforehand and
              // they survive into the guest. XuiElementSetBounds takes its
              // width and height there (`fmr f31,f1` / `fmr f30,f2` at
              // 8193212C/81932130), so this is a proper call rather than the
              // phase 194 poke, which wrote two words at offsets guessed from
              // a different object type and proved nothing.
              uint32_t f_bounds = xmb ? xmb->GetProcAddressByOrdinal(0x336) : 0;
              auto rdo = [bm](uint32_t a) {
                return xe::load_and_swap<uint32_t>(bm->TranslateVirtual(a));
              };
              // Snapshot the whole object either side of the call and report
              // which words move. Reading two fixed offsets is what made
              // phase 194 wrong; a diff cannot pick the wrong field because it
              // does not pick one at all.
              // Diff the object the SETTER resolves, not the one
              // XuiObjectFromHandle hands back. 81931040 is the internal
              // resolver every typed accessor funnels through, and there is no
              // guarantee the public API returns the same pointer. Given how
              // many results this session were invalidated by watching a
              // neighbouring object, check rather than assume.
              uint32_t so_int = 0;
              {
                uint64_t ra[] = {root};
                so_int = uint32_t(kernel_state()->processor()->Execute(
                    gth->thread_state(), GuideConst(0x81931040u), ra, xe::countof(ra)));
              }
              XELOGI("GuideBounds: XuiObjectFromHandle={:08X} "
                     "internal 81931040={:08X} {}",
                     so, so_int, so == so_int ? "(same)" : "(DIFFERENT)");
              if (so_int) {
                so = so_int;
              }
              // Dump the INTERNAL object head. Every object head recorded in
              // this investigation came from XuiObjectFromHandle, which
              // returns a different pointer - so those dumps describe
              // something adjacent to what was meant. Re-derive it here.
              if (so_int) {
                std::string head;
                for (uint32_t i = 0; i < 32; ++i) {
                  head += fmt::format("{:02X}:{:08X} ", i * 4,
                                      rdo(so_int + i * 4));
                }
                XELOGI("GuideBounds: internal scene object {:08X}: {}", so_int,
                       head);
              }
              const uint32_t kObjWords = 96;
              std::vector<uint32_t> snap(kObjWords);
              for (uint32_t i = 0; i < kObjWords; ++i) {
                snap[i] = rdo(so + i * 4);
              }
              uint32_t before8 = rdo(so + 8u), beforeC = rdo(so + 0x0Cu);
              uint32_t br = 0xDEAD;
              if (f_bounds) {
                auto* ctx = gth->thread_state()->context();
                ctx->f[1] = double(::cvars::guide_scene_bounds_w);
                ctx->f[2] = double(::cvars::guide_scene_bounds_h);
                uint64_t ba[] = {root};
                br = uint32_t(kernel_state()->processor()->Execute(
                    gth->thread_state(), f_bounds, ba, xe::countof(ba)));
              }
              std::string diff;
              for (uint32_t i = 0; i < kObjWords; ++i) {
                uint32_t now = rdo(so + i * 4);
                if (now != snap[i]) {
                  diff += fmt::format("+{:02X}:{:08X}->{:08X} ", i * 4,
                                      snap[i], now);
                }
              }
              XELOGI("GuideBounds: SetBounds(scene {:08X}, {}f, {}f) -> "
                     "{:08X}; obj {:08X} [+8]={:08X}->{:08X} "
                     "[+C]={:08X}->{:08X}",
                     root, ::cvars::guide_scene_bounds_w,
                     ::cvars::guide_scene_bounds_h, br, so, before8,
                     rdo(so + 8u), beforeC, rdo(so + 0x0Cu));
              XELOGI("GuideBounds: object diff: {}",
                     diff.empty() ? std::string("NOTHING CHANGED") : diff);
            }
          }
        }
      }
      // Does the render message reach the child elements at all?
      //
      // The draw is a message dispatch and every stage returns S_OK, so
      // success tells nothing. Snapshot the element objects before the draw
      // and diff them after: untouched children mean the message is not
      // arriving, changed children mean they receive it and decline to paint.
      // Objects are resolved through 81931040, the path XUI's own accessors
      // use - XuiObjectFromHandle returns a different pointer (phase 196).
      static std::vector<uint32_t> objdiff_addr;
      static std::vector<uint32_t> objdiff_snap;
      const uint32_t kOdWords = 64;
      bool objdiff_on = ::cvars::guide_diff_draw_objects;
      if (objdiff_on) {
        static uint32_t od_runs = 0;
        objdiff_on = od_runs++ < 3;
      }
      if (objdiff_on) {
        auto* om2 = kernel_state()->memory();
        auto ord2 = [om2](uint32_t a) {
          return a ? xe::load_and_swap<uint32_t>(om2->TranslateVirtual(a)) : 0u;
        };
        uint32_t root_h = ord2(guide_draw_this_ + 8u);
        objdiff_addr.clear();
        objdiff_snap.clear();
        if (root_h) {
          uint64_t ra2[] = {root_h};
          uint32_t root_i = uint32_t(kernel_state()->processor()->Execute(
              gth->thread_state(), GuideConst(0x81931040u), ra2, xe::countof(ra2)));
          if (root_i) {
            objdiff_addr.push_back(root_i);
            // [+8] and [+0x0C] both point at the same object on the scene -
            // a child list head/tail - so follow it one level.
            uint32_t kid_i = ord2(root_i + 8u);
            if (kid_i) objdiff_addr.push_back(kid_i);
            uint32_t kid2 = kid_i ? ord2(kid_i + 8u) : 0;
            if (kid2) objdiff_addr.push_back(kid2);
          }
        }
        for (uint32_t a : objdiff_addr) {
          for (uint32_t i = 0; i < kOdWords; ++i) {
            objdiff_snap.push_back(ord2(a + i * 4));
          }
        }
      }
      // Re-mark the elements dirty before each draw.
      //
      // ObjDiff showed a flag at [obj+0xB4] going 01008003 -> 01008001 on the
      // root and its first child during draw #1, and nothing changing on any
      // draw after. Bit 1 is a dirty flag: XUI repaints an element only while
      // it is set, clears it once painted, and from frame 2 on there is
      // nothing dirty to repaint. That is exactly the observed shape - draw #1
      // does work, every draw after emits zero words - and it is the first
      // explanation that accounts for the *change* between frames rather than
      // just the absence of output.
      if (::cvars::guide_force_dirty && guide_draw_this_) {
        auto* dm = kernel_state()->memory();
        auto drd = [dm](uint32_t a) {
          return a ? xe::load_and_swap<uint32_t>(dm->TranslateVirtual(a)) : 0u;
        };
        uint32_t rh = drd(guide_draw_this_ + 8u);
        if (rh) {
          uint64_t ra3[] = {rh};
          uint32_t ri = uint32_t(kernel_state()->processor()->Execute(
              gth->thread_state(), GuideConst(0x81931040u), ra3, xe::countof(ra3)));
          uint32_t marked = 0;
          for (uint32_t o = ri; o; o = drd(o + 8u)) {
            uint32_t f = drd(o + 0xB4u);
            if (!(f & 2u)) {
              xe::store_and_swap<uint32_t>(dm->TranslateVirtual(o + 0xB4u),
                                           f | 2u);
              ++marked;
            }
            if (marked > 32) break;
          }
          static uint32_t dirty_logs = 0;
          if (dirty_logs++ < 3) {
            XELOGI("GuideDirty: marked {} elements dirty from root obj {:08X}",
                   marked, ri);
          }
        }
      }
      // Put the scene's children into the collection the RENDER walks.
      //
      // 8195AA00 forwards a render message by iterating a {ptr,count} pair at
      // [obj+12]/[obj+16] (and [obj+24]/[obj+28]), 8-byte entries whose first
      // word is a child handle. On our scene both counts are zero, so the
      // walk forwards to nothing - which is why every stage returns S_OK and
      // nothing paints.
      //
      // XuiElementAddChild's worker 81967390 reads [parent+12] and hands it to
      // 81963C50, so AddChild is what fills that collection. The navigable
      // tree (XuiElementGetLastChild) is populated and the render collection
      // is not - they are different structures, and only the first was ever
      // built.
      //
      // Phase 192 used AddChild but then made the newly added child the draw
      // root, so the render iterated the child's own empty collection. Here
      // the root stays the scene and its existing child is added to it.
      if (::cvars::guide_add_render_children && guide_draw_this_) {
        static bool added = false;
        if (!added) {
          added = true;
          auto* am = kernel_state()->memory();
          auto ard = [am](uint32_t a) {
            return a ? xe::load_and_swap<uint32_t>(am->TranslateVirtual(a))
                     : 0u;
          };
          auto xma = kernel_state()->GetModule("xam.xex", true);
          uint32_t f_last = xma ? xma->GetProcAddressByOrdinal(0x32F) : 0;
          uint32_t f_add = xma ? xma->GetProcAddressByOrdinal(0x328) : 0;
          uint32_t f_obj = xma ? xma->GetProcAddressByOrdinal(0x346) : 0;
          uint32_t root = ard(guide_draw_this_ + 8u);
          uint32_t ob = am->SystemHeapAlloc(16, 16);
          if (f_last && f_add && f_obj && root && ob) {
            auto* pr = kernel_state()->processor();
            auto* tsa = gth->thread_state();
            // The object whose collection the handler reads is the one
            // XuiObjectFromHandle returns - node 0's ctx - not 81931040's.
            std::memset(am->TranslateVirtual(ob), 0, 16);
            uint64_t oa[] = {root, ob};
            pr->Execute(tsa, f_obj, oa, xe::countof(oa));
            uint32_t ro = ard(ob);
            uint32_t c0 = ard(ro + 16u), d0 = ard(ro + 12u);
            std::memset(am->TranslateVirtual(ob), 0, 16);
            uint64_t la[] = {root, ob};
            pr->Execute(tsa, f_last, la, xe::countof(la));
            uint32_t kid = ard(ob);
            uint32_t ar = 0xDEAD;
            if (kid) {
              uint64_t aa[] = {root, kid};
              ar = uint32_t(pr->Execute(tsa, f_add, aa, xe::countof(aa)));
            }
            XELOGI("GuideAddChild: root={:08X} obj={:08X} child={:08X} "
                   "AddChild->{:08X}; coll [12]={:08X}->{:08X} "
                   "[16]={}->{}",
                   root, ro, kid, ar, d0, ard(ro + 12u), c0, ard(ro + 16u));
          }
        }
      }
      // Make the Guide's elements visible.
      //
      // 81954468, the paint gate, requires bit 0 of [element+0xB4] set. On
      // scnInfoUpsellLive - the one element that produces real geometry, 2881
      // words of it, when painted directly - that word reads 11000004: bit 0
      // CLEAR. So the render path correctly skips a hidden element, and every
      // "everything succeeds and nothing draws" measurement in this
      // investigation has been the correct behaviour of a scene whose content
      // element is not visible.
      if (::cvars::guide_force_visible && guide_draw_this_) {
        auto* vm = kernel_state()->memory();
        auto vrd = [vm](uint32_t a) {
          return a ? xe::load_and_swap<uint32_t>(vm->TranslateVirtual(a)) : 0u;
        };
        auto xmv = kernel_state()->GetModule("xam.xex", true);
        uint32_t f_last3 = xmv ? xmv->GetProcAddressByOrdinal(0x32F) : 0;
        uint32_t vob = vm->SystemHeapAlloc(16, 16);
        uint32_t hh = vrd(guide_draw_this_ + 8u);
        uint32_t fixed = 0;
        // Walk the chain the PAINT walks, not the navigable tree.
        //
        // 81963A38 descends via [obj+8] for the first child and [child+16] for
        // siblings, gating each with 81954468. That is a different set of
        // objects from XuiElementGetLastChild, which is what this cvar used to
        // follow - so elements the descent visits could stay hidden while the
        // ones it does not were being fixed.
        {
          uint32_t root_i2 = 0;
          if (hh) {
            uint64_t rr[] = {hh};
            // Phase 308: is root_i2 the same object the draw root handle
            // resolves to? Log both so the two trees can be compared within
            // one run (cross-run pointers are invalid - phase 306).
            XELOGI("RootCompare: guide_draw_this_+8 handle={:08X} "
                   "guide_bs_scene_={:08X}",
                   hh, guide_bs_scene_);
            // Phase 310: resolve the same handle the way the guest does and
            // compare with the handle-table lookup. If they differ, the
            // phase 303-308 identification of the draw root was reading the
            // wrong table (the phase 304 self-test only covered binding
            // nodes).
            {
              uint32_t f_base = xmv ? xmv->GetProcAddressByOrdinal(0x33C) : 0;
              // XuiGetBaseObject(hObj, HXUIOBJ* phBase) returns an HRESULT
              // and writes the result through the out-pointer. Calling it with
              // one argument and reading r3 gives the STATUS (0 = S_OK) and
              // looks exactly like a null object - the same trap as the
              // XexGetModuleSection log misread in phase 277.
              uint32_t api_hr = 0, api_obj = 0;
              uint32_t obuf = vm->SystemHeapAlloc(16, 16);
              if (f_base && hh && obuf) {
                xe::store_and_swap<uint32_t>(vm->TranslateVirtual(obuf), 0);
                uint64_t ba[] = {hh, obuf};
                api_hr = uint32_t(kernel_state()->processor()->Execute(
                    gth->thread_state(), f_base, ba, xe::countof(ba)));
                api_obj = vrd(obuf);
              }
              XELOGI("HandleResolveCompare: handle={:08X} hr={:08X} "
                     "XuiGetBaseObject-> {:08X}",
                     hh, api_hr, api_obj);
            }
            root_i2 = uint32_t(kernel_state()->processor()->Execute(
                gth->thread_state(), GuideConst(0x81931040u), rr, xe::countof(rr)));
            // Phase 329: is "XuiTabScene" resolvable by name? Call the class
            // registry lookup 81949B60 directly with the name and compare the
            // result against the class object the census already identified
            // (tabclass, read from [81D6CE38]). Cheaper than enumerating the
            // map, and decisive either way.
            {
              static bool done = false;
              if (!done) {
                done = true;
                const char* names[] = {"XuiTabScene", "XuiScene", "XuiShader"};
                std::string out;
                for (const char* nm : names) {
                  size_t len = std::strlen(nm);
                  uint32_t buf = vm->SystemHeapAlloc(
                      static_cast<uint32_t>((len + 1) * 2), 16);
                  if (!buf) continue;
                  for (size_t k = 0; k < len; ++k) {
                    xe::store_and_swap<uint16_t>(
                        vm->TranslateVirtual(buf + uint32_t(k * 2)),
                        static_cast<uint16_t>(nm[k]));
                  }
                  xe::store_and_swap<uint16_t>(
                      vm->TranslateVirtual(buf + uint32_t(len * 2)), 0);
                  uint64_t la2[] = {buf};
                  uint32_t got = uint32_t(kernel_state()->processor()->Execute(
                      gth->thread_state(), GuideConst(0x81949B60u), la2,
                      xe::countof(la2)));
                  out += fmt::format("{}->{:08X} ", nm, got);
                }
                XELOGI("ClassByName: {}", out);
                // Phase 332: 8194F568 creates BY NAME (phase 331). Ask it to
                // create each class directly. This distinguishes phase 330's
                // two possibilities: if XuiTabScene/XuiShader instantiate on
                // demand they are live classes nothing happens to ask for; if
                // creation fails they are vestigial, like the phase 273 ring.
                std::string mk;
                for (const char* nm : names) {
                  size_t len = std::strlen(nm);
                  uint32_t nbuf = vm->SystemHeapAlloc(
                      static_cast<uint32_t>((len + 1) * 2), 16);
                  uint32_t obuf2 = vm->SystemHeapAlloc(16, 16);
                  if (!nbuf || !obuf2) continue;
                  for (size_t k = 0; k < len; ++k) {
                    xe::store_and_swap<uint16_t>(
                        vm->TranslateVirtual(nbuf + uint32_t(k * 2)),
                        static_cast<uint16_t>(nm[k]));
                  }
                  xe::store_and_swap<uint16_t>(
                      vm->TranslateVirtual(nbuf + uint32_t(len * 2)), 0);
                  xe::store_and_swap<uint32_t>(vm->TranslateVirtual(obuf2), 0);
                  uint64_t ca2[] = {nbuf, obuf2};
                  uint32_t hr2 = uint32_t(kernel_state()->processor()->Execute(
                      gth->thread_state(), GuideConst(0x8194F568u), ca2,
                      xe::countof(ca2)));
                  mk += fmt::format("{}: hr={:08X} obj={:08X} | ", nm, hr2,
                                    vrd(obuf2));
                }
                XELOGI("CreateByName: {}", mk);
                // Phase 334: create a XuiShader and attach it to the draw-root
                // element through XuiElementAddChild (ordinal 0x328) - the API
                // GuideNavM already uses successfully - rather than writing
                // into the collection at [element+0] whose node layout is not
                // established. Verified afterwards by the binding census.
                if (::cvars::guide_inject_shader && guide_bs_scene_) {
                  auto xm2 = kernel_state()->GetModule("xam.xex", true);
                  uint32_t f_add2 = xm2 ? xm2->GetProcAddressByOrdinal(0x328) : 0;
                  const char* sn = "XuiShader";
                  size_t sl = std::strlen(sn);
                  uint32_t nb = vm->SystemHeapAlloc(uint32_t((sl + 1) * 2), 16);
                  uint32_t ob = vm->SystemHeapAlloc(16, 16);
                  if (f_add2 && nb && ob) {
                    for (size_t k = 0; k < sl; ++k) {
                      xe::store_and_swap<uint16_t>(
                          vm->TranslateVirtual(nb + uint32_t(k * 2)),
                          static_cast<uint16_t>(sn[k]));
                    }
                    xe::store_and_swap<uint16_t>(
                        vm->TranslateVirtual(nb + uint32_t(sl * 2)), 0);
                    xe::store_and_swap<uint32_t>(vm->TranslateVirtual(ob), 0);
                    uint64_t ma[] = {nb, ob};
                    uint32_t chr = uint32_t(kernel_state()->processor()->Execute(
                        gth->thread_state(), GuideConst(0x8194F568u), ma, xe::countof(ma)));
                    uint32_t sh = vrd(ob);
                    uint32_t ahr = 0;
                    if (!chr && sh) {
                      uint64_t aa[] = {guide_bs_scene_, sh};
                      ahr = uint32_t(kernel_state()->processor()->Execute(
                          gth->thread_state(), f_add2, aa, xe::countof(aa)));
                    }
                    // Phase 336: the painter walks a chain threaded through
                    // [obj+20], not the child tree (phase 335), so AddChild
                    // was the wrong linkage. XuiControlAttachVisual (0x38A) is
                    // the API shaped like that relationship - attach a visual
                    // to a control - so try it as well and report both.
                    uint32_t f_av = xm2 ? xm2->GetProcAddressByOrdinal(0x38A) : 0;
                    uint32_t vhr = 0xFFFFFFFFu;
                    if (f_av && sh) {
                      uint64_t va[] = {guide_bs_scene_, sh};
                      vhr = uint32_t(kernel_state()->processor()->Execute(
                          gth->thread_state(), f_av, va, xe::countof(va)));
                    }
                    XELOGI("InjectShader: create hr={:08X} shader={:08X}; "
                           "AddChild -> {:08X}; AttachVisual(0x38A) -> {:08X}",
                           chr, sh, ahr, vhr);
                  }
                }
              }
            }
            // 81931040 takes the handle and returns a POINTER (the walk below
            // dereferences it). Resolving the same handle through the handle
            // table should give the same pointer; if it does not, the table
            // lookup used in phases 303-308 was reading the wrong structure.
            {
              uint32_t ti = hh & 0xFFFFu;
              auto trd2 = [&](uint32_t a) {
                return (a >= 0x40000000u && a < 0x50000000u && !(a & 3))
                           ? vrd(a) : 0u;
              };
              uint32_t tb = trd2(0x81D6D0D8u + ((ti >> 6) * 4u));
              uint32_t tobj = tb ? trd2(tb + ((ti * 8u) & 0x7F8u) + 4u) : 0u;
              XELOGI("RootResolveCheck: handle={:08X} api(81931040)={:08X} "
                     "table={:08X} -> {}",
                     hh, root_i2, tobj,
                     (root_i2 == tobj) ? "AGREE" : "DIFFER");
              // Phase 311: with the CORRECT object (the API's), redo the
              // class-chain read that phase 303 did on the wrong one.
              if (root_i2 >= 0x40000000u && root_i2 < 0x50000000u) {
                std::string chain;
                for (uint32_t nd = vrd(root_i2 + 12u), hop = 0;
                     nd >= 0x40000000u && nd < 0x50000000u && hop < 12;
                     nd = vrd(nd + 8u), ++hop) {
                  uint32_t cp = vrd(nd + 0x18u);
                  std::string nm;
                  if (cp >= 0x40000000u && cp < 0x50000000u) {
                    uint32_t np = vrd(cp + 4u);
                    if (np >= 0x40000000u && np < 0x50000000u) {
                      for (uint32_t c = 0; c < 24; ++c) {
                        uint32_t ad = np + c * 2u;
                        uint32_t wv = vrd(ad & ~3u);
                        uint16_t ch = (ad & 2u) ? (wv & 0xFFFF) : (wv >> 16);
                        if (!ch || ch < 0x20 || ch > 0x7E) break;
                        nm += (char)ch;
                      }
                    }
                  }
                  chain += nm.empty() ? fmt::format("{:08X} ", cp) : (nm + " ");
                }
                XELOGI("DrawRootClasses: obj={:08X} chain: {}", root_i2,
                       chain.empty() ? std::string("(none)") : chain);
                // Phase 314: a class object carries its own name at +4 and its
                // DECLARED BASE name at +8 - that is how XuiTabScene's base was
                // read as XuiScene. Read HUDScene's base directly instead of
                // inferring it from XUR string adjacency (phase 313's dead end).
                {
                  uint32_t nd0 = vrd(root_i2 + 12u);
                  uint32_t cls0 = (nd0 >= 0x40000000u && nd0 < 0x50000000u)
                                      ? vrd(nd0 + 0x18u) : 0u;
                  // Names may live in the HEAP (custom classes) or in the xam
                  // IMAGE (built-ins, e.g. XuiShader's name at 81646698). A
                  // heap-only guard silently returns "?" for every built-in
                  // base - the phase 276 lesson about filters removing data.
                  auto rdname = [&](uint32_t sp) {
                    std::string nm;
                    // Third range-filter miss in this investigation: hud.xex
                    // is mapped at 0x913E0000, so a CUSTOM class's name lives
                    // there, not in xam's image or the heap. Log the raw
                    // pointer too, so an out-of-range value is visible as a
                    // value rather than as "?".
                    bool okp = (sp >= 0x40000000u && sp < 0x50000000u) ||
                               (sp >= 0x81000000u && sp < 0x81E00000u) ||
                               (sp >= 0x90000000u && sp < 0x92000000u);
                    if (!okp) return nm;
                    for (uint32_t c = 0; c < 28; ++c) {
                      uint32_t ad = sp + c * 2u;
                      uint32_t wv = vrd(ad & ~3u);
                      uint16_t ch = (ad & 2u) ? (wv & 0xFFFF) : (wv >> 16);
                      if (!ch || ch < 0x20 || ch > 0x7E) break;
                      nm += (char)ch;
                    }
                    return nm;
                  };
                  if (cls0 >= 0x40000000u && cls0 < 0x50000000u) {
                    std::string self = rdname(vrd(cls0 + 4u));
                    std::string base = rdname(vrd(cls0 + 8u));
                    // [cls+8] read as a string works for built-ins
                    // (XuiTabScene -> 'XuiScene') but not here, so for this
                    // class it is probably the base CLASS OBJECT, whose own
                    // name is at its +4. Try that as well before concluding.
                    uint32_t bp = vrd(cls0 + 8u);
                    std::string base2 = rdname(vrd(bp + 4u));
                    // The binder (8194EBB0) recurses on [class+40], NOT
                    // [class+8]: `lwz r11,40(r28); beq -> stop; lwz r3,4(r11);
                    // bl 8194EBB0`. So +40 is the link that decides whether a
                    // base gets bound at all.
                    // Phase 322: full field dump of the class object reached
                    // through the API-resolved root_i2 - the only sound route
                    // for elements (phase 310). Phase 321 dumped the right
                    // layout but the wrong object because it used the handle
                    // table again.
                    {
                      std::string w;
                      for (uint32_t q = 0; q < 15; ++q) {
                        w += fmt::format("+{}:{:08X} ", q * 4, vrd(cls0 + q * 4u));
                      }
                      XELOGI("DrawRootClassDump: cls={:08X} {}", cls0, w);
                    }
                    XELOGI("DrawRootClassLinks: cls={:08X} +8={:08X} +40={:08X} "
                           "(+40 is what the binder follows)",
                           cls0, vrd(cls0 + 8u), vrd(cls0 + 40u));
                    XELOGI("DrawRootClass2: baseptr={:08X} as-object name='{}' "
                           "its base={:08X}",
                           bp, base2.empty() ? "?" : base2, vrd(bp + 8u));
                    XELOGI("DrawRootClass: {:08X} name='{}' declared base='{}' "
                           "(nameptr={:08X} baseptr={:08X})",
                           cls0, self.empty() ? "?" : self,
                           base.empty() ? "?" : base,
                           vrd(cls0 + 4u), vrd(cls0 + 8u));
                  }
                }
              }
            }
          }
          uint32_t chain_fixed = 0, visited = 0;
          // Depth-first over (first child, next sibling), bounded.
          // The bound was 64, chosen when the scene had 62 objects. After
          // navigation loads real content the scene has 109+, so the walk
          // truncated and most elements never got bit 0 - which is exactly
          // what the paint gate 81954468 tests. Symptom: element paint calls
          // FELL from 26 to 6 when content was added (phase 280).
          static constexpr int kMaxWalk = 1024;
          uint32_t stack[kMaxWalk];
          int sp = 0;
          if (root_i2) stack[sp++] = root_i2;
          while (sp > 0 && visited < kMaxWalk) {
            uint32_t o = stack[--sp];
            ++visited;
            uint32_t fl = vrd(o + 0xB4u);
            // bit 0  - the visibility gate 81954468 requires (phase 208)
            // bit 17 - gates 8195E190 inside 81963608, the element's
            //          hand-off to the device context:
            //            81963624  lwz r11, 180(r29)
            //            81963628  rlwinm. r11, r11, 0, 14, 14
            //            8196362C  beq -> skip 8195E190
            //          It is clear on every element observed (11000005,
            //          01008005), so that call has never run.
            uint32_t want = 1u | (::cvars::guide_force_flag17 ? 0x20000u : 0u);
            if ((fl & want) != want) {
              xe::store_and_swap<uint32_t>(vm->TranslateVirtual(o + 0xB4u),
                                           fl | want);
              ++chain_fixed;
            }
            uint32_t kid = vrd(o + 8u);
            uint32_t sib = vrd(o + 16u);
            if (sib && sp < kMaxWalk - 1) stack[sp++] = sib;
            if (kid && sp < kMaxWalk - 1) stack[sp++] = kid;
          }
          // Name the objects actually modified, so they can be compared
          // against the ones the per-element log reads. Phase 229 assumed the
          // two sets overlapped and the comparison it drew from that was
          // wrong; this makes the assumption checkable.
          {
            static uint32_t idl = 0;
            if (idl++ < 2) {
              std::string mods;
              int sp4 = 0;
              uint32_t st4[64], seen4 = 0;
              if (root_i2) st4[sp4++] = root_i2;
              while (sp4 > 0 && seen4 < 10) {
                uint32_t o = st4[--sp4];
                ++seen4;
                mods += fmt::format("{:08X}:{:08X} ", o, vrd(o + 0xB4u));
                uint32_t kid = vrd(o + 8u), sib = vrd(o + 16u);
                if (sib && sp4 < 63) st4[sp4++] = sib;
                if (kid && sp4 < 63) st4[sp4++] = kid;
              }
              XELOGI("GuideChainIds: first 10 walked objects (post-write): {}",
                     mods);
            }
          }
          static uint32_t ch_logs = 0;
          if (ch_logs++ < 1) {
            XELOGI("GuideVisibleChain: visited {} objects on the paint chain, "
                   "set bit0 on {}",
                   visited, chain_fixed);
            // Name each element's class. [obj+4] points at a descriptor whose
            // first bytes are a UTF-16 name - the visual's read "XuiScene".
            // A 59-element scene that emits no primitives should contain image
            // or text leaves; if it is containers all the way down, there is
            // nothing in it that would ever draw.
            int sp2 = 0;
            uint32_t st2[64];
            std::map<std::string, int> classes;
            if (root_i2) st2[sp2++] = root_i2;
            uint32_t seen2 = 0;
            while (sp2 > 0 && seen2 < 64) {
              uint32_t o = st2[--sp2];
              ++seen2;
              uint32_t cd = vrd(o + 4u);
              std::string nm;
              if (cd) {
                for (uint32_t i = 0; i < 24; ++i) {
                  uint16_t c = xe::load_and_swap<uint16_t>(
                      vm->TranslateVirtual(cd + i * 2));
                  if (!c) break;
                  if (c >= 32 && c < 127) nm.push_back(char(c));
                  else { nm.clear(); break; }
                }
              }
              if (nm.empty()) nm = "?";
              classes[nm]++;
              uint32_t kid = vrd(o + 8u), sib = vrd(o + 16u);
              if (sib && sp2 < 63) st2[sp2++] = sib;
              if (kid && sp2 < 63) st2[sp2++] = kid;
            }
            std::string cl;
            for (auto& kv : classes) {
              cl += fmt::format("{}x{} ", kv.first, kv.second);
            }
            XELOGI("GuideVisibleChain: classes {}", cl);
            // Census the HANDLERS. Each element is dispatched by handle
            // through its own chain, and the chain node's [+0x1C] is the
            // function that paints it. If every element shares one generic
            // handler then no class-specific painter is installed for the
            // buttons, labels and figures - which would explain a scene full
            // of drawable content that never draws.
            {
              const uint32_t kTb = 0x81D6D0D8u;
              std::map<uint32_t, int> handlers;
              int sp3 = 0;
              // Was 64, which sampled 64 of the blade's 464 elements and made
              // "every element uses handler 8193FA30" look established when it
              // was a sampling artefact - the same trap as the phase 282
              // census. Cover the whole tree.
              uint32_t st3[1024];
              uint32_t seen3 = 0;
              if (root_i2) st3[sp3++] = root_i2;
              while (sp3 > 0 && seen3 < 1024) {
                uint32_t o = st3[--sp3];
                ++seen3;
                uint32_t hnd = vrd(o);
                uint32_t ix = hnd & 0xFFFFu;
                uint32_t cnt3 = vrd(kTb + 1056u);
                if (ix < cnt3) {
                  uint32_t sb4 = vrd(kTb + (ix >> 8) * 4u);
                  uint32_t en4 = sb4 ? (sb4 + (ix & 0xFFu) * 8u) : 0;
                  uint32_t gen4 = en4 ? vrd(en4) : 0;
                  if (en4 && gen4 == ((hnd >> 16) & 0xFFFFu)) {
                    for (uint32_t nd = vrd(en4 + 4u), hop = 0;
                         nd && hop < 6; nd = vrd(nd + 8u), ++hop) {
                      handlers[vrd(nd + 0x1Cu)]++;
                      // Dump one node in full. The handler values are not
                      // referenced by any code (phase 292), so the node must
                      // be built at runtime; its word 0 should be a vtable
                      // that identifies what builds it.
                      {
                        static bool dumped = false;
                        if (!dumped) {
                          dumped = true;
                          std::string w;
                          for (uint32_t q = 0; q < 10; ++q) {
                            w += fmt::format("+{:X}:{:08X} ", q * 4,
                                             vrd(nd + q * 4u));
                          }
                          XELOGI("HandlerNode {:08X} on element {:08X}: {}",
                                 nd, o, w);
                        }
                      }
                    }
                  }
                }
                uint32_t kid = vrd(o + 8u), sib = vrd(o + 16u);
                if (sib && sp3 < 1023) st3[sp3++] = sib;
                if (kid && sp3 < 1023) st3[sp3++] = kid;
              }
              std::string hs;
              for (auto& kv : handlers) {
                hs += fmt::format("{:08X}x{} ", kv.first, kv.second);
              }
              XELOGI("GuideVisibleChain: handlers {}", hs);
              // Phase 312 control: is "custom class -> 1 binding, built-in ->
              // leaf+base" a real pattern, or a coincidence of the two objects
              // compared in phase 311? Walk the tree again and record, per
              // leaf class name, the binding-chain length.
              {
                std::map<std::string, std::pair<uint32_t, uint32_t>> shape;
                int sp5 = 0; uint32_t st5[1024], seen5 = 0;
                if (root_i2) st5[sp5++] = root_i2;
                while (sp5 > 0 && seen5 < 1024) {
                  uint32_t o5 = st5[--sp5]; ++seen5;
                  uint32_t len = 0; std::string leaf;
                  for (uint32_t nd = vrd(o5 + 12u), hop = 0;
                       nd >= 0x40000000u && nd < 0x50000000u && hop < 12;
                       nd = vrd(nd + 8u), ++hop) {
                    ++len;
                    if (leaf.empty()) {
                      uint32_t cp = vrd(nd + 0x18u);
                      uint32_t np = (cp >= 0x40000000u && cp < 0x50000000u)
                                        ? vrd(cp + 4u) : 0u;
                      if (np >= 0x40000000u && np < 0x50000000u) {
                        for (uint32_t c = 0; c < 24; ++c) {
                          uint32_t ad = np + c * 2u;
                          uint32_t wv = vrd(ad & ~3u);
                          uint16_t ch = (ad & 2u) ? (wv & 0xFFFF) : (wv >> 16);
                          if (!ch || ch < 0x20 || ch > 0x7E) break;
                          leaf += (char)ch;
                        }
                      }
                    }
                  }
                  // Phase 315: also walk the DECLARED inheritance from the
                  // leaf class object ([cls+8] -> base class object) and
                  // compare its depth with the binding-chain length. If they
                  // agree for deep-chained classes and disagree for HUDScene,
                  // the difference between those cases is the defect.
                  uint32_t decl = 0;
                  {
                    uint32_t nd1 = vrd(o5 + 12u);
                    uint32_t c1 = (nd1 >= 0x40000000u && nd1 < 0x50000000u)
                                      ? vrd(nd1 + 0x18u) : 0u;
                    // [cls+8] is a base CLASS OBJECT for custom classes but a
                    // base NAME STRING for built-ins (phase 314). Following it
                    // blindly, as phase 315 did, reads [string+8] as a pointer
                    // and produces garbage. Disambiguate at every hop: if the
                    // target has a readable UTF-16 name at ITS +4 it is a class
                    // object and the walk continues; if the target is itself a
                    // readable string it is a terminal base name; anything else
                    // ends the walk.
                    auto readable = [&](uint32_t sp) {
                      bool okp = (sp >= 0x40000000u && sp < 0x50000000u) ||
                                 (sp >= 0x81000000u && sp < 0x81E00000u) ||
                                 (sp >= 0x90000000u && sp < 0x92000000u);
                      if (!okp) return false;
                      uint32_t wv = vrd(sp & ~3u);
                      uint16_t ch = (sp & 2u) ? (wv & 0xFFFF) : (wv >> 16);
                      return ch >= 0x20 && ch <= 0x7E;
                    };
                    while (c1 >= 0x40000000u && c1 < 0x50000000u && decl < 12) {
                      ++decl;
                      uint32_t nxt = vrd(c1 + 8u);
                      // nxt must be validated BEFORE dereferencing nxt+4 -
                      // guarding the value read is not the same as guarding
                      // the address read from (phase 287).
                      bool nxt_ok = (nxt >= 0x40000000u && nxt < 0x50000000u) ||
                                    (nxt >= 0x81000000u && nxt < 0x81E00000u) ||
                                    (nxt >= 0x90000000u && nxt < 0x92000000u);
                      if (!nxt_ok) break;
                      if (readable(vrd(nxt + 4u))) {
                        c1 = nxt;          // base is a class object
                      } else if (readable(nxt)) {
                        ++decl;            // terminal base name
                        break;
                      } else {
                        break;
                      }
                    }
                  }
                  if (!leaf.empty()) {
                    auto& e = shape[leaf];
                    e.first++; e.second = (len << 8) | (decl & 0xFF);
                  }
                  uint32_t kid = vrd(o5 + 8u), sib = vrd(o5 + 16u);
                  if (sib && sp5 < 1023) st5[sp5++] = sib;
                  if (kid && sp5 < 1023) st5[sp5++] = kid;
                }
                std::string sh;
                for (auto& kv : shape) {
                  sh += fmt::format("{}(n={},chain={},decl={}) ", kv.first,
                                    kv.second.first, kv.second.second >> 8,
                                    kv.second.second & 0xFF);
                }
                XELOGI("ChainShapes: {}", sh);
              }
            }
          }
        }
        static uint32_t vis_logs = 0;
        std::string rep;
        for (int d = 0; d < 6 && hh && f_last3 && vob; ++d) {
          uint64_t ra4[] = {hh};
          uint32_t oi = uint32_t(kernel_state()->processor()->Execute(
              gth->thread_state(), GuideConst(0x81931040u), ra4, xe::countof(ra4)));
          if (oi) {
            uint32_t fl = vrd(oi + 0xB4u);
            if (!(fl & 1u)) {
              xe::store_and_swap<uint32_t>(vm->TranslateVirtual(oi + 0xB4u),
                                           fl | 1u);
              ++fixed;
              if (vis_logs < 2) {
                rep += fmt::format("{:08X}:{:08X}->{:08X} ", oi, fl, fl | 1u);
              }
            }
          }
          std::memset(vm->TranslateVirtual(vob), 0, 16);
          uint64_t la3[] = {hh, vob};
          kernel_state()->processor()->Execute(gth->thread_state(), f_last3,
                                               la3, xe::countof(la3));
          hh = vrd(vob);
        }
        if (vis_logs++ < 2) {
          XELOGI("GuideVisible: set bit0 on {} elements {}", fixed, rep);
        }
      }
      // Drive a real frame and paint the elements that have content.
      //
      // WORKAROUND, not a fix. The render message never descends past depth 1
      // because the forwarding collections are empty (phase 207), so the
      // element that actually draws - scnInfoUpsellLive, 2881 words when
      // painted - is never asked to. This replicates hud's own frame
      // (RenderBegin, LayoutTree, ..., RenderEnd, Present) and calls the paint
      // directly on each element that owns a visual, standing in for the
      // descent that is missing.
      if (::cvars::guide_paint_frame && guide_draw_this_) {
        auto* pm2 = kernel_state()->memory();
        // Clear the 0x20 latch immediately before the paint, not only in the
        // composite-draw hook. 81A02940 refuses every vertex allocation while
        // that bit is set, and the paint is what makes the allocations - so a
        // clear applied in the draw hook, on a different call and possibly a
        // different frame, tests nothing. The command-buffer-end path
        // (81A00458 -> 81A018B8) re-sets the bit, so it has to be cleared
        // adjacent to the work that depends on it.
        // Supply the structure the guard protects, before clearing the
        // guard. Phase 400: clearing 0x20 alone sends 81A02940 into
        // 81A019B8, which does lwz r10,11024(r31); lwz r10,4(r10) and faults
        // because a type-2 device never allocated that field. The type-1
        // initializer allocates 128 bytes there (81A0FEF4), so allocate the
        // same and zero it. Ordering matters: this has to happen before the
        // clear, and both before the paint that makes the allocations.
        if (::cvars::guide_supply_ring_base && guide_resv_dev_) {
          uint32_t cur = xe::load_and_swap<uint32_t>(
              pm2->TranslateVirtual(guide_resv_dev_ + 11024u));
          if (!cur) {
            uint32_t nb = pm2->SystemHeapAlloc(128, 128);
            if (nb) {
              std::memset(pm2->TranslateVirtual(nb), 0, 128);
              xe::store_and_swap<uint32_t>(
                  pm2->TranslateVirtual(guide_resv_dev_ + 11024u), nb);
              static uint32_t slog = 0;
              if (slog++ < 3) {
                XELOGI("PrePaintRing: supplied [dev+11024] = {:08X}", nb);
              }
            }
          }
        }
        if (::cvars::guide_clear_cmd_overflow && guide_resv_dev_) {
          uint8_t* pf =
              pm2->TranslateVirtual<uint8_t*>(guide_resv_dev_ + 11069u);
          if (pf) {
            static uint32_t plog = 0;
            uint8_t was = *pf;
            *pf = static_cast<uint8_t>(was & ~0x20u);
            if (plog++ < 3) {
              XELOGI("PrePaintClear: [dev+11069] {:02X} -> {:02X}", was, *pf);
            }
          }
        }
        auto prd2 = [pm2](uint32_t a) {
          return a ? xe::load_and_swap<uint32_t>(pm2->TranslateVirtual(a)) : 0u;
        };
        auto xmp = kernel_state()->GetModule("xam.xex", true);
        auto po = [&](uint32_t o) {
          return xmp ? xmp->GetProcAddressByOrdinal(o) : 0u;
        };
        static uint32_t pmsg = 0, ppay = 0, pout = 0;
        if (!pmsg) {
          pmsg = pm2->SystemHeapAlloc(256, 16);
          ppay = pm2->SystemHeapAlloc(256, 16);
          pout = pm2->SystemHeapAlloc(16, 16);
        }
        uint32_t dc2 = prd2(guide_draw_this_ + 0x0Cu);
        uint32_t rt2 = prd2(guide_draw_this_ + 8u);
        uint32_t f_b = po(0x34B), f_l = po(0x82A), f_e = po(0x34F),
                 f_p = po(0x353), f_lc = po(0x32F), f_v = po(0x395);
        uint32_t f_bounds_api = po(0x336);
        if (dc2 && rt2 && pmsg && f_b && f_e && f_p && f_lc && f_v) {
          auto* prc = kernel_state()->processor();
          auto* tsp = gth->thread_state();
          auto pcall = [&](uint32_t fn, std::initializer_list<uint64_t> a) {
            std::vector<uint64_t> v(a);
            return fn ? uint32_t(prc->Execute(tsp, fn, v.data(), v.size()))
                      : 0xDEADu;
          };
          // Sample the dirty bit between SetBounds and the layout.
          //
          // Phase 230 changed an element's bounds through the API and found
          // bit 17 clear when the paint read it - but the paint runs after
          // XuiElementLayoutTree, and a layout pass is exactly what would
          // consume dirty flags. Reading the flag immediately after the API
          // call, before RenderBegin and LayoutTree, says whether the API sets
          // it at all.
          if (::cvars::guide_dirty_via_api && f_bounds_api) {
            auto* vm2 = kernel_state()->memory();
            uint32_t ri3 = pcall(0x81931040u, {rt2});
            if (ri3) {
              uint32_t f_before = prd2(ri3 + 0xB4u);
              uint32_t bw3 = prd2(ri3 + 0x1Cu), bh3 = prd2(ri3 + 0x20u);
              float cw3, ch3;
              std::memcpy(&cw3, &bw3, 4);
              std::memcpy(&ch3, &bh3, 4);
              auto* cx3 = gth->thread_state()->context();
              cx3->f[1] = double(cw3 + 1.0f);
              cx3->f[2] = double(ch3);
              uint64_t sa3[] = {rt2};
              prc->Execute(tsp, f_bounds_api, sa3, xe::countof(sa3));
              uint32_t f_mid = prd2(ri3 + 0xB4u);
              // Does the LAYOUT clear a forced bit? That is the last open
              // reading of phase 229: bit 17 was set on 59 objects and read
              // back clear inside the paint frame, and the only thing running
              // in between is this frame's RenderBegin and LayoutTree. Force
              // it here, run the layout, read it back.
              xe::store_and_swap<uint32_t>(
                  vm2->TranslateVirtual(ri3 + 0xB4u), f_mid | 0x20000u);
              uint32_t f_forced = prd2(ri3 + 0xB4u);
              pcall(f_b, {dc2, 0xFF000000});
              pcall(f_l, {rt2});
              uint32_t f_after_layout = prd2(ri3 + 0xB4u);
              static uint32_t dl = 0;
              if (dl++ < 3) {
                XELOGI("GuideDirty: [B4] {:08X} -> {:08X} after SetBounds "
                       "(bit17 {} -> {}); forced {:08X}; after "
                       "RenderBegin+LayoutTree {:08X} (bit17 {})",
                       f_before, f_mid, (f_before >> 17) & 1,
                       (f_mid >> 17) & 1, f_forced, f_after_layout,
                       (f_after_layout >> 17) & 1);
              }
            }
          }
          if (!::cvars::guide_dirty_via_api) {
            pcall(f_b, {dc2, 0xFF000000});
            pcall(f_l, {rt2});
          }
          // Sample the cursor AFTER RenderBegin. Phase 209's reading spanned
          // the rebind that happens inside it - 3009C030 to 40875814 - and so
          // measured a pointer change rather than an amount of output. Taking
          // both samples after the rebind makes the delta the paint's own.
          // Phase 503: this sampled through guide_resv_dev_, which is 0 on this
          // configuration - the same broken instrument phase 487 found and fixed
          // at one site only. With a zero base both samples read 0 and the delta
          // is forced to zero, so "paint wrote 0 words" was a property of the
          // probe, not of the paint. Derive the device the way the draw-entry
          // probe does, which yields a real one (40870D00) in these runs.
          uint32_t paint_dc_ = prd2(guide_draw_this_ + 12u);
          uint32_t paint_wr_ = paint_dc_ ? prd2(paint_dc_ + 0x1CCu) : 0;
          uint32_t paint_dev_ = paint_wr_ ? prd2(paint_wr_ + 0x0Cu) : 0;
          if (!paint_dev_) paint_dev_ = guide_resv_dev_;
          uint32_t before = paint_dev_ ? prd2(paint_dev_ + 0x30u) : 0;
          // Phase 513: the 6927 words are three 0x905-word reserve calls this
          // harness makes itself, one per element (ReserveCall returns exactly
          // the measured cursors). A reserve hands out memory without writing
          // it, so the "gamma ramp" phases 505/506/511/512 analysed may simply
          // be whatever was already there. Stamp the range with a sentinel: if
          // the LUT pattern comes back, something really writes it; if the
          // sentinel survives, the paint emits nothing at all.
          if (before && ::cvars::guide_paint_sentinel) {
            static bool stamped = false;
            if (!stamped) {
              stamped = true;
              for (uint32_t k = 0; k < 0x4000u; ++k) {
                xe::store_and_swap<uint32_t>(
                    pm2->TranslateVirtual(before + k * 4u), 0xDEADBEEFu);
              }
              XELOGI("PaintSentinel: stamped 0x4000 words at {:08X}", before);
            }
          }
          // [dev+0x30] is the reserve window, not the PM4 command block. The
          // first walk of that range desynced on word 0 (02D00500 parses as a
          // type-0 header claiming 721 registers) and the raw dump shows
          // register/value pairs - 1921, 1922, 1925, 1927 - rather than
          // packets. The command block cursor is [dev+0x2B4C]; sample it too so
          // the two ranges can be compared instead of assumed equal.
          uint32_t before_cb = paint_dev_ ? prd2(paint_dev_ + 0x2B4Cu) : 0;
          std::memset(pm2->TranslateVirtual(pmsg), 0, 256);
          std::memset(pm2->TranslateVirtual(ppay), 0, 256);
          pcall(guide_bs_hud_base_ + 0xA888u,
                {pmsg, ppay, dc2, 0xFFFFFFFFull, 1});
          // The paint gets a visual from 81931040 -> 819426F0, which does not read a
          // field on the widget: it indexes a global table based at 81D6D0D8 with the
          // bound at [81D6D4F8] and twi traps on the range (phase 459). A zero bound
          // means the table was never built; a non-zero bound with a miss means the
          // index is wrong. Those need opposite fixes, so measure before changing.
          if (XamIsDashrootLayout()) {
            XELOGI("VisualTable: base=81D6D0D8 [0]={:08X} [4]={:08X} [8]={:08X} "
                   "bound[81D6D4F8]={:08X}",
                   prd2(0x81D6D0D8u), prd2(0x81D6D0DCu), prd2(0x81D6D0E0u),
                   prd2(0x81D6D4F8u));
          }
          uint32_t painted = 0;
          uint32_t hp = rt2;
          // 819426F0 indexes the visual table with rlwinm r10,rObj,0,16,31 - the low
          // 16 bits of what it is handed - and traps/returns 0 when that exceeds the
          // bound (0x400). A handle like 0001005A gives index 5A; a heap pointer like
          // 408C5500 gives 5500, which is out of range. rt2 is read as a pointer, so
          // log the index this walk actually produces.
          XELOGI("VisualIndex: hp={:08X} low16={:04X} bound=0400 {}", hp, hp & 0xFFFFu,
                 ((hp & 0xFFFFu) < 0x400u) ? "in range" : "OUT OF RANGE");
          for (int d = 0; d < 6 && hp; ++d) {
            std::memset(pm2->TranslateVirtual(pout), 0, 16);
            // The return value was discarded; only `out` was checked. An
            // HRESULT says why 0x395 declines, which a null out cannot.
            // Phase 568: 0x395 (81935B60) uses its first argument directly as
            // an object - it null-checks only the OUT pointer, then casts arg0
            // against the visual class at 81935BA4. It does not resolve
            // handles. The walk carries XUI handles, so pass the resolved
            // object.
            uint32_t hp_obj = ::cvars::guide_resolve_paint_handles
                                  ? GuideResolveHandle(hp)
                                  : 0;
            uint32_t vhr = pcall(f_v, {hp_obj ? hp_obj : hp, pout});
            if (hp_obj) {
              static uint32_t rhlog = 0;
              if (rhlog++ < 4) {
                XELOGI("PaintResolve: handle {:08X} -> object {:08X} for 0x395",
                       hp, hp_obj);
              }
            }
            uint32_t vh2 = prd2(pout);
            if (d < 3) {
              // 81931C90 returns S_OK with *out = 0 when [obj+8] is zero
              // (81931D08: r3=0 / stw r3,0(r30) / return 0), and the export
              // then reports 80300017. Resolve the object the same way it
              // does and read +8 directly, rather than trusting the read.
              // 0x395 does NOT hand hp to 81931040. It first calls
              // 81943378(hp, typeid) - which clobbers r3 - and passes THAT
              // result on (phase 465). Probing with hp directly resolved a
              // different object, which is what made phases 462-464
              // contradict the disassembly. Mirror the export exactly.
              uint32_t typid = prd2(0x81D6CDDCu);
              uint32_t h2 = pcall(0x81943378u, {hp, typid});
              uint32_t obj = h2 ? pcall(0x81931040u, {h2}) : 0;
              uint32_t obj_old = pcall(0x81931040u, {hp});
              XELOGI("VisualCall: 0x395@{:08X}({:08X}) hr={:08X} out={:08X} "
                     "d={} | obj={:08X} [obj+8]={:08X}",
                     f_v, hp, vhr, vh2, d, obj,
                     obj ? prd2(obj + 8u) : 0);
              XELOGI("VisualObj: hp={:08X} typeid={:08X} 81943378->{:08X} "
                     "obj={:08X} [obj+8]={:08X} | hp-direct obj={:08X}",
                     hp, typid, h2, obj, obj ? prd2(obj + 8u) : 0, obj_old);
              // The objects match, so phase 465's account of the contradiction
              // is wrong too. Decompose one level further: call 81931C90 with
              // exactly what the export passes it and see what IT writes,
              // instead of inferring from the pieces.
              std::memset(pm2->TranslateVirtual(pout), 0, 16);
              uint32_t r90 = pcall(0x81931C90u, {h2, pout});
              uint32_t vis90 = prd2(pout);  // before VisualAgain clears it
              XELOGI("VisualInner: 81931C90({:08X},out) -> hr={:08X} out={:08X}",
                     h2, r90, prd2(pout));
              // Does pcall reproduce the real call at all? Invoke 0x395 again,
              // same handle, immediately after. If the repeat also fails then
              // pcall is faithful and the difference lies inside 0x395; if it
              // succeeds, the first call changed state and no amount of
              // decomposing the second one explains the first.
              std::memset(pm2->TranslateVirtual(pout), 0, 16);
              uint32_t again = pcall(f_v, {hp_obj ? hp_obj : hp, pout});
              XELOGI("VisualAgain: 0x395({:08X}) 2nd call -> hr={:08X} out={:08X}",
                     hp, again, prd2(pout));
              // 0x395 validates TWICE. The second check (81935BE8) takes the
              // visual handle 81931C90 returned and validates it against a
              // different type id at 81D6CDE0; failing that is what raises
              // 80300017. So the error does not mean "no visual" - for a node
              // that has one, it means the visual is the wrong type.
              uint32_t typid2 = prd2(0x81D6CDE0u);
              XELOGI("VisualType: visual={:08X} typeid2={:08X} validate->{:08X}",
                     vis90, typid2, vis90 ? pcall(0x81943378u, {vis90, typid2}) : 0);
              // 8193F1B8 walks the base chain via +8, comparing [node+0x18]
              // against the required type id, and returns 0 if it runs off the
              // end. Walk the same chain here: if every [node+0x18] is null or
              // none equals typeid2, that is why the cast fails.
              uint32_t vo = vis90 ? pcall(0x81931040u, {vis90}) : 0;
              std::string chain;
              for (uint32_t nptr = vo, k = 0; nptr && k < 6; ++k) {
                chain += fmt::format("{:08X}(type={:08X}) -> ", nptr,
                                     prd2(nptr + 0x18u));
                nptr = prd2(nptr + 8u);
              }
              XELOGI("VisualChain: visobj={:08X} want={:08X} chain: {}end",
                     vo, typid2, chain);
              // Phase 563: when the walk finds nothing, name the type it wanted.
              // The class descriptor carries its names at +4 and +8 (the
              // ClassName probe reads them the same way), which turns "some type
              // has no visual" into a named control - and a name can be looked
              // for among what skin initialisation registered.
              if (!vo && typid2) {
                auto nm = [&](uint32_t a) {
                  std::string out;
                  if (!a) return out;
                  // UTF-16 big-endian: an ASCII character is 00 xx, so a
                  // byte-wise read stops on the first character and returns
                  // empty - which is what the first attempt reported.
                  for (uint32_t c = 0; c < 48; ++c) {
                    uint16_t ch = xe::load_and_swap<uint16_t>(
                        pm2->TranslateVirtual(a + c * 2u));
                    if (!ch) break;
                    out += (ch >= 0x20 && ch < 0x7F) ? char(ch) : '?';
                  }
                  return out;
                };
                XELOGI("VisualMissing: type {:08X} = '{}' / '{}'", typid2,
                       nm(prd2(typid2 + 4u)), nm(prd2(typid2 + 8u)));
                // Phase 566: 0x8030000A is a failed cast to XuiVisual, not a
                // lookup miss, so the question is what class the object we pass
                // actually is. Resolve the handle and walk its descriptor chain,
                // naming each level - the same +8 chain and +4/+8 names used
                // above.
                uint32_t oidx = hp & 0xFFFFu, otag = hp >> 16;
                uint32_t obkt = prd2(0x81D6D0D8u + (oidx >> 8) * 4u);
                uint32_t oent = obkt ? obkt + (oidx & 0xFFu) * 8u : 0;
                uint32_t oobj = (oent && prd2(oent) == otag) ? prd2(oent + 4u) : 0;
                std::string cls;
                for (uint32_t d = oobj ? prd2(oobj) : 0, k = 0; d && k < 6; ++k) {
                  cls += fmt::format("{:08X}'{}' -> ", d, nm(prd2(d + 4u)));
                  d = prd2(d + 8u);
                }
                XELOGI("SceneClass: handle {:08X} obj={:08X} [obj+0]={:08X} "
                       "chain: {}end",
                       hp, oobj, oobj ? prd2(oobj) : 0, cls);
              }
              // Distinguishing test (phase 468): is +0x18 a type-id field that
              // these objects fill wrongly, or are these objects simply not
              // class descriptors? Dump the two type ids themselves - if a
              // real descriptor has a different shape from a widget, the
              // second reading holds.
              static bool td_once = false;
              if (!td_once) {
                td_once = true;
                for (uint32_t td : {typid, typid2}) {
                  std::string wds;
                  for (uint32_t q = 0; q < 8; ++q) {
                    wds += fmt::format("+{:X}:{:08X} ", q * 4, prd2(td + q * 4u));
                  }
                  XELOGI("TypeDesc: {:08X} {}", td, wds);
                }
                std::string ww;
                for (uint32_t q = 0; q < 8; ++q) {
                  ww += fmt::format("+{:X}:{:08X} ", q * 4, prd2(vo + q * 4u));
                }
                XELOGI("WidgetObj: {:08X} {}", vo, ww);
                // Does ANY widget in this scene have +0x18 pointing into the
                // descriptor range (4088xxxx)? If some do and some do not,
                // construction is incomplete for a subset; if none do, +0x18
                // is not a type field on widgets and the cast is being handed
                // the wrong kind of object entirely (phase 469's open pair).
                std::string survey;
                for (uint32_t h = 0x00010040u; h <= 0x00010060u; ++h) {
                  uint32_t o = pcall(0x81931040u, {h});
                  if (!o) continue;
                  uint32_t t = prd2(o + 0x18u);
                  survey += fmt::format("{:04X}->{:08X}[+18={:08X}{}] ",
                                        h & 0xFFFF, o, t,
                                        (t >= 0x40880000u && t < 0x40890000u)
                                            ? " DESC" : "");
                }
                XELOGI("TypeSurvey: {}", survey);
                // Which objects in this scene ARE of the class the second
                // validation demands? Scan the whole table (bound 0x400),
                // resolving entries the decoded way and walking each chain.
                {
                  auto rok = [](uint32_t a) {
                    return a >= 0x10000000u && a < 0xA0000000u;
                  };
                  uint32_t live = 0, hits = 0;
                  std::string found;
                  for (uint32_t i = 0; i < 0x400u; ++i) {
                    uint32_t bs = 0x81D6D0D8u + (i >> 8) * 4u;
                    uint32_t bk = prd2(bs);
                    if (!rok(bk)) continue;
                    uint32_t en = bk + (i & 0xFFu) * 8u;
                    if (!rok(en)) continue;
                    uint32_t o = prd2(en + 4u);
                    if (!rok(o)) continue;
                    ++live;
                    for (uint32_t nd = o, k = 0; rok(nd) && k < 10; ++k) {
                      if (prd2(nd + 0x18u) == typid2) {
                        ++hits;
                        if (hits <= 6) {
                          found += fmt::format("idx{:03X}:obj{:08X} ", i, o);
                        }
                        break;
                      }
                      nd = prd2(nd + 8u);
                    }
                  }
                  XELOGI("ClassScan: want={:08X} live={} matching={} {}",
                         typid2, live, hits, found);
                  // Nothing in the scene is of that class, so name it: the
                  // descriptors' +4/+8 look like name/base pointers (the
                  // widget type 40881270 has the same shape). Read both as
                  // ASCII and as UTF-16 - XUI class names came through as
                  // UTF-16 elsewhere in this file.
                  // These are UTF-16BE: an ASCII char reads as 00 xx, so a
                  // byte-wise reader stops on the very first character.
                  auto rdstr = [&](uint32_t a) {
                    if (!rok(a)) return std::string("<unreadable>");
                    std::string o;
                    for (uint32_t w = 0; w < 12; ++w) {
                      uint32_t v = prd2(a + w * 4u);
                      for (int h = 1; h >= 0; --h) {
                        uint16_t u = uint16_t((v >> (h * 16)) & 0xFFFFu);
                        if (!u) return o.empty() ? std::string("<empty>") : o;
                        o += (u >= 32 && u < 127) ? char(u) : '.';
                      }
                    }
                    return o;
                  };
                  for (uint32_t td : {typid, typid2}) {
                    XELOGI("ClassName: desc={:08X} +4={:08X} '{}' +8={:08X} '{}'",
                           td, prd2(td + 4u), rdstr(prd2(td + 4u)),
                           prd2(td + 8u), rdstr(prd2(td + 8u)));
                  }
                }
                // 81931040 returns [tail-of-+8-chain + 0x20], NOT the object
                // (phase 471), so every "obj" dumped since phase 460 was the
                // wrong pointer. Resolve entry[1] the way 81943378 does:
                // bucket = [base + (idx>>6)*4], entry = bucket + (idx&3F)*8.
                // entry[0] must equal the handle's high half - that tag check
                // is what tells us the model is right.
                // Guard every read: an unguarded prd2 on a garbage bucket
                // faulted the host at guest 0x11B and perturbed the run.
                auto ok = [](uint32_t a) {
                  return a >= 0x10000000u && a < 0xA0000000u;
                };
                for (uint32_t h : {hp, vis90}) {
                  if (!h) continue;
                  uint32_t idx = h & 0xFFFFu, tag = h >> 16;
                  // Decoded, not guessed: rlwinm r9,r10,26,6,29 masks with
                  // 0x03FFFFFC, giving (idx>>8)<<2 - the bucket is idx>>8, not
                  // idx>>6. rlwinm r11,r10,3,21,28 masks with 0x7F8, so the
                  // entry offset is (idx & 0xFF)*8: 256 entries of 8 bytes per
                  // bucket, which matches the 0x810 bucket spacing measured.
                  uint32_t bslot = 0x81D6D0D8u + (idx >> 8) * 4u;
                  uint32_t bucket = ok(bslot) ? prd2(bslot) : 0;
                  uint32_t ent = bucket + (idx & 0xFFu) * 8u;
                  if (!ok(bucket) || !ok(ent)) {
                    XELOGI("Entry: h={:08X} idx={:04X} bslot={:08X} "
                           "bucket={:08X} - not a readable bucket",
                           h, idx, bslot, bucket);
                    continue;
                  }
                  uint32_t e0 = prd2(ent), e1 = prd2(ent + 4u);
                  XELOGI("Entry: h={:08X} idx={:04X} bucket={:08X} entry={:08X} "
                         "tag={:04X}/{:04X}{} obj={:08X} [+8]={:08X} "
                         "[+18]={:08X}",
                         h, idx, bucket, ent, e0, tag,
                         (e0 == tag) ? " OK" : " MISMATCH", e1,
                         ok(e1) ? prd2(e1 + 8u) : 0,
                         ok(e1) ? prd2(e1 + 0x18u) : 0);
                  // Walk exactly as 8193F1B8 does: objects via +8, comparing
                  // each [+0x18] against the required type. Does 40881D10
                  // appear anywhere in the chain, or is the visual simply a
                  // different class?
                  std::string w;
                  bool found = false;
                  for (uint32_t nd = e1, k = 0; ok(nd) && k < 10; ++k) {
                    uint32_t ty = prd2(nd + 0x18u);
                    if (ty == typid2) found = true;
                    w += fmt::format("{:08X}[t={:08X}]{} -> ", nd, ty,
                                     (ty == typid2) ? "*MATCH*" : "");
                    nd = prd2(nd + 8u);
                  }
                  XELOGI("TypeWalk: from={:08X} want={:08X} {}end | {}",
                         e1, typid2, w, found ? "REACHES IT" : "never reaches it");
                }
              }
            }
            if (vh2) {
              uint32_t oi2 = pcall(0x81931040u, {hp});
              if (oi2) {
                // Log each element as it is painted, with the fields that
                // differ between a scene that emits state and one that emits
                // nothing: the visibility flags, the opacity, the bounds, and
                // how many words this particular paint produced.
                // Paint TWICE and report both. The first paint after a
                // state invalidation carries one-time pipeline setup - shader
                // upload and a register block - and whichever element runs
                // first pays for it. Phase 217 showed that is the whole
                // difference between "2881 words" and "0 words" on two
                // otherwise identical scene roots. The second figure is the
                // element's own contribution.
                // Dirty the element through the API before painting it.
                //
                // Bit 17 of [obj+0xB4] gates 8195E190 inside 81963608 and is
                // transient - phase 229 forced it and found it cleared again
                // before the paint read it. It is set when xam dirties an
                // element, so the way to raise it is to change a property, not
                // to poke the bit. XuiElementSetBounds takes its width and
                // height as floats in f1/f2 (phase 195) and is known to write
                // through to the object.
                if (::cvars::guide_dirty_via_api && f_bounds_api) {
                  uint32_t bw2 = prd2(oi2 + 0x1Cu), bh2 = prd2(oi2 + 0x20u);
                  float cw, ch;
                  std::memcpy(&cw, &bw2, 4);
                  std::memcpy(&ch, &bh2, 4);
                  auto* cx2 = gth->thread_state()->context();
                  // Pass a CHANGED value. Setting the current bounds back
                  // lets xam short-circuit on "no change" and dirty nothing,
                  // which would make this test unable to distinguish "dirtying
                  // does not help" from "nothing was dirtied". One pixel wider
                  // is a real change and visually negligible.
                  cx2->f[1] = double(cw + 1.0f);
                  cx2->f[2] = double(ch);
                  uint64_t sb5[] = {hp};
                  prc->Execute(tsp, f_bounds_api, sb5, xe::countof(sb5));
                }
                // Set bit 17 IMMEDIATELY before this element's paint.
                //
                // It gates 8195E190 inside 81963608 - the element's hand-off
                // to the device context - and painting clears it (phase 233).
                // Setting it in a separate walk is useless because the descent
                // paints elements before the loop reaches them, clearing it
                // first. Here it cannot be cleared before 81963608 reads it.
                if (::cvars::guide_force_flag17) {
                  uint32_t fl17 = prd2(oi2 + 0xB4u);
                  xe::store_and_swap<uint32_t>(
                      pm2->TranslateVirtual(oi2 + 0xB4u), fl17 | 0x20000u);
                }
                // The emit happens inside 81968890, and the packet emitter
                // traps unless a command block is open (phase 483: 81A01358
                // begins, 81A01490 ends by zeroing [dev+0x2B4C]). Phase 484
                // re-opened it before the composite draw, which this
                // configuration never runs - the failing emit is here, on the
                // paint path. Re-open when the cursor is cold.
                {
                  // The re-open produced no log at all last run, which means
                  // skipped, not failed. Report the guard's inputs once so the
                  // blocking term is visible instead of inferred.
                  static bool gonce = false;
                  if (!gonce) {
                    gonce = true;
                    XELOGI("PaintReopenGuard: resv_dev={:08X} cur={:08X} "
                           "cmdbuf_base={:08X} cmdbuf_size={}",
                           guide_resv_dev_,
                           guide_resv_dev_ ? prd2(guide_resv_dev_ + 0x2B4Cu) : 0,
                           guide_cmdbuf_base_, guide_cmdbuf_size_);
                  }
                }
                // guide_resv_dev_ is 0 here - it is assigned in a block this
                // configuration does not run. Derive the device the same way
                // the draw-entry probe does (dc -> [dc+0x1CC] -> [wrap+0x0C]),
                // which yielded a real device (40870D00) in the same runs.
                uint32_t pdc = prd2(guide_draw_this_ + 12u);
                uint32_t pwr = pdc ? prd2(pdc + 0x1CCu) : 0;
                uint32_t pdev = pwr ? prd2(pwr + 0x0Cu) : 0;
                // 81A042E0 SETS [dev+0x2B4C] itself and expects it zero on
                // entry (81A0439C: lwz / beq-skip-trap), then bails to 81A043FC
                // when [dev+0x30] is zero. Re-opening the block beforehand
                // leaves 2B4C non-zero and may be what breaks the reserve in
                // place while a direct call succeeds. Gate it so the default
                // run tests without it.
                if (::cvars::guide_call_present_bracket && pdev &&
                    !prd2(pdev + 0x2B4Cu) &&
                    guide_cmdbuf_base_ && guide_cmdbuf_size_) {
                  uint32_t pr = pcall(GuideConst(0x81A01358u),
                                      {pdev, guide_cmdbuf_base_,
                                       guide_cmdbuf_size_ / 4u});
                  static uint32_t rop = 0;
                  if (rop++ < 3) {
                    XELOGI("PaintReopen: 81A01358(dev {:08X}, {:08X}, {}) -> "
                           "{:08X} | cur now {:08X}",
                           pdev, guide_cmdbuf_base_,
                           guide_cmdbuf_size_ / 4u, pr,
                           prd2(pdev + 0x2B4Cu));
                  }
                }
                if (::cvars::guide_call_present_bracket && pdev) {
                  uint32_t bres = pcall(GuideConst(0x819FEB78u), {pdev});
                  static uint32_t bl2 = 0;
                  if (bl2++ < 3) {
                    // 81A042E0(dev, words) is what produces the cursor the
                    // emitter uses, and it reserves from [dev+0x30]..[dev+0x34]
                    // - not the 0x2B4C block. It needs count*4 bytes to fit.
                    uint32_t rc = prd2(pdev + 0x30u), re = prd2(pdev + 0x34u);
                    XELOGI("Reserve: cur[30]={:08X} end[34]={:08X} window={} "
                           "bytes; 0x905 words needs {} -> {}",
                           rc, re, (re > rc) ? (re - rc) : 0, 0x905u * 4u,
                           (re > rc && (re - rc) >= 0x905u * 4u) ? "FITS"
                                                                 : "TOO SMALL");
                  }
                }
                // 81A042E0(dev, words) reserves from [dev+0x30]..[dev+0x34]
                // and returns 0 when it will not fit - that zero is the cursor
                // the emitter then dereferences. The existing widening ran on a
                // different device instance (3009D000..3011D000) than the paint
                // uses, leaving this one at 3728 bytes against 9236 needed.
                // Widen the device actually in play.
                {
                  // Log unconditionally: the previous "too small" reading came
                  // from a run with the bracket flag set, so the window may
                  // differ here. An absent line would otherwise be ambiguous
                  // between "did not fire" and "did not need to".
                  static uint32_t wdiag = 0;
                  if (wdiag++ < 3) {
                    uint32_t dc0 = pdev ? prd2(pdev + 0x30u) : 0;
                    uint32_t de0 = pdev ? prd2(pdev + 0x34u) : 0;
                    // 81A0F1C8 is a teardown: it nulls the RT slots, then
                    // releases each default surface and zeroes [3F74]/[3F78]/
                    // [3F70]. Those three fields are its signature - if they
                    // read zero here, having been bound at the draw hook, the
                    // teardown ran in between.
                    XELOGI("ReserveState: pdev={:08X} cur={:08X} end={:08X} "
                           "window={} need={} base={:08X} size={} | "
                           "[3F74]={:08X} [3F78]={:08X} [3F70]={:08X} "
                           "[32A0]={:08X}",
                           pdev, dc0, de0, (de0 > dc0) ? (de0 - dc0) : 0,
                           0x905u * 4u, guide_cmdbuf_base_, guide_cmdbuf_size_,
                           prd2(pdev + 0x3F74u), prd2(pdev + 0x3F78u),
                           prd2(pdev + 0x3F70u), prd2(pdev + 0x32A0u));
                    // The window fits and the cursor never advances, so ask the
                    // reserve itself: 81A042E0(dev, words) is what hands the
                    // emitter its cursor, and a zero from here is the whole
                    // failure. Same args the caller uses (0x905).
                    // Does the dispatch use a different device? Scan the
                    // element object for pointers whose [+0x30]/[+0x34] form a
                    // reserve window like pdev's - another device would have
                    // its own, and its [+0x30] may be the zero that fails.
                    {
                      std::string cand;
                      for (uint32_t w = 0; w < 48; ++w) {
                        uint32_t v = prd2(oi2 + w * 4u);
                        // A bare 0x4xxxxxxx range test is not a mapped test:
                        // candidate 44550000 faulted the host at +0x30. Devices
                        // live near pdev, so bound the scan to its neighbourhood
                        // rather than the whole range.
                        if (!pdev) continue;
                        uint32_t lo = (pdev > 0x100000u) ? pdev - 0x100000u : 0;
                        if (v < lo || v > pdev + 0x100000u) continue;
                        uint32_t c0 = prd2(v + 0x30u), c1 = prd2(v + 0x34u);
                        if (c1 > c0 && (c1 - c0) < 0x1000000u) {
                          cand += fmt::format("+{:X}:{:08X}[30={:08X} 34={:08X}] ",
                                              w * 4, v, c0, c1);
                        }
                      }
                      XELOGI("DevCandidates: obj={:08X} pdev={:08X} | {}",
                             oi2, pdev, cand.empty() ? "none" : cand);
                      // The dispatch may take its device from a global rather
                      // than the element. XamDeviceSlot is exactly such a
                      // global and is already resolved; if it names a different
                      // device, that device's window is what the reserve sees.
                      uint32_t gslot = XamDeviceSlot();
                      uint32_t gdev = gslot ? prd2(gslot) : 0;
                      if (gdev) {
                        uint32_t g0 = prd2(gdev + 0x30u), g1 = prd2(gdev + 0x34u);
                        XELOGI("GlobalDev: slot={:08X} dev={:08X} cur={:08X} "
                               "end={:08X} window={} {}",
                               gslot, gdev, g0, g1, (g1 > g0) ? (g1 - g0) : 0,
                               (gdev == pdev) ? "== pdev" : "DIFFERENT from pdev");
                        // The dispatch reserves from THIS device, not from the
                        // DC's. Its window is 4676 bytes against 9236 needed -
                        // the same 4676 phase 439 measured - so 81A042E0 fails
                        // here and returns the zero the emitter dereferences.
                        // Every widening so far went to pdev, which the paint
                        // never touches.
                        if (guide_cmdbuf_base_ && guide_cmdbuf_size_ &&
                            (g1 <= g0 || (g1 - g0) < 0x905u * 4u)) {
                          auto* gm = kernel_state()->memory();
                          xe::store_and_swap<uint32_t>(
                              gm->TranslateVirtual(gdev + 0x30u),
                              guide_cmdbuf_base_);
                          xe::store_and_swap<uint32_t>(
                              gm->TranslateVirtual(gdev + 0x34u),
                              guide_cmdbuf_base_ + guide_cmdbuf_size_);
                          XELOGI("GlobalDevWiden: dev={:08X} was {} -> cur={:08X}"
                                 " end={:08X} ({} bytes)",
                                 gdev, (g1 > g0) ? (g1 - g0) : 0,
                                 prd2(gdev + 0x30u), prd2(gdev + 0x34u),
                                 guide_cmdbuf_size_);
                        }
                      }
                    }
                    // 81A0A668(dev, x) does the reserve AND the emit - it is
                    // the frame directly above the emitter. If it succeeds when
                    // called here, the failure is in how the dispatch reaches
                    // it, not in the function; if it fails the same way, the
                    // failure is reproducible without the dispatch and can be
                    // studied directly. Cheaper than hooking the guest.
                    if (pdev) {
                      uint32_t c_before = prd2(pdev + 0x30u);
                      uint32_t ar = pcall(GuideConst(0x81A0A668u), {pdev, 0u});
                      XELOGI("EmitFrame: 81A0A668(dev {:08X}, 0) -> {:08X} | "
                             "cur {:08X} -> {:08X} (delta {})",
                             pdev, ar, c_before, prd2(pdev + 0x30u),
                             prd2(pdev + 0x30u) - c_before);
                      // Words are landing in our buffer for the first time.
                      // Walk them as PM4: type-3 headers carry the opcode in
                      // bits 8..14 and a count in 16..29. DRAW_INDX is 0x22.
                      uint32_t wa = c_before, wend = prd2(pdev + 0x30u);
                      if (wend > wa && (wend - wa) < 0x40000u) {
                        uint32_t nw = (wend - wa) / 4u, t3 = 0, draws = 0;
                        std::string first;
                        for (uint32_t i = 0; i < nw; ) {
                          uint32_t hdr = prd2(wa + i * 4u);
                          uint32_t ty = hdr >> 30;
                          if (ty == 3) {
                            uint32_t op = (hdr >> 8) & 0x7Fu;
                            uint32_t cnt = ((hdr >> 16) & 0x3FFFu) + 1u;
                            ++t3;
                            if (op == 0x22u || op == 0x36u) ++draws;
                            if (t3 <= 8) first += fmt::format("{:02X}x{} ", op, cnt);
                            i += cnt + 1u;
                          } else {
                            ++i;
                          }
                        }
                        XELOGI("EmitWalk: {} words, {} type3 packets, DRAW_INDX={} "
                               "| first: {}",
                               nw, t3, draws, first);
                      }
                    }
                    if (pdev) {
                      uint32_t rr = pcall(GuideConst(0x81A042E0u), {pdev, 0x905u});
                      XELOGI("ReserveCall: 81A042E0(dev {:08X}, 0x905) -> {:08X}"
                             " | cur now {:08X}",
                             pdev, rr, prd2(pdev + 0x30u));
                    }
                  }
                }
                if (pdev && guide_cmdbuf_base_ && guide_cmdbuf_size_) {
                  uint32_t wc = prd2(pdev + 0x30u), we = prd2(pdev + 0x34u);
                  if (we <= wc || (we - wc) < 0x905u * 4u) {
                    auto* wm = kernel_state()->memory();
                    xe::store_and_swap<uint32_t>(
                        wm->TranslateVirtual(pdev + 0x30u), guide_cmdbuf_base_);
                    xe::store_and_swap<uint32_t>(
                        wm->TranslateVirtual(pdev + 0x34u),
                        guide_cmdbuf_base_ + guide_cmdbuf_size_);
                    static uint32_t wl = 0;
                    if (wl++ < 3) {
                      XELOGI("ReserveWiden: dev={:08X} was {} bytes -> "
                             "cur[30]={:08X} end[34]={:08X} ({} bytes)",
                             pdev, (we > wc) ? (we - wc) : 0,
                             prd2(pdev + 0x30u), prd2(pdev + 0x34u),
                             guide_cmdbuf_size_);
                    }
                  }
                }
                // Phase 513: [dev+0x30] is the RESERVE cursor. Writing into an
                // already-reserved block advances the block cursor [2B4C], not
                // this one, so sampling 0x30 around the render reports zero for
                // an element that emits normally. Phase 512 concluded "the
                // element render emits nothing" off exactly that mistake - the
                // phase-504 two-buffer lesson, not applied one level down.
                // Sample both.
                uint32_t e0 = pdev ? prd2(pdev + 0x30u) : 0;
                uint32_t b0 = pdev ? prd2(pdev + 0x2B4Cu) : 0;
                uint32_t gg = pcall(0x81954468u, {oi2});
                pcall(0x81968890u, {oi2, pmsg});
                uint32_t e1 = pdev ? prd2(pdev + 0x30u) : 0;
                uint32_t b1 = pdev ? prd2(pdev + 0x2B4Cu) : 0;
                pcall(0x81968890u, {oi2, pmsg});
                uint32_t e2 = pdev ? prd2(pdev + 0x30u) : 0;
                uint32_t b2 = pdev ? prd2(pdev + 0x2B4Cu) : 0;
                static uint32_t el_logs = 0;
                if (el_logs++ < 8) {
                  uint32_t bw = prd2(oi2 + 0x1Cu), bh = prd2(oi2 + 0x20u);
                  float fbw, fbh;
                  std::memcpy(&fbw, &bw, 4);
                  std::memcpy(&fbh, &bh, 4);
                  // pdev is printed because "0 words" is exactly the shape of
                  // the phase-503 false negative: if the device is zero then
                  // e0/e1/e2 all read zero and the deltas are forced to zero no
                  // matter what the element emits. The reading is worthless
                  // without it.
                  XELOGI("GuideElem: dev={:08X} [30]={:08X} h={:08X} obj={:08X} "
                         "vis={:08X} gate={} [B4]={:08X} bounds {}x{} -> "
                         "first {} words, repeat {} words",
                         pdev, e0, hp, oi2, vh2, gg, prd2(oi2 + 0xB4u), fbw, fbh,
                         (e1 > e0) ? (e1 - e0) / 4 : 0,
                         (e2 > e1) ? (e2 - e1) / 4 : 0);
                  XELOGI("GuideElem:   block cursor {:08X}->{:08X}->{:08X} "
                         "(first {} words, repeat {} words)",
                         b0, b1, b2, (b1 > b0) ? (b1 - b0) / 4 : 0,
                         (b2 > b1) ? (b2 - b1) / 4 : 0);
                  // Execute the REPEAT range - the element's own steady-state
                  // output, with the one-time setup already paid. Two prior
                  // streams were confirmed draw-free this way; this is the
                  // first range that is actually the element's contribution
                  // rather than the pipeline's.
                  if (e2 > e1 && (e2 - e1) < 0x40000u) {
                    auto* gsy =
                        kernel_state()->emulator()->graphics_system();
                    if (gsy && gsy->command_processor()) {
                      auto* cpy = gsy->command_processor();
                      uint32_t db = cpy->guide_draw_count_;
                      cpy->ExecuteGuestBufferVirtualUnsafe(e1, (e2 - e1) / 4);
                      XELOGI("GuideElem:   repeat range {:08X} +{} words -> "
                             "GPU draws +{}",
                             e1, (e2 - e1) / 4,
                             cpy->guide_draw_count_ - db);
                    }
                  }
                }
                ++painted;
              }
            }
            std::memset(pm2->TranslateVirtual(pout), 0, 16);
            pcall(f_lc, {hp, pout});
            hp = prd2(pout);
          }
          uint32_t after = paint_dev_ ? prd2(paint_dev_ + 0x30u) : 0;
          uint32_t after_cb = paint_dev_ ? prd2(paint_dev_ + 0x2B4Cu) : 0;
          XELOGI("PaintCursors: dev={:08X} resv {:08X}->{:08X} ({} words) | "
                 "cmdblk {:08X}->{:08X} ({} words) base[2B48]={:08X} "
                 "limit[2B50]={:08X}",
                 paint_dev_, before, after,
                 (after > before) ? (after - before) / 4 : 0, before_cb,
                 after_cb, (after_cb > before_cb) ? (after_cb - before_cb) / 4 : 0,
                 paint_dev_ ? prd2(paint_dev_ + 0x2B48u) : 0,
                 paint_dev_ ? prd2(paint_dev_ + 0x2B50u) : 0);
          // Segment the rest of the frame too. The paints emit state and no
          // draws, twice confirmed by executing their ranges - so if XUI
          // batches geometry and submits it when the frame closes, the draws
          // would appear across RenderEnd or Present, neither of which has
          // ever been measured.
          pcall(f_e, {dc2});
          uint32_t afterEnd = guide_resv_dev_
                                  ? prd2(guide_resv_dev_ + 0x30u) : 0;
          uint32_t pres = pcall(f_p, {dc2, 0, 0, 0});
          uint32_t afterPresent = guide_resv_dev_
                                      ? prd2(guide_resv_dev_ + 0x30u) : 0;
          {
            static uint32_t seg_logs = 0;
            if (seg_logs++ < 4) {
              XELOGI("GuideFrameSeg: paint {:08X}->{:08X} ({} w) | "
                     "RenderEnd ->{:08X} ({} w) | Present ->{:08X} ({} w)",
                     before, after, (after > before) ? (after - before) / 4 : 0,
                     afterEnd,
                     (afterEnd > after) ? (afterEnd - after) / 4 : 0,
                     afterPresent,
                     (afterPresent > afterEnd) ? (afterPresent - afterEnd) / 4
                                               : 0);
            }
          }
          // Hand the range the paint just wrote to the swap-time executor.
          // guide_overlay_at_swap has existed since phase 173 and has never
          // had a stream worth running; guide_paint_frame produces one. The
          // range must come from the paint itself, not from the whole frame.
          // Phase 368 reported this flag "logs as false despite being passed
          // on the command line", and the diagnostic that found it was later
          // removed, so the issue's status has been unknown ever since - while
          // sitting directly under the composition path. Report the flag and
          // the range unconditionally, once, so a null result here can be told
          // apart from the flag never arriving.
          {
            static bool once = false;
            if (!once) {
              once = true;
              XELOGI("OverlayGate: guide_overlay_at_swap={} range {:08X}->{:08X}"
                     " ({} words) -> {}",
                     ::cvars::guide_overlay_at_swap ? "true" : "FALSE", before,
                     after, (after > before) ? (after - before) / 4 : 0,
                     (::cvars::guide_overlay_at_swap && after > before &&
                      (after - before) < 0x40000u)
                         ? "executing"
                         : "skipped");
            }
          }
          if (::cvars::guide_overlay_at_swap && after > before &&
              (after - before) < 0x40000u) {
            auto* gso2 = kernel_state()->emulator()->graphics_system();
            if (gso2 && gso2->command_processor()) {
              // Publishing to the swap handler does not work: only ONE
              // XE_SWAP packet is seen in a whole run, so that hook is on a
              // path the title almost never takes. Execute the range here
              // instead. Not thread safe - it drives the command processor
              // from the title thread - but it is the same path the phase 171
              // probes used and it is the only way to find out whether this
              // geometry reaches the screen.
              uint32_t words = (after - before) / 4u;
              // Walk the range as PM4 before executing it. "0 GPU draws" from
              // the executor could mean the stream has no draws or that the
              // executor mis-parsed it; decoding the packets distinguishes
              // those, and says what the paint actually produced.
              // Phase 507: set when the PM4 walk desyncs, so the range is not
              // handed to the command processor. See the guard below.
              uint32_t parse_bad = 0;
              // Phase 514: every walk since phase 504 began on word 0, which
              // the sentinel proved is never written - it is stale memory that
              // decodes as a type-0 header claiming 721 registers, desyncing
              // the parse immediately. With the sentinel on, skip the words the
              // paint did not write and start where it actually did.
              uint32_t skip = 0;
              while (skip < words &&
                     prd2(before + skip * 4u) == 0xDEADBEEFu) {
                ++skip;
              }
              if (skip) {
                XELOGI("GuidePaintWalk: skipping {} unwritten word(s) at {:08X}",
                       skip, before);
              }
              {
                uint32_t counts[128] = {0};
                uint32_t t0 = 0, t2 = 0, pk = 0, bad = 0, iw = skip;
                std::string first;
                while (iw < words) {
                  uint32_t wd = prd2(before + iw * 4);
                  uint32_t ty = wd >> 30;
                  uint32_t cn = ((wd >> 16) & 0x3FFF) + 1;
                  if (ty == 3) {
                    uint32_t op = (wd >> 8) & 0x7F;
                    counts[op]++;
                    ++pk;
                    if (pk <= 20) first += fmt::format("{:02X}x{} ", op, cn);
                    iw += 1 + cn;
                  } else if (ty == 0) {
                    ++t0; iw += 1 + cn;
                  } else if (ty == 2) {
                    ++t2; ++iw;
                  } else { iw += 2; }
                  if (iw > words) { ++bad; break; }
                }
                std::string hist;
                for (uint32_t o = 0; o < 128; ++o)
                  if (counts[o]) hist += fmt::format("{:02X}:{} ", o, counts[o]);
                static uint32_t wlog = 0;
                if (wlog++ < 2) {
                  // 688 identical type-3 headers with overrun=1 reads more like
                  // a fill pattern or a desynced parse than like geometry. Dump
                  // the raw words so the two can be told apart.
                  std::string hx;
                  for (uint32_t k = 0; k < 48 && k < words; ++k)
                    hx += fmt::format("{:08X} ", prd2(before + k * 4));
                  XELOGI("GuidePaintWalk: raw {}", hx);
                  // The first dump only covered LUT indices 1-4, the dark end
                  // of the ramp, where a zero colour is exactly what a normal
                  // ramp holds - so "ramp to black" cannot be concluded from
                  // it. Sample the middle and the top before believing that.
                  for (uint32_t base_w : {words / 2, words > 60 ? words - 60 : 0u}) {
                    std::string mid;
                    for (uint32_t k = 0; k < 27 && base_w + k < words; ++k)
                      mid += fmt::format("{:08X} ", prd2(before + (base_w + k) * 4));
                    XELOGI("GuidePaintWalk: at word {} {}", base_w, mid);
                  }
                  // Phase 511: static hunting for "the draw emitter" has now
                  // mis-read constants twice - [sp+0x1CC] is an index-offset
                  // argument, not a draw gate, and the li r5, 0x22 / 0x36 sites
                  // are register indices, not PM4 opcodes. Measure instead:
                  // histogram everything in the range that looks like a GPU
                  // register index, so "the paint only touches display LUT
                  // registers" is a finding rather than an inference from a
                  // 48-word window.
                  {
                    uint32_t dc = 0, gfx = 0, other = 0;
                    uint32_t lo_gfx = 0xFFFFFFFFu, hi_gfx = 0;
                    for (uint32_t k = 0; k < words; ++k) {
                      uint32_t v = prd2(before + k * 4);
                      if (v >= 0x1900u && v <= 0x19FFu) ++dc;
                      else if (v >= 0x2000u && v <= 0x2FFFu) {
                        ++gfx;
                        if (v < lo_gfx) lo_gfx = v;
                        if (v > hi_gfx) hi_gfx = v;
                      } else if (v >= 0x1000u && v <= 0x4FFFu) ++other;
                    }
                    XELOGI("GuidePaintRegs: {} words | DC_LUT-range={} "
                           "gfx-range={} (lo={:04X} hi={:04X}) other={}",
                           words, dc, gfx,
                           gfx ? lo_gfx : 0, hi_gfx, other);
                  }
                  XELOGI("GuidePaintWalk: {} words -> {} type3, {} type0, "
                         "{} type2, overrun={}",
                         words, pk, t0, t2, bad);
                  XELOGI("GuidePaintWalk: opcodes {}", hist);
                  XELOGI("GuidePaintWalk: first {}", first);
                  XELOGI("GuidePaintWalk: DRAW_INDX(22)={} DRAW_INDX_2(36)={}",
                         counts[0x22], counts[0x36]);
                }
                parse_bad = bad;
              }
              // The reserve range is NOT a command stream - phases 505/506 show
              // it is a 769-entry gamma ramp - and executing it anyway is not
              // merely useless. Xenia implements DC_LUT_*: command_processor.cc
              // keeps a gamma_ramp_256_entry_table_ and handles
              // XE_GPU_REG_DC_LUT_RW_INDEX. Parsed as packets, an all-zero
              // 256-entry ramp reaching the register path blacks the display on
              // its own, independently of the missing geometry. Refuse to
              // execute a range whose parse overran.
              if (parse_bad) {
                static uint32_t skip_logs = 0;
                if (skip_logs++ < 2) {
                  XELOGW("GuidePaintFrame: refusing to execute {} words at "
                         "{:08X} - PM4 parse desynced, this range is not a "
                         "command stream",
                         words, before);
                }
              } else {
              gso2->command_processor()->guide_overlay_words_ = words;
              gso2->command_processor()->guide_overlay_ptr_ = before;
              if (::cvars::guide_execute_command_stream) {
                uint32_t d0 = gso2->command_processor()->guide_draw_count_;
                gso2->command_processor()->ExecuteGuestBufferVirtualUnsafe(
                    before, words);
                static uint32_t ex_logs = 0;
                if (ex_logs++ < 4) {
                  XELOGI("GuidePaintFrame: executed {} words at {:08X}; "
                         "GPU draws +{}",
                         words, before,
                         gso2->command_processor()->guide_draw_count_ - d0);
                }
              }
              }
            }
          }
          static uint32_t pf_logs = 0;
          if (pf_logs++ < 4) {
            XELOGI("GuidePaintFrame: painted {} elements; present={:08X}; "
                   "paint wrote {:08X}->{:08X} ({} words)",
                   painted, pres, before, after,
                   (after > before) ? (after - before) / 4 : 0);
          }
        }
      }
      // Sample the reservation window IMMEDIATELY before the draw.
      //
      // CmdbufAtDraw samples at hook entry, before guide_bind_cmdbuf_kb
      // widens it, so its 4676-byte reading says nothing about the state the
      // draw actually sees. The reservation that fails asks for 2309 words
      // (9236 bytes); whether the widened 512KB window is still in place at
      // this point decides whether the re-widen is being overwritten or simply
      // runs too early.
      // Re-widen the window HERE, immediately before the draw.
      //
      // guide_bind_cmdbuf_kb widens it early in the hook and the measurement
      // above shows only 3728 bytes survive to this point - the guest calls
      // made in between (surface creation, render-target binding) restore the
      // device's own narrow window. The reservation needs 9236 bytes, so it
      // fails, returns a null cursor, and the emitter stores through it.
      // Widening at the last possible moment leaves nothing in between to
      // undo it.
      if (::cvars::guide_widen_at_draw && guide_resv_dev_ &&
          guide_cmdbuf_base_ && guide_cmdbuf_size_) {
        auto* ww = kernel_state()->memory();
        xe::store_and_swap<uint32_t>(
            ww->TranslateVirtual(guide_resv_dev_ + 0x30u), guide_cmdbuf_base_);
        xe::store_and_swap<uint32_t>(
            ww->TranslateVirtual(guide_resv_dev_ + 0x34u),
            guide_cmdbuf_base_ + guide_cmdbuf_size_);
        // Phase 372: [dev+16344] is the base the overflow check subtracts from
        // the cursor ((cursor - base + 4) >> 2 vs 0x100000, at 81A00774).
        // Measured it as either 0 or ABOVE the cursor, giving counts of
        // 201,496,577 and 1,073,733,145 - so the device latches
        // "command buffer overflow" (bit 0x20 of [dev+11069]) and refuses every
        // vertex allocation. Point it at our buffer so the count is real.
        xe::store_and_swap<uint32_t>(
            ww->TranslateVirtual(guide_resv_dev_ + 16344u),
            guide_cmdbuf_base_);
        // The overflow flag LATCHES: once bit 0x20 of [dev+11069] is set,
        // EndCommandBuffer fails, so the buffer is never retired and the
        // condition sustains itself. Fixing the base makes the count correct
        // (measured: words=1) but does not clear the bit, so clear it too -
        // now that the count it was derived from is no longer nonsense.
        // Phase 374: the vertex allocator's success path dereferences
        // [dev+0x2B10] (null since phase 271) at 81A019B8. The guest code that
        // would build it (81A0FE48, allocating 128 bytes) is unreachable in
        // this build - phase 273 - so supply an equivalent zeroed block.
        // Diagnostic: this is a guess at the structure, not a reconstruction.
        {
          uint32_t rb = xe::load_and_swap<uint32_t>(
              ww->TranslateVirtual(guide_resv_dev_ + 11024u));
          if (!rb) {
            uint32_t nb = kernel_state()->memory()->SystemHeapAlloc(128, 128);
            if (nb) {
              std::memset(ww->TranslateVirtual(nb), 0, 128);
              xe::store_and_swap<uint32_t>(
                  ww->TranslateVirtual(guide_resv_dev_ + 11024u), nb);
              XELOGI("RingBase: supplied [dev+2B10] = {:08X} (was null)", nb);
            }
          }
        }
        {
          uint8_t* fp = ww->TranslateVirtual<uint8_t*>(guide_resv_dev_ + 11069u);
          if (fp && (*fp & 0x20u)) {
            *fp = static_cast<uint8_t>(*fp & ~0x20u);
            static uint32_t cleared = 0;
            if (cleared++ < 3) {
              XELOGI("CmdOverflow: cleared [dev+11069] bit 0x20 (now {:02X})",
                     *fp);
            }
          }
        }
      }
      {
        static uint32_t win_logs = 0;
        if (win_logs++ < 80 && guide_resv_dev_) {
          auto* wm3 = kernel_state()->memory();
          auto w3 = [wm3](uint32_t a) {
            return xe::load_and_swap<uint32_t>(wm3->TranslateVirtual(a));
          };
          if (win_logs <= 4) {
          uint32_t c30 = w3(guide_resv_dev_ + 0x30u);
          uint32_t e34 = w3(guide_resv_dev_ + 0x34u);
          XELOGI("WindowAtDraw #{}: dev={:08X} cur[30]={:08X} end[34]={:08X} "
                 "= {} bytes (need 9236) cursor[2B4C]={:08X}",
                 win_logs, guide_resv_dev_, c30, e34,
                 (e34 > c30) ? (e34 - c30) : 0,
                 w3(guide_resv_dev_ + 0x2B4Cu));
          }
          // Phase 270 traced the flush's untaken branches back to a single
          // device field. 819FE48C loads [dev+23048] and 819FE4B0 branches on
          // it being zero; that skips the ring-buffer block which is the only
          // site that sets r28=1, and r28==0 at 819FE760 skips the call that
          // would populate the stack slot guarding the surface-format block.
          // The offsets are read off a disassembly, so measure them rather
          // than trust them.
          // Phase 274 left the live gate at 819432B0: a handle-table lookup
          // that returns 1 only if [obj+24] == r4, where r4 is the fixed
          // global [81D6CE50]. It returns 0, so the final dispatch
          // (bl 81957750 / bl 819636C8) never runs. Both sides are readable
          // from the host, so measure rather than reason: dump the expected
          // class token and scan the handle table for any object carrying it.
          // Sample the handle table late as well as early. matching=0 was only
          // ever measured on the first two draws; if shader objects appear
          // later that reading would be an artefact of when we looked.
          if (win_logs <= 2 || win_logs == 30 || win_logs == 79) {
            const uint32_t kTbl = 0x81D6D0D8u;   // 0x81D70000 - 12072
            const uint32_t kTok = 0x81D6CE50u;   // 0x81D70000 - 12720
            // Every address below comes from guest memory and can be
            // garbage. Phase 248 cost several phases by reading a guest
            // pointer without checking it; the first version of THIS block
            // repeated that exact mistake and faulted the host twice.
            // "non-null and below 4GB" is NOT a mapped-address test - the
            // first version of this probe accepted guest 0x11C00 and faulted
            // the host. Restrict to the two regions everything read here
            // actually lives in: the guest heap objects (0x4xxxxxxx) and the
            // xam image's code/data (0x81xxxxxx-0x81Exxxxx).
            auto ok = [](uint32_t a) {
              if (a & 3u) return false;
              return (a >= 0x40000000u && a < 0x50000000u) ||
                     (a >= 0x81000000u && a < 0x81E00000u);
            };
            auto rdz = [&](uint32_t a) -> uint32_t {
              return ok(a) ? w3(a) : 0u;
            };
            uint32_t expect = rdz(kTok);
            // Phase 299: XuiTabSceneGetCurrentTab (81937FB0) casts its scene
            // to the class at [81D6CE38] and returns -1 if the cast fails.
            // -1 matches no tab, so no tab ever activates. Count objects of
            // that class exactly as the shader class is counted.
            const uint32_t kTabTok = 0x81D6CE38u;
            uint32_t tab_expect = rdz(kTabTok);
            // Name the tab-scene class. If it is a base that the Guide's
            // scenes should derive from, the absence of a binding for it says
            // the inheritance chain is not being bound; if it is an unrelated
            // class, it says the Guide's scene simply is not a tab scene.
            std::string tabname;
            {
              uint32_t np = rdz(tab_expect + 4u);
              if (ok(np)) {
                for (uint32_t c = 0; c < 24; ++c) {
                  uint32_t ad = np + c * 2u;
                  uint32_t wv = rdz(ad & ~3u);
                  uint16_t ch = (ad & 2u) ? (wv & 0xFFFF) : (wv >> 16);
                  if (!ch || ch < 0x20 || ch > 0x7E) break;
                  tabname += (char)ch;
                }
              }
              uint32_t bp = rdz(tab_expect + 8u);
              std::string basename;
              if (ok(bp)) {
                for (uint32_t c = 0; c < 24; ++c) {
                  uint32_t ad = bp + c * 2u;
                  uint32_t wv = rdz(ad & ~3u);
                  uint16_t ch = (ad & 2u) ? (wv & 0xFFFF) : (wv >> 16);
                  if (!ch || ch < 0x20 || ch > 0x7E) break;
                  basename += (char)ch;
                }
              }
              // Phase 323: apply the phase 322 discriminator to the two
              // classes the surviving chain depends on. A real class has a
              // CODE pointer at +16 (the handler trampoline); an element has a
              // heap pointer there. Verify before trusting the names.
              XELOGI("ClassCheck: tab {:08X} +16={:08X} +40={:08X} {} | "
                     "shader {:08X} +16={:08X} +40={:08X} {}",
                     tab_expect, rdz(tab_expect + 16u), rdz(tab_expect + 40u),
                     (rdz(tab_expect + 16u) >= 0x81000000u &&
                      rdz(tab_expect + 16u) < 0x81E00000u) ? "CLASS" : "NOT-CLASS",
                     expect, rdz(expect + 16u), rdz(expect + 40u),
                     (rdz(expect + 16u) >= 0x81000000u &&
                      rdz(expect + 16u) < 0x81E00000u) ? "CLASS" : "NOT-CLASS");
              XELOGI("TabClass {:08X} = '{}' (base '{}')", tab_expect,
                     tabname.empty() ? "?" : tabname,
                     basename.empty() ? "?" : basename);
            }
            uint32_t tab_match = 0;
            uint32_t cap = rdz(kTbl + 1056u);
            uint32_t live = 0, match = 0;
            std::string sample;
            for (uint32_t idx = 0; idx < 256 && idx < cap && cap < 0x10000u;
                 ++idx) {
              uint32_t bucket = rdz(kTbl + ((idx >> 6) * 4u));
              if (!ok(bucket)) continue;
              uint32_t ent = bucket + ((idx * 8u) & 0x7F8u);
              uint32_t obj = rdz(ent + 4u);
              if (!ok(obj)) continue;
              ++live;
              uint32_t cls = rdz(obj + 24u);
              if (cls == expect && expect) ++match;
              if (cls == tab_expect && tab_expect) ++tab_match;
              if (live <= 6) {
                sample += fmt::format("{}:obj={:08X} cls={:08X} ", idx, obj, cls);
              }
            }
            XELOGI("HandleTable: expect[81D6CE50]={:08X} cap={} live={} "
                   "matching={} | tabclass[81D6CE38]={:08X} tabmatch={} | {}",
                   expect, cap, live, match, tab_expect, tab_match, sample);
            // Phase 275 showed the wanted class is XuiShader and none exists.
            // 62 objects across two classes is very thin for the Guide, so
            // census the scene: distinct class tokens and their names.
            // Name strings are read ONLY from the xam image range, which is
            // fully mapped - this is what makes the read safe, unlike the
            // probe in phase 275 that walked heap pointers and faulted.
            {
              uint32_t distinct[16] = {0};
              uint32_t counts[16] = {0};
              uint32_t nd = 0;
              for (uint32_t idx = 0; idx < 256 && idx < cap && cap < 0x10000u;
                   ++idx) {
                uint32_t bucket = rdz(kTbl + ((idx >> 6) * 4u));
                if (!ok(bucket)) continue;
                uint32_t obj = rdz(bucket + ((idx * 8u) & 0x7F8u) + 4u);
                if (!ok(obj)) continue;
                uint32_t cls = rdz(obj + 24u);
                if (!cls) continue;
                uint32_t k = 0;
                for (; k < nd; ++k) {
                  if (distinct[k] == cls) break;
                }
                if (k == nd && nd < 16) distinct[nd++] = cls;
                if (k < 16) ++counts[k];
              }
              std::string census;
              for (uint32_t k = 0; k < nd; ++k) {
                uint32_t np = rdz(distinct[k] + 4u);
                std::string nm;
                // The name string lives in the HEAP (np is typically cls+0x50),
                // not in the image - an image-only filter excluded exactly the
                // pointers that matter. ok() still bounds the read, and this
                // walks one pointer per class rather than scanning candidates,
                // which is what made the phase 275 probe fault.
                if (ok(np)) {
                  for (uint32_t c = 0; c < 20; ++c) {
                    uint32_t addr = np + c * 2u;
                    uint32_t wv = rdz(addr & ~3u);
                    uint16_t ch = (addr & 2u) ? (wv & 0xFFFF) : (wv >> 16);
                    if (!ch || ch < 0x20 || ch > 0x7E) break;
                    nm += (char)ch;
                  }
                }
                census += fmt::format("{}x{} ", counts[k],
                                      nm.empty() ? fmt::format("{:08X}", distinct[k]) : nm);
              }
              XELOGI("SceneCensus: {} distinct classes | {}", nd, census);
              // Phase 294: census every (class, handler) BINDING in the
              // session, not just the ones reachable from the paint chain.
              // A binding node has a class pointer at +0x18 and a code
              // pointer at +0x1C. This answers directly whether ANY
              // class-specific binding exists, rather than inferring it from
              // the 464 elements walked in phase 291.
              {
                uint32_t tabbind = 0, shdbind = 0;
                // XuiScene's class pointer, via its known handler 8193FC60.
                uint32_t scene_cls = 0;
                for (uint32_t idx = 0; idx < 1024 && idx < cap; ++idx) {
                  uint32_t bk = rdz(kTbl + ((idx >> 6) * 4u));
                  if (!ok(bk)) continue;
                  uint32_t ob = rdz(bk + ((idx * 8u) & 0x7F8u) + 4u);
                  if (!ok(ob)) continue;
                  if (rdz(ob + 0x1Cu) == 0x8193FC60u) {
                    scene_cls = rdz(ob + 0x18u);
                    break;
                  }
                }
                uint32_t scenes_shown = 0;
                std::string scene_list;
                std::map<uint32_t, uint32_t> binds;   // handler -> count
                std::map<uint32_t, uint32_t> bcls;    // handler -> a class
                uint32_t nodes = 0;
                for (uint32_t idx = 0; idx < 1024 && idx < cap; ++idx) {
                  uint32_t bucket = rdz(kTbl + ((idx >> 6) * 4u));
                  if (!ok(bucket)) continue;
                  uint32_t obj = rdz(bucket + ((idx * 8u) & 0x7F8u) + 4u);
                  if (!ok(obj)) continue;
                  uint32_t h = rdz(obj + 0x1Cu);
                  if (h < 0x81930000u || h >= 0x819A0000u) continue;
                  ++nodes;
                  binds[h]++;
                  bcls[h] = rdz(obj + 0x18u);
                  // The cast 8193F1B8 walks this chain comparing [node+0x18]
                  // against the wanted class, so a BINDING carrying the class
                  // is what makes a cast succeed - not an object whose own
                  // [obj+24] equals it. Count bindings for the tab-scene and
                  // shader classes on the same basis.
                  // Phase 306: list the XuiScene-bound objects. A binding
                  // node holds the element it binds at +0x20 and its own
                  // handle at +0. If one of these is GuideMain's scene, the
                  // draw root should have been it.
                  if (rdz(obj + 0x18u) == scene_cls && scene_cls &&
                      scenes_shown < 8) {
                    ++scenes_shown;
                    scene_list += fmt::format(
                        "[node={:08X} h={:08X} elem={:08X} sc={:08X}] ",
                        obj, rdz(obj), rdz(obj + 0x20u), rdz(obj + 0x24u));
                  }
                  if (rdz(obj + 0x18u) == 0x40888E80u) ++tabbind;
                  if (rdz(obj + 0x18u) == 0x408891D0u) ++shdbind;
                }
                std::string bs;
                for (auto& kv : binds) {
                  uint32_t cp = bcls[kv.first];
                  uint32_t np = rdz(cp + 4u);
                  std::string nm;
                  if (ok(np)) {
                    for (uint32_t c = 0; c < 20; ++c) {
                      uint32_t ad = np + c * 2u;
                      uint32_t wv = rdz(ad & ~3u);
                      uint16_t ch = (ad & 2u) ? (wv & 0xFFFF) : (wv >> 16);
                      if (!ch || ch < 0x20 || ch > 0x7E) break;
                      nm += (char)ch;
                    }
                  }
                  bs += fmt::format("{:08X}->{}x{} ", kv.first,
                                    nm.empty() ? fmt::format("{:08X}", cp) : nm,
                                    kv.second);
                }
                XELOGI("BindCensus: {} binding nodes, {} distinct handlers | "
                       "tabclass-bindings={} shaderclass-bindings={} | {}",
                       nodes, binds.size(), tabbind, shdbind, bs);
              // Phase 319 control: for each distinct bound class, report its
              // name, its +40 (the binder's base link) and the text at
              // [[+8]+4] (the base-name string object). If built-ins have +40
              // populated and custom classes do not, that is the split; if
              // some built-ins also have +40=0, the story is different.
              {
                std::string tbl;
                uint32_t shown3 = 0;
                for (auto& kv : bcls) {
                  uint32_t cp = kv.second;
                  if (!ok(cp) || shown3 >= 10) continue;
                  ++shown3;
                  auto txt = [&](uint32_t sp) {
                    std::string nm;
                    if (!ok(sp)) return nm;
                    for (uint32_t c = 0; c < 20; ++c) {
                      uint32_t ad = sp + c * 2u;
                      uint32_t wv = rdz(ad & ~3u);
                      uint16_t ch = (ad & 2u) ? (wv & 0xFFFF) : (wv >> 16);
                      if (!ch || ch < 0x20 || ch > 0x7E) break;
                      nm += (char)ch;
                    }
                    return nm;
                  };
                  std::string nm = txt(rdz(cp + 4u));
                  uint32_t s8 = rdz(cp + 8u);
                  std::string basetxt = ok(s8) ? txt(rdz(s8 + 4u)) : std::string();
                  tbl += fmt::format("{}[+40={:08X} base='{}'] ",
                                     nm.empty() ? fmt::format("{:08X}", cp) : nm,
                                     rdz(cp + 40u),
                                     basetxt.empty() ? "-" : basetxt);
                }
                XELOGI("ClassLinks: {}", tbl);
                // Phase 358: does ANY bound class derive from XuiShader? Walk
                // each class's +40 base chain looking for the shader class. If
                // none does, no element could ever satisfy the painter's check
                // and the design-A reading needs re-examining.
                {
                  std::string der;
                  uint32_t any = 0;
                  for (auto& kv : bcls) {
                    uint32_t cp = kv.second;
                    if (!ok(cp)) continue;
                    uint32_t c = cp, hops = 0;
                    bool found = false;
                    while (ok(c) && hops < 12) {
                      if (c == expect) { found = true; break; }
                      c = rdz(c + 40u); ++hops;
                    }
                    if (found) { ++any; der += fmt::format("{:08X} ", cp); }
                  }
                  XELOGI("ShaderDerived: {} of {} bound classes derive from "
                         "XuiShader {:08X} | {}",
                         any, bcls.size(), expect,
                         der.empty() ? std::string("(none)") : der);
                }
                // Phase 321: are the Guide's "classes" the same kind of object
                // as registered built-in classes? Registration (8194F118)
                // allocates 60 bytes and sets +44 (flags), +48 (=-1) and +52
                // (=0) before any lookup. Dump those fields for a known
                // registered class and for a Guide class and compare.
                {
                  auto dump = [&](const char* tag, uint32_t cp) {
                    if (!ok(cp)) return;
                    std::string w;
                    for (uint32_t q = 0; q < 15; ++q) {
                      w += fmt::format("+{}:{:08X} ", q * 4, rdz(cp + q * 4u));
                    }
                    XELOGI("ClassDump {}: {:08X} {}", tag, cp, w);
                  };
                  uint32_t builtin = 0, guide = 0;
                  for (auto& kv : bcls) {
                    uint32_t cp = kv.second;
                    if (!ok(cp)) continue;
                    if (!builtin) builtin = cp;
                  }
                  // the draw root's own class, resolved earlier in this block
                  if (guide_bs_scene_) {
                    uint32_t hi2 = guide_bs_scene_ & 0xFFFFu;
                    uint32_t hb3 = rdz(kTbl + ((hi2 >> 6) * 4u));
                    uint32_t rt3 = ok(hb3)
                        ? rdz(hb3 + ((hi2 * 8u) & 0x7F8u) + 4u) : 0u;
                    uint32_t nd3 = ok(rt3) ? rdz(rt3 + 12u) : 0u;
                    guide = ok(nd3) ? rdz(nd3 + 0x18u) : 0u;
                  }
                  dump("builtin", builtin);
                  dump("guide", guide);
                }
              }
              XELOGI("SceneObjects: XuiScene class {:08X} | {}", scene_cls,
                     scene_list.empty() ? std::string("(none)") : scene_list);
              // Phase 307: is the draw root a DESCENDANT of any scene object?
              // Same run, so pointer comparison is valid here (phase 306).
              if (guide_bs_scene_ && scene_cls) {
                uint32_t hidx2 = guide_bs_scene_ & 0xFFFFu;
                uint32_t hb2 = rdz(kTbl + ((hidx2 >> 6) * 4u));
                uint32_t root2 = ok(hb2)
                    ? rdz(hb2 + ((hidx2 * 8u) & 0x7F8u) + 4u) : 0u;
                std::string verdict = "no scene contains it";
                for (uint32_t idx = 0; idx < 1024 && idx < cap && root2; ++idx) {
                  uint32_t bk = rdz(kTbl + ((idx >> 6) * 4u));
                  if (!ok(bk)) continue;
                  uint32_t nd2 = rdz(bk + ((idx * 8u) & 0x7F8u) + 4u);
                  if (!ok(nd2) || rdz(nd2 + 0x18u) != scene_cls) continue;
                  uint32_t sroot = rdz(nd2 + 0x20u);
                  if (!ok(sroot)) continue;
                  // DFS over (first child +8, next sibling +16)
                  uint32_t st[256]; int sp = 0, seen = 0;
                  st[sp++] = sroot;
                  bool found = false;
                  while (sp > 0 && seen < 256) {
                    uint32_t o2 = st[--sp]; ++seen;
                    if (o2 == root2) { found = true; break; }
                    uint32_t kid = rdz(o2 + 8u), sib = rdz(o2 + 16u);
                    if (ok(sib) && sp < 255) st[sp++] = sib;
                    if (ok(kid) && sp < 255) st[sp++] = kid;
                  }
                  if (found) {
                    verdict = fmt::format(
                        "DESCENDANT of scene elem {:08X} (node h={:08X}), "
                        "found after {} nodes", sroot, rdz(nd2), seen);
                    break;
                  }
                }
                XELOGI("RootParentage: root={:08X} -> {}", root2, verdict);
                // Phase 309: how big is each scene's subtree? The draw root's
                // subtree holds 464 blade elements; if one scene has a
                // comparable tree it is the candidate the draw should use,
                // and if all are tiny the Guide's content is not under any
                // scene at all.
                {
                  std::string sizes;
                  uint32_t shown2 = 0;
                  for (uint32_t idx = 0; idx < 1024 && idx < cap && shown2 < 8;
                       ++idx) {
                    uint32_t bk = rdz(kTbl + ((idx >> 6) * 4u));
                    if (!ok(bk)) continue;
                    uint32_t nd2 = rdz(bk + ((idx * 8u) & 0x7F8u) + 4u);
                    if (!ok(nd2) || rdz(nd2 + 0x18u) != scene_cls) continue;
                    uint32_t sroot = rdz(nd2 + 0x20u);
                    if (!ok(sroot)) continue;
                    uint32_t st[512]; int sp = 0, cnt = 0;
                    st[sp++] = sroot;
                    while (sp > 0 && cnt < 512) {
                      uint32_t o2 = st[--sp]; ++cnt;
                      uint32_t kid = rdz(o2 + 8u), sib = rdz(o2 + 16u);
                      if (ok(sib) && sp < 511) st[sp++] = sib;
                      if (ok(kid) && sp < 511) st[sp++] = kid;
                    }
                    sizes += fmt::format("h={:08X}:{} ", rdz(nd2), cnt);
                    ++shown2;
                  }
                  // Control: the same DFS on the draw root must find its
                  // 464-element subtree. If it reports 1 as well, the walk or
                  // the child/sibling offsets are wrong and "all scenes are
                  // empty" would be an artefact.
                  uint32_t ctl = 0;
                  if (root2) {
                    uint32_t st[512]; int sp = 0;
                    st[sp++] = root2;
                    while (sp > 0 && ctl < 512) {
                      uint32_t o2 = st[--sp]; ++ctl;
                      uint32_t kid = rdz(o2 + 8u), sib = rdz(o2 + 16u);
                      if (ok(sib) && sp < 511) st[sp++] = sib;
                      if (ok(kid) && sp < 511) st[sp++] = kid;
                    }
                  }
                  XELOGI("SceneSizes: {} | CONTROL drawroot {:08X} subtree={}",
                         sizes, root2, ctl);
                }
              }
              // Self-test the handle resolution before trusting it. Every
              // binding node carries its OWN handle at +0, so resolving that
              // handle must yield the node's own address. If it does not, the
              // table or the index math is wrong - which would make the
              // phase 303 "the draw root is an XuiImagePresenter" reading an
              // artefact rather than a finding.
              {
                for (uint32_t idx = 0; idx < 1024 && idx < cap; ++idx) {
                  uint32_t bk = rdz(kTbl + ((idx >> 6) * 4u));
                  if (!ok(bk)) continue;
                  uint32_t ob = rdz(bk + ((idx * 8u) & 0x7F8u) + 4u);
                  if (!ok(ob)) continue;
                  uint32_t hh = rdz(ob + 0x1Cu);
                  if (hh < 0x81930000u || hh >= 0x819A0000u) continue;
                  uint32_t own = rdz(ob);            // the node's own handle
                  uint32_t oi = own & 0xFFFFu;
                  uint32_t ob2b = rdz(kTbl + ((oi >> 6) * 4u));
                  uint32_t back = ok(ob2b)
                                      ? rdz(ob2b + ((oi * 8u) & 0x7F8u) + 4u)
                                      : 0u;
                  XELOGI("HandleSelfTest: node {:08X} says handle {:08X}; "
                         "resolving it gives {:08X} -> {}",
                         ob, own, back, (back == ob) ? "OK" : "MISMATCH");
                  break;
                }
              }
              // Phase 303: what class chain does the DRAW ROOT scene have?
              // The cast in XuiTabSceneGetCurrentTab walks exactly this chain,
              // so naming it says directly whether our root is a tab scene.
              if (guide_bs_scene_) {
                // guide_bs_scene_ is a HANDLE (e.g. 00010135), not a pointer.
                // Resolve it the way the guest does: index = handle & 0xFFFF,
                // bucket = [tbl + (idx>>6)*4], entry = bucket + (idx*8 & 0x7F8),
                // object = [entry+4].
                uint32_t hidx = guide_bs_scene_ & 0xFFFFu;
                uint32_t hbucket = rdz(kTbl + ((hidx >> 6) * 4u));
                uint32_t root = ok(hbucket)
                                    ? rdz(hbucket + ((hidx * 8u) & 0x7F8u) + 4u)
                                    : 0u;
                // The guest also checks the generation at [entry+0] against
                // handle>>16 and rejects a mismatch, so verify it here rather
                // than trusting a possibly recycled slot.
                uint32_t hent = ok(hbucket)
                                    ? hbucket + ((hidx * 8u) & 0x7F8u) : 0u;
                uint32_t hgen = ok(hent) ? rdz(hent) : 0u;
                XELOGI("RootScene: handle={:08X} idx={} gen_want={} gen_slot={} "
                       "{} -> object {:08X}",
                       guide_bs_scene_, hidx, guide_bs_scene_ >> 16, hgen,
                       (hgen == (guide_bs_scene_ >> 16)) ? "MATCH" : "STALE",
                       root);
                std::string chain;
                for (uint32_t nd = rdz(root + 12u), hop = 0;
                     ok(nd) && hop < 12; nd = rdz(nd + 8u), ++hop) {
                  uint32_t cp = rdz(nd + 0x18u);
                  std::string nm;
                  uint32_t np = rdz(cp + 4u);
                  if (ok(np)) {
                    for (uint32_t c = 0; c < 24; ++c) {
                      uint32_t ad = np + c * 2u;
                      uint32_t wv = rdz(ad & ~3u);
                      uint16_t ch = (ad & 2u) ? (wv & 0xFFFF) : (wv >> 16);
                      if (!ch || ch < 0x20 || ch > 0x7E) break;
                      nm += (char)ch;
                    }
                  }
                  chain += nm.empty() ? fmt::format("{:08X} ", cp)
                                      : (nm + " ");
                }
                XELOGI("RootSceneChain: scene={:08X} [+12]={:08X} classes: {}",
                       root, rdz(root + 12u),
                       chain.empty() ? std::string("(empty)") : chain);
              }
              }
            }
            // The class-name probe that answered this lived here. It read
            // candidate strings out of class descriptors and faulted the host
            // twice before it was tuned; once it had produced the answer
            // (expect = XuiShader, present = XuiCanvas / XuiText) it was
            // removed rather than left in the draw path. See phase 275.
          }
          // Phase 372: the overflow check at 81A00774 computes
          // ((cursor - [dev+16344]) + 4) >> 2 and trips when it exceeds
          // 0x100000. Log both terms and the computed count.
          {
            uint32_t cur = w3(guide_resv_dev_ + 0x30u);
            uint32_t cbase = w3(guide_resv_dev_ + 16344u);
            uint32_t words = (cur - cbase + 4u) >> 2;
            XELOGI("CmdCount: cursor[30]={:08X} base[3FD8]={:08X} diff={:08X} "
                   "words={} limit=1048576 {}",
                   cur, cbase, cur - cbase, words,
                   (words > 0x100000u) ? "OVER LIMIT" : "within limit");
          }
          XELOGI("RingAtDraw #{}: head[5A00]={:08X} tail[5A04]={:08X} "
                 "gate[5A08]={:08X} ringbase[2B10]={:08X} "
                 "cfg[3AFC]={:08X} cfg[3B00]={:08X}",
                 win_logs, w3(guide_resv_dev_ + 0x5A00u),
                 w3(guide_resv_dev_ + 0x5A04u), w3(guide_resv_dev_ + 0x5A08u),
                 w3(guide_resv_dev_ + 0x2B10u), w3(guide_resv_dev_ + 0x3AFCu),
                 w3(guide_resv_dev_ + 0x3B00u));
        }
      }
      static uint32_t drawbr = 0;
      bool brk = drawbr++ < 4;
      if (brk) {
        XELOGI("GuideDrawEnter #{} fn={:08X} this={:08X}", drawbr,
               guide_draw_fn_, guide_draw_this_);
      }
      if (brk) {
        // The cursor is released by a paired end (81A01490 zeroes +2B4C), not
        // left uninitialised (phase 483). Log the whole block immediately
        // before the draw - the emitter runs inside this call - to see whether
        // it is already released by the time we get here.
        auto* qm = kernel_state()->memory();
        auto q = [qm](uint32_t a) {
          return (a >= 0x10000000u && a < 0xA0000000u)
                     ? xe::load_and_swap<uint32_t>(qm->TranslateVirtual(a))
                     : 0u;
        };
        uint32_t qdc = q(guide_draw_this_ + 12u);
        uint32_t qwr = qdc ? q(qdc + 0x1CCu) : 0;
        uint32_t qdv = qwr ? q(qwr + 0x0Cu) : 0;
        // Phase 514: the reserve cursor read after the composite draw is
        // 40875814 - xam's own buffer - while the value read just before it is
        // whatever DrawWiden/CursorReopen last wrote, so the delta is
        // meaningless. Capture [dev+0x30] here, at hook entry, before anything
        // in this file touches it. Disabling the cmdbuf binding instead is not
        // an option: with --guide_bind_cmdbuf_kb=0 the hook never fires at all
        // (draws: 0).
        g_draw_entry_reserve = q(qdv + 0x30u);
        XELOGI("CursorAtDraw #{}: dev={:08X} base[2B48]={:08X} cur[2B4C]={:08X} "
               "limit[2B50]={:08X} pend[2B54]={:08X} reserve[30]={:08X}",
               drawbr, qdv, q(qdv + 0x2B48u), q(qdv + 0x2B4Cu),
               q(qdv + 0x2B50u), q(qdv + 0x2B54u), g_draw_entry_reserve);
        // The crash unwinds to 913EABC4, inside the hud render entry
        // (913EAB28) - it is THIS path that faults, not the paint. The reserve
        // 81A042E0 draws its window from [dev+0x30]/[dev+0x34]; every widening
        // so far was applied at the paint site, which this path never reaches.
        // Widen here, on the device this draw actually uses.
        if (qdv && guide_cmdbuf_base_ && guide_cmdbuf_size_) {
          uint32_t dc0 = q(qdv + 0x30u), de0 = q(qdv + 0x34u);
          if (de0 <= dc0 || (de0 - dc0) < 0x905u * 4u) {
            auto* dm = kernel_state()->memory();
            xe::store_and_swap<uint32_t>(dm->TranslateVirtual(qdv + 0x30u),
                                         guide_cmdbuf_base_);
            xe::store_and_swap<uint32_t>(
                dm->TranslateVirtual(qdv + 0x34u),
                guide_cmdbuf_base_ + guide_cmdbuf_size_);
            XELOGI("DrawWiden #{}: dev={:08X} was {} -> cur={:08X} end={:08X}",
                   drawbr, qdv, (de0 > dc0) ? (de0 - dc0) : 0,
                   q(qdv + 0x30u), q(qdv + 0x34u));
            // Widening cleared 81A01638 and the failure moved to 819F5F60 -
            // the RT-slot fault of phase 453 (lwz r11,0x32B0(r31), null slot).
            // Phase 454 fixed that by binding the device's default surfaces,
            // but at the predraw site, which this path does not reach either.
            // Bind them here on the same device.
            // The slots read non-zero here and zero at the fault, so the
            // unbind-all clears them in between and restores from a default
            // that is evidently not usable. Bind unconditionally with a surface
            // created once, and log what was there first - "non-zero" was not
            // enough to justify skipping.
            {
              static uint32_t cached_surf = 0;
              // All five slots hold the same surface here yet read zero at the
              // fault, so it is being rejected rather than lost. This file
              // already records the rule: a real surface has bit 30 set in
              // word 0. Check ours against it.
              uint32_t sfc = q(qdv + 0x32B0u);
              uint32_t w0 = sfc ? q(sfc) : 0;
              XELOGI("DrawSurfWas #{}: [32A0]={:08X} [32B0]={:08X} [3F70]={:08X}"
                     " [3F74]={:08X} [3F78]={:08X} | surf w0={:08X} bit30={}",
                     drawbr, q(qdv + 0x32A0u), q(qdv + 0x32B0u),
                     q(qdv + 0x3F70u), q(qdv + 0x3F74u), q(qdv + 0x3F78u),
                     w0, (w0 & 0x40000000u) ? "set" : "CLEAR");
              // Our synthesised surface has bit 30 CLEAR in word 0 and so
              // fails SetRenderTarget's validity test - the render path
              // discards it and writes zero, which is the null slot at
              // 819F5F60. The original predraw design used the TITLE's live
              // surface ([0x801E6FC4] -> +0x3AC4) rather than making one.
              // Prefer that, and only fall back to creating one.
              // Phase 496: 819F4C00 nulls any slot whose value differs from
              // its default - [3F78] governs the four RT slots, [3F70] the
              // depth slot at [32B0]. The slots already hold a surface here, so
              // no creation is needed: make the defaults match the slots and
              // the unbind has nothing to null. This also avoids the early-out
              // that has been skipping this block entirely.
              {
                uint32_t rt = q(qdv + 0x32A0u), dp = q(qdv + 0x32B0u);
                if (rt && q(qdv + 0x3F78u) != rt) {
                  xe::store_and_swap<uint32_t>(
                      dm->TranslateVirtual(qdv + 0x3F78u), rt);
                }
                if (dp && q(qdv + 0x3F70u) != dp) {
                  xe::store_and_swap<uint32_t>(
                      dm->TranslateVirtual(qdv + 0x3F70u), dp);
                }
                XELOGI("DefaultsMatch #{}: rt={:08X} [3F78]={:08X} | dp={:08X} "
                       "[3F70]={:08X}",
                       drawbr, rt, q(qdv + 0x3F78u), dp, q(qdv + 0x3F70u));
              }
              uint32_t surf = cached_surf;
              if (!surf) {
                uint32_t tdev = q(0x801E6FC4u);
                uint32_t tsurf = tdev ? q(tdev + 0x3AC4u) : 0;
                uint32_t tw0 = tsurf ? q(tsurf) : 0;
                XELOGI("TitleSurf: tdev={:08X} surf={:08X} w0={:08X} bit30={}",
                       tdev, tsurf, tw0,
                       (tw0 & 0x40000000u) ? "set" : "CLEAR");
                if (tsurf && (tw0 & 0x40000000u)) {
                  surf = tsurf;
                } else {
                  // Phase 509: 1280x720 needs 0x2AA blocks and only ~0x286 are
                  // free, so 819E7528 fails its fit check at 819E75EC and the
                  // Guide gets no render target at all (phase 508). A surface
                  // that fits is strictly better than none: fall back through
                  // smaller sizes and take the first that succeeds. 640x640 and
                  // below were measured to work against the same pool.
                  //
                  // This is a diagnostic fallback, not the fix - a smaller
                  // target means the Guide renders at the wrong resolution. It
                  // exists to answer whether "no render target" is what stops
                  // the elements emitting, which is the open question from 506.
                  surf = GuideMakeSurface(kernel_state()->processor(),
                                          gth->thread_state());
                }
                cached_surf = surf;
              }
              if (surf) {
                for (uint32_t off : {0x32A0u, 0x32B0u, 0x3F70u, 0x3F74u,
                                     0x3F78u}) {
                  xe::store_and_swap<uint32_t>(
                      dm->TranslateVirtual(qdv + off), surf);
                }
                XELOGI("DrawSurfBind #{}: dev={:08X} surf={:08X} -> "
                       "[32A0]={:08X} [32B0]={:08X} [3F78]={:08X}",
                       drawbr, qdv, surf, q(qdv + 0x32A0u), q(qdv + 0x32B0u),
                       q(qdv + 0x3F78u));
              } else {
                XELOGW("DrawSurfBind #{}: surface creation returned 0", drawbr);
                // Phase 508: 819E7528 has THREE failure exits, not one. The
                // first is the descriptor allocation at its top:
                //   819E754C  li r3, 0x30
                //   819E7550  bl 81A0A148        <- allocate 0x30 bytes
                //   819E7558  bne                <- non-zero: continue
                //   819E7560  li r3, 0           <- zero: return 0
                // so it is not failing on its arguments. Walk the allocator
                // ladder directly and report which rung fails, rather than
                // reading four more levels of disassembly:
                //   81A0A148(size)          -> 817B5588(size, tag 0x64800000)
                //   817B5588(size, tag)     -> 817B53B0(0x02000000, size, out)
                //   817B53B0 returns NTSTATUS; 817B5588 maps <0 to a null ptr
                // ...but that ladder succeeds when called directly (40807740 /
                // 40807780), so the failure is one of the other two exits:
                //   819E75CC  bl 81A00E50   <- surface backing store
                //   819E75D4  bne           <- zero: free descriptor, return 0
                //   819E75EC  cmplwi r10, 0x800 / bgt -> same failure path
                // 81A00E50(size, &out) is the one that actually allocates the
                // surface memory, so probe it at a plausible surface size.
                static bool alloc_probe_done = false;
                if (!alloc_probe_done && guide_cmdbuf_base_ &&
                    guide_cmdbuf_size_ > 0x200u) {
                  alloc_probe_done = true;
                  uint32_t scratch =
                      guide_cmdbuf_base_ + guide_cmdbuf_size_ - 0x100u;
                  xe::store_and_swap<uint32_t>(dm->TranslateVirtual(scratch), 0);
                  // 819E75EC compares blockIndex + size against 0x800 and
                  // fails when the surface does not fit in what is LEFT of
                  // the pool. This must run FIRST: the 81A00E50 probes below
                  // leak 0x494 blocks, which pushes the index past 0x800 and
                  // makes every size fail for a reason that is the probe's
                  // own doing. The previous ordering did exactly that, so its
                  // "all sizes fail" reading established nothing.
                  for (uint32_t dim : {64u, 256u, 640u}) {
                    uint64_t sa[] = {dim, dim, 0x18280186u, 0, 0};
                    uint32_t sv = uint32_t(kernel_state()->processor()->Execute(
                        gth->thread_state(), GuideConst(0x819E7528u), sa,
                        xe::countof(sa)));
                    XELOGI("AllocProbe: 819E7528({}x{})={:08X}", dim, dim, sv);
                  }
                  uint64_t a1[] = {0x30};
                  uint32_t r1v = uint32_t(kernel_state()->processor()->Execute(
                      gth->thread_state(), GuideConst(0x81A0A148u), a1,
                      xe::countof(a1)));
                  uint64_t a2[] = {0x30, 0x64800000u};
                  uint32_t r2v = uint32_t(kernel_state()->processor()->Execute(
                      gth->thread_state(), GuideConst(0x817B5588u), a2,
                      xe::countof(a2)));
                  uint64_t a3[] = {0x02000000u, 0x30, scratch};
                  uint32_t r3v = uint32_t(kernel_state()->processor()->Execute(
                      gth->thread_state(), GuideConst(0x817B53B0u), a3,
                      xe::countof(a3)));
                  uint32_t scratch2 = scratch + 0x40u;
                  xe::store_and_swap<uint32_t>(dm->TranslateVirtual(scratch2), 0);
                  // 81A00E50 takes a BLOCK COUNT capped at 0x1800, not bytes:
                  //   81A00E7C  cmplwi r28, 0x1800
                  //   81A00E84  twui              <- over: trap (a no-op here)
                  // The first version of this probe passed 0x384000 (1280*720*4)
                  // and read the resulting 0 as "the allocator fails". It only
                  // showed the argument was out of range. Probe in range.
                  for (uint32_t nblk : {0x384u, 0x100u, 0x10u}) {
                    xe::store_and_swap<uint32_t>(
                        dm->TranslateVirtual(scratch2), 0);
                    uint64_t a4[] = {nblk, scratch2};
                    uint32_t r4v = uint32_t(kernel_state()->processor()->Execute(
                        gth->thread_state(), GuideConst(0x81A00E50u), a4,
                        xe::countof(a4)));
                    XELOGI("AllocProbe: 81A00E50({:X} blocks)={:08X} out={:08X}",
                           nblk, r4v,
                           xe::load_and_swap<uint32_t>(
                               dm->TranslateVirtual(scratch2)));
                  }
                  // The image's own call site 81790304 passes exactly these
                  // arguments (852, 980, 0x18280186, 0, 0), and every size
                  // fails, so the sizing call is the suspect. 819E6F10 takes
                  // the descriptor in r8 and two out-pointers in r9/r10 and
                  // fills [sp+0x50]/[sp+0x54], which 819E7528 then feeds to
                  // 81A00E50 and to the 0x800 block check.
                  if (r1v) {
                    uint32_t o50 = scratch2 + 0x10u, o54 = scratch2 + 0x14u;
                    xe::store_and_swap<uint32_t>(dm->TranslateVirtual(o50), 0);
                    xe::store_and_swap<uint32_t>(dm->TranslateVirtual(o54), 0);
                    uint64_t ga[] = {852, 980, 0x18280186u, 0, 0, r1v, o50, o54};
                    uint32_t gv = uint32_t(kernel_state()->processor()->Execute(
                        gth->thread_state(), GuideConst(0x819E6F10u), ga,
                        xe::countof(ga)));
                    XELOGI("AllocProbe: 819E6F10(852,980,18280186)={:08X} "
                           "size[50]={:08X} [54]={:08X} (cap 0x1800, total cap "
                           "0x800)",
                           gv,
                           xe::load_and_swap<uint32_t>(dm->TranslateVirtual(o50)),
                           xe::load_and_swap<uint32_t>(dm->TranslateVirtual(o54)));
                  }
                  XELOGI("AllocProbe: 81A0A148(0x30)={:08X} | "
                         "817B5588(0x30,64800000)={:08X} | "
                         "817B53B0(02000000,0x30)=status {:08X} out={:08X}",
                         r1v, r2v, r3v,
                         xe::load_and_swap<uint32_t>(
                             dm->TranslateVirtual(scratch)));
                }
              }
            }
          } else {
            XELOGI("DrawWiden #{}: dev={:08X} window {} already sufficient",
                   drawbr, qdv, de0 - dc0);
          }
        }
        // Measured: our begin sets base/cursor/limit, but by draw entry the
        // block is closed and the limit belongs to xam's own buffer - xam runs
        // its own begin/end cycles and ours is overwritten. The emitter is
        // then entered with no block open, which is the address-4 store.
        // Re-open immediately before the draw when the cursor is cold, which
        // is what the paired protocol requires.
        if (qdv && !q(qdv + 0x2B4Cu) && guide_cmdbuf_base_ && guide_cmdbuf_size_) {
          uint64_t ba[] = {qdv, guide_cmdbuf_base_, guide_cmdbuf_size_ / 4u};
          uint64_t br2 = kernel_state()->processor()->Execute(
              gth->thread_state(), GuideConst(0x81A01358u), ba,
              xe::countof(ba));
          XELOGI("CursorReopen #{}: 81A01358(dev {:08X}, {:08X}, {}) -> {:08X}"
                 " | cur now {:08X}",
                 drawbr, qdv, guide_cmdbuf_base_, guide_cmdbuf_size_ / 4u,
                 static_cast<uint32_t>(br2), q(qdv + 0x2B4Cu));
        }
      }
      // Phase 514: the paint path (hud_base + 0xA888) emits only LUT
      // programming - 768 COND_WRITE packets, 0 DRAW_INDX, now confirmed with a
      // valid parse. But the geometry path is this one, the composite draw
      // (0xAB28), which is where the phase-502 crash lived and is therefore
      // definitely doing graphics work. It has never been measured. Sample both
      // cursors across it and walk whatever it writes.
      auto* cdm = kernel_state()->memory();
      auto cq = [cdm](uint32_t a) {
        return a ? xe::load_and_swap<uint32_t>(cdm->TranslateVirtual(a)) : 0u;
      };
      uint32_t cd_dc = cq(guide_draw_this_ + 12u);
      uint32_t cd_wr = cd_dc ? cq(cd_dc + 0x1CCu) : 0;
      uint32_t cd_dev = cd_wr ? cq(cd_wr + 0x0Cu) : 0;
      uint32_t cd_r0 = cq(cd_dev + 0x30u);
      uint32_t cd_b0 = cq(cd_dev + 0x2B4Cu);
      in_guide_draw_scope = true;
      // Phase 525: tell the command processor which draws are the Guide's, so
      // its blend state can be overridden without touching the title's.
      auto* gs_scope = kernel_state()->emulator()->graphics_system();
      if (gs_scope && gs_scope->command_processor()) {
        gs_scope->command_processor()->guide_in_draw_scope_ = true;
        xe::gpu::g_guide_in_draw_scope = true;
      }
      uint64_t gr = kernel_state()->processor()->Execute(
          gth->thread_state(), guide_draw_fn_, gargs, xe::countof(gargs));
      if (gs_scope && gs_scope->command_processor()) {
        gs_scope->command_processor()->guide_in_draw_scope_ = false;
        xe::gpu::g_guide_in_draw_scope = false;
      }
      in_guide_draw_scope = false;
      if (cd_dev) {
        uint32_t cd_r1 = cq(cd_dev + 0x30u), cd_b1 = cq(cd_dev + 0x2B4Cu);
        static uint32_t cdlog = 0;
        if (cdlog++ < 4) {
          uint32_t pre = g_draw_entry_reserve;
        XELOGI("CompositeEmit #{}: entry[30]={:08X} -> {:08X} ({} words in "
               "xam's buffer)",
               drawbr, pre, cd_r1,
               (cd_r1 > pre) ? (cd_r1 - pre) / 4 : 0);
        XELOGI("CompositeEmit #{}: reserve {:08X}->{:08X} ({} words) | "
                 "block {:08X}->{:08X} ({} words)",
                 drawbr, cd_r0, cd_r1,
                 (cd_r1 > cd_r0) ? (cd_r1 - cd_r0) / 4 : 0, cd_b0, cd_b1,
                 (cd_b1 > cd_b0) ? (cd_b1 - cd_b0) / 4 : 0);
          uint32_t ws = 0, we2 = 0;
          if (cd_r1 > cd_r0) { ws = cd_r0; we2 = cd_r1; }
          else if (cd_b1 > cd_b0) { ws = cd_b0; we2 = cd_b1; }
          if (ws && (we2 - ws) < 0x40000u) {
            // Dump before parsing. The paint walk spent ten phases reporting
            // opcode soup because it began on a word that was not a packet
            // boundary; this one decodes its first header as opcode 00 with a
            // count of 16257, which is the same symptom.
            std::string craw;
            for (uint32_t k = 0; k < 16 && ws + k * 4u < we2; ++k)
              craw += fmt::format("{:08X} ", cq(ws + k * 4u));
            XELOGI("CompositeEmit #{}: raw at {:08X}: {}", drawbr, ws, craw);
            uint32_t nw = (we2 - ws) / 4u, t3 = 0, dr = 0;
            std::string firstp;
            for (uint32_t i = 0; i < nw;) {
              uint32_t hd = cq(ws + i * 4u);
              if ((hd >> 30) == 3) {
                uint32_t op = (hd >> 8) & 0x7Fu;
                uint32_t cn = ((hd >> 16) & 0x3FFFu) + 1u;
                ++t3;
                if (op == 0x22u || op == 0x36u) ++dr;
                if (t3 <= 10) firstp += fmt::format("{:02X}x{} ", op, cn);
                i += cn + 1u;
              } else {
                ++i;
              }
            }
            XELOGI("CompositeEmit #{}: {} words, {} type3, DRAW_INDX={} | {}",
                   drawbr, nw, t3, dr, firstp);
          }
        }
      }
      if (brk) {
        XELOGI("GuideDrawLeave #{} -> {:08X}", drawbr,
               static_cast<uint32_t>(gr));
      }
      if (objdiff_on && !objdiff_addr.empty()) {
        auto* om3 = kernel_state()->memory();
        auto ord3 = [om3](uint32_t a) {
          return a ? xe::load_and_swap<uint32_t>(om3->TranslateVirtual(a)) : 0u;
        };
        size_t k = 0;
        for (uint32_t a : objdiff_addr) {
          std::string diff;
          for (uint32_t i = 0; i < kOdWords; ++i, ++k) {
            uint32_t now = ord3(a + i * 4);
            if (k < objdiff_snap.size() && now != objdiff_snap[k]) {
              diff += fmt::format("+{:02X}:{:08X}->{:08X} ", i * 4,
                                  objdiff_snap[k], now);
            }
          }
          XELOGI("ObjDiff: {:08X} {}", a,
                 diff.empty() ? std::string("untouched") : diff);
        }
      }
      // --- second rendering context: submit ---
      // The begin left the cursor at buffer-4 and every emitted word
      if (ckpt_on) XELOGI("GuideCk: force_front_buffer");
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
      // Diff only the first few draws. The Guide runs 3600+ composite draws in
      // a 25s harness run and each one emitted ~2 lines per changed block, so
      // the interesting first-frame result scrolled past thousands of
      // identical repeats. Nothing has ever differed between draw 4 and draw
      // 3600 that draw 1..3 did not already show.
      static uint32_t diff_draws = 0;
      if (::cvars::guide_diff_draw_writes && !pre_sums.empty() &&
          diff_draws++ < 3) {
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
          // Walk the run as a real PM4 stream: read each header, honour its
          // count, and skip its payload. The older dump printed every word
          // with a type-3 interpretation, which turns shader microcode into
          // imaginary packets - the 22000000 inside an IM_LOAD payload read as
          // a DRAW_INDX header, and the walk lost sync immediately. Only a
          // walker that skips payload can say what the frame actually
          // contains.
          if (::cvars::guide_word_diff && len >= 32) {
            auto* mmw = kernel_state()->memory();
            uint32_t total_words = start + len;
            uint32_t counts[128] = {0};
            uint32_t t0 = 0, t1 = 0, t2 = 0, bad = 0, pkts = 0;
            std::string first;
            uint32_t i2 = 0;
            while (i2 < total_words) {
              uint32_t w = xe::load_and_swap<uint32_t>(
                  mmw->TranslateVirtual(kWdLo + i2 * 4));
              uint32_t type = w >> 30;
              uint32_t cnt = ((w >> 16) & 0x3FFF) + 1;
              if (type == 3) {
                uint32_t op = (w >> 8) & 0x7F;
                counts[op]++;
                ++pkts;
                if (pkts <= 24) {
                  first += fmt::format("{:02X}x{} ", op, cnt);
                }
                i2 += 1 + cnt;
              } else if (type == 0) {
                ++t0;
                i2 += 1 + cnt;
              } else if (type == 2) {
                ++t2;
                i2 += 1;
              } else {
                ++t1;
                i2 += 2;
              }
              if (i2 > total_words) {
                ++bad;
                break;
              }
            }
            std::string hist;
            for (uint32_t o = 0; o < 128; ++o) {
              if (counts[o]) {
                hist += fmt::format("{:02X}:{} ", o, counts[o]);
              }
            }
            XELOGI(
                "GuideWalk: {} words -> {} type3 pkts, {} type0, {} type2, "
                "{} type1, overrun={}",
                total_words, pkts, t0, t2, t1, bad);
            XELOGI("GuideWalk: opcodes {}", hist);
            XELOGI("GuideWalk: first {}", first);
            XELOGI("GuideWalk: DRAW_INDX(22)={} DRAW_INDX_2(36)={}",
                   counts[0x22], counts[0x36]);
          }

          // Hand the run to the GPU thread to draw over the title's next
          // frame, instead of executing it here. This is the mechanism; the
          // in-line execution below is the probe.
          if (::cvars::guide_overlay_at_swap && len >= 32) {
            auto* gso = kernel_state()->emulator()->graphics_system();
            if (gso && gso->command_processor()) {
              gso->command_processor()->guide_overlay_words_ = start + len;
              gso->command_processor()->guide_overlay_ptr_ = kWdLo;
            }
          }
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
              // Start at the BUFFER BASE, not at the first changed word. The
              // diff's start is wherever memory first differs, which lands
              // mid-packet: the observed run began at 3009C034 (+13 words) and
              // its head decoded as 000B2200 - a type-0 header - so the parser
              // was picking up the stream mid-stride and reported 0 regs and
              // 0 draws. 81A01358 sets the cursor to base-4 and emission
              // pre-increments, so the first packet really is at base; the
              // leading words simply happen to be written as zero and so do
              // not show up as "changed".
              uint32_t exec_at = kWdLo;
              uint32_t exec_len = start + len;
              gs2->command_processor()->ExecuteGuestBufferUnsafe(exec_at,
                                                                 exec_len);
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
          // guide_cmdbuf_base_ added: the Guide's packets go wherever the
          // reservation window points, and since guide_bind_cmdbuf_kb now
          // aims [dev+0x30]/[dev+0x34] at our own allocation, they land there
          // rather than in FE03xxxx/FE04xxxx. Scanning only the hardcoded
          // blocks meant this never looked at what the Guide actually wrote.
          for (uint32_t base : {0xFE030000u, 0xFE040000u, guide_cmdbuf_base_}) {
            if (!base) continue;
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
            {
              // Dump the chain. Each type-3 header is (3<<30)|(count<<16)|
              // (opcode<<8); decoding the opcodes says exactly which emission
              // sites in 819F5D18 ran, which pins where execution diverged
              // without needing coverage tracing or breakpoints on hot paths.
              std::string hex;
              for (uint32_t k = 0; k < words && k < 48; ++k) {
                uint32_t wv = rdc(start + k * 4);
                hex += fmt::format("{:08X} ", wv);
                if ((wv >> 30) == 3) {
                  hex += fmt::format("[op={:02X} cnt={}] ", (wv >> 8) & 0x7F,
                                     ((wv >> 16) & 0x3FFF) + 2);
                }
              }
              XELOGI("GuideExecDump: {}", hex);
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
          // Phase 520: this cap meant the GPU-draw counter sampled 5 of 2700+
          // composite draws. "15 draws on #2, zero on #1 and #3-#5" was that
          // sample, not a property of the draws. Always accumulate; only the
          // verbose walk stays capped.
          static uint32_t reported = 0;
          ++reported;
          {
            ++reported;
            if (reported <= 5 && guide_resv_dev_ && guide_resv_pre_) {
              auto* rm = kernel_state()->memory();
              uint32_t post = xe::load_and_swap<uint32_t>(
                  rm->TranslateVirtual(guide_resv_dev_ + 0x30u));
              if (post > guide_resv_pre_ &&
                  (post - guide_resv_pre_) < 0x100000u) {
                uint32_t nwords = (post - guide_resv_pre_) / 4u;
                uint32_t counts[128] = {0};
                uint32_t t0 = 0, t2 = 0, pkts = 0, bad = 0, iw = 0;
                std::string firstpk;
                while (iw < nwords) {
                  uint32_t w = xe::load_and_swap<uint32_t>(
                      rm->TranslateVirtual(guide_resv_pre_ + iw * 4));
                  uint32_t type = w >> 30;
                  uint32_t cnt = ((w >> 16) & 0x3FFF) + 1;
                  if (type == 3) {
                    uint32_t op = (w >> 8) & 0x7F;
                    counts[op]++;
                    ++pkts;
                    if (pkts <= 24) firstpk += fmt::format("{:02X}x{} ", op, cnt);
                    iw += 1 + cnt;
                  } else if (type == 0) {
                    ++t0;
                    iw += 1 + cnt;
                  } else if (type == 2) {
                    ++t2;
                    iw += 1;
                  } else {
                    iw += 2;
                  }
                  if (iw > nwords) { ++bad; break; }
                }
                std::string h;
                for (uint32_t o = 0; o < 128; ++o)
                  if (counts[o]) h += fmt::format("{:02X}:{} ", o, counts[o]);
                XELOGI("ResvWalk: {:08X}..{:08X} {} words -> {} type3, {} "
                       "type0, {} type2, overrun={}",
                       guide_resv_pre_, post, nwords, pkts, t0, t2, bad);
                XELOGI("ResvWalk: opcodes {}", h);
                XELOGI("ResvWalk: first {}", firstpk);
                XELOGI("ResvWalk: DRAW_INDX(22)={} DRAW_INDX_2(36)={}",
                       counts[0x22], counts[0x36]);
              } else {
                XELOGI("ResvWalk: cursor {:08X} -> {:08X} (no forward "
                       "progress; the draw emitted nothing here)",
                       guide_resv_pre_, post);
              }
            }
            // Phase 520: this log is capped, and the composite-draw numbering
            // reaches #600+, so seeing "15 draws" on #2 and zero on #1/#3-#5
            // sampled five of several hundred. Accumulate instead, and report a
            // running total periodically, so the question "how many of the
            // draws produce geometry" has an answer rather than a sample.
            static uint32_t gpu_total = 0, gpu_frames = 0, gpu_calls = 0;
            uint32_t gd_delta = gd_after - gd_before;
            gpu_total += gd_delta;
            ++gpu_calls;
            if (gd_delta) ++gpu_frames;
            if (reported <= 5) {
              XELOGI("GuideDrawGPU: the guest draw dispatched {} GPU draws",
                     gd_delta);
            }
            static uint32_t gpu_log = 0;
            if ((gpu_calls % 100u) == 0u || (gd_delta && gpu_log++ < 8)) {
              XELOGI("GuideGPUTotal: {} draws over {} calls; {} calls produced "
                     "geometry",
                     gpu_total, gpu_calls, gpu_frames);
            }
            // Phase 521: 2071 submitted draws is not a visible overlay. Where do
            // they land? The composite-draw line reports realdev[32A0]=0 even on
            // the calls that produce geometry, so read the GPU's own view rather
            // than the guest device's: RB_COLOR_INFO carries the colour target
            // base and format, RB_SURFACE_INFO the pitch. If these name a
            // surface the title never presents, that is geometry rendered
            // nowhere.
            if (gd_delta) {
              auto* rf = kernel_state()->emulator()->graphics_system()
                             ? kernel_state()->emulator()->graphics_system()
                                   ->register_file()
                             : nullptr;
              static uint32_t rtlog = 0;
              // Phase 522: the sequence that matters. If resolves stop
              // advancing while the Guide keeps drawing, the geometry is landing
              // in EDRAM after the frame has already been resolved and is
              // cleared unseen.
              XELOGI("GuideScoped: {} draws seen by the GPU thread inside the "
                     "Guide's scope (delta attributes {})",
                     gsx->command_processor()->guide_scoped_draws_, gpu_total);
              XELOGI("GuideSeq: +{} draws | resolves={} swaps={} (total draws {})",
                     gd_delta, gsx->command_processor()->guide_resolve_count_,
                     gsx->command_processor()->guide_swap_count_, gpu_total);
              // Phase 525: the draws produce no fragments (phase 524), so read
              // the state that can reject every one of them. Read, do not infer
              // - this is a value question and those have gone wrong here.
              if (rf && rtlog < 6) {
                float xs, xo;
                uint32_t xs_u = (*rf)[0x210F], xo_u = (*rf)[0x2110];
                std::memcpy(&xs, &xs_u, 4);
                std::memcpy(&xo, &xo_u, 4);
                XELOGI("GuideDrawState: scissor TL={:08X} BR={:08X} | "
                       "DEPTHCONTROL={:08X} BLENDCONTROL0={:08X} "
                       "COLORCONTROL={:08X} | CLIP_CNTL={:08X} "
                       "SU_SC_MODE={:08X} VTE_CNTL={:08X} | vport x scale={} "
                       "offset={}",
                       (*rf)[0x2081], (*rf)[0x2082], (*rf)[0x2200],
                       (*rf)[0x2201], (*rf)[0x2202], (*rf)[0x2204],
                       (*rf)[0x2205], (*rf)[0x2206], xs, xo);
              }
              if (rf && rtlog++ < 6) {
                XELOGI("GuideDrawRT: +{} draws | RB_SURFACE_INFO={:08X} "
                       "RB_COLOR_INFO={:08X} RB_MODECONTROL={:08X} "
                       "RB_COLOR_MASK={:08X}",
                       gd_delta, (*rf)[0x2000], (*rf)[0x2001],
                       (*rf)[0x2208], (*rf)[0x2104]);
              }
            }
          }
        }
      }
      EmitGuideCoverageOnce();
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
        // Also report the device's surface fields, sampled here rather than
        // at bootstrap time. The present picks
        //   [dev+0x32A0] ? [dev+0x32A0] : [dev+0x32B0]
        // and faults when both are zero. A survey taken at Guide-button time
        // showed all three known device globals with both fields zero, but
        // that is a snapshot long before the present runs and while dash is
        // demonstrably rendering - so the question is whether a surface
        // appears later, on this device, by the time it would be used.
        // Phase 342: does 818FB2B8 (slot-59 accessor + AddRef) hand back the
        // D3D wrapper? 81918EC0 dispatches slots 19/22 on whatever it returns,
        // and the significance of the skipped block at 8195E368 rests on that
        // object being the wrapper (vtable 81640680). One runtime read settles
        // it; static reading cannot (phase 341).
        {
          static bool probed = false;
          // 818FB2B8 is a dashroot address (one of the 22 in phase 440);
          // on retail it is a different function and executing it faults.
          if (!probed && ddc) {
            probed = true;
            auto* pm3 = kernel_state()->memory();
            uint32_t pbuf = pm3->SystemHeapAlloc(16, 16);
            if (pbuf) {
              xe::store_and_swap<uint32_t>(pm3->TranslateVirtual(pbuf), 0);
              uint64_t pa[] = {ddc, pbuf};
              uint32_t phr = uint32_t(kernel_state()->processor()->Execute(
                  XThread::GetCurrentThread()->thread_state(), GuideConst(0x818FB2B8u), pa,
                  xe::countof(pa)));
              uint32_t got = xe::load_and_swap<uint32_t>(
                  pm3->TranslateVirtual(pbuf));
              uint32_t gvt = (got >= 0x40000000u && got < 0x50000000u)
                                 ? xe::load_and_swap<uint32_t>(
                                       pm3->TranslateVirtual(got))
                                 : 0u;
              // Phase 351: which renderer is the Guide attached to? The dc's
              // own vtable and the wrapper's identify the implementation; the
              // live path sits around 818F while the vertex-message machinery
              // in 8172x/8184x/818Dx/818Ex is inactive (phase 350).
              // Phase 367: the shape draw dispatches slot 30 on [dc+460].
              // Identify that object and its vtable so the emitter can be
              // followed from a known class rather than guessed at.
              {
                uint32_t sub = rdw(ddc + 460u);
                XELOGI("DrawSub: [dc+460]={:08X} vtable={:08X} slot30={:08X} "
                       "slot31={:08X}",
                       sub, sub ? rdw(sub) : 0,
                       sub && rdw(sub) ? rdw(rdw(sub) + 120u) : 0,
                       sub && rdw(sub) ? rdw(rdw(sub) + 124u) : 0);
              }
              XELOGI("RendererIds: dc={:08X} dc.vtable={:08X} | wrapper={:08X} "
                     "wrapper.vtable={:08X} | dev[0C]={:08X}",
                     ddc, rdw(ddc), dev, dev ? rdw(dev) : 0,
                     dev ? rdw(dev + 0x0Cu) : 0);
              XELOGI("AccessorProbe: 818FB2B8(dc {:08X}) hr={:08X} -> obj={:08X} "
                     "vtable={:08X} | wrapper[1CC]={:08X} its vtable={:08X} | {}",
                     ddc, phr, got, gvt, dev, dev ? rdw(dev) : 0,
                     (gvt == 0x81640680u) ? "IS THE WRAPPER VTABLE"
                                          : "different vtable");
            }
          }
        }
        XELOGI("Guide composite draw #{} -> {:08X}; draw dc={:08X} "
               "[11C]={:08X} [134]={:08X} [1CC]={:08X} "
               "wrap[0]={:08X} wrap[0C]={:08X} "
               "realdev[32A0]={:08X} realdev[32B0]={:08X}",
               gn, static_cast<uint32_t>(gr), ddc, ddc ? rdw(ddc + 0x11Cu) : 0,
               ddc ? rdw(ddc + 0x134u) : 0, dev,
               dev ? rdw(dev) : 0, dev ? rdw(dev + 0x0Cu) : 0,
               (dev && rdw(dev + 0x0Cu))
                   ? rdw(rdw(dev + 0x0Cu) + 0x32A0u)
                   : 0,
               (dev && rdw(dev + 0x0Cu))
                   ? rdw(rdw(dev + 0x0Cu) + 0x32B0u)
                   : 0);
        // The DRAW_INDX gate, read off the REAL device (wrapper+0x0C).
        // 819F6BC0 keeps the low 12 bits of [dev+0x10] (rldicl r10,r11,0,52)
        // and skips the draw when they are zero; [dev+0x28] gates the block
        // above it. Both are 64-bit, so log the low words too. This is the
        // question a 00000000 composite-draw return does not answer: whether
        // geometry ever marks the device dirty.
        if (dev) {
          uint32_t rdev = rdw(dev + 0x0Cu);
          if (rdev) {
            if (::cvars::guide_force_drawgate) {
              xe::store_and_swap<uint32_t>(
                  kernel_state()->memory()->TranslateVirtual(rdev + 0x14u),
                  rdw(rdev + 0x14u) | 0xFFFu);
            }
            XELOGI("DrawGate #{}: dev={:08X} [10]={:08X}:{:08X} "
                   "[28]={:08X}:{:08X} low12={:03X}",
                   gn, rdev, rdw(rdev + 0x10u), rdw(rdev + 0x14u),
                   rdw(rdev + 0x28u), rdw(rdev + 0x2Cu),
                   rdw(rdev + 0x14u) & 0xFFFu);
          }
        }
        // The real present path (819DE94C) picks a surface as
        //   r11 = [dev+32A0] ? [dev+32A0] : [dev+32B0]
        // and immediately does lwz r9,36(r11). Both null => null deref at
        // guest 0x24, which is the crash seen with guide_force_real_present.
        if (dev) {
          XELOGI("Guide device {:08X}: [32A0]={:08X} [32B0]={:08X}", dev,
                 rdw(dev + 0x32A0u), rdw(dev + 0x32B0u));
          // guide_bind_title_rt binds RT0 on the device it reaches through
          // the render wrapper, which is a different object from the one the
          // present reads out of [dc+0x1CC] - measured as 40883A80 versus
          // 4088B7A0, the first already carrying a valid RT0 and the second
          // holding 00000060. Bind the same surface here, where the present's
          // own device is in hand.
          {
            static uint32_t dbg = 0;
            if (dbg++ < 3) {
              XELOGI("Guide: present-device bind check: flag={} cached surf "
                     "{:08X} dev {:08X}",
                     ::cvars::guide_bind_title_rt, guide_title_surface_, dev);
            }
          }
          // OFF by default (XENIA_PRESENT_RT=1 to try it). Binding RT0 on
          // the present's device makes things worse, not better: without it
          // the draw loop runs continuously - 16 composite draws still going
          // when the run is killed - and with it the loop stops after a
          // single draw and xam reports the device finalizing. Zeroing RT0
          // first, so the setter skips releasing the junk it holds, does not
          // help either. Kept behind a switch because the diagnosis of *why*
          // is unfinished, not because it works.
          // [dc+0x1CC] is the *wrapper*, a 140-byte object of class
          // 81640680 - not a device. Its +0x32A0 is 12KB past the end of it,
          // so every earlier attempt here bound RT0 into unrelated memory and
          // the "junk 00000060" it tried to release was simply whatever lived
          // there. The real device hangs off the wrapper at +0x0C, and that is
          // the object the present reads (819DE8F8 takes it as its first
          // argument via wrapper vtable slot 11). Bind on that instead.
          uint32_t real_dev = dev ? rdw(dev + 0x0Cu) : 0;
          // Point the wrapper at the device mode 1 actually set up. The two
          // are different objects: the wrapper is bound around log line 5026,
          // mode 1 runs at 21509, and nothing re-binds it - so the front
          // buffer mode 1 allocates never reaches the emitter.
          //
          // 8191BAC8(wrapper, device, params) asserts device != 0, no-ops if
          // unchanged, addrefs the new device, releases the old, stores it at
          // +0x0C, and finally memcpy's 124 bytes from `params` into
          // wrapper+0x10. So the third argument must be a readable 124-byte
          // block, NOT zero - passing 0 here would memcpy from null, the same
          // class of mistake that made the earlier 81A0FE48(dev, 0) call
          // crash. The wrapper's own +0x10 is such a block, making the copy a
          // self-copy and leaving those fields untouched.
          // Lend the Guide's OWN device the title's front buffer, instead of
          // swapping the whole device. Rebinding to the title device does get
          // a front buffer into the draw path - GuidePreDraw went from
          // [3F74]=00000000 to A240A380 for the first time - but it then
          // faults in 819E5350 at `stw r11,8(r28)` with r28 = 0000FFFF, read
          // by `lwzx r28,r27,r30` from [title_dev+0x3308]. That slot is a
          // sentinel on dash's device: xam's draw code indexes a per-device
          // table that only a xam-created device has populated, null-checks
          // it, and stores through it.
          //
          // So the device identity matters and the front buffer does not.
          // The Guide's device 40870D00 has the tables xam set up but no
          // front buffer; the title's has the buffer but not the tables.
          // Copy across only the one field that is missing.
          if (::cvars::guide_borrow_front_buffer && dev) {
            static bool lent = false;
            uint32_t tdev = rdw(0x801E6FC4u);
            uint32_t fb = tdev ? rdw(tdev + 0x3F74u) : 0;
            if (!lent && real_dev && fb && !rdw(real_dev + 0x3F74u)) {
              lent = true;
              xe::store_and_swap<uint32_t>(
                  mem->TranslateVirtual(real_dev + 0x3F74u), fb);
              XELOGI("Guide: lent front buffer {:08X} from title device {:08X} "
                     "to guide device {:08X}; [3F74] now {:08X}",
                     fb, tdev, real_dev, rdw(real_dev + 0x3F74u));
            }
          }
          if (::cvars::guide_rebind_wrapper_device && dev) {
            static bool rebound = false;
            // Prefer the TITLE's device. Measured: mode 1's device 407CB880
            // has [3F74]=0 for the whole run - it never finishes init, so it
            // never gets a front buffer, which was the premise of this rebind
            // and was wrong. The only device with a real front buffer is
            // VdGlobalDevice = 40952400, [3F74]=A240A380: dash's own device,
            // the one actually presenting. That also matches how the Guide is
            // supposed to work - it composites OVER the running title rather
            // than owning a display of its own.
            uint32_t title_dev = rdw(0x801E6FC4u);
            uint32_t target = (title_dev && rdw(title_dev + 0x3F74u))
                                  ? title_dev
                                  : guide_mode1_device_.load();
            if (!rebound && target && target != real_dev) {
              rebound = true;
              auto* rth = XThread::GetCurrentThread();
              uint64_t rargs[] = {dev, target, dev + 0x10u};
              uint64_t rr = rth ? kernel_state()->processor()->Execute(
                                      rth->thread_state(), GuideConst(0x8191BAC8u), rargs,
                                      xe::countof(rargs))
                                : 0;
              XELOGI("Guide: rebound wrapper {:08X} from device {:08X} to "
                     "{:08X} -> {:08X}; [wrapper+0C] now {:08X} "
                     "([3F74]={:08X})",
                     dev, real_dev, target, static_cast<uint32_t>(rr),
                     rdw(dev + 0x0Cu), rdw(rdw(dev + 0x0Cu) + 0x3F74u));
            }
          }
          {
            // Unconditional one-shot probe: the force block below silently did
            // nothing across several runs and each of its three conditions is
            // individually plausible as the cause. Report all of them once.
            static bool probed = false;
            if (!probed) {
              probed = true;
              XELOGI("Guide: force-fb gate: cvar={} dev={:08X} real_dev={:08X} "
                     "[3F74]={:08X}",
                     ::cvars::guide_force_front_buffer ? 1 : 0, dev, real_dev,
                     real_dev ? rdw(real_dev + 0x3F74u) : 0);
            }
          }
          if (::cvars::guide_force_front_buffer && real_dev) {
            static bool fb_done = false;
            if (!fb_done && !rdw(real_dev + 0x3F74u)) {
              // 81A0FE48's second argument is NOT zero. Its one real caller
              // is 819F4E8C inside 819F4D28:
              //     819F4E84: mr r4,r28
              //     819F4E88: mr r3,r31
              //     819F4E8C: bl 81A0FE48
              // and r28 is written exactly once, in the prologue:
              //     819F4D40: mr r28,r7      (arg5)
              // Passing 0 makes 81A0FE48 compute r4 = 0 + 0x48 at 81A0FF70 and
              // fault at 81A04648 reading 0x4C - which is precisely the crash
              // recorded against this cvar, and was misread as the routine
              // depending on device state mode 2 never establishes.
              // Mode 2 hardcodes `li r7,0` at 8178F7BC, so the captured arg5
              // is legitimately zero on that path and there is nothing to pass
              // through. But 8191BAC8 memcpy's 124 bytes - the exact size of
              // the block 8178E9F0 builds at r1+0x90 - into wrapper+0x10. If
              // that copy is populated it IS this structure, and no
              // fabrication is needed. Dump it before deciding.
              // Mode 2 hardcodes `li r7,0` at 8178F7BC, so there is nothing to
              // pass through and the block has to be built. 8178E9F0 memsets
              // 124 bytes at r1+0x90 (81A0EA70-81A0EA7C) and then writes only
              // the fields below, so a zeroed buffer plus these is a faithful
              // copy of what the mode-1 creator hands over. Offsets are
              // relative to the block, i.e. the r1 displacement minus 0x90.
              //
              // The 640x480 pair is 8178E9F0's own fallback branch
              // (li r10,640 / li r11,480 at 8178EAE4/8178EAEC), used when it
              // has no display mode to read - which is exactly our situation.
              uint32_t arg5 = GuideGetDevCreateArg5();
              if (!arg5) {
                static uint32_t synth = 0;
                if (!synth) {
                  synth = kernel_state()->memory()->SystemHeapAlloc(0x7C, 16);
                }
                if (synth) {
                  auto* mm = kernel_state()->memory();
                  std::memset(mm->TranslateVirtual(synth), 0, 0x7C);
                  auto wr = [&](uint32_t off, uint32_t v) {
                    xe::store_and_swap<uint32_t>(
                        mm->TranslateVirtual(synth + off), v);
                  };
                  const uint32_t kW = 640, kH = 480;
                  wr(0x00, kW);          // 8178EAF4  width
                  wr(0x04, kH);          // 8178EAF8  height
                  wr(0x08, 0x28280186);  // 8178EB08/EB18
                  wr(0x34, 0x80000000);  // 8178EB14
                  wr(0x3C, 1);           // 8178EB30/EB40
                  wr(0x4C, 4096);        // 8178EB2C/EB50 - read via +0x48
                  wr(0x54, 0x00010000);  // 8178EB20/EB34
                  wr(0x68, kW);          // 8178EB00
                  wr(0x6C, kH);          // 8178EB04
                  arg5 = synth;
                  XELOGI("Guide: synthesized 81A0FE48 param block @{:08X} "
                         "({}x{}, [+4C]=4096)",
                         synth, kW, kH);
                }
              }
              if (!arg5) {
                static bool warned = false;
                if (!warned) {
                  warned = true;
                  XELOGW(
                      "Guide: skipping forced front-buffer setup - no usable "
                      "parameter block for 81A0FE48 arg2. Passing 0 faults at "
                      "81A04648 reading block+0x4C.");
                }
              } else {
                fb_done = true;
                auto* fth = XThread::GetCurrentThread();
                uint64_t fargs[] = {real_dev, arg5};
                uint64_t fr = fth ? kernel_state()->processor()->Execute(
                                        fth->thread_state(), GuideConst(0x81A0FE48u),
                                        fargs, xe::countof(fargs))
                                  : 0;
                XELOGI("Guide: forced front-buffer setup on {:08X} arg5={:08X}"
                       " -> {:08X}; [3F74] now {:08X}, [2B10] now {:08X}",
                       real_dev, arg5, static_cast<uint32_t>(fr),
                       rdw(real_dev + 0x3F74u), rdw(real_dev + 0x2B10u));
              }
            }
          }
          if (std::getenv("XENIA_PRESENT_RT") &&
              ::cvars::guide_bind_title_rt && guide_title_surface_ && real_dev) {
            static bool present_rt_done = false;
            uint32_t cur = rdw(real_dev + 0x32A0u);
            bool plausible = false;
            if (cur >= 0x10000u) {
              auto* ph = kernel_state()->memory()->LookupHeap(cur);
              plausible = ph && ph->QueryRangeAccess(cur, cur + 0x27u) !=
                                    xe::memory::PageAccess::kNoAccess;
            }
            if (!present_rt_done && !plausible) {
              present_rt_done = true;
              // Zero the slot first. 819F31A8 releases whatever RT0 already
              // holds before storing the new surface:
              //   lwzx r30,r27,r31 ; if r30 != 0 -> release(r30)
              // and RT0 holds 00000060 here, which is not a refcounted
              // surface. Letting it release that is what tips the device into
              // D3DDevice_Release - measured as draws dropping from 14 to 1
              // and a "currently finalizing" warning appearing the moment the
              // bind was allowed to run. With the slot zeroed the setter
              // skips its release path.
              if (cur) {
                xe::store_and_swap<uint32_t>(
                    kernel_state()->memory()->TranslateVirtual(real_dev + 0x32A0u),
                    0u);
                XELOGI("Guide: cleared junk RT0 {:08X} before binding", cur);
              }
              auto* pth = XThread::GetCurrentThread();
              uint64_t pargs[] = {real_dev, 0ull, guide_title_surface_};
              uint64_t pres = pth ? kernel_state()->processor()->Execute(
                                        pth->thread_state(), GuideConst(0x819F31A8u),
                                        pargs, xe::countof(pargs))
                                  : 0;
              XELOGI("Guide: bound RT0 {:08X} on the present's device {:08X} "
                     "-> {:08X}; [32A0] now {:08X}",
                     guide_title_surface_, real_dev,
                     static_cast<uint32_t>(pres), rdw(real_dev + 0x32A0u));
            }
          }
        }
      }
      // in_guide_draw is cleared by GuideDrawScope's destructor.
      // Hand the device back to whoever owned it before this draw.
      if (guide_claim_dev_ && guide_claim_prev_) {
        xe::store_and_swap<uint32_t>(
            kernel_state()->memory()->TranslateVirtual(guide_claim_dev_ +
                                                       0x2B08u),
            guide_claim_prev_);
        guide_claim_dev_ = 0;
      }
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
