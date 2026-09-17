/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <ranges>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <vector>
#include <set>
#include <mutex>
#include <thread>

#include "xenia/base/mutex.h"

#include "xenia/emulator.h"
#include "xenia/hid/input.h"

// For naming OS threads in the Guide thread probe (Windows-only file paths
// already: this translation unit uses CONTEXT/SuspendThread directly).
#include <tlhelp32.h>

#include "config.h"
#include "third_party/fmt/include/fmt/format.h"
#include "third_party/tabulate/single_include/tabulate/tabulate.hpp"
#include "third_party/zarchive/include/zarchive/zarchivecommon.h"
#include "third_party/zarchive/include/zarchive/zarchivewriter.h"
#include "third_party/zarchive/src/sha_256.h"
#include "xenia/apu/audio_system.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/thread_debug_info.h"
#include "xenia/cpu/backend/null_backend.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/apu/xma_decoder.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/ui/presenter.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/user_module.h"
#include <array>
#include <utility>
#include "xenia/cpu/export_resolver.h"  // phase 1055 menus: the import trace
#include "xenia/cpu/function.h"
#include "xenia/cpu/symbol.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/power_reset.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/xam/achievement_manager.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ani.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_video.h"

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include "xenia/kernel/xam/xdbf/spa_info.h"
#include "xenia/kernel/xbdm/xbdm_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/memory.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_host_notification.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/disc_zarchive_device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/virtual_file_system.h"

#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_backend.h"
#endif  // XE_ARCH

DEFINE_double(time_scalar, 1.0,
              "Scalar used to speed or slow time (1x, 2x, 1/2x, etc).",
              "General");

DEFINE_string(
    launch_module, "",
    "Executable to launch from the .iso or the package instead of default.xex "
    "or the module specified by the game. Leave blank to launch the default "
    "module.",
    "General");

DEFINE_bool(allow_dll_module_launch, false,
            "Allow launching a non-executable (DLL) XEX module directly, such "
            "as the Xbox 360 Guide (hud.xex) or xam.xex. Normally these are "
            "loaded by xam rather than booted.",
            "General");

DEFINE_bool(allow_game_relative_writes, false,
            "Not useful to non-developers. Allows code to write to paths "
            "relative to game://. Used for "
            "generating test data to compare with original hardware. ",
            "General");

DECLARE_int32(draw_resolution_scale_x);
DECLARE_int32(draw_resolution_scale_y);
DECLARE_bool(allow_plugins);

DEFINE_int32(priority_class, 0,
             "Forces Xenia to use different process priority than default one. "
             "It might affect performance and cause unexpected bugs. Possible "
             "values: 0 - Normal, 1 - Above normal, 2 - High",
             "General");

DECLARE_int32(console_type);

namespace xe {
// Phase 1096: EXTERNAL linkage on purpose - the per-paint probe in
// xboxkrnl_video.cc reads it. The other g_guide_* globals here live in
// the anonymous namespace below and cannot be referenced across TUs.
uint32_t g_guide_msgbox_parent = 0;
using namespace xe::literals;

Emulator::GameConfigLoadCallback::GameConfigLoadCallback(Emulator& emulator)
    : emulator_(emulator) {
  emulator_.AddGameConfigLoadCallback(this);
}

Emulator::GameConfigLoadCallback::~GameConfigLoadCallback() {
  emulator_.RemoveGameConfigLoadCallback(this);
}

Emulator::Emulator(const std::filesystem::path& command_line,
                   const std::filesystem::path& storage_root,
                   const std::filesystem::path& content_root,
                   const std::filesystem::path& cache_root)
    : on_launch(),
      on_terminate(),
      on_exit(),
      command_line_(command_line),
      storage_root_(storage_root),
      content_root_(content_root),
      cache_root_(cache_root),
      title_name_(),
      title_version_(),
      display_window_(nullptr),
      memory_(),
      audio_system_(),
      audio_media_player_(),
      graphics_system_(),
      input_system_(),
      export_resolver_(),
      file_system_(),
      kernel_state_(),
      main_thread_(),
      title_id_(std::nullopt),
      game_info_database_(),
      paused_(false),
      restoring_(false),
      restore_fence_() {
  if (cvars::priority_class != 0) {
    if (SetProcessPriorityClass(cvars::priority_class)) {
      XELOGI("Higher priority class request: Successful. New priority: {}",
             cvars::priority_class);
    }
  }

#if XE_PLATFORM_WIN32 == 1
  // Show a disclaimer that links to the quickstart
  // guide the first time they ever open the emulator
  uint64_t persistent_flags = GetPersistentEmulatorFlags();
  if (!(persistent_flags & EmulatorFlagDisclaimerAcknowledged)) {
    if ((MessageBoxW(
             nullptr,
             L"DISCLAIMER: Xenia is not for enabling illegal activity, and "
             "support is unavailable for illegally obtained software.\n\n"
             "Please respect this policy as no further reminders will be "
             "given.\n\nThe quickstart guide explains how to use digital or "
             "physical games from your Xbox 360 console.\n\nWould you like "
             "to open it?",
             L"Xenia", MB_YESNO | MB_ICONQUESTION) == IDYES)) {
      LaunchWebBrowser(
          "https://github.com/xenia-canary/xenia-canary/wiki/"
          "Quickstart#how-to-rip-games");
    }
    SetPersistentEmulatorFlags(persistent_flags |
                               EmulatorFlagDisclaimerAcknowledged);
  }
#endif
}

Emulator::~Emulator() {
  // Phase 1055 bugs: the Guide's paint thread runs guest code through the
  // systems deleted below; stop it first.
  kernel::xboxkrnl::GuidePaintThreadStop();
  // Note that we delete things in the reverse order they were initialized.

  // Give the systems time to shutdown before we delete them.
  if (graphics_system_) {
    graphics_system_->Shutdown();
  }
  if (audio_system_) {
    audio_system_->Shutdown();
  }

  input_system_.reset();
  graphics_system_.reset();
  audio_system_.reset();
  audio_media_player_.reset();

  // Host Power Off: ~XThread unregisters from the kernel state, so the main
  // thread and module-level object refs must go first (as a member it would
  // outlive kernel_state_).
  kernel::ResetModuleStateForPowerOff();
  main_thread_.reset();
  kernel_state_.reset();
  file_system_.reset();

  processor_.reset();

  export_resolver_.reset();

  ExceptionHandler::Uninstall(Emulator::ExceptionCallbackThunk, this);
}

X_STATUS Emulator::Setup(
    ui::Window* display_window, ui::ImGuiDrawer* imgui_drawer,
    bool require_cpu_backend,
    std::function<std::unique_ptr<apu::AudioSystem>(cpu::Processor*)>
        audio_system_factory,
    std::function<std::unique_ptr<gpu::GraphicsSystem>()>
        graphics_system_factory,
    std::function<std::vector<std::unique_ptr<hid::InputDriver>>(ui::Window*)>
        input_driver_factory) {
  X_STATUS result = X_STATUS_UNSUCCESSFUL;

  display_window_ = display_window;
  imgui_drawer_ = imgui_drawer;

  // Initialize clock.
  // 360 uses a 50MHz clock.
  Clock::set_guest_tick_frequency(50000000);
  // We could reset this with save state data/constant value to help replays.
  Clock::set_guest_system_time_base(Clock::QueryHostSystemTime());
  // This can be adjusted dynamically, as well.
  Clock::set_guest_time_scalar(cvars::time_scalar);

  // Before we can set thread affinity we must enable the process to use all
  // logical processors.
  xe::threading::EnableAffinityConfiguration();

  XELOGI("{}: Initializing Memory...", __func__);
  // Create memory system first, as it is required for other systems.
  memory_ = std::make_unique<Memory>();
  if (!memory_->Initialize()) {
    XELOGE("{}: Cannot initalize memory!", __func__);
    return result;
  }

  XELOGI("{}: Initializing Exports...", __func__);
  // Shared export resolver used to attach and query for HLE exports.
  export_resolver_ = std::make_unique<xe::cpu::ExportResolver>();

  std::unique_ptr<xe::cpu::backend::Backend> backend;
#if XE_ARCH_AMD64
  if (cvars::cpu == "x64") {
    backend.reset(new xe::cpu::backend::x64::X64Backend());
  }
#elif XE_ARCH_ARM64
  if (cvars::cpu == "a64") {
    backend.reset(new xe::cpu::backend::a64::A64Backend());
  }
#endif  // XE_ARCH
  if (cvars::cpu == "any") {
    if (!backend) {
#if XE_ARCH_AMD64
      backend.reset(new xe::cpu::backend::x64::X64Backend());
#elif XE_ARCH_ARM64
      backend.reset(new xe::cpu::backend::a64::A64Backend());
#endif  // XE_ARCH
    }
  }
  if (!backend && !require_cpu_backend) {
    backend.reset(new xe::cpu::backend::NullBackend());
  }

  XELOGI("{}: Initializing Processor...", __func__);
  // Initialize the CPU.
  processor_ = std::make_unique<xe::cpu::Processor>(memory_.get(),
                                                    export_resolver_.get());
  if (!processor_->Setup(std::move(backend))) {
    XELOGE("{}: Cannot initalize processor!", __func__);
    return X_STATUS_UNSUCCESSFUL;
  }
  if (!cvars::trace_guest_pcs.empty()) {
    // 1099z17559-3: diagnostic PC trace, addresses from the command line.
    const std::string& tp = cvars::trace_guest_pcs;
    size_t i = 0;
    while (i < tp.size()) {
      size_t comma = tp.find(',', i);
      if (comma == std::string::npos) comma = tp.size();
      // 1099z160: "addr:max" raises the per-address hit cap (default 8).
      const std::string item = tp.substr(i, comma - i);
      const uint32_t addr = uint32_t(std::strtoul(item.c_str(), nullptr, 16));
      const size_t colon = item.find(':');
      const uint32_t max_hits =
          colon == std::string::npos
              ? 8u
              : uint32_t(std::strtoul(item.c_str() + colon + 1, nullptr, 10));
      // "addr:max:rN:hexoff" dumps the words at rN+off instead of r1+0x70.
      uint32_t dump_reg = 1, dump_off = 0x70;
      const size_t colon2 =
          colon == std::string::npos ? colon : item.find(':', colon + 1);
      if (colon2 != std::string::npos && colon2 + 1 < item.size() &&
          item[colon2 + 1] == 'r') {
        char* end = nullptr;
        dump_reg =
            uint32_t(std::strtoul(item.c_str() + colon2 + 2, &end, 10)) & 31;
        if (end && *end == ':') {
          dump_off = uint32_t(std::strtoul(end + 1, nullptr, 16));
        }
      }
      i = comma + 1;
      if (!addr) continue;
      auto hits = std::make_shared<std::atomic<uint32_t>>(0);
      auto* mem = memory_.get();
      processor_->AddGuestHook(
          addr, [addr, hits, max_hits, mem, dump_reg,
                 dump_off](cpu::ppc::PPCContext* c) {
            // 1099z166: when --kernel_sample_to_ms is set, trace only inside
            // that window (a pc that runs constantly burns its cap long
            // before the moment of interest otherwise).
            static const auto t0 = std::chrono::steady_clock::now();
            if (cvars::kernel_sample_to_ms > 0) {
              const int64_t now_ms =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
              if (now_ms < cvars::kernel_sample_from_ms ||
                  now_ms > cvars::kernel_sample_to_ms) {
                return;
              }
            }
            const uint32_t n = ++*hits;
            if (n > max_hits) return;
            auto* th = kernel::XThread::GetCurrentThread();
            auto* ks = th ? th->kernel_state() : nullptr;
            // 1099z160: r1 and 4 words at rN+off (default r1+0x70).
            const uint32_t sp = uint32_t(c->r[1]);
            const uint32_t base = uint32_t(c->r[dump_reg]) + dump_off;
            uint32_t w[4] = {0, 0, 0, 0};
            if (base >= 0x10000 && base < 0xFFFF0000) {
              for (int k = 0; k < 4; ++k) {
                w[k] = xe::load_and_swap<uint32_t>(
                    mem->TranslateVirtual(base + 4 * k));
              }
            }
            XELOGI("TracePC {:08X} #{}: r3 {:08X} r4 {:08X} r5 {:08X} r6 "
                   "{:08X} r7 {:08X} r8 {:08X} r9 {:08X} "
                   "r10 {:08X} r11 {:08X} r31 {:08X} lr {:08X} r1 "
                   "{:08X} [r{}+{:X}] {:08X} {:08X} {:08X} {:08X} tid {:08X} "
                   "| {}",
                   addr, n, uint32_t(c->r[3]), uint32_t(c->r[4]),
                   uint32_t(c->r[5]), uint32_t(c->r[6]), uint32_t(c->r[7]),
                   uint32_t(c->r[8]), uint32_t(c->r[9]), uint32_t(c->r[10]),
                   uint32_t(c->r[11]), uint32_t(c->r[31]), uint32_t(c->lr),
                   sp, dump_reg, dump_off, w[0], w[1], w[2], w[3],
                   th ? th->thread_id() : 0u,
                   ks ? ks->GuestBackChain(8) : std::string());
          });
    }
  }
  if (!cvars::trace_guest_list.empty()) {
    // Phase 1099z161 (DIAGNOSTIC): "pc:rN:headoff:nextoff:words" - at guest
    // pc, walk a singly linked list starting at [rN+headoff], following
    // [node+nextoff] (up to 32 nodes, first 4 hits), and log each node's
    // first `words` words plus any word that points at printable ASCII.
    const std::string& s = cvars::trace_guest_list;
    uint32_t pc = 0, reg = 3, head_off = 0, next_off = 0, words = 8;
    char* p = nullptr;
    pc = uint32_t(std::strtoul(s.c_str(), &p, 16));
    if (p && *p == ':' && p[1] == 'r') {
      reg = uint32_t(std::strtoul(p + 2, &p, 10)) & 31;
      if (p && *p == ':') head_off = uint32_t(std::strtoul(p + 1, &p, 16));
      if (p && *p == ':') next_off = uint32_t(std::strtoul(p + 1, &p, 16));
      if (p && *p == ':') words = uint32_t(std::strtoul(p + 1, &p, 10));
    }
    words = std::min<uint32_t>(std::max<uint32_t>(words, 1), 32);
    auto hits = std::make_shared<std::atomic<uint32_t>>(0);
    auto* mem = memory_.get();
    if (pc) {
      processor_->AddGuestHook(pc, [=](cpu::ppc::PPCContext* c) {
        if (++*hits > 4) return;
        auto readable = [mem](uint32_t a) {
          if (a < 0x10000 || a >= 0xA0000000) return false;
          auto* heap = mem->LookupHeap(a);
          uint32_t prot = 0;
          return heap && heap->QueryProtect(a, &prot) &&
                 (prot & kMemoryProtectRead) != 0;
        };
        auto rd = [mem](uint32_t a) {
          return xe::load_and_swap<uint32_t>(mem->TranslateVirtual(a));
        };
        uint32_t base = uint32_t(c->r[reg]) + head_off;
        uint32_t node = readable(base) ? rd(base) : 0;
        XELOGI("TraceList {:08X} hit {}: r{}={:08X} head {:08X}", pc,
               hits->load(), reg, uint32_t(c->r[reg]), node);
        for (int n = 0; n < 32 && readable(node); ++n) {
          std::string line;
          for (uint32_t k = 0; k < words; ++k) {
            const uint32_t w = rd(node + 4 * k);
            line += fmt::format("{:08X} ", w);
            if (readable(w)) {
              std::string str;
              for (int j = 0; j < 64; ++j) {
                if (((w + uint32_t(j)) & 0xFFF) == 0 &&
                    !readable(w + uint32_t(j))) {
                  break;
                }
                const uint8_t ch =
                    *mem->TranslateVirtual<uint8_t*>(w + uint32_t(j));
                if (ch < 0x20 || ch >= 0x7F) break;
                str.push_back(char(ch));
              }
              if (str.size() >= 4) line += "\"" + str + "\" ";
            }
          }
          XELOGI("TraceList   node {} @{:08X}: {}", n, node, line);
          const uint32_t next = rd(node + next_off);
          if (next == node) break;
          node = next;
        }
      });
    }
  }
  // HOST-SIDE (xui_glyph_2x): the XUI text scale follows the integer render
  // scale, which the app has fixed by now (upscale_to_window included).
  const uint32_t g2_scale = uint32_t(std::clamp(
      cvars::xui_glyph_scale > 0
          ? cvars::xui_glyph_scale
          : std::min(cvars::draw_resolution_scale_x,
                     cvars::draw_resolution_scale_y),
      1, 8));
  if (cvars::xui_glyph_2x && g2_scale >= 2) {
    // HOST-SIDE (xui_glyph_2x): bake XUI glyphs at N times their pixel size
    // (N = the render scale) while every layout value stays 1x, in xam
    // (Guide/HUD) and the statically linked copy in dash. Sites from static
    // analysis of xam and dash 17559, located in every other build by
    // research/glyph2x_sites.py + research/g2old.py (xam and dash 6770-17559;
    // 6770-8955 from runtime images, their modules are flash-patched):
    //  dpi     the renderer's DPI store: 96 -> 96N, so fonts are built at Nx.
    //  metric  the three x0.125 conversions from 8x font units to pixels:
    //          x1/N, so advances, boxes and line metrics stay 1x.
    //  scratch the x8.0 metric->scratch conversions (-8.0 before 12625's
    //          dash): xN, so the 8x oversampled scratch fits an Nx glyph.
    //          Both hooks scale the constant the previous instruction loaded,
    //          and only that value (a loop can re-enter the site).
    //  create  the cache page texture create: width and height xN. The
    //          allocator and the UV divisor keep the 1x page size, so UVs
    //          are unchanged and address the same fraction of an Nx texture.
    //          kCreateR3R4: xam <= 8955 passes (w, h) in r3/r4 to a helper
    //          with one caller; the hook sits on that call.
    //  write   the glyph write call: cell x, y, w, h and the 3/2 padding xN,
    //          so glyphs land in the Nx texture where the 1x UVs point.
    //          kWriteR7: xam <= 8955 passes x, y, w, h in r7..r10 and the
    //          padding at sp+0x54/0x5C; it rasterizes straight into the
    //          locked rect, so it has no second scratch conversion.
    //  phase   the rasterizer keeps a glyph's sub-pixel phase as |left| mod 8
    //          subsamples (one font pixel) and stores the scratch x offset
    //          (phase - left, left in r29) at sp+0x50. At Nx one font pixel
    //          is one texel, not one layout pixel, so glyphs whose 1x phase
    //          was >= 0.5 px landed a texel left of where the 1x quad expects
    //          (measured at 2x: letters up to 1 px off). Use mod 8N. The
    //          stored register comes from the site's own stw.
    // Not covered: the placeholder box drawn into each new page stays 1x, and
    // any glyph supplied by the precomputed lookup (xam 81790418) stays 1x.
    // Every dash loads at the same base, so one build's site is another
    // build's unrelated code (kCreate/kWrite are a bare mtctr). A hook acts
    // only for the build whose nine words around the site match exactly, so
    // other code and unlisted builds are left alone.
    enum class G2 { kDpi, kMetricF0, kMetricF31, kScratchF11, kScratchF12,
                    kCreate, kCreateR3R4, kWrite, kWriteR7, kPhase };
    struct G2Site {
      uint32_t addr;
      G2 kind;
      uint32_t words[9];  // site-4 .. site+4
    };
    static const G2Site kSites[] = {
#include "xenia/xui_glyph_2x_sites.inc"
    };
    XELOGI("XuiGlyph2x: text scale {}x, {} sites (HOST-SIDE)", g2_scale,
           std::size(kSites));
    auto* mem = memory_.get();
    static std::atomic<uint32_t> g2_counts[std::size(kSites)] = {};
    // One hook per address; it picks the build whose fingerprint matches.
    std::map<uint32_t, std::vector<size_t>> by_addr;
    for (size_t i = 0; i < std::size(kSites); ++i) {
      by_addr[kSites[i].addr].push_back(i);
    }
    for (auto& [addr, cands] : by_addr) {
      processor_->AddGuestHook(addr, [mem, cands,
                                      g2_scale](cpu::ppc::PPCContext* c) {
        const G2Site* sp = nullptr;
        size_t i = 0;
        for (size_t k : cands) {
          const G2Site& t = kSites[k];
          const uint8_t* ip = mem->TranslateVirtual(t.addr - 16);
          if (!ip) continue;
          bool match = true;
          for (int j = 0; j < 9 && match; ++j) {
            match = xe::load_and_swap<uint32_t>(ip + 4 * j) == t.words[j];
          }
          if (match) {
            sp = &t;
            i = k;
            break;
          }
        }
        if (!sp) return;
        const G2Site& s = *sp;
        const uint32_t n = ++g2_counts[i];
        const double inv = 1.0 / double(g2_scale);
        auto scale_stack = [&](std::initializer_list<uint32_t> offs) {
          for (uint32_t off : offs) {
            uint8_t* p = mem->TranslateVirtual(uint32_t(c->r[1]) + off);
            xe::store_and_swap<uint32_t>(
                p, xe::load_and_swap<uint32_t>(p) * g2_scale);
          }
        };
        // Metric/scratch sites scale the constant only while the register
        // still holds it: newer engines loop back to their first scratch
        // site (9222A0EC -> 9222A068 in dash 17559) without reloading f11.
        auto metric = [&](double& f) {
          if (f == 0.125) f = 0.125 * inv;
        };
        auto scratch = [&](double& f) {
          if (f == 8.0 || f == -8.0) f *= g2_scale;
        };
        switch (s.kind) {
          case G2::kDpi:
            c->f[31] *= g2_scale;
            break;
          case G2::kMetricF0:
            metric(c->f[0]);
            break;
          case G2::kMetricF31:
            metric(c->f[31]);
            break;
          case G2::kScratchF11:
            scratch(c->f[11]);
            break;
          case G2::kScratchF12:
            scratch(c->f[12]);
            break;
          case G2::kCreate:
            c->r[4] *= g2_scale;
            c->r[5] *= g2_scale;
            break;
          case G2::kCreateR3R4:
            c->r[3] *= g2_scale;
            c->r[4] *= g2_scale;
            break;
          case G2::kWrite:
            c->r[8] *= g2_scale;
            c->r[9] *= g2_scale;
            c->r[10] *= g2_scale;
            scale_stack({0x54u, 0x5Cu, 0x64u});
            break;
          case G2::kWriteR7:
            c->r[7] *= g2_scale;
            c->r[8] *= g2_scale;
            c->r[9] *= g2_scale;
            c->r[10] *= g2_scale;
            scale_stack({0x54u, 0x5Cu});
            break;
          case G2::kPhase: {
            // r29 = glyph left in subsamples (lha, sign-extended); the site
            // is `stw rS, 0x50(r1)`.
            const uint32_t rs = (s.words[4] >> 21) & 31;
            const int32_t left = int16_t(uint32_t(c->r[29]) & 0xFFFF);
            const int32_t phase =
                (left < 0 ? -left : left) % int32_t(8 * g2_scale);
            c->r[rs] = uint32_t(phase - left);
            break;
          }
        }
        if (n <= 2) {
          XELOGI("XuiGlyph2x: {:08X} kind {} hit {} (HOST-SIDE) r3={:X} "
                 "r4={:X} r5={:X} r7={:X} r8={:X} r9={:X} r10={:X} f31={}",
                 s.addr, int(s.kind), n, uint32_t(c->r[3]), uint32_t(c->r[4]),
                 uint32_t(c->r[5]), uint32_t(c->r[7]), uint32_t(c->r[8]),
                 uint32_t(c->r[9]), uint32_t(c->r[10]), c->f[31]);
        }
      });
    }
  }
  if (cvars::xui_glyph_dpi > 0.0) {
    // HOST-SIDE (xui_glyph_dpi): xam 17559's XUI text renderer init stores
    // its DPI with `stfs f31, 8(r31)` at 8178E30C; CreateFont (8178F640)
    // turns a font size into pixels as size * [renderer+8] / 72. Replace the
    // value about to be stored. Acts only if the instruction there is the
    // expected one, so another xam build is left alone.
    auto* mem = memory_.get();
    const float dpi = float(cvars::xui_glyph_dpi);
    processor_->AddGuestHook(0x8178E30C, [mem, dpi](cpu::ppc::PPCContext* c) {
      const uint32_t insn =
          xe::load_and_swap<uint32_t>(mem->TranslateVirtual(0x8178E30C));
      static std::atomic<uint32_t> hits{0};
      const uint32_t n = ++hits;
      if (insn != 0xD3FF0008u) {
        if (n == 1) {
          XELOGW("XuiGlyphDpi: 8178E30C is {:08X}, not xam 17559's stfs; "
                 "left unchanged",
                 insn);
        }
        return;
      }
      if (n <= 4) {
        XELOGI("XuiGlyphDpi: renderer {:08X} dpi {} -> {} (HOST-SIDE)",
               uint32_t(c->r[31]), c->f[31], dpi);
      }
      c->f[31] = dpi;
    });
  }

  XELOGI("{}: Initializing Audio...", __func__);
  // Initialize the APU.
  if (audio_system_factory) {
    audio_system_ = audio_system_factory(processor_.get());
    if (!audio_system_) {
      XELOGE("{}: Cannot initalize audio_system!", __func__);
      return X_STATUS_NOT_IMPLEMENTED;
    }
  }

  XELOGI("{}: Initializing Graphics...", __func__);
  // Initialize the GPU.
  graphics_system_ = graphics_system_factory();
  if (!graphics_system_) {
    XELOGE("{}: Cannot initalize graphics_system!", __func__);
    return X_STATUS_NOT_IMPLEMENTED;
  }

  XELOGI("{}: Initializing HID...", __func__);
  // Initialize the HID.
  input_system_ = std::make_unique<xe::hid::InputSystem>(display_window_);
  if (!input_system_) {
    XELOGE("{}: Cannot initalize input_system!", __func__);
    return X_STATUS_NOT_IMPLEMENTED;
  }
  if (input_driver_factory) {
    auto input_drivers = input_driver_factory(display_window_);
    for (size_t i = 0; i < input_drivers.size(); ++i) {
      input_system_->AddDriver(std::move(input_drivers[i]));
    }
  }

  result = input_system_->Setup();
  if (result) {
    return result;
  }

  // Add inputSystem to UI
  imgui_drawer_->LoadInputSystem(input_system_.get());

  XELOGI("{}: Initializing VFS...", __func__);
  // Bring up the virtual filesystem used by the kernel.
  file_system_ = std::make_unique<xe::vfs::VirtualFileSystem>();

  patcher_ = std::make_unique<xe::patcher::Patcher>(storage_root_);

  XELOGI("{}: Initializing Kernel...", __func__);
  // Shared kernel state.
  kernel_state_ = std::make_unique<xe::kernel::KernelState>(this);
#define LOAD_KERNEL_MODULE(t) \
  static_cast<void>(kernel_state_->LoadKernelModule<kernel::t>())
  // HLE kernel modules.
  LOAD_KERNEL_MODULE(xboxkrnl::XboxkrnlModule);
  LOAD_KERNEL_MODULE(xam::XamModule);

  // 415608C3 anti-cheat checks if XDBM is loaded.
  if (cvars::console_type >= 0) {
    LOAD_KERNEL_MODULE(xbdm::XbdmModule);
  }
#undef LOAD_KERNEL_MODULE
  plugin_loader_ = std::make_unique<xe::patcher::PluginLoader>(
      kernel_state_.get(), storage_root() / "plugins");

  XELOGI("{}: Starting graphics_system...", __func__);
  // Setup the core components.
  result = graphics_system_->Setup(
      processor_.get(), kernel_state_.get(),
      display_window_ ? &display_window_->app_context() : nullptr,
      display_window_ != nullptr);
  if (result) {
    XELOGE("{}: Failed to setup graphics_system!", __func__);
    return result;
  }

  if (audio_system_) {
    XELOGI("{}: Starting audio_system...", __func__);
    result = audio_system_->Setup(kernel_state_.get());
    if (result) {
      XELOGE("{}: Failed to setup audio_system!", __func__);
      return result;
    }
    audio_media_player_ = std::make_unique<apu::AudioMediaPlayer>(
        audio_system_.get(), kernel_state_.get());
    audio_media_player_->Setup();
  }

  // Initialize emulator fallback exception handling last.
  ExceptionHandler::Install(Emulator::ExceptionCallbackThunk, this);

  return result;
}

// Phase 1055 menus: a logging trampoline on hud's xam imports. The import
// thunk reads its target from the import address table; the entry is pointed
// at a host builtin that logs the call and runs the real (LLE) function with
// the same registers, so nothing changes for hud but the log.
namespace {
struct GuideXamTraceRec {
  std::string name;
  uint32_t real = 0;
  uint32_t ordinal = 0;
  std::atomic<uint32_t> count{0};
};
constexpr size_t kGuideXamTraceSlots = 192;
GuideXamTraceRec g_guide_xam_trace[kGuideXamTraceSlots];
size_t g_guide_xam_trace_used = 0;
void GuideXamTraceCall(xe::cpu::ppc::PPCContext* ctx, kernel::KernelState* ks, GuideXamTraceRec* rec) {
  const uint32_t n = ++rec->count;
  const bool log = n <= 24 || (n % 2000) == 0;
  const uint32_t a3 = uint32_t(ctx->r[3]), a4 = uint32_t(ctx->r[4]), a5 = uint32_t(ctx->r[5]),
                 a6 = uint32_t(ctx->r[6]), a7 = uint32_t(ctx->r[7]), a8 = uint32_t(ctx->r[8]);
  const uint32_t lr = uint32_t(ctx->lr);
  if (log) {
    XELOGI("GuideXamCall: {} #{} ({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) from {:08X}",
           rec->name, n, a3, a4, a5, a6, a7, a8, lr);
  }
  if (ks && ks->processor() && rec->real) ks->processor()->Execute(ctx->thread_state, rec->real);
  if (log) XELOGI("GuideXamCall: {} #{} -> {:08X}", rec->name, n, uint32_t(ctx->r[3]));
}
template <size_t I>
void GuideXamTraceThunk(xe::cpu::ppc::PPCContext* ctx, kernel::KernelState* ks) {
  GuideXamTraceCall(ctx, ks, &g_guide_xam_trace[I]);
}
template <size_t... I>
constexpr std::array<xe::cpu::GuestFunction::ExternHandler, sizeof...(I)> GuideXamTraceTable(std::index_sequence<I...>) {
  return {{&GuideXamTraceThunk<I>...}};
}
const auto g_guide_xam_trace_handlers = GuideXamTraceTable(std::make_index_sequence<kGuideXamTraceSlots>{});
// Phase 1055 menus: hud's scene teardown (913F75B0) waits on a task it
// scheduled (XamTaskSchedule) after cancelling it; xam's task pool workers are
// created by xam's own boot and never run here, so the wait never returned
// and the paint thread froze (Quick Launch's B). hud ignores the wait's
// result and closes the handle right after: return at once while pending.
uint32_t g_guide_xam_task_getstatus = 0, g_guide_xam_task_wait_real = 0;
uint32_t g_guide_xam_app_load_real = 0;
// Phase 1096: hud shows exactly ONE message box (XamShowMessageBox, coverage
// 51/79 calls=1, from 913E7370) and passes a parent handle. xam posts the
// button result 0x7EC back to that handle via 8194A4E0, which DROPS the message
// and returns 0x8030000A if the handle does not resolve (8194A568..8194A58C).
// Capture the handle here so the per-paint probe can say whether it still
// resolves. Read-only: the real export still runs and its arguments are
// untouched.
uint32_t g_guide_xam_show_msgbox_real = 0;
// Phase 1056: XamAppLoad stores through the app manager's task at
// [81D43C50+4]. When that slot is null (xam's boot never ran and the
// bootstrap's own creation failed) the store faults the calling thread, so
// refuse the load instead and say so.
void GuideXamShowMessageBoxExtern(xe::cpu::ppc::PPCContext* ctx,
                                  kernel::KernelState* ks) {
  g_guide_msgbox_parent = uint32_t(ctx->r[3]);
  XELOGI("GuideMsgBox: XamShowMessageBox parent handle {:08X} buttons {} from "
         "{:08X} - watching whether it still resolves when the button result "
         "0x7EC is posted back to it",
         g_guide_msgbox_parent, uint32_t(ctx->r[6]), uint32_t(ctx->lr));
  if (g_guide_xam_show_msgbox_real && ks && ks->processor()) {
    ks->processor()->Execute(ctx->thread_state, g_guide_xam_show_msgbox_real);
  }
}
void GuideXamAppLoadExtern(xe::cpu::ppc::PPCContext* ctx, kernel::KernelState* ks) {
  const uint32_t task = ks ? xe::load_and_swap<uint32_t>(
                                 ks->memory()->TranslateVirtual(0x81D43C54u))
                           : 0;
  if (!task) {
    static uint32_t logs = 0;
    if (logs++ < 8) {
      XELOGW("GuideAppLoad: the app manager has no task at [81D43C50+4]; refusing the load (would fault at 8177B2F0)");
    }
    ctx->r[3] = 0x80004005u;
    return;
  }
  // Phase 1079: log here rather than relying on guide_trace_xam_imports. The
  // trace installs its own extern on the same thunk, and SetupExtern REPLACES
  // the handler, so tracing XamAppLoad used to silently disable this very
  // guard - the fault at 8177B2F0 it exists to prevent would have come back
  // whenever the trace was on and the app manager had no task.
  static uint32_t app_load_n = 0;
  const uint32_t n = ++app_load_n;
  if (n <= 24) {
    // Phase 1081: print the state BEFORE the real call too - a once-a-paint
    // sampler cannot say whether XamAppLoad FOUND state 2 or SET it.
    auto rdb = [&](uint32_t a) {
      return ks ? xe::load_and_swap<uint32_t>(ks->memory()->TranslateVirtual(a)) : 0u;
    };
    XELOGI("GuideXamCall: XamAppLoad #{} ({:08X} {:08X} {:08X}) from {:08X} | "
           "BEFORE [81D43C50]={:08X} (81793AA0 asserts == 1)", n,
           uint32_t(ctx->r[3]), uint32_t(ctx->r[4]), uint32_t(ctx->r[5]),
           uint32_t(ctx->lr), rdb(0x81D43C50u));
  }
  if (g_guide_xam_app_load_real && ks && ks->processor()) {
    ks->processor()->Execute(ctx->thread_state, g_guide_xam_app_load_real);
  }
  if (n <= 24) {
    // Phase 1080: 81793AA0 asserts [81D43C50] == 1 (a silent twui here) and
    // then bails to 81793D10 unless BOTH [81D43C50+0xB4] and [+0xB8] are
    // non-null - which is how it can return S_OK and load nothing. Print the
    // fields with the result so the early-out is visible.
    auto rdf = [&](uint32_t a) {
      return ks ? xe::load_and_swap<uint32_t>(ks->memory()->TranslateVirtual(a)) : 0u;
    };
    XELOGI("GuideXamCall: XamAppLoad #{} -> {:08X} (real {:08X} executed) | "
           "[81D43C50]={:08X} [+B4]={:08X} [+B8]={:08X} [+E0]={:08X} [+4 task]={:08X}",
           n, uint32_t(ctx->r[3]), g_guide_xam_app_load_real, rdf(0x81D43C50u),
           rdf(0x81D43D04u), rdf(0x81D43D08u), rdf(0x81D43D30u), rdf(0x81D43C54u));
  }
}
void GuideXamTaskWaitExtern(xe::cpu::ppc::PPCContext* ctx, kernel::KernelState* ks) {
  const uint32_t task = uint32_t(ctx->r[3]);
  uint32_t status = 0;
  if (g_guide_xam_task_getstatus && ks && ks->processor()) {
    ks->processor()->Execute(ctx->thread_state, g_guide_xam_task_getstatus);
    status = uint32_t(ctx->r[3]);
    ctx->r[3] = task;
  }
  if (status == 0x8000000Au) {
    static uint32_t logs = 0;
    if (logs++ < 8) {
      XELOGI("GuideTask: XamTaskWaitOnCompletion({:08X}) on a pending task: xam's task workers never run here, not waiting", task);
    }
    ctx->r[3] = 0x8000000Au;
    return;
  }
  if (g_guide_xam_task_wait_real && ks && ks->processor()) {
    ks->processor()->Execute(ctx->thread_state, g_guide_xam_task_wait_real);
  }
}
}  // namespace

void Emulator::GuideInstallXamImportTrace(kernel::UserModule* hud) {
  // Phase 1055 menus: (1) the fix hooks, always; (2) the trace hooks, with
  // guide_trace_xam_imports. Both replace an import thunk's lis/ori with the
  // extern-call pattern and give the declared thunk Function a handler.
  auto* xm = hud->xex_module();
  auto* er = export_resolver();
  auto* mem = memory();
  if (!xm || !er || !mem) return;
  static const char* const kSkip[] = {"XamInput", "XamUserGet", "XamUserCheck", "XamUserRead",
                                      "XamGetSystemVersion", "XamGetLocale", "XamLoaderGetMediaInfo",
                                      "XamIsCurrentTitleDash", "XamGetOverlappedResult",
                                      "XamContentGetLicenseMask", "XamGetExecutionId", "XamUserIsOnlineEnabled"};
  static const char* const kXui[] = {"XuiSendMessage", "XuiBubbleMessage", "XuiBroadcastMessage", "XuiSceneNavigate",
                                     "XuiSceneCreate", "XuiSetFocus", "XuiElementSetFocus", "XuiElementSetShow",
                                     "XuiElementPlayTimeline", "XuiDestroyObject", "XuiElementDiscardResources",
                                     "XuiScenePlay", "XuiSceneInterrupt", "XuiElementBeginShow", "XuiElementEndShow",
                                     "XuiSceneSetInputHandled", "XuiElementSetInput", "XuiControlSetEnable",
                                     "XuiSceneClose", "XuiSceneGetNavigator", "XuiSetTimer", "XuiKillTimer",
                                     "XuiElementAddChild", "XuiElementRemoveChild", "XuiCreateObject", "XuiElementSetVisual"};
  // The LLE resolver rewrote each thunk to `lis r11,hi; ori r11,r11,lo; mtctr
  // r11; bctr` (xex_module.cc) and the import record's page is not readable
  // after the load, so the target comes from the thunk's two words. Read them
  // all first: a hook overwrites them.
  struct Imp { std::string name; uint32_t thunk, real, ordinal; };
  std::vector<Imp> imps;
  for (const auto& lib : *xm->import_libraries()) {
    if (lib.name.find("xam") == std::string::npos) continue;
    for (const auto& fn : lib.imports) {
      if (!fn.thunk_address || !fn.value_address) continue;
      auto* ex = er->GetExportByOrdinal(lib.name, fn.ordinal);
      std::string n = (ex && ex->name) ? std::string(ex->name) : fmt::format("xam_{:03X}", fn.ordinal);
      const uint32_t w0 = xe::load_and_swap<uint32_t>(mem->TranslateVirtual(fn.thunk_address));
      const uint32_t w1 = xe::load_and_swap<uint32_t>(mem->TranslateVirtual(fn.thunk_address + 4u));
      if ((w0 & 0xFFFF0000u) != 0x3D600000u || (w1 & 0xFFFF0000u) != 0x616B0000u) continue;
      imps.push_back({n, fn.thunk_address, ((w0 & 0xFFFFu) << 16) | (w1 & 0xFFFFu), fn.ordinal});
    }
  }
  auto hook = [&](const Imp& im, xe::cpu::GuestFunction::ExternHandler h, const char* why) -> bool {
    auto* sym = xm->LookupSymbol(im.thunk, false);
    auto* gf = (sym && sym->type() == xe::cpu::Symbol::Type::kFunction)
                   ? static_cast<xe::cpu::GuestFunction*>(sym)
                   : nullptr;
    if (!gf) {
      XELOGI("GuideXamHook: {} thunk {:08X}: no declared function: skipped", im.name, im.thunk);
      return false;
    }
    const uint32_t w0 = xe::load_and_swap<uint32_t>(mem->TranslateVirtual(im.thunk));
    const uint32_t w1 = xe::load_and_swap<uint32_t>(mem->TranslateVirtual(im.thunk + 4u));
    // the pattern the loader writes for HLE exports (xex_module.cc): the
    // extern-call instruction and a blr; the JIT calls the handler for a bl
    // to this address and for the instruction itself
    if (!kernel::xboxkrnl::GuidePatchWord(im.thunk, w0, 0x44000042u, "GuideXamHook") ||
        !kernel::xboxkrnl::GuidePatchWord(im.thunk + 4u, w1, 0x4E800020u, "GuideXamHook")) {
      return false;
    }
    gf->SetupExtern(h, nullptr);
    XELOGI("GuideXamHook: {} ord {:03X}: thunk {:08X} -> real {:08X} ({})", im.name, im.ordinal, im.thunk, im.real, why);
    return true;
  };
  // (1) the fixes
  for (const auto& im : imps) {
    if (im.name == "XamTaskGetStatus") g_guide_xam_task_getstatus = im.real;
    if (im.name == "XamAppLoad") g_guide_xam_app_load_real = im.real;
    if (im.name == "XamShowMessageBox") g_guide_xam_show_msgbox_real = im.real;
  }
  for (const auto& im : imps) {
    if (im.name == "XamAppLoad") {
      hook(im, GuideXamAppLoadExtern, "refuse the load when the app manager has no task");
    }
    if (im.name == "XamShowMessageBox") {
      hook(im, GuideXamShowMessageBoxExtern, "record the dialog's parent handle");
    }
  }
  for (const auto& im : imps) {
    if (im.name == "XamTaskWaitOnCompletion") {
      g_guide_xam_task_wait_real = im.real;
      hook(im, GuideXamTaskWaitExtern, "no wait on a pending task: xam's task workers never run here");
    }
  }
  // (2) the trace
  if (!cvars::guide_trace_xam_imports) return;
  int installed = 0;
  for (const auto& im : imps) {
    const std::string& n = im.name;
    // Phase 1079: never let the trace overwrite a FIX hook - SetupExtern
    // replaces the handler, it does not chain. Both of these log their own
    // calls, so nothing is lost by skipping them here.
    if (n == "XamTaskWaitOnCompletion") continue;  // hooked above
    if (n == "XamAppLoad") continue;               // hooked above
    bool xui_ok = false;
    for (const char* s : kXui) {
      if (n.rfind(s, 0) == 0) xui_ok = true;
    }
    if (!(n.rfind("Xam", 0) == 0 || n.rfind("XNotify", 0) == 0 || n.rfind("XMsg", 0) == 0 || xui_ok)) continue;
    bool skip = false;
    for (const char* s : kSkip) {
      if (n.rfind(s, 0) == 0) skip = true;
    }
    if (skip) continue;
    if (g_guide_xam_trace_used >= kGuideXamTraceSlots) break;
    const size_t slot = g_guide_xam_trace_used;
    auto& rec = g_guide_xam_trace[slot];
    rec.name = n;
    rec.real = im.real;
    rec.ordinal = im.ordinal;
    if (!hook(im, g_guide_xam_trace_handlers[slot], "trace")) continue;
    ++g_guide_xam_trace_used;
    ++installed;
  }
  XELOGI("GuideXamTrace: {} of {} xam imports traced", installed, imps.size());
}

X_STATUS Emulator::TerminateTitle() {
  if (!is_title_open()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  kernel::xboxkrnl::GuidePaintThreadStop();  // phase 1055 bugs: before the threads go
  kernel_state_->TerminateTitle();
  title_id_ = std::nullopt;
  title_name_ = "";
  title_version_ = "";
  on_terminate();
  return X_STATUS_SUCCESS;
}

// Host Power Off: detached diagnostic threads that poll guest memory count
// themselves here and exit once stop is set, before Memory is destroyed.
std::atomic<bool> g_memory_pollers_stop{false};
std::atomic<int> g_memory_pollers{0};

void kernel::ResetModuleStateForPowerOff() {
  kernel::ResetXObjectStateForPowerOff();
  kernel::ResetXTimerStateForPowerOff();
  kernel::xboxkrnl::ResetObStateForPowerOff();
  kernel::xboxkrnl::ResetXInputdStateForPowerOff();
  kernel::xboxkrnl::ResetAniStateForPowerOff();
  kernel::xboxkrnl::ResetVideoStateForPowerOff();
  kernel::xboxkrnl::ResetMiscStateForPowerOff();
  kernel::xboxkrnl::ResetIoStateForPowerOff();
  kernel::xboxkrnl::ResetCryptStateForPowerOff();
  kernel::xboxkrnl::ResetAudioStateForPowerOff();
  kernel::xboxkrnl::ResetLzxStateForPowerOff();
  kernel::xboxkrnl::ResetMemoryStateForPowerOff();
  kernel::xboxkrnl::ResetModulesStateForPowerOff();
  kernel::ResetXThreadStateForPowerOff();
}

void Emulator::PowerOff() {
  XELOGI("Power: powering off (title open: {})", is_title_open());
  awaiting_power_on_ = false;
  kernel::xboxkrnl::GuidePaintThreadStop();
  // Every guest thread - title, LLE xam, hud, the boot animation - the way a
  // title switch stops title threads. Not KernelState::TerminateTitle: its
  // StepToGuestSafePoint never returns for a thread parked in a wait
  // (measured: Power Off hung there with the dashboard up).
  // Host XThreads too: most run guest code (SMC, ani, timers, XInputd RGC,
  // "Boot Launch (xam)", which the app's WaitUntilExit waits on) and have no
  // owner to stop them; the audio worker blocks forever inside xam's audio
  // callback once xam's threads are gone (measured: AudioSystem::Shutdown
  // hung). Spared: host threads whose owner stops them on destruction - the
  // GPU and XMA workers, and kernel dispatch (killed inside its condition
  // variable wait, ~KernelState's notify_all faulted - measured).
  std::vector<kernel::XThread*> spared = {kernel_state_->dispatch_thread()};
  if (graphics_system_) {
    spared.push_back(graphics_system_->frame_limiter_thread());
    if (graphics_system_->command_processor()) {
      spared.push_back(graphics_system_->command_processor()->worker_thread());
    }
  }
  if (audio_system_ && audio_system_->xma_decoder()) {
    spared.push_back(audio_system_->xma_decoder()->worker_thread());
  }
  g_memory_pollers_stop = true;
  for (int i = 0; i < 2000 && g_memory_pollers > 0; ++i) {
    xe::threading::Sleep(std::chrono::milliseconds(1));
  }
  // Left set if one is still running, so it cannot outlive its Memory.
  if (g_memory_pollers == 0) g_memory_pollers_stop = false;
  kernel_state_->TerminateGuestThreadsSafely(
      [&spared](kernel::XThread* thread) {
        return std::find(spared.begin(), spared.end(), thread) == spared.end();
      },
      "Power", false);
  title_id_ = std::nullopt;
  title_name_ = "";
  title_version_ = "";
  on_terminate();
}

const std::unique_ptr<vfs::Device> Emulator::CreateVfsDevice(
    const std::filesystem::path& path, const std::string_view mount_path) {
  // Must check if the type has changed e.g. XamSwapDisc
  switch (GetFileSignature(path)) {
    case FileSignatureType::XEX0:
    case FileSignatureType::XEXQ:
    case FileSignatureType::XEXH:
    case FileSignatureType::XEX25:
    case FileSignatureType::XEX1:
    case FileSignatureType::XEX2:
    case FileSignatureType::ELF: {
      auto parent_path = path.parent_path();
      return std::make_unique<vfs::HostPathDevice>(
          mount_path, parent_path, !cvars::allow_game_relative_writes);
    } break;
    case FileSignatureType::LIVE:
    case FileSignatureType::CON:
    case FileSignatureType::PIRS: {
      return vfs::XContentContainerDevice::CreateContentDevice(mount_path,
                                                               path);
    } break;
    case FileSignatureType::XISO: {
      return std::make_unique<vfs::DiscImageDevice>(mount_path, path);
    } break;
    case FileSignatureType::ZAR: {
      return std::make_unique<vfs::DiscZarchiveDevice>(mount_path, path);
    } break;
    case FileSignatureType::XBE:
    case FileSignatureType::EXE:
    case FileSignatureType::Unknown:
    default:
      return nullptr;
      break;
  }
}

uint64_t Emulator::GetPersistentEmulatorFlags() {
#if XE_PLATFORM_WIN32 == 1
  uint64_t value = 0;
  DWORD value_size = sizeof(value);
  HKEY xenia_hkey = nullptr;
  LSTATUS lstat =
      RegOpenKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Xenia", &xenia_hkey);
  if (!xenia_hkey) {
    // let the Set function create the key and initialize it to 0
    SetPersistentEmulatorFlags(0ULL);
    return 0ULL;
  }

  lstat = RegQueryValueExA(xenia_hkey, "XEFLAGS", 0, NULL,
                           reinterpret_cast<LPBYTE>(&value), &value_size);
  RegCloseKey(xenia_hkey);
  if (lstat) {
    return 0ULL;
  }
  return value;
#else
  return EmulatorFlagDisclaimerAcknowledged;
#endif
}
void Emulator::SetPersistentEmulatorFlags(uint64_t new_flags) {
#if XE_PLATFORM_WIN32 == 1
  uint64_t value = new_flags;
  DWORD value_size = sizeof(value);
  HKEY xenia_hkey = nullptr;
  LSTATUS lstat =
      RegOpenKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Xenia", &xenia_hkey);
  if (!xenia_hkey) {
    lstat = RegCreateKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Xenia", &xenia_hkey);
  }

  lstat = RegSetValueExA(xenia_hkey, "XEFLAGS", 0, REG_QWORD,
                         reinterpret_cast<const BYTE*>(&value), 8);
  RegFlushKey(xenia_hkey);
  RegCloseKey(xenia_hkey);
#endif
}

X_STATUS Emulator::MountPath(const std::filesystem::path& path,
                             const std::string_view mount_path) {
  auto device = CreateVfsDevice(path, mount_path);
  if (!device || !device->Initialize()) {
    XELOGE(
        "Unable to mount the selected file, it is an unsupported format or "
        "corrupted.");
    return X_STATUS_NO_SUCH_FILE;
  }
  if (!file_system_->RegisterDevice(std::move(device))) {
    XELOGE("Unable to register the input file to {}.", mount_path);
    return X_STATUS_NO_SUCH_FILE;
  }

  file_system_->UnregisterSymbolicLink(kDefaultPartitionSymbolicLink);
  file_system_->UnregisterSymbolicLink(kDefaultGameSymbolicLink);
  file_system_->UnregisterSymbolicLink("plugins:");

  // Create symlinks to the device.
  file_system_->RegisterSymbolicLink(kDefaultGameSymbolicLink, mount_path);
  file_system_->RegisterSymbolicLink(kDefaultPartitionSymbolicLink, mount_path);

  return X_STATUS_SUCCESS;
}

Emulator::FileSignatureType Emulator::GetFileSignature(
    const std::filesystem::path& path) {
  FILE* file = xe::filesystem::OpenFile(path, "rb");

  if (!file) {
    return FileSignatureType::Unknown;
  }

  const uint64_t file_size = std::filesystem::file_size(path);
  constexpr int64_t header_size = 4;

  if (file_size < header_size) {
    return FileSignatureType::Unknown;
  }

  char file_magic[header_size];
  fread(file_magic, sizeof(file_magic), 1, file);

  fourcc_t magic_value =
      make_fourcc(file_magic[0], file_magic[1], file_magic[2], file_magic[3]);

  fclose(file);

  switch (magic_value) {
    case xe::cpu::kXEX0Signature:
      return FileSignatureType::XEX0;
    case xe::cpu::kXEXQSignature:
      return FileSignatureType::XEXQ;
    case xe::cpu::kXEXHSignature:
      return FileSignatureType::XEXH;
    case xe::cpu::kXEX25Signature:
      return FileSignatureType::XEX25;
    case xe::cpu::kXEX1Signature:
      return FileSignatureType::XEX1;
    case xe::cpu::kXEX2Signature:
      return FileSignatureType::XEX2;
    case xe::vfs::kCONSignature:
      return FileSignatureType::CON;
    case xe::vfs::kLIVESignature:
      return FileSignatureType::LIVE;
    case xe::vfs::kPIRSSignature:
      return FileSignatureType::PIRS;
    case xe::vfs::kXSFSignature:
      return FileSignatureType::XISO;
    case xe::cpu::kXBESignature:
      return FileSignatureType::XBE;
    case xe::cpu::kElfSignature:
      return FileSignatureType::ELF;
    default:
      break;
  }

  magic_value = make_fourcc(file_magic[0], file_magic[1], 0, 0);

  if (xe::kernel::kEXESignature == magic_value) {
    return FileSignatureType::EXE;
  }

  file = xe::filesystem::OpenFile(path, "rb");
  xe::filesystem::Seek(file, -header_size, SEEK_END);
  fread(file_magic, 1, header_size, file);
  fclose(file);

  magic_value =
      make_fourcc(file_magic[0], file_magic[1], file_magic[2], file_magic[3]);

  if (xe::vfs::kZarMagic == magic_value) {
    return FileSignatureType::ZAR;
  }

  // Check if XISO
  std::unique_ptr<vfs::Device> device =
      std::make_unique<vfs::DiscImageDevice>("", path);

  XELOGI("Checking for XISO");

  if (device->Initialize()) {
    return FileSignatureType::XISO;
  }

  XELOGE("{}: {} ({:08X})", __func__, path.extension(), magic_value);
  return FileSignatureType::Unknown;
}

X_STATUS Emulator::LaunchPath(const std::filesystem::path& path) {
  X_STATUS mount_result = X_STATUS_SUCCESS;

  // Phase 1099n: a VIRTUAL HARD DRIVE. xam registers \Device\Harddisk0\
  // Partition1 as a Hard Drive (with XboxHardwareInfo bit 0x20) and needs it
  // WRITABLE - its cache setup creates Partition1\Cache (measured C0000022
  // against the read-only title folder) and profiles are written there. Mount
  // a dedicated host folder there for every title type, and move an .xex
  // title's own folder to a separate read-only device so it is never written.
  const bool virtual_hdd = !cvars::guide_hdd_path.empty();
  if (virtual_hdd) {
    std::filesystem::path hdd = cvars::guide_hdd_path;
    std::error_code ec;
    std::filesystem::create_directories(hdd, ec);
    auto hdd_device = std::make_unique<vfs::HostPathDevice>(
        "\\Device\\Harddisk0\\Partition1", hdd, false);
    // Phase 1099z159: xam 17559 reads this volume's size for storage devices
    // and save locations; Xenia's fixed answer is a 64 MB drive.
    hdd_device->set_model_bytes(uint64_t(std::max(cvars::kernel_hdd_size_gb, 0)) *
                                1000 * 1000 * 1000);
    if (hdd_device->Initialize() &&
        file_system_->RegisterDevice(std::move(hdd_device))) {
      // The dash's own \??\HDD: link (it creates it only for EDID uploads);
      // file://HDD:/... box art paths need it (InstallCoverArtHooks).
      if (cvars::guide_game_library_cover_art) {
        file_system_->RegisterSymbolicLink("HDD:",
                                           "\\Device\\Harddisk0\\Partition1");
      }
      XELOGI("VirtualHDD: \\Device\\Harddisk0\\Partition1 -> {} (writable)",
             xe::path_to_utf8(hdd));
    } else {
      XELOGE("VirtualHDD: could not mount {}", xe::path_to_utf8(hdd));
    }
  }

  switch (GetFileSignature(path)) {
    case FileSignatureType::XEX0:
    case FileSignatureType::XEXQ:
    case FileSignatureType::XEXH:
    case FileSignatureType::XEX25:
    case FileSignatureType::XEX1:
    case FileSignatureType::XEX2:
    case FileSignatureType::ELF: {
      mount_result = MountPath(path, virtual_hdd
                                         ? "\\Device\\TitleXex"
                                         : "\\Device\\Harddisk0\\Partition1");
      return mount_result ? mount_result : LaunchXexFile(path);
    } break;
    case FileSignatureType::LIVE:
    case FileSignatureType::CON:
    case FileSignatureType::PIRS: {
      mount_result = MountPath(path, "\\Device\\Package_0");
      return mount_result ? mount_result : LaunchStfsContainer(path);
    } break;
    case FileSignatureType::XISO: {
      mount_result = MountPath(path, "\\Device\\Cdrom0");
      return mount_result ? mount_result : LaunchDiscImage(path);
    } break;
    case FileSignatureType::XBE: {
      XELOGE("OG Xbox games are not supported");
      return X_STATUS_NOT_SUPPORTED;
    } break;
    case FileSignatureType::ZAR: {
      mount_result = MountPath(path, "\\Device\\Cdrom0");
      return mount_result ? mount_result : LaunchDiscArchive(path);
    } break;
    case FileSignatureType::EXE:
    case FileSignatureType::Unknown:
    default:
      return X_STATUS_NOT_SUPPORTED;
      break;
  }
}

X_STATUS Emulator::LaunchXexFile(const std::filesystem::path& path) {
  // We create a virtual filesystem pointing to its directory and symlink
  // that to the game filesystem.
  // e.g., /my/files/foo.xex will get a local fs at:
  // \\Device\\Harddisk0\\Partition1
  // and then get that symlinked to game:\, so
  // -> game:\foo.xex
  // Get just the filename (foo.xex).
  auto file_name = path.filename();

  // Launch the game.
  auto fs_path = fmt::format("{}\\", kDefaultGameSymbolicLink) +
                 xe::path_to_utf8(file_name);
  X_STATUS result = CompleteLaunch(path, fs_path);

  if (XFAILED(result)) {
    return result;
  }

  kernel_state_->deployment_type_ = XDeploymentType::kInstalledToHDD;

  if (!kernel::IsSystemTitle(kernel_state_->title_id())) {
    return result;
  }

  const std::string mount_path =
      utf8::find_base_guest_path(kernel_state_->GetExecutableModule()->path());

  // System related symlinks. This should point to dashboard location in the
  // future.
  XELOGI("System title: registering SystemRoot -> '{}' (exe '{}')", mount_path, kernel_state_->GetExecutableModule()->path());
  file_system_->RegisterSymbolicLink("\\SystemRoot", mount_path);

  auto module = kernel_state_->LoadUserModule("xam.xex");

  if (!module) {
    module = kernel_state_->LoadUserModule("$flash_xam.xex");
  }

  if (module) {
    result = kernel_state_->FinishLoadingUserModule(module, false);
  }

  return result;
}

X_STATUS Emulator::LaunchDiscImage(const std::filesystem::path& path) {
  std::string module_path = FindLaunchModule();
  X_STATUS result = CompleteLaunch(path, module_path);

  if (result == X_STATUS_NOT_FOUND && !cvars::launch_module.empty()) {
    return LaunchDefaultModule(path);
  }
  kernel_state_->deployment_type_ = XDeploymentType::kOpticalDisc;
  return result;
}

X_STATUS Emulator::LaunchDiscArchive(const std::filesystem::path& path) {
  std::string module_path = FindLaunchModule();
  X_STATUS result = CompleteLaunch(path, module_path);

  if (result == X_STATUS_NOT_FOUND && !cvars::launch_module.empty()) {
    return LaunchDefaultModule(path);
  }
  kernel_state_->deployment_type_ = XDeploymentType::kOpticalDisc;
  return result;
}

X_STATUS Emulator::LaunchStfsContainer(const std::filesystem::path& path) {
  std::string module_path = FindLaunchModule();
  X_STATUS result = CompleteLaunch(path, module_path);

  if (result == X_STATUS_NOT_FOUND && !cvars::launch_module.empty()) {
    return LaunchDefaultModule(path);
  }
  kernel_state_->deployment_type_ = XDeploymentType::kDownload;
  return result;
}

X_STATUS Emulator::LaunchDefaultModule(const std::filesystem::path& path) {
  cvars::launch_module = "";
  std::string module_path = FindLaunchModule();
  X_STATUS result = CompleteLaunch(path, module_path);

  if (XSUCCEEDED(result)) {
    kernel_state_->deployment_type_ = XDeploymentType::kInstalledToHDD;
    auto title_id = kernel_state_->title_id();
    if (!kernel::IsSystemTitle(title_id)) {
      // Assumption that any loaded game is loaded as a disc.
      kernel_state_->deployment_type_ = XDeploymentType::kOpticalDisc;
    }
  }
  return result;
}

X_STATUS Emulator::DataMigration(const uint64_t xuid) {
  uint32_t failure_count = 0;
  const std::string xuid_string = fmt::format("{:016X}", xuid);
  const std::string common_xuid_string = fmt::format("{:016X}", 0);
  const std::filesystem::path path_to_profile_data =
      content_root_ / xuid_string / "FFFE07D1" / "00010000" / xuid_string;
  // Filter directories inside. First we need to find any content type
  // directories.
  // Savefiles must go to user specific directory
  // Everything else goes to common
  const auto titles_to_move = xe::filesystem::FilterByName(
      xe::filesystem::ListDirectories(content_root_),
      std::regex("[A-F0-9]{8}"));

  for (const auto& title : titles_to_move) {
    if (xe::path_to_utf8(title.name) == "FFFE07D1" ||
        xe::path_to_utf8(title.name) == "00000000") {
      // SKip any dashboard/profile related data that was previously installed
      continue;
    }

    const auto content_type_dirs = xe::filesystem::FilterByName(
        xe::filesystem::ListDirectories(title.path / title.name),
        std::regex("[A-F0-9]{8}"));

    for (const auto& content_type : content_type_dirs) {
      const std::string used_xuid =
          xe::path_to_utf8(content_type.name) == "00000001"
              ? xuid_string
              : common_xuid_string;

      const auto previous_path = content_root_ / title.name / content_type.name;
      const auto path = content_root_ / used_xuid / title.name;

      if (!std::filesystem::exists(path)) {
        std::filesystem::create_directories(path);
      }

      std::error_code ec;
      std::filesystem::rename(previous_path, path / content_type.name, ec);

      if (ec) {
        failure_count++;
        XELOGW("{}: Moving from: {} to: {} failed! Error message: {} ({:08X})",
               __func__, previous_path, path / content_type.name, ec.message(),
               ec.value());
      }
    }
    // Other directories:
    // Headers - Just copy everything to both common and xuid locations
    // profile - ?
    if (std::filesystem::exists(title.path / title.name / "Headers")) {
      const auto xuid_path =
          content_root_ / xuid_string / title.name / "Headers";

      std::filesystem::create_directories(xuid_path);

      std::error_code ec;
      // Copy to specific user
      std::filesystem::copy(title.path / title.name / "Headers", xuid_path,
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::skip_existing,
                            ec);
      if (ec) {
        failure_count++;
        XELOGW("{}: Copying from: {} to: {} failed! Error message: {} ({:08X})",
               __func__, title.path / title.name / "Headers", xuid_path,
               ec.message(), ec.value());
      }

      const auto header_types =
          xe::filesystem::ListDirectories(title.path / title.name / "Headers");

      if (!(header_types.size() == 1 &&
            header_types.at(0).name == "00000001")) {
        const auto common_path =
            content_root_ / common_xuid_string / title.name / "Headers";

        std::filesystem::create_directories(common_path);

        // Copy to common, skip cases where only savefile header is available
        std::filesystem::copy(title.path / title.name / "Headers", common_path,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::skip_existing,
                              ec);
        if (ec) {
          failure_count++;
          XELOGW(
              "{}: Copying from: {} to: {} failed! Error message: {} ({:08X})",
              __func__, title.path / title.name / "Headers", common_path,
              ec.message(), ec.value());
        }
      }

      if (!ec) {
        // Remove previous directory
        std::error_code ec;
        std::filesystem::remove_all(title.path / title.name / "Headers", ec);
      }
    }

    if (std::filesystem::exists(title.path / title.name / "profile")) {
      // Find directory with previous username. There should be only one!
      const auto old_profile_data =
          xe::filesystem::ListDirectories(title.path / title.name / "profile");

      xe::filesystem::FileInfo entry_to_copy = xe::filesystem::FileInfo();
      if (old_profile_data.size() != 1) {
        for (const auto& entry : old_profile_data) {
          if (entry.name == "User") {
            entry_to_copy = entry;
          }
        }
      } else {
        entry_to_copy = old_profile_data.front();
      }

      const auto path_from =
          title.path / title.name / "profile" / entry_to_copy.name;
      std::error_code ec;
      // Move files from inside to outside for convenience
      std::filesystem::rename(path_from, path_to_profile_data / title.name, ec);
      if (ec) {
        failure_count++;
        XELOGW("{}: Moving from: {} to: {} failed! Error message: {} ({:08X})",
               __func__, path_from, path_to_profile_data / title.name,
               ec.message(), ec.value());
      } else {
        std::error_code ec;
        std::filesystem::remove_all(title.path / title.name / "profile", ec);
      }
    }

    const auto remaining_file_list =
        xe::filesystem::ListDirectories(title.path / title.name);

    if (remaining_file_list.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(title.path / title.name, ec);
    }
  }

  std::string migration_status_message =
      fmt::format("Migration finished with {} {}.", failure_count,
                  failure_count == 1 ? "error" : "errors");

  if (failure_count) {
    migration_status_message.append(
        " For more information check xenia.log file.");
  }
  new xe::ui::HostNotificationWindow(imgui_drawer_, "Migration Status",
                                     migration_status_message, 0);
  return X_STATUS_SUCCESS;
}

X_STATUS Emulator::ProcessContentPackageHeader(
    const std::filesystem::path& path, ContentInstallEntry& installation_info) {
  installation_info.name_ = "Invalid Content Package!";
  installation_info.content_type_ = XContentType::kInvalid;
  installation_info.data_installation_path_ = xe::path_to_utf8(path.filename());

  const auto header = vfs::XContentContainerDevice::ReadContainerHeader(path);

  if (!header || !header->content_header.is_magic_valid()) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_result_ = X_STATUS_INVALID_PARAMETER;
    installation_info.installation_error_message_ = "Invalid Package Type!";
    XELOGE("Failed to initialize device");
    return X_STATUS_INVALID_PARAMETER;
  }

  // Always install savefiles to user signed to slot 0.
  const auto profile =
      kernel_state_->xam_state()->profile_manager()->GetProfile(
          static_cast<uint8_t>(0));

  uint64_t xuid = header->content_metadata.profile_id;
  if (header->content_metadata.content_type == XContentType::kSavedGame &&
      profile) {
    xuid = profile->xuid();
  }

  installation_info.data_installation_path_ = fmt::format(
      "{:016X}/{:08X}/{:08X}/{}", xuid,
      header->content_metadata.execution_info.title_id.get(),
      static_cast<uint32_t>(header->content_metadata.content_type.get()),
      path.filename());

  installation_info.header_installation_path_ = fmt::format(
      "{:016X}/{:08X}/Headers/{:08X}/{}", xuid,
      header->content_metadata.execution_info.title_id.get(),
      static_cast<uint32_t>(header->content_metadata.content_type.get()),
      path.filename());

  installation_info.name_ =
      xe::to_utf8(header->content_metadata.display_name(XLanguage::kEnglish));
  installation_info.content_type_ =
      static_cast<XContentType>(header->content_metadata.content_type);
  installation_info.content_size_ = header->content_metadata.content_size;
  installation_info.installation_state_ = InstallState::pending;

  installation_info.icon_ = imgui_drawer_->LoadImGuiIcon(
      std::span<const uint8_t>(header->content_metadata.title_thumbnail,
                               header->content_metadata.title_thumbnail_size));
  return X_STATUS_SUCCESS;
}

X_STATUS Emulator::InstallContentPackage(
    const std::filesystem::path& path, ContentInstallEntry& installation_info) {
  installation_info.installation_state_ = InstallState::preparing;

  std::unique_ptr<vfs::XContentContainerDevice> device =
      vfs::XContentContainerDevice::CreateContentDevice("", path);

  if (!device || !device->Initialize()) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_error_message_ =
        "Device initialization failed!";
    installation_info.installation_result_ = X_STATUS_ACCESS_DENIED;
    XELOGE("Failed to initialize device");
    return X_STATUS_INVALID_PARAMETER;
  }

  const std::filesystem::path installation_path =
      content_root() / installation_info.data_installation_path_;

  const std::filesystem::path header_path =
      content_root() / installation_info.header_installation_path_;

  if (!std::filesystem::exists(content_root())) {
    const std::error_code ec = xe::filesystem::CreateFolder(content_root());
    if (ec) {
      installation_info.installation_state_ = InstallState::failed;
      installation_info.installation_error_message_ = ec.message();
      installation_info.installation_result_ = X_STATUS_ACCESS_DENIED;
      return X_STATUS_ACCESS_DENIED;
    }
  }

  const auto disk_space = std::filesystem::space(content_root());
  if (disk_space.available < installation_info.content_size_ * 1.1f) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_error_message_ = "Insufficient disk space!";
    installation_info.installation_result_ = X_STATUS_DISK_FULL;
    return X_STATUS_DISK_FULL;
  }

  if (std::filesystem::exists(installation_path)) {
    // TODO(Gliniak): Popup
    // Do you want to overwrite already existing data?
  } else {
    std::error_code error_code;
    std::filesystem::create_directories(installation_path, error_code);
    if (error_code) {
      installation_info.installation_state_ = InstallState::failed;
      installation_info.installation_error_message_ =
          "Cannot Create Content Directory!";
      installation_info.installation_result_ = error_code.value();
      return error_code.value();
    }
  }

  installation_info.content_size_ = device->data_size();
  installation_info.installation_state_ = InstallState::installing;

  vfs::VirtualFileSystem::ExtractContentHeader(device.get(), header_path);

  X_STATUS error_code = vfs::VirtualFileSystem::ExtractContentFiles(
      device.get(), installation_path,
      installation_info.currently_installed_size_);
  if (error_code != X_ERROR_SUCCESS) {
    installation_info.installation_state_ = InstallState::failed;
    return error_code;
  }

  installation_info.installation_state_ = InstallState::installed;
  installation_info.currently_installed_size_ = installation_info.content_size_;
  kernel_state()->BroadcastNotification(kXNotificationLiveContentInstalled, 0);

  if (installation_info.content_type_ == XContentType::kProfile) {
    kernel_state_->xam_state()->profile_manager()->ReloadProfiles();
  }

  return error_code;
}

X_STATUS Emulator::ExtractZarchivePackage(
    const std::filesystem::path& path,
    const std::filesystem::path& extract_dir) {
  std::unique_ptr<vfs::Device> device =
      std::make_unique<vfs::DiscZarchiveDevice>("", path);
  if (!device->Initialize()) {
    XELOGE("Failed to initialize device");
    return X_STATUS_INVALID_PARAMETER;
  }

  if (std::filesystem::exists(extract_dir)) {
    // TODO(Gliniak): Popup
    // Do you want to overwrite already existing data?
  } else {
    std::error_code error_code;
    std::filesystem::create_directories(extract_dir, error_code);
    if (error_code) {
      return error_code.value();
    }
  }

  uint64_t progress = 0;
  return vfs::VirtualFileSystem::ExtractContentFiles(device.get(), extract_dir,
                                                     progress);
}

X_STATUS Emulator::CreateZarchivePackage(
    const std::filesystem::path& inputDirectory,
    const std::filesystem::path& outputFile) {
  std::vector<uint8_t> buffer;
  buffer.resize(64 * 1024);

  std::error_code ec;
  PackContext packContext;
  packContext.outputFilePath = outputFile;

  ZArchiveWriter zWriter(
      [](int32_t partIndex, void* ctx) {
        PackContext* packContext = reinterpret_cast<PackContext*>(ctx);
        packContext->currentOutputFile =
            std::ofstream(packContext->outputFilePath, std::ios::binary);

        if (!packContext->currentOutputFile.is_open()) {
          XELOGI("Failed to create output file: {}\n",
                 packContext->outputFilePath.string());
          packContext->hasError = true;
        }
      },
      [](const void* data, size_t length, void* ctx) {
        PackContext* packContext = reinterpret_cast<PackContext*>(ctx);
        packContext->currentOutputFile.write(
            reinterpret_cast<const char*>(data), length);
      },
      &packContext);

  if (packContext.hasError) {
    return X_STATUS_UNSUCCESSFUL;
  }

  for (auto const& dirEntry :
       std::filesystem::recursive_directory_iterator(inputDirectory)) {
    std::filesystem::path pathEntry =
        std::filesystem::relative(dirEntry.path(), inputDirectory, ec);

    if (ec) {
      XELOGI("Failed to get relative path {}\n", pathEntry.string());
      return X_STATUS_UNSUCCESSFUL;
    }

    if (dirEntry.is_directory()) {
      if (!zWriter.MakeDir(pathEntry.generic_string().c_str(), false)) {
        XELOGI("Failed to create directory {}\n", pathEntry.string());
        return X_STATUS_UNSUCCESSFUL;
      }
    } else if (dirEntry.is_regular_file()) {
      // Don't pack itself to prevent infinite packing.
      if (dirEntry == outputFile) {
        continue;
      }

      XELOGI("Adding file: {}\n", pathEntry.string());

      if (!zWriter.StartNewFile(pathEntry.generic_string().c_str())) {
        XELOGI("Failed to create archive file {}\n", pathEntry.string());
        return X_STATUS_UNSUCCESSFUL;
      }

      std::filesystem::path file_to_pack_path = inputDirectory / pathEntry;
      FILE* file = xe::filesystem::OpenFile(file_to_pack_path, "rb");

      if (!file) {
        XELOGI("Failed to open input file {}\n", pathEntry.string());
        return X_STATUS_UNSUCCESSFUL;
      }

      const uint64_t file_size = std::filesystem::file_size(file_to_pack_path);
      uint64_t total_bytes_read = 0;

      while (total_bytes_read < file_size) {
        uint64_t bytes_read = fread(buffer.data(), 1, buffer.size(), file);

        total_bytes_read += bytes_read;

        zWriter.AppendData(buffer.data(), bytes_read);
      }

      fclose(file);
    }

    if (packContext.hasError) {
      return X_STATUS_UNSUCCESSFUL;
    }
  }

  zWriter.Finalize();

  return X_STATUS_SUCCESS;
}

// Extent of the loaded xam image. Set once at load. Every dashroot-derived
// constant in this file must be bounds-checked against it before use: on
// another build those addresses are outside the image and reading them
// host-faults (phases 407, 416, 417).
// hud entry points located by code signature rather than by offset. The two
// hud builds put them in different places (dashroot +0xA898/+0xAB28, retail
// 17559 +0x98F8/+0x9B88) and calling the dashroot offsets on retail lands
// mid-prologue of unrelated functions. Branch displacements and lis immediates
// are masked, so only the instruction shape has to match; each pattern matched
// exactly once in each image.
static const uint32_t kHudXuiInitSig[14] = {
    0x7D8802A6u, 0x48000000u, 0x9421FF70u, 0x81630014u,
    0x7C7F1B78u, 0x7C9E2378u, 0x2F0B0000u, 0x419A0000u,
    0x8163000Cu, 0x3BA3000Cu, 0x2B0B0000u, 0x409A0000u,
    0x7FA3EB78u, 0x48000000u,
};
static const uint32_t kHudRenderSig[14] = {
    0x7D8802A6u, 0x9181FFF8u, 0xFBC1FFE8u, 0xFBE1FFF0u,
    0x9421FF60u, 0x3C800000u, 0x7C7F1B78u, 0x8063000Cu,
    0x48000000u, 0x2C030000u, 0x41800000u, 0x807F0008u,
    0x48000000u, 0x3BC0FFFFu,
};
static uint32_t MaskInsn(uint32_t w) {
  uint32_t op = w >> 26;
  if (op == 18) return 0x48000000u;
  if (op == 16) return w & 0xFFFF0000u;
  if (op == 15) return w & 0xFFE00000u;
  return w;
}
static uint32_t FindHudSig(xe::Memory* mem, uint32_t base, uint32_t size,
                           const uint32_t* sig, uint32_t n) {
  if (!mem || !size) return 0;
  for (uint32_t off = 0; off + n * 4 <= size; off += 4) {
    bool ok = true;
    for (uint32_t i = 0; i < n && ok; ++i) {
      ok = MaskInsn(xe::load_and_swap<uint32_t>(
               mem->TranslateVirtual(base + off + i * 4))) == sig[i];
    }
    if (ok) return base + off;
  }
  return 0;
}

static uint32_t g_hud_xuiinit = 0;  // hud XUI-init entry, located by signature
static uint32_t g_hud_render = 0;   // hud render entry, located by signature
static uint32_t g_xam_img_lo = 0;
static uint32_t g_xam_img_hi = 0;
static bool XamConstOk(uint32_t a, uint32_t len) {
  return g_xam_img_lo && a >= g_xam_img_lo && a + len <= g_xam_img_hi;
}

static void InstallGuideStoreTraces(xe::kernel::KernelState* ks);
static void ArmGuideWriteWatch(xe::kernel::KernelState* ks, uint32_t addr);
static void ArmGuideThreadProbe(xe::kernel::KernelState* ks, int pdelay);
static void TagEFailSites(Memory* memory, const char* spec, const char* what);
static void ReportXamTextPopulation(Memory* memory, const char* when);

// Phase 1091h: BRACKET THE WRITE OF [81D43C50+0x28]. The slot holds the object
// 81795924 registers xam's HUD show loop 81794BC8 onto; it is statically zero in
// xam.bin, reads 30052000 at runtime, and 8177BFB0's own assert says that value
// is wrong (it requires the tag bit, which 30052000 has clear - a twui
// ignore_trap_instructions swallows every run). No `stw` with a tracked base
// targets the slot, its address is never formed as a constant, and it is already
// 30052000 by the time guide_xam_app_task runs - so the writer is one of the
// bootstrap steps before that, holding the manager base in a register it was
// handed. Sample the slot at each step and log only on CHANGE: the first line
// names the step that wrote it.
static void GuideHudSlotMark(xe::kernel::KernelState* ks, const char* where) {
  // Gated like every other probe here: the default path logs nothing.
  if (!cvars::guide_bkgnd_watch) return;
  if (!ks || !ks->memory()) return;
  uint32_t v =
      xe::load_and_swap<uint32_t>(ks->memory()->TranslateVirtual(0x81D43C78u));
  static uint32_t prev = 0xFFFFFFFFu;
  if (v == prev) return;
  XELOGI("GuideHudSlotMark: [81D43C50+0x28] {:08X} -> {:08X} at \"{}\" | tag bit {}",
         prev, v, where, (v & 1u) ? "SET" : "CLEAR (8177BFB0 asserts it SET)");
  prev = v;
  // Phase 1091i: WHAT IS THE OBJECT? The slot's occupant is mapped and entirely
  // zero, so "is it a real allocation" has never been asked. Ask the heap: its
  // allocation base, region size and state say whether something allocated it
  // (and how big) or whether the pointer just happens to land in committed
  // memory belonging to something else. Report every field, and say plainly
  // when the query fails rather than printing zeros.
  uint32_t obj = v & ~1u;
  if (!obj) return;
  auto* heap = ks->memory()->LookupHeap(obj);
  if (!heap) {
    XELOGW("GuideHudSlotMark: {:08X} is in NO HEAP - not queried", obj);
    return;
  }
  uint32_t st = 0, ap = 0, cp = 0, ba = 0, rp = 0;
  if (!heap->QueryPageEntry(obj, &st, &ap, &cp, &ba, &rp)) {
    XELOGW("GuideHudSlotMark: QueryPageEntry({:08X}) FAILED - the object's "
           "allocation is unknown, nothing below was read", obj);
    return;
  }
  XELOGI("GuideHudSlotMark: object {:08X} | heap base {:08X} page size {} | "
         "alloc base {:08X} region {} pages ({} bytes) | state {:08X} "
         "alloc-protect {:08X} cur-protect {:08X} | offset into region {:08X}",
         obj, heap->heap_base(), heap->page_size(), ba, rp,
         rp * heap->page_size(), st, ap, cp, obj - ba);
  // Phase 1091k: WHO COULD HAVE WRITTEN IT. The write lands after xam's DllMain
  // returns and before the Guide press (1091j), on a guest thread, so name the
  // threads that exist at this point and where they start. A start address
  // inside the loaded xam image is a candidate for the writer; the title's own
  // threads (0x82xxxxxx) are not. Report EVERY thread, and the image extent
  // used to classify them, so "no candidate" would be a statement about a
  // known set rather than an empty one.
  static bool threads_dumped = false;
  if (threads_dumped) return;
  threads_dumped = true;
  auto threads = ks->object_table()->GetObjectsByType<xe::kernel::XThread>(
      xe::kernel::XObject::Type::Thread);
  std::string in_xam, others;
  uint32_t n_xam = 0, n_other = 0;
  for (auto& th : threads) {
    if (!th) continue;
    uint32_t sa = th->start_address();
    if (sa >= g_xam_img_lo && sa < g_xam_img_hi) {
      ++n_xam;
      in_xam += fmt::format("tid {:08X} start {:08X}; ", th->thread_id(), sa);
    } else {
      ++n_other;
      others += fmt::format("{:08X} ", sa);
    }
  }
  XELOGI("GuideHudSlotMark: {} thread(s) live; {} start inside the xam image "
         "[{:08X}..{:08X}) -> {} | {} start elsewhere: {}",
         threads.size(), n_xam, g_xam_img_lo, g_xam_img_hi,
         n_xam ? in_xam : std::string("(none)"), n_other, others);
}

// Phase 1099p: INPUT RECORDER. Every press delivered to xam while the user
// plays is appended to guide_input_record_path in guide_input_script format
// ("delay_ms:name,..."), delay measured on the real clock from the previous
// press (the first from the input pump starting), so a session can be replayed
// press for press with --guide_input_script=<file contents>.
namespace {
std::mutex g_guide_rec_lock;
FILE* g_guide_rec_file = nullptr;
std::chrono::steady_clock::time_point g_guide_rec_last;
bool g_guide_rec_first = true;
}  // namespace

// XInput VK_PAD codes <-> script names. Anything unnamed is written/read as hex.
static const std::pair<const char*, uint16_t> kGuideVkNames[] = {
    {"a", 0x5800},         {"b", 0x5801},          {"x", 0x5802},
    {"y", 0x5803},         {"rb", 0x5804},         {"lb", 0x5805},
    {"lt", 0x5806},        {"rt", 0x5807},         {"up", 0x5810},
    {"down", 0x5811},      {"left", 0x5812},       {"right", 0x5813},
    {"start", 0x5814},     {"back", 0x5815},       {"ls", 0x5816},
    {"rs", 0x5817},        {"ls_up", 0x5820},      {"ls_down", 0x5821},
    {"ls_right", 0x5822},  {"ls_left", 0x5823},    {"ls_upleft", 0x5824},
    {"ls_upright", 0x5825}, {"ls_downright", 0x5826}, {"ls_downleft", 0x5827},
    {"rs_up", 0x5830},     {"rs_down", 0x5831},    {"rs_right", 0x5832},
    {"rs_left", 0x5833},   {"rs_upleft", 0x5834},  {"rs_upright", 0x5835},
    {"rs_downright", 0x5836}, {"rs_downleft", 0x5837},
    {"guide", 0xFFFF},     {"hostguide", 0xFFFE}};

static std::string GuideVkName(uint16_t vk) {
  for (auto& kv : kGuideVkNames) {
    if (kv.second == vk) return kv.first;
  }
  return fmt::format("0x{:04X}", vk);
}

static uint16_t GuideVkFromName(const std::string& name) {
  for (auto& kv : kGuideVkNames) {
    if (name == kv.first) return kv.second;
  }
  if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
    return uint16_t(std::strtoul(name.c_str() + 2, nullptr, 16));
  }
  return 0;
}

static void GuideRecordStart() {
  if (cvars::guide_input_record_path.empty()) return;
  std::lock_guard<std::mutex> lock(g_guide_rec_lock);
  if (g_guide_rec_file) return;
  g_guide_rec_file =
      xe::filesystem::OpenFile(cvars::guide_input_record_path, "wb");
  g_guide_rec_last = std::chrono::steady_clock::now();
  g_guide_rec_first = true;
  XELOGI("GuideInputRec: recording to {} (timing from pump start)",
         xe::path_to_utf8(cvars::guide_input_record_path));
}

static void GuideRecordPress(uint16_t vk) {
  std::lock_guard<std::mutex> lock(g_guide_rec_lock);
  if (!g_guide_rec_file) return;
  const auto now = std::chrono::steady_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - g_guide_rec_last)
                      .count();
  g_guide_rec_last = now;
  const std::string item =
      fmt::format("{}{}:{}", g_guide_rec_first ? "" : ",", ms, GuideVkName(vk));
  g_guide_rec_first = false;
  fwrite(item.data(), 1, item.size(), g_guide_rec_file);
  fflush(g_guide_rec_file);
  XELOGI("GuideInputRec: +{} ms {}", ms, GuideVkName(vk));
}

void Emulator::on_guide_button_pressed(uint8_t user_index) {
  if (awaiting_power_on()) {
    // Phase 1099z138: nothing running - this press is the power button.
    guide_power_press_ = true;
    XELOGI("Guide button: pressed with no title open (power on)");
    return;
  }
  // Phase 1099p: every Guide press (pad, keyboard key, ImGui) comes through
  // here; replay uses the same host path ("hostguide").
  GuideRecordPress(0xFFFE);
  XELOGI("Guide button: pressed (user {}), handler={:08X} buf={:08X} "
         "out_sz={:08X}",
         user_index, guide_handler_, guide_buf_, guide_out_sz_);
  // Phase 1091h: the earliest mark inside the device block already read
  // 30052000, so sample before ANY of this handler runs. If it is already set
  // here, the write predates the Guide press entirely and belongs to xam code
  // that runs during the title's own boot, not to the bootstrap.
  GuideHudSlotMark(kernel_state(), "top of on_guide_button_pressed");
  // Phase 1097: THE HARDWARE PATH. xam asks the kernel to tell it when the
  // Xenon button is pressed (DrvSetSysReqCallback, xboxkrnl ordinal 0x20C);
  // the kernel calls that callback (DrvXenonButtonPressed, ordinal 0x278).
  // Both were declared in xboxkrnl_table.inc and never implemented, so xam's
  // registration was an "undefined extern call" and its callback pointer was
  // discarded - which is why no press has ever reached xam and why the UI gate
  // [81D43C50+0xA8] has never opened. Nothing below draws, places or decides
  // anything: it hands the press to the callback xam itself registered.
  if (!cvars::guide_sysreq_button.empty()) {
    unsigned dev = 0, cls = 0, kind = 1;
    if (std::sscanf(cvars::guide_sysreq_button.c_str(), "%u,%u,%u", &dev, &cls,
                    &kind) != 3) {
      XELOGW("GuideSysReq: could not parse guide_sysreq_button=\"{}\" "
             "(want \"device,class,kind\")",
             cvars::guide_sysreq_button);
    } else {
      auto* ks = kernel_state();
      auto rdx = [&](uint32_t a) -> uint32_t {
        // xam-image addresses first: QueryRangeAccess is the known-wrong guard
        // for those (phase 416). The button context is a HEAP allocation
        // (817B5688), so fall back to a heap-committed check for the rest -
        // without it every ctx field read back as the FFFFFFFF sentinel.
        if (XamConstOk(a, 4)) {
          return xe::load_and_swap<uint32_t>(ks->memory()->TranslateVirtual(a));
        }
        auto* hp = ks->memory()->LookupHeap(a);
        if (!hp) return 0xFFFFFFFFu;
        // Same pair of tests XamTextWatch settled on in 1096hr: the access
        // query alone refuses readable pages, and reading anyway host-faults
        // on an uncommitted one.
        bool committed = false;
        HeapAllocationInfo info = {};
        if (hp->QueryRegionInfo(a & ~0xFFFu, &info)) {
          committed = (info.state & kMemoryAllocationCommit) != 0;
        }
        if (hp->QueryRangeAccess(a, a + 3) ==
                xe::memory::PageAccess::kNoAccess &&
            !committed) {
          return 0xFFFFFFFFu;
        }
        return xe::load_and_swap<uint32_t>(ks->memory()->TranslateVirtual(a));
      };
      // Phase 1097za: the store-trace breakpoints are installed from deep
      // inside the hud block below, which this path returns before ever
      // reaching - so guide_trace_stores measured nothing in this arm and the
      // run produced no StoreTrace line at all (caught by its own missing
      // "installed N breakpoints" marker). Install them here, BEFORE the press
      // is delivered, because the state transitions this is meant to catch
      // happen as a consequence of the press.
      InstallGuideStoreTraces(ks);
      if (cvars::guide_watch_write_addr) {
        ArmGuideWriteWatch(ks, cvars::guide_watch_write_addr);
      }
      const uint32_t cb = kernel::xboxkrnl::GuideSysReqCallback();
      const uint32_t ctx = rdx(0x81D4F610u);
      // 817C23A8 refuses to post unless [81D4F614] is non-zero, and the worker
      // rejects a slot that is not the 0xFE "free" sentinel; log both so a
      // press that does nothing says which gate stopped it.
      XELOGI("GuideSysReq: before | cb={:08X} ctx={:08X} enable={:08X} "
             "slot={:08X} gate={:08X}",
             cb, ctx, rdx(0x81D4F614u),
             ctx ? rdx(ctx + 0x10u) : 0xFFFFFFFFu, rdx(0x81D43CF8u));
      // Phase 1097zq: THE DIAGNOSTIC, and it is labelled as one. 1097zo
      // concluded that of the three paths that create the Guide app record at
      // [81D426C8], two are behind a HUD state (0x10/0x20) this xam can never
      // reach, leaving xam's sys-app pass 8177F588 - which runs, but three
      // times inside the 293 ms window before hud registers, reading zero
      // every time (1097x). Re-running it AFTER registration tests exactly
      // that claim.
      //
      // The argument is MEASURED, not invented: r3 at the live call site
      // 817800B8 reads 81D42FD0, a static xam .data address, and the other
      // three arguments are li 0 there.
      //
      // THIS IS NOT A FIX. The host calling the pass is the host driving guest
      // code, which is what this phase's best result came from REMOVING. It
      // exists to make the synthesis falsifiable in one run: if the record
      // appears and the tail unlocks, the console-side question is what
      // re-invokes the pass (1096hl); if it does not, 1097zo is wrong.
      if (cvars::guide_rerun_sysapp_pass) {
        auto rdp = [&](uint32_t a) -> uint32_t {
          return XamConstOk(a, 4) ? xe::load_and_swap<uint32_t>(
                                        ks->memory()->TranslateVirtual(a))
                                  : 0xFFFFFFFFu;
        };
        XELOGI("SysAppPassRerun: before | [81D426C8]={:08X} [81D42688]={:08X}",
               rdp(0x81D426C8u), rdp(0x81D42688u));
        auto t = kernel::object_ref<kernel::XHostThread>(
            new kernel::XHostThread(ks, 256 * 1024, 0, [ks]() -> int {
              auto* ts = kernel::XThread::GetCurrentThread()->thread_state();
              uint64_t a[] = {0x81D42FD0ull, 0, 0, 0};
              uint64_t r = ks->processor()->Execute(ts, 0x8177F588u, a,
                                                    xe::countof(a));
              XELOGI("SysAppPassRerun: 8177F588(81D42FD0,0,0,0) -> {:08X}",
                     static_cast<uint32_t>(r));
              return 0;
            }));
        t->set_name("SysAppPassRerun");
        if (XSUCCEEDED(t->Create())) {
          t->Wait(0, 0, 0, nullptr);
        }
        XELOGI("SysAppPassRerun: after  | [81D426C8]={:08X} [81D42688]={:08X}",
               rdp(0x81D426C8u), rdp(0x81D42688u));
      }
      // Phase 1099g: PREFER xam's OWN Xenon button over the host's synthesised
      // one. XAutomationpInputPress (ordinal 0x3D8) with the sentinel 0xFFFF
      // routes to XAutomationpInputXenonButton (81723E08) - the door xam
      // already has for this.
      //
      // MEASURED, one script step apart, everything else identical:
      //   close via xam's automation button  -> the Guide tears down and THE
      //     DASHBOARD COMES BACK (frames 0-5 Guide, 6-17 dashboard), 0 crashes
      //   close via GuideDeliverXenonButton  -> GUEST CRASH at 817A6174
      //     (fault_addr 100000018) and the screen is left with an empty grey
      //     panel and the clock over black, dashboard never restored
      // The second is the "background never goes away after closing" report.
      // It is the host driving guest code, which this project has repeatedly
      // found to be the bug; xam's own entry point does it properly.
      // Phase 1099i: the two doors do DIFFERENT jobs, and 1099g used one for
      // both.
      //
      // XAutomationpInputXenonButton dispatches through a handler pointer at
      // [81D4F610] that only exists while the Guide is UP - 817C2BC8 sets it
      // when the Guide is created and 817C1FD8 clears it on teardown. So it
      // automates the button INSIDE a running Guide; with the Guide closed it
      // finds null and returns 1 having done nothing. That is why 1099g fixed
      // the close and left the reopen dead: measured, the dispatch 817C2090
      // ran calls=1 across three presses, and the Guide-open function
      // 817935B8 likewise calls=1.
      //
      // Opening from closed is the other door - the kernel callback, which
      // reaches xam's worker and XamAppRequestLoadEx.
      //
      // Pick by asking the GUEST which state it is in: xam resolves the Guide's
      // surface every frame it is up and stops when it closes, which the
      // command processor already tracks for presentation. No address is read
      // and no state is kept here.
      bool guide_is_up = false;
      {
        auto* gsys = graphics_system();
        auto* cproc = gsys ? gsys->command_processor() : nullptr;
        if (cproc) guide_is_up = cproc->GuideSurfaceIsLive();
      }
      XELOGI("GuideButton: guide is {} - using {}",
             guide_is_up ? "UP" : "CLOSED",
             guide_is_up ? "xam's automation button" : "the kernel callback");
      bool guide_button_done = false;
      if (guide_is_up && cvars::guide_button_via_automation) {
        auto xm2 = ks->GetModule("xam.xex", true);
        const uint32_t ip = xm2 ? xm2->GetProcAddressByOrdinal(0x3D8) : 0u;
        if (ip) {
          auto t2 = kernel::object_ref<kernel::XHostThread>(
              new kernel::XHostThread(ks, 256 * 1024, 0, [ks, ip]() -> int {
                auto* ts = kernel::XThread::GetCurrentThread()->thread_state();
                uint64_t a2[] = {0, 0xFFFF};
                uint64_t r2 =
                    ks->processor()->Execute(ts, ip, a2, xe::countof(a2));
                XELOGI("GuideButton: XAutomationpInputPress(0,FFFF) -> {:08X}",
                       static_cast<uint32_t>(r2));
                return 0;
              }));
          t2->set_name("GuideButtonAutomation");
          if (XSUCCEEDED(t2->Create())) {
            t2->Wait(0, 0, 0, nullptr);
            guide_button_done = true;
          }
        } else {
          XELOGW("GuideButton: xam does not export ordinal 0x3D8");
        }
      }
      if (!guide_button_done) {
        kernel::xboxkrnl::GuideDeliverXenonButton(dev, cls, kind);
      }
      // The second door, tried in the same place so the before/after fields
      // are comparable: xam EXPORTS a Xenon button press as ordinal 0x506,
      // and that entry goes straight to the poster without 817C23A8's gates.
      //
      // Phase 1099g: but NOT when xam's own Xenon button already handled the
      // press. Ringing both doors for one button crashes the guest at 817A6174
      // on the CLOSE and leaves the Guide half torn down - measured, one flag
      // apart, with everything else identical:
      //     automation + 0x506  -> GUEST CRASH, dashboard never returns
      //     automation alone    -> clean close, dashboard restored, 0 crashes
      // Two doors were useful while it was unknown which one worked; now that
      // one does, the other is a second press the guest never asked for.
      if (!guide_button_done && cvars::guide_xenon_press >= 0) {
        auto xm = ks->GetModule("xam.xex", true);
        const uint32_t ord506 =
            xm ? xm->GetProcAddressByOrdinal(0x506) : 0u;
        if (!ord506) {
          XELOGW("GuideXenonPress: xam does not export ordinal 0x506");
        } else {
          const uint32_t v = uint32_t(cvars::guide_xenon_press);
          auto t = kernel::object_ref<kernel::XHostThread>(
              new kernel::XHostThread(ks, 256 * 1024, 0,
                                      [ks, ord506, v]() -> int {
                auto* ts = kernel::XThread::GetCurrentThread()->thread_state();
                uint64_t a[] = {v};
                uint64_t r = ks->processor()->Execute(ts, ord506, a,
                                                      xe::countof(a));
                XELOGI("GuideXenonPress: 0x506 {:08X}({:X}) -> {:08X}", ord506,
                       v, static_cast<uint32_t>(r));
                return 0;
              }));
          t->set_name("GuideXenonPress");
          if (XSUCCEEDED(t->Create())) {
            t->Wait(0, 0, 0, nullptr);
          } else {
            XELOGE("GuideXenonPress: could not create the delivery thread");
          }
        }
      }
      const uint32_t ctx2 = rdx(0x81D4F610u);
      XELOGI("GuideSysReq: after  | ctx={:08X} slot={:08X} kindslot={:08X} "
             "devslot={:08X} gate={:08X}",
             ctx2, ctx2 ? rdx(ctx2 + 0x10u) : 0xFFFFFFFFu,
             ctx2 ? rdx(ctx2 + 0x14u) : 0xFFFFFFFFu,
             ctx2 ? rdx(ctx2 + 0x18u) : 0xFFFFFFFFu, rdx(0x81D43CF8u));
      return;
    }
  }
  // Phase 1054: once the Guide is up, a press toggles it (the swap hook owns
  // that); re-dispatching the open request built a whole new scene tree per
  // press (p1054b/c: three trees, three arenas, the re-shown Guide a stub).
  if (kernel::xboxkrnl::GuideBootstrapReady()) {
    XELOGI("Guide button: Guide already up; the press is a toggle");
    return;
  }
  // Drive the Guide open sequence if hud.xex is loaded and registered. This
  // runs the message dispatch on a guest thread - hud's handler must not be
  // called from the host UI thread. It does not yet produce a visible Guide
  // (hud needs to be hosted by xam to build and draw its scenes) but it is
  // the real entry point, so this is where an open belongs once hosting
  // exists.
  if (guide_handler_ && guide_buf_ && guide_out_sz_ && kernel_state_) {
    auto* ks = kernel_state_.get();
    uint32_t handler = guide_handler_;
    uint32_t buf = guide_buf_;
    uint32_t osz = guide_out_sz_;
    uint32_t hud_base = guide_hud_base_;
    // Decoded from the handler at load time; 0 means fall back to dashroot's.
    uint32_t obj_slot = guide_obj_slot_;
    uint32_t skin_mod = guide_skin_module_;
    auto t = kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
        ks, 512 * 1024, 0,
        [ks, handler, buf, osz, hud_base, skin_mod, obj_slot]() -> int {
          auto* ts = kernel::XThread::GetCurrentThread()->thread_state();
          uint64_t a[] = {0x80000004ull, buf, osz};
          XELOGI("Guide button: dispatching open to {:08X}", handler);
          uint64_t r =
              ks->processor()->Execute(ts, handler, a, xe::countof(a));
          XELOGI("Guide button: handler returned {:08X}",
                 static_cast<uint32_t>(r));
          // Then drive hud's own XUI init and render loop. hud is a system
          // app: the system normally creates its thread and calls these. Its
          // entry points sit at fixed offsets from the module base -
          // base+0xA898 calls XuiInit and XuiRenderCreateDC, base+0xAB28
          // calls XuiRenderBegin/End/Present - and both take the Guide object
          // in r3 and only read from it.
          if (hud_base) {
            uint32_t slot0 = obj_slot ? obj_slot : 0x91400690u;
            uint32_t obj = xe::load_and_swap<uint32_t>(
                ks->memory()->TranslateVirtual(slot0));
            // Same slot, same hazard as the loader path: on a hud build that
            // does not store the Guide object here, this holds unrelated
            // bytes and `if (obj)` passes. Require a mapped heap pointer.
            if (obj && !(obj >= 0x30000000u && obj < 0x50000000u &&
                         ks->memory()->LookupHeap(obj) &&
                         ks->memory()->LookupHeap(obj)->QueryRangeAccess(
                             obj, obj + 0x20u) !=
                             xe::memory::PageAccess::kNoAccess)) {
              XELOGW("Guide button: object slot 91400690 = {:08X} is not a "
                     "mapped heap pointer - skipping the hud draw calls", obj);
              obj = 0;
            }
            if (obj) {
              // Dump the Guide object's vtable. The draw entry points used
              // below were guessed from static scanning; the object's own
              // vtable is the authoritative list of its virtual methods.
              uint32_t vt = xe::load_and_swap<uint32_t>(
                  ks->memory()->TranslateVirtual(obj));
              XELOGI("Guide button: obj={:08X} vtable={:08X}", obj, vt);
              if (vt) {
                for (int i = 0; i < 48; ++i) {
                  uint32_t fn = xe::load_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(vt + i * 4));
                  XELOGI("Guide button: vtable[{}] = {:08X}", i, fn);
                }
              }
              auto rd = [&](uint32_t a) -> uint32_t {
                // Reject 0x81xxxxxx constants that are not inside THIS xam
                // image; on another build they host-fault (phase 416).
                if (a >= 0x81000000u && a < 0x82000000u &&
                    !XamConstOk(a, 4)) {
                  return 0;
                }
                return xe::load_and_swap<uint32_t>(
                    ks->memory()->TranslateVirtual(a));
              };
              // Dump hud's XUI import thunks. The import-table dump marks
              // these "!!" (no HLE implementation), which says nothing about
              // where the LLE override actually pointed them.
              // Phase 793: 913FE104 and 913FE4C4 are the two render calls in hud's
              // render body (913EAB80 and 913EABA4). The static image holds
              // unrelocated import records there, and resolving their
              // ordinals against xam's export table gave an address that is
              // never called - so read what the loader actually wrote.
              for (uint32_t th : {0x913FE7E4u, 0x913FE7F4u, 0x913FE874u,
                                  0x913FE104u, 0x913FE4C4u, 0x913FE864u}) {
                XELOGI("Guide button: thunk {:08X}: {:08X} {:08X} {:08X} "
                       "{:08X}",
                       th, rd(th), rd(th + 4), rd(th + 8), rd(th + 12));
              }
              XELOGI("Guide button: pre-init  +8={:08X} +12={:08X} "
                     "+20={:08X}",
                     rd(obj + 16 + 8), rd(obj + 16 + 12),
                     rd(obj + 16 + 20));
              // [obj+20] gates DC creation in the init at hud+0xA898 and is
              // never dereferenced there (the register is reloaded from
              // [obj+12] immediately after the test), so forcing it non-zero
              // is safe and lets XuiRenderCreateDC run.
              // The XUI render context global (81D6C978) is written inside
              // the function containing runtime 818FF278. Find which export
              // that is by listing the XUI ordinal range.
              {
                // Does the REAL xam export ordinal 0x506? The image is a
                // XEX with its own export mechanism, not a PE .edata, so
                // parsing it offline does not work - ask the loader.
                auto bm = ks->GetModule("xam.xex", true);
                uint32_t ba = bm ? bm->GetProcAddressByOrdinal(0x506) : 0;
                XELOGI("Guide button: XamInputSendXenonButtonPress "
                       "(ord 506) -> {:08X}", ba);
                if (ba && cvars::guide_xam_button_api >= 0) {
                  // The wrapper loads its context from [81D4F610] and
                  // the worker dereferences it at +0x10, so a null there
                  // faults. 817C2B68 takes no arguments, asserts the
                  // global is still 0, allocates a 36-byte context and
                  // stores it. Run it first if nothing else has.
                  uint32_t ictx = xe::load_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(0x81D4F610u));
                  if (!ictx) {
                    uint64_t ia2[] = {0};
                    uint64_t ir2 = ks->processor()->Execute(
                        ts, kernel::xboxkrnl::GuideConst(0x817C2B68u), ia2, xe::countof(ia2));
                    ictx = xe::load_and_swap<uint32_t>(
                        ks->memory()->TranslateVirtual(0x81D4F610u));
                    XELOGI("Guide button: xam input ctx init -> {:08X}, "
                           "[81D4F610] now {:08X}",
                           static_cast<uint32_t>(ir2), ictx);
                  }
                  uint64_t ba_args[] = {
                      uint64_t(uint32_t(cvars::guide_xam_button_api))};
                  uint64_t br = ks->processor()->Execute(
                      ts, ba, ba_args, xe::countof(ba_args));
                  XELOGI("Guide button: xam button API({}, {:X}) -> "
                         "{:08X}",
                         0,
                         uint32_t(cvars::guide_xam_button_api),
                         static_cast<uint32_t>(br));
                }
              }
              auto xm = ks->GetModule("xam.xex", true);
              if (xm) {
                for (uint32_t ord = 0x340; ord <= 0x358; ++ord) {
                  uint32_t fa = xm->GetProcAddressByOrdinal(ord);
                  if (fa) {
                    XELOGI("Guide button: xam ord {:03X} -> {:08X}", ord, fa);
                  }
                }
              }
              if (cvars::guide_call_xuiinit) {
                // Target read out of hud's XuiInit thunk at 913FE7E4
                // (lis 0x8195 / ori 0x3760). XuiInit accepts null params -
                // that case branches straight to the real init - and returns
                // 1 if XUI was already initialised.
                XELOGI("Guide button: XUI ctx before = {:08X}",
                       rd(0x81D6C978u));
                // XuiInit is xam export ordinal 0x340. It was hardcoded as
                // 0x81953760, which is where dashroot's xam happens to put it;
                // on retail 17559 that ordinal resolves to 817BC088 and the
                // old constant lands on unrelated code, which crashed inside
                // __restgprlr_14 with lr=0 (phase 423). Resolve the export.
                uint32_t xui_init = xm ? xm->GetProcAddressByOrdinal(0x340)
                                       : 0u;
                if (!xui_init) {
                  xui_init = 0x81953760u;
                  XELOGW("Guide button: XuiInit ordinal 0x340 unresolved, "
                         "falling back to the dashroot constant");
                }
                uint64_t xa[] = {0};
                XELOGI("Guide button: XuiInit -> {:08X}", xui_init);
                uint64_t xr = ks->processor()->Execute(ts, xui_init, xa,
                                                       xe::countof(xa));
                XELOGI("Guide button: XuiInit returned {:08X}, ctx now {:08X}",
                       static_cast<uint32_t>(xr), rd(0x81D6C978u));
                // The XUI device context reaches this object through
                // [dc+0x1C8] and calls [it+0x0C] as a function pointer.
                // At the crash that slot held 006E0065 - two UTF-16 code
                // units, not code - so dump the head of the context to see
                // whether it is a real object with a bad slot or simply not
                // the thing [dc+0x1C8] should be pointing at.
                uint32_t xctx = rd(0x81D6C978u);
                if (xctx) {
                  std::string cw;
                  for (uint32_t i = 0; i < 12; ++i) {
                    cw += fmt::format("{:08X} ", rd(xctx + i * 4));
                  }
                  XELOGI("Guide button: XUI ctx @{:08X}: {}", xctx, cw);
                }
              }
                // Armed early instead (see ArmGuideThreadProbe): a probe
                // armed from this handler cannot fire on a freeze that
                // happens before the handler is ever published.
                // Arm again here: breakpoints set before the target is
                // JIT-translated do not take, so the xam-load call is
                // inert for anything not yet executed.
                InstallGuideStoreTraces(ks);
                static std::unique_ptr<cpu::Breakpoint> pump_bp;
                if (cvars::guide_trace_pump && !pump_bp) {
                  pump_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      uint64_t(uint32_t(cvars::guide_trace_pump)),
                      [](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                         uint64_t host_pc) {
                        static std::atomic<uint32_t> n{0};
                        uint32_t k = ++n;
                        if (k <= 3 || k % 500 == 0) {
                          auto* th = kernel::XThread::GetCurrentThread();
                          auto* c = th ? th->thread_state()->context()
                                       : nullptr;
                          // r3 at the DC vtable slots is the device
                          // context: [dc+0x1CC] is its device wrapper and
                          // [dev+0x32A0] its RT0. Follow the chain so the
                          // device xam picks for itself is visible.
                          auto* pm = th ? th->kernel_state()->memory()
                                        : nullptr;
                          auto rp = [pm](uint32_t a) -> uint32_t {
                            if (!pm) return 0;
                            if (a < 0x1000u) return 0;
                            auto* hp = pm->LookupHeap(a);
                            if (!hp || hp->QueryRangeAccess(a, a + 4) ==
                                           xe::memory::PageAccess::kNoAccess)
                              return 0;
                            return xe::load_and_swap<uint32_t>(
                                pm->TranslateVirtual(a));
                          };
                          uint32_t dc = c ? uint32_t(c->r[3]) : 0;
                          uint32_t wrap = rp(dc + 0x1CCu);
                          uint32_t dev = rp(wrap + 12u);
                          XELOGI("GuidePump hit #{} thread {:08X} r3={:08X} "
                                 "lr={:08X} [dc+1CC]={:08X} dev={:08X} "
                                 "RT0={:08X} [3F74]={:08X} [134]={:08X}",
                                 k, th ? th->handle() : 0, dc,
                                 c ? uint32_t(c->lr) : 0, wrap, dev,
                                 rp(dev + 0x32A0u), rp(dev + 0x3F74u),
                                 rp(dc + 0x134u));
                        }
                      });
                  ks->processor()->AddBreakpoint(pump_bp.get());
                  XELOGI("GuidePump: counting breakpoint at {:08X}",
                         uint32_t(cvars::guide_trace_pump));
                }
                static std::unique_ptr<cpu::Breakpoint> srt_bp;
                if (cvars::guide_trace_setrendertarget && !srt_bp) {
                  srt_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x819F31A8ull,
                      [](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                         uint64_t host_pc) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) return;
                        auto* c = th->thread_state()->context();
                        static std::atomic<uint32_t> n{0};
                        uint32_t k = ++n;
                        if (k > 40) return;
                        XELOGI("SetRenderTarget #{}: dev={:08X} index={} "
                               "surface={:08X} lr={:08X}  <- {}",
                               k, static_cast<uint32_t>(c->r[3]),
                               static_cast<uint32_t>(c->r[4]),
                               static_cast<uint32_t>(c->r[5]),
                               static_cast<uint32_t>(c->lr),
                               c->r[5] ? "BIND" : "unbind");
                      });
                  ks->processor()->AddBreakpoint(srt_bp.get());
                  XELOGI("SetRenderTarget trace installed at 819F31A8");
                }
                static std::unique_ptr<cpu::Breakpoint> devsetup_bp;
                if (cvars::guide_trace_devsetup && !devsetup_bp) {
                  devsetup_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x81A0FE48ull,
                      [](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                         uint64_t host_pc) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) return;
                        auto* c = th->thread_state()->context();
                        static std::atomic<uint32_t> n{0};
                        if (++n > 8) return;
                        xe::kernel::xboxkrnl::GuideSetMode1Device(
                            static_cast<uint32_t>(c->r[3]));
                        XELOGI("DevSetup #{}: device={:08X} arg2={:08X} "
                               "lr={:08X}",
                               static_cast<uint32_t>(n),
                               static_cast<uint32_t>(c->r[3]),
                               static_cast<uint32_t>(c->r[4]),
                               static_cast<uint32_t>(c->lr));
                      });
                  ks->processor()->AddBreakpoint(devsetup_bp.get());
                  XELOGI("DevSetup trace installed at 81A0FE48");
                }
                // 819F4D28 is the device-creation core both mode 1 and mode 2
                // funnel into, so unlike the 81A0FE48 breakpoint above it
                // fires on the mode-2 path too. Its arg5 (r7) is the pointer
                // that its own call site at 819F4E8C hands to 81A0FE48 as that
                // function's second argument - the one guide_force_front_buffer
                // previously guessed as 0 and crashed on.
                static std::unique_ptr<cpu::Breakpoint> devcreate_bp;
                if (cvars::guide_trace_devsetup && !devcreate_bp) {
                  devcreate_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x819F4D28ull,
                      [](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                         uint64_t host_pc) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) return;
                        auto* c = th->thread_state()->context();
                        uint32_t arg5 = static_cast<uint32_t>(c->r[7]);
                        xe::kernel::xboxkrnl::GuideSetDevCreateArg5(arg5);
                        static std::atomic<uint32_t> n{0};
                        if (++n > 8) return;
                        // Ground truth for the 124-byte parameter block.
                        // Mode 1 (8178E9F0) builds a real one at r1+0x90 and
                        // passes it here as arg5; mode 2 passes 0. Dumping it
                        // gives every field's true value instead of inferring
                        // them one validator complaint at a time.
                        if (arg5) {
                          // Reached through the thread: this lambda captures
                          // nothing, so Emulator::kernel_state() is not
                          // callable here.
                          auto* bm = th->memory();
                          std::string bd;
                          for (uint32_t o = 0; o < 0x7Cu; o += 4) {
                            bd += fmt::format(
                                "{:02X}:{:08X} ", o,
                                xe::load_and_swap<uint32_t>(
                                    bm->TranslateVirtual(arg5 + o)));
                          }
                          XELOGI("DevCreateBlock @{:08X}: {}", arg5, bd);
                        }
                        XELOGI("DevCreate #{}: device={:08X} mode={} r6={:08X} "
                               "arg5(r7)={:08X} r8={:08X} lr={:08X}",
                               static_cast<uint32_t>(n),
                               static_cast<uint32_t>(c->r[3]),
                               static_cast<uint32_t>(c->r[4]),
                               static_cast<uint32_t>(c->r[6]), arg5,
                               static_cast<uint32_t>(c->r[8]),
                               static_cast<uint32_t>(c->lr));
                      });
                  ks->processor()->AddBreakpoint(devcreate_bp.get());
                  XELOGI("DevCreate trace installed at 819F4D28");
                }
                // And the wrapper's SetDevice itself. Before calling
                // 8191BAC8(wrapper, device, x) by hand, learn the triple it is
                // actually invoked with - guessing 81A0FE48's second argument
                // as 0 was wrong (it is a stack pointer) and that call
                // crashed. r5 here is unknown and worth reading rather than
                // assuming.
                static std::unique_ptr<cpu::Breakpoint> setdev_bp;
                if (cvars::guide_trace_devsetup && !setdev_bp) {
                  setdev_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x8191BAC8ull,
                      [](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                         uint64_t host_pc) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) return;
                        auto* c = th->thread_state()->context();
                        static std::atomic<uint32_t> n{0};
                        if (++n > 8) return;
                        XELOGI("SetDevice #{}: wrapper={:08X} device={:08X} "
                               "r5={:08X} lr={:08X}",
                               static_cast<uint32_t>(n),
                               static_cast<uint32_t>(c->r[3]),
                               static_cast<uint32_t>(c->r[4]),
                               static_cast<uint32_t>(c->r[5]),
                               static_cast<uint32_t>(c->lr));
                      });
                  ks->processor()->AddBreakpoint(setdev_bp.get());
                  XELOGI("SetDevice trace installed at 8191BAC8");
                }
                // The emitter runs (DemandFunction confirms 819F5D18,
                // 819F7F20, 81A015B8 and 819F31A8 all execute) but the frame
                // carries 1 word - and no literal 0x200E exists anywhere in
                // 819F5D18's 0x2208 bytes, so that word is xam's begin, not
                // the draw. The draw emits ZERO. guide_coverage_fn is the
                // tool for "where does it stop", but per-instruction counters
                // destabilise the JIT badly enough to crash the skin-init
                // path at 81812FB0, so read the arguments instead: if a
                // count/geometry argument arrives as 0 there is nothing to
                // build and the ~390 branch points do not need reading.
                static std::unique_ptr<cpu::Breakpoint> emit_bp;
                if (cvars::guide_trace_emitter && !emit_bp) {
                  emit_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x819F5D18ull,
                      [](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                         uint64_t host_pc) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) return;
                        auto* c = th->thread_state()->context();
                        static std::atomic<uint32_t> n{0};
                        if (++n > 8) return;
                        XELOGI("Emitter #{}: r3={:08X} r4={:08X} r5={:08X} "
                               "r6={:08X} r7={:08X} lr={:08X}",
                               static_cast<uint32_t>(n),
                               static_cast<uint32_t>(c->r[3]),
                               static_cast<uint32_t>(c->r[4]),
                               static_cast<uint32_t>(c->r[5]),
                               static_cast<uint32_t>(c->r[6]),
                               static_cast<uint32_t>(c->r[7]),
                               static_cast<uint32_t>(c->lr));
                      });
                  ks->processor()->AddBreakpoint(emit_bp.get());
                  XELOGI("Emitter trace installed at 819F5D18");
                }
                // 819F7F20 hands the emitter r4/r5 straight from its own
                // arg2/arg3, and both arrive 0. It has 8 callers and the
                // emitter's lr (819F7FB4) is inside 819F7F20 itself, so it
                // cannot say which one is on the Guide's path. Log lr here to
                // identify the caller, and the args to see whether the zeros
                // originate at this level or above it.
                static std::unique_ptr<cpu::Breakpoint> drawfn_bp;
                if (cvars::guide_trace_emitter && !drawfn_bp) {
                  drawfn_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x819F7F20ull,
                      [](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                         uint64_t host_pc) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) return;
                        auto* c = th->thread_state()->context();
                        static std::atomic<uint32_t> n{0};
                        if (++n > 12) return;
                        XELOGI("DrawFn #{}: r3={:08X} r4={:08X} r5={:08X} "
                               "r6={:08X} lr={:08X}",
                               static_cast<uint32_t>(n),
                               static_cast<uint32_t>(c->r[3]),
                               static_cast<uint32_t>(c->r[4]),
                               static_cast<uint32_t>(c->r[5]),
                               static_cast<uint32_t>(c->r[6]),
                               static_cast<uint32_t>(c->lr));
                      });
                  ks->processor()->AddBreakpoint(drawfn_bp.get());
                  XELOGI("DrawFn trace installed at 819F7F20");
                }
                // The vtable[20] crash is a null [wrapper+0x0C]: 8191AFD0
                // does `lwz r3,12(r31)` with r31 = its first argument and
                // hands that straight to 819DEA70, which passes it on to
                // 819DE8F8 where [r3+0x24] faults with r3=0. But GuideLendFB
                // reads [40877E00+0x0C] = 40870D00, non-null, a second into
                // boot - so either a DIFFERENT wrapper reaches here, or that
                // slot is cleared again. Read the wrapper identity and its
                // device slot instead of assuming which.
                static std::unique_ptr<cpu::Breakpoint> wrapdev_bp;
                if (cvars::guide_trace_devsetup && !wrapdev_bp) {
                  wrapdev_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x8191AFD0ull,
                      [ks](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                           uint64_t host_pc) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) return;
                        auto* c = th->thread_state()->context();
                        static std::atomic<uint32_t> n{0};
                        if (++n > 10) return;
                        uint32_t wrap = static_cast<uint32_t>(c->r[3]);
                        auto* m = ks->memory();
                        uint32_t dev = 0;
                        if (wrap) {
                          if (auto* hp = m->TranslateVirtual(wrap + 0x0Cu)) {
                            dev = xe::load_and_swap<uint32_t>(hp);
                          }
                        }
                        XELOGI("WrapRender #{}: wrapper={:08X} [+0C]={:08X} "
                               "lr={:08X}",
                               static_cast<uint32_t>(n), wrap, dev,
                               static_cast<uint32_t>(c->lr));
                      });
                  ks->processor()->AddBreakpoint(wrapdev_bp.get());
                  XELOGI("WrapRender trace installed at 8191AFD0");
                }
                // Localise the vtable[20] hang. With [dc+134] cleared the
                // draw call never returns: WrapRender fires at 8191AFD0 and
                // then the title thread spins in KeWaitForMultipleObjects.
                // The frames below it are 819DEA70 and 819DE8F8 (the crash's
                // own unwind), so whichever is the LAST to log bounds the
                // hang to one function. Breakpoint overhead is what makes the
                // race hang instead of crash, so tracing is the right regime
                // for this question rather than a distortion of it.
                struct HangSite { uint32_t addr; const char* name; };
                // Neither 819DEA70 nor 819DE8F8 fired while WrapRender did,
                // and 8191AFD0 is straight-line from entry to its call:
                //   8191B004  lwz r3,12(r31)
                //   ...       register moves
                //   8191B020  bl 819DEA70
                // Nothing there can block, so bracket the inside of the
                // function too. If B004 fires and B020 does not, the hang is
                // in code that cannot hang - which would instead indict the
                // breakpoint mechanism, and that is worth knowing before any
                // more of this path is read.
                static const HangSite kHangSites[] = {
                    {0x8191B004u, "8191B004-lwz"},
                    {0x8191B020u, "8191B020-call"},
                    {0x819DEA70u, "819DEA70"},
                    {0x819DE8F8u, "819DE8F8"}};
                static std::vector<std::unique_ptr<cpu::Breakpoint>> hang_bps;
                if (cvars::guide_trace_hang && hang_bps.empty()) {
                  for (const auto& site : kHangSites) {
                    const char* nm = site.name;
                    auto bp = std::make_unique<cpu::Breakpoint>(
                        ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                        static_cast<uint64_t>(site.addr),
                        [nm](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                             uint64_t host_pc) {
                          auto* th = kernel::XThread::GetCurrentThread();
                          if (!th) return;
                          auto* c = th->thread_state()->context();
                          static std::atomic<uint32_t> n{0};
                          if (++n > 24) return;
                          XELOGI("HangSite {} #{}: r3={:08X} r4={:08X} "
                                 "lr={:08X}",
                                 nm, static_cast<uint32_t>(n),
                                 static_cast<uint32_t>(c->r[3]),
                                 static_cast<uint32_t>(c->r[4]),
                                 static_cast<uint32_t>(c->lr));
                        });
                    ks->processor()->AddBreakpoint(bp.get());
                    hang_bps.push_back(std::move(bp));
                  }
                  XELOGI("Hang trace installed at {} sites",
                         hang_bps.size());
                }
                // 819F7F20 passes its 4th argument (r6) down to 819F5D18 as
                // r8, which becomes r14 there and is dereferenced at +32
                // without a guard. 819F7F20 itself guards the same read. Log
                // r6 and lr to see which caller supplies the null.
                // 81A0FA80 allocates the front buffer (819E7310) and stores
                // it at [r29+3F74] at 81A0FCD4, then binds RT0 further down.
                // The bind is observed to happen but no device ends up with a
                // front buffer, so either the store runs against a third
                // object or the bind is reachable without it. Break on both.
                static std::unique_ptr<cpu::Breakpoint> fb_bp;
                if (cvars::guide_trace_setrendertarget && !fb_bp) {
                  fb_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x81A0FCD4ull,
                      [](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                         uint64_t host_pc) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) return;
                        auto* c = th->thread_state()->context();
                        static std::atomic<uint32_t> n{0};
                        if (++n > 10) return;
                        XELOGI("FrontBufferStore: [r29={:08X} +3F74] = r3={:08X}"
                               "  r31={:08X} lr={:08X}",
                               static_cast<uint32_t>(c->r[29]),
                               static_cast<uint32_t>(c->r[3]),
                               static_cast<uint32_t>(c->r[31]),
                               static_cast<uint32_t>(c->lr));
                      });
                  ks->processor()->AddBreakpoint(fb_bp.get());
                  XELOGI("FrontBufferStore trace installed at 81A0FCD4");
                }
                static std::unique_ptr<cpu::Breakpoint> r6_bp;
                if (cvars::guide_trace_setrendertarget && !r6_bp) {
                  r6_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x819F7F20ull,
                      [](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                         uint64_t host_pc) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) return;
                        auto* c = th->thread_state()->context();
                        static std::atomic<uint32_t> n{0};
                        uint32_t k = ++n;
                        if (k > 30) return;
                        XELOGI("819F7F20 #{}: r3={:08X} r4={:08X} r5={:08X} "
                               "r6={:08X} r7={:08X} lr={:08X} {}",
                               k, static_cast<uint32_t>(c->r[3]),
                               static_cast<uint32_t>(c->r[4]),
                               static_cast<uint32_t>(c->r[5]),
                               static_cast<uint32_t>(c->r[6]),
                               static_cast<uint32_t>(c->r[7]),
                               static_cast<uint32_t>(c->lr),
                               c->r[6] ? "" : "  <-- r6 NULL");
                      });
                  ks->processor()->AddBreakpoint(r6_bp.get());
                  XELOGI("819F7F20 trace installed");
                }
              // Watchdog: sample the guest PC of whichever thread parks in
              // the Guide render-begin call, and report a histogram. Every
              // stall theory so far has been derived from disassembly and
              // then tested by forcing an exit condition; three in a row
              // turned out to target loops the thread was not in. This reads
              // where it actually is.
              if (cvars::guide_stall_watchdog > 0) {
                static bool sw_started = false;
                if (!sw_started) {
                  sw_started = true;
                  auto* procw = ks->processor();
                  int wdelay = cvars::guide_stall_watchdog;
                  std::thread([procw, wdelay]() {
                    xe::threading::set_name("GuideStallWatch");
                    void* h = nullptr;
                    for (int i = 0; i < 900 && !h; ++i) {
                      h = kernel::xboxkrnl::GuideStallThread();
                      if (!h) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(50));
                      }
                    }
                    if (!h) {
                      XELOGW("StallWatch: no thread ever entered render-begin");
                      return;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::seconds(wdelay));
                    auto* cc = procw->backend()->code_cache();
                    std::map<uint32_t, int> hist;
                    std::map<std::string, int> host_hist;
                    std::vector<uint32_t> stack_guest;
                    int nosample = 0, nonguest = 0;
                    for (int i = 0; i < 40; ++i) {
                      CONTEXT ctx = {};
                      ctx.ContextFlags = CONTEXT_CONTROL;
                      uint64_t rip = 0, rsp = 0;
                      if (SuspendThread(reinterpret_cast<HANDLE>(h)) !=
                          static_cast<DWORD>(-1)) {
                        if (GetThreadContext(reinterpret_cast<HANDLE>(h),
                                             &ctx)) {
                          rip = ctx.Rip;
                          rsp = ctx.Rsp;
                        }
                        ResumeThread(reinterpret_cast<HANDLE>(h));
                      }
                      // Walk the stack for return addresses that land in
                      // JIT'd guest code. The PC alone says "blocked in
                      // ntdll", which is true and useless - it does not say
                      // which guest call reached a host wait. Return
                      // addresses left on the stack do.
                      if (rsp && stack_guest.empty()) {
                        // Bound the read with VirtualQuery rather than SEH -
                        // __try cannot be used in a lambda that needs object
                        // unwinding, and the stack's committed region is
                        // exactly the extent that is safe to read anyway.
                        MEMORY_BASIC_INFORMATION mbi = {};
                        size_t words = 0;
                        if (VirtualQuery(reinterpret_cast<LPCVOID>(rsp), &mbi,
                                         sizeof(mbi)) &&
                            mbi.State == MEM_COMMIT) {
                          uint64_t end = reinterpret_cast<uint64_t>(
                                             mbi.BaseAddress) +
                                         mbi.RegionSize;
                          words = static_cast<size_t>((end - rsp) / 8);
                          // 8KB was too small. A guest call chain that deep
                          // leaves its return addresses well above rsp, and
                          // capping the scan there reported a single frame -
                          // which reads as "not in guest code" when it may
                          // just be "not in the first 8KB".
                          if (words > 65536) words = 65536;
                        }
                        auto* p64 = reinterpret_cast<uint64_t*>(rsp);
                        for (size_t q = 0; q < words; ++q) {
                          uint64_t v = p64[q];
                          if (v < 0x10000) continue;
                          auto* gf = cc->LookupFunction(v);
                          if (gf) {
                            uint32_t ga = gf->MapMachineCodeToGuestAddress(v);
                            if (ga) stack_guest.push_back(ga);
                            if (stack_guest.size() >= 64) break;
                          }
                        }
                      }
                      if (!rip) {
                        ++nosample;
                      } else {
                        auto* f = cc->LookupFunction(rip);
                        if (!f) {
                          ++nonguest;
                          // Not guest code. Name the host module and offset -
                          // "40 samples in host code" says the thread is
                          // blocked in Xenia or in Windows, but not which,
                          // and those need completely different fixes.
                          HMODULE hm = nullptr;
                          if (GetModuleHandleExA(
                                  GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCSTR>(rip), &hm) &&
                              hm) {
                            char mp[MAX_PATH] = {};
                            GetModuleFileNameA(hm, mp, MAX_PATH);
                            const char* base = strrchr(mp, '\\');
                            uint64_t off =
                                rip - reinterpret_cast<uint64_t>(hm);
                            host_hist[fmt::format("{}+{:X}",
                                                  base ? base + 1 : mp, off)]++;
                          } else {
                            host_hist["<unmapped>"]++;
                          }
                        } else {
                          hist[f->MapMachineCodeToGuestAddress(rip)]++;
                        }
                      }
                      std::this_thread::sleep_for(
                          std::chrono::milliseconds(25));
                    }
                    XELOGI("StallWatch: {} samples in guest code, {} host, "
                           "{} failed",
                           static_cast<int>(hist.size()), nonguest, nosample);
                    for (auto& kv : hist) {
                      XELOGI("StallWatch: guest {:08X}  x{}", kv.first,
                             kv.second);
                    }
                    for (auto& kv : host_hist) {
                      XELOGI("StallWatch: host  {}  x{}", kv.first, kv.second);
                    }
                    if (stack_guest.empty()) {
                      XELOGI("StallWatch: NO guest return addresses on the "
                             "stack - the wait was entered from host code, "
                             "not from guest code we called");
                    } else {
                      std::string chain;
                      for (uint32_t g : stack_guest) {
                        chain += fmt::format("{:08X} ", g);
                      }
                      XELOGI("StallWatch: guest frames on stack: {}", chain);
                    }
                  }).detach();
                  XELOGI("StallWatch: armed, sampling {}s after the call",
                         wdelay);
                }
              }
              if (cvars::guide_force_cmdbuf_complete > 0) {
                static bool fc_started = false;
                if (!fc_started) {
                  fc_started = true;
                  auto* fm4 = ks->memory();
                  int delay = cvars::guide_force_cmdbuf_complete;
                  std::thread([fm4, delay]() {
                    xe::threading::set_name("CmdBufComplete");
                    std::this_thread::sleep_for(std::chrono::seconds(delay));
                    auto rd4 = [fm4](uint32_t a) {
                      return a ? xe::load_and_swap<uint32_t>(
                                     fm4->TranslateVirtual(a)) : 0u;
                    };
                    for (int i = 0; i < 4000; ++i) {
                      for (uint32_t slot : {kernel::xboxkrnl::XamDeviceSlot(), 0x801E6FC8u}) {
                        uint32_t dev = rd4(slot);
                        if (!dev) continue;
                        auto* p = fm4->TranslateVirtual(dev + 0x2B3Du);
                        // Report each device ONCE with the bit's state as
                        // found. The loop below only logs when it flips the
                        // bit, so a device that already had it set is
                        // indistinguishable from one never seen - which made
                        // it impossible to tell whether the draw's device was
                        // reached at all.
                        {
                          static std::set<uint32_t> seen_devs;
                          if (seen_devs.insert(dev).second) {
                            XELOGI("CmdBufComplete: device {:08X} slot {:08X} "
                                   "[2B3D]={:02X} bit1={}",
                                   dev, slot, p ? *p : 0,
                                   (p && (*p & 0x02)) ? "already set"
                                                      : "clear");
                          }
                        }
                        if (p && !(*p & 0x02)) {
                          *p = static_cast<uint8_t>(*p | 0x02);
                          XELOGI("CmdBufComplete: set bit1 of [{:08X}+2B3D]",
                                 dev);
                        }
                        // Advance the GPU writeback word this device's fence
                        // wait actually polls. Disassembling 81A03E90 gives
                        // the loop exactly:
                        //   r11 = [[device+0x2B10]]      writeback word
                        //   exit when (r10-r30) >= (r10-r11)
                        //   else bl 819F4488 and spin
                        // so the spin ends when the writeback reaches the
                        // fence value. guide_fake_gpu_writeback does this
                        // already but is nested inside the StallProbe block
                        // and never runs unless that whole apparatus is
                        // enabled - so it has been reporting nothing for a
                        // treatment it never applied. Driving it from here
                        // keeps it tied to the device the draw uses.
                        uint32_t wbp = rd4(dev + 0x2B10u);
                        // [device+0x2B10] is NULL on both devices - measured,
                        // not assumed. The fence loop at 81A03E90 does
                        // `lwz r11, 0(r11)` on it, so it reads guest address
                        // zero every iteration and the comparison can never
                        // satisfy: that is the hang. Its known writers
                        // (81A0F858, 81A0FE48) only populate it on a device
                        // that completed bring-up, which this one did not.
                        // Give it a word of its own so the wait has something
                        // real to poll. A probe, not a system command buffer.
                        if (!wbp) {
                          static std::map<uint32_t, uint32_t> made;
                          auto it = made.find(dev);
                          if (it == made.end()) {
                            uint32_t nw = fm4->SystemHeapAlloc(16, 16);
                            if (nw) {
                              std::memset(fm4->TranslateVirtual(nw), 0, 16);
                              xe::store_and_swap<uint32_t>(
                                  fm4->TranslateVirtual(dev + 0x2B10u), nw);
                              made[dev] = nw;
                              wbp = nw;
                              XELOGI("CmdBufComplete: [{:08X}+2B10] was NULL, "
                                     "gave it writeback word {:08X}",
                                     dev, nw);
                            }
                          } else {
                            wbp = it->second;
                          }
                        }
                        if (wbp) {
                          auto* wp = fm4->TranslateVirtual(wbp);
                          if (wp) {
                            // TRACK the fence counter, do not just increment.
                            // The loop compares wrapped distances:
                            //   r10 = [dev+0x2B1C]   fence counter
                            //   r30 = target
                            //   r11 = [[dev+0x2B10]] writeback
                            //   exit when (r10-r30) >= (r10-r11)
                            // Running the writeback ahead by an arbitrary
                            // amount makes (r10-r11) wrap to a huge value and
                            // the test harder to satisfy, not easier. Parking
                            // it exactly on the fence counter makes (r10-r11)
                            // zero, which satisfies the comparison for any
                            // target at or behind the counter.
                            uint32_t cur = xe::load_and_swap<uint32_t>(wp);
                            uint32_t fence = rd4(dev + 0x2B1Cu);
                            xe::store_and_swap<uint32_t>(wp, fence);
                            static std::set<uint32_t> wb_seen;
                            if (wb_seen.insert(dev).second) {
                              XELOGI("CmdBufComplete: writeback [{:08X}] via "
                                     "[{:08X}+2B10] was {:08X}, parking on "
                                     "fence [2B1C]={:08X}",
                                     wbp, dev, cur, fence);
                            }
                          }
                        }
                      }
                      std::this_thread::sleep_for(
                          std::chrono::milliseconds(5));
                    }
                  }).detach();
                  XELOGI("CmdBufComplete: armed, {}s", delay);
                }
              }
              if (cvars::guide_watch_null_render) {
                static bool nr_started = false;
                if (!nr_started) {
                  nr_started = true;
                  auto* nm = ks->memory();
                  std::thread([nm]() {
                    xe::threading::set_name("NullRenderWatch");
                    auto rd5 = [nm](uint32_t a) {
                      if (a >= 0x81000000u && a < 0x82000000u &&
                          !XamConstOk(a, 4)) {
                        return uint32_t(0);
                      }
                      return a ? xe::load_and_swap<uint32_t>(
                                     nm->TranslateVirtual(a)) : 0u;
                    };
                    uint32_t last_ctx = 0, last_flag = 0xFFFFFFFF;
                    for (int i = 0; i < 120000; ++i) {
                      uint32_t ctx = rd5(0x81D6C978u);
                      if (ctx != last_ctx) {
                        XELOGI("NullRenderWatch: ctx {:08X} -> {:08X}",
                               last_ctx, ctx);
                        last_ctx = ctx;
                        last_flag = 0xFFFFFFFF;
                      }
                      if (ctx) {
                        uint32_t f = rd5(ctx + 0x1Cu);
                        if (f != last_flag) {
                          XELOGI("NullRenderWatch: [ctx+1C] {:08X} -> {:08X}",
                                 last_flag, f);
                          last_flag = f;
                        }
                      }
                      std::this_thread::sleep_for(
                          std::chrono::microseconds(250));
                    }
                  }).detach();
                  XELOGI("NullRenderWatch: armed");
                }
              }
              if (cvars::guide_watch_front_buffer) {
                static bool watch_started = false;
                if (!watch_started) {
                  watch_started = true;
                  auto* wm = ks->memory();
                  std::thread([wm]() {
                    xe::threading::set_name("FrontBufferWatch");
                    // "a != 0" is not a mapped-address test. This runs in a
                    // 60000-iteration poll over globals that hold garbage
                    // until the Guide device exists - which in subcommand 0
                    // never happens - so an unmapped read faults the host and
                    // kills the run before anything draws. Mirror the guard
                    // `rp` already uses: look the heap up and check access.
                    auto rdw = [wm](uint32_t a) -> uint32_t {
                      if (a < 0x1000u) return 0u;
                      auto* hp = wm->LookupHeap(a);
                      if (!hp || hp->QueryRangeAccess(a, a + 4) ==
                                     xe::memory::PageAccess::kNoAccess) {
                        return 0u;
                      }
                      return xe::load_and_swap<uint32_t>(wm->TranslateVirtual(a));
                    };
                    uint32_t last_dev[2] = {0, 0}, last_fb[2] = {0, 0};
                    const uint32_t slots[2] = {kernel::xboxkrnl::XamDeviceSlot(), 0x801E6FC8u};
                    const char* names[2] = {"81D43684", "801E6FC8"};
                    for (int i = 0; i < 60000; ++i) {
                      for (int k = 0; k < 2; ++k) {
                        uint32_t dev = rdw(slots[k]);
                        uint32_t fb = dev ? rdw(dev + 0x3F74u) : 0;
                        if (dev != last_dev[k]) {
                          XELOGI("FBWatch[{}]: device {:08X} -> {:08X}",
                                 names[k], last_dev[k], dev);
                          last_dev[k] = dev;
                          last_fb[k] = 0;
                        }
                        if (fb != last_fb[k]) {
                          XELOGI("FBWatch[{}]: dev {:08X} front buffer "
                                 "{:08X} -> {:08X}",
                                 names[k], dev, last_fb[k], fb);
                          last_fb[k] = fb;
                        }
                      }
                      std::this_thread::sleep_for(
                          std::chrono::microseconds(500));
                    }
                    XELOGI("FBWatch: finished");
                  }).detach();
                  XELOGI("FBWatch: started");
                }
              }
              if (cvars::guide_bootstrap_before_device &&
                  cvars::guide_bootstrap_on_title_thread) {
                // The mode-1 creator below never returns, so anything after it
                // is dead code in that configuration - including the queue
                // call that starts the Guide bootstrap.
                // Phase 649: applied here rather than from the xam bootstrap
                // block, which runs AFTER the render-host call it is meant to
                // affect - the same ordering mistake as PresentRTPatch in
                // phase 613, and visible the same way: the patch verified
                // clean and changed nothing.
                if (cvars::guide_patch_class_reg) {
                  kernel::xboxkrnl::GuidePatchWord(0x8194F164u, 0x41820018u,
                                                   0x48000018u,
                                                   "ClassRegPatch(early)");
                }
                XELOGI("Guide button: queueing bootstrap BEFORE device creation");
                GuideHudSlotMark(ks, "after QueueGuideBootstrap is queued");
                kernel::xboxkrnl::QueueGuideBootstrap(
                    hud_base, obj, cvars::guide_use_title_device, skin_mod);
              }
              // Every address in this block - the two import thunks, both
              // device creators, the boot entries, 81D3C8E8 - is a dashroot
              // constant. They all fall inside retail's image too, so a range
              // check cannot reject them; what does is that the thunks read
              // DEADC0DE there, which is Xenia's unresolved-import marker.
              // Reading a foreign build's memory through them produced
              // "bit200=clear" (meaningless, not a finding) and then crashed
              // executing 8178F748 as if it were the device creator.
              bool xam_layout_ok = rd(0x815F048Cu) != 0xDEADC0DEu &&
                                   rd(0x815F044Cu) != 0xDEADC0DEu;
              if (cvars::guide_create_xam_device && !xam_layout_ok) {
                XELOGW("Guide button: xam device-creation block is keyed to "
                       "dashroot's layout (thunks read DEADC0DE here) - "
                       "skipping it on this build");
              }
              if (cvars::guide_create_xam_device && xam_layout_ok) {
                uint32_t gate_ptr = rd(0x815F048Cu);
                uint32_t gate = gate_ptr ? rd(gate_ptr) : 0;
                XELOGI("Guide button: VdGlobalDevice(801E6FC4) = {:08X}",
                       rd(0x801E6FC4u));
                // 817439D0 registers callbacks by dispatching through
                // [[815F044C]]->vtable[6]; if that object is null it returns
                // having registered nothing. 81723D98 registers 81723D70
                // there, and 81723D70 is the head of the chain that would
                // eventually run 817915A0 and signal the three events.
                {
                  uint32_t slot = rd(0x815F044Cu);
                  uint32_t obj = slot ? rd(slot) : 0;
                  XELOGI("Guide button: callback registry [815F044C]={:08X} "
                         "-> obj {:08X} {}",
                         slot, obj,
                         obj ? "(present)" : "(NULL - registration is a no-op)");
                }
                XELOGI("Guide button: device gate [815F048C]={:08X} "
                       "[*]={:08X} bit200={}",
                       gate_ptr, gate, (gate & 0x200) ? "set" : "clear");
                GuideHudSlotMark(ks, "device block, before the creator args");
                uint64_t ca[] = {0};
                // 8178F748 asks 819F4D28 for a mode-2 device, which skips the
                // ring buffer bring-up by design. 8178E9F0 asks for mode 1,
                // which takes it - and which nothing inside xam ever calls, so
                // on hardware it comes from the system boot.
                uint32_t create_fn =
                    cvars::guide_create_primary_device ? 0x8178E9F0u
                                                       : 0x8178F748u;
                // Mode 1 calls KeGetCurrentProcessType and asserts unless the
                // matching device global is still empty. As SYSTEM it looks at
                // VdGlobalXamDevice (empty); as anything else at VdGlobalDevice
                // (the title's, already set).
                uint8_t saved_pt = 0, saved_ptd = 0;
                kernel::XThread* cur = kernel::XThread::GetCurrentThread();
                if (cvars::guide_system_process_type && cur) {
                  auto* kt = cur->guest_object<kernel::X_KTHREAD>();
                  saved_pt = kt->process_type;
                  saved_ptd = kt->process_type_dup;
                  kt->process_type = kernel::X_PROCTYPE_SYSTEM;
                  kt->process_type_dup = kernel::X_PROCTYPE_SYSTEM;
                  XELOGI("Guide button: process type {} -> SYSTEM for device "
                         "creation", saved_pt);
                }
                if (cvars::guide_call_boot_entry) {
                  // 81751428 crashed on a null global at 81D3C8E8. The only
                  // code that takes that global's address is 81727500, which
                  // hands it to an initialiser - and 81727500 is reached only
                  // from 81750FA8, another entry point with no caller inside
                  // xam. So the dependency is derived, not guessed: run the
                  // initialiser first.
                  // 81750FA8 already runs every session, but with its first
                  // argument zero - and that argument is r29, which gates the
                  // call to 81727500 that initialises 81D3C8E8
                  // ("cmpwi cr6,r29,0 / bne -> 81751294"). Calling it with 0,
                  // as the previous attempt did, takes the same path that
                  // skips the initialiser. Pass 1.
                  // Publish this thread so guide_stall_watchdog can sample
                  // it. The boot entry blocks and never returns (phase 237),
                  // and the watchdog built in phase 182 names exactly this
                  // kind of stall - it just has never been pointed at this
                  // call.
                  {
                    auto* bt = kernel::XThread::GetCurrentThread();
                    if (bt && bt->thread()) {
                      kernel::xboxkrnl::GuidePublishStallThread(
                          bt->thread()->native_handle());
                      XELOGI("Guide button: published boot-entry thread for "
                             "the stall watchdog");
                    }
                  }
                  for (uint32_t entry : {0x81750FA8u, 0x81751428u}) {
                    uint64_t ba[] = {entry == 0x81750FA8u ? 1u : 0u};
                    uint64_t br = ks->processor()->Execute(ts, entry, ba,
                                                           xe::countof(ba));
                    XELOGI("Guide button: boot entry {:08X} returned {:08X}; "
                           "[81D3C8E8]={:08X}",
                           entry, static_cast<uint32_t>(br),
                           xe::load_and_swap<uint32_t>(
                               ks->memory()->TranslateVirtual(0x81D3C8E8u)));
                  }
                }
                if (cvars::guide_restore_title_ring) {
                  kernel::xboxkrnl::GuideSaveTitleRing();
                }
                if (cvars::guide_xam_task_init) {
                  // Phase 1056: xam's task pool, in xam's own boot order (its
                  // init sits immediately before this device creator in
                  // 81751718). No arguments: 8177B970 finds the pool at
                  // 81D423C0 itself.
                  auto rdw = [&](uint32_t a) {
                    return xe::load_and_swap<uint32_t>(ks->memory()->TranslateVirtual(a));
                  };
                  XELOGI("GuideTaskInit: before: [81D42450]={:08X} [81D424A8]={:08X} [81D424E4]={:08X} [81D42514]={:08X}",
                         rdw(0x81D42450u), rdw(0x81D424A8u), rdw(0x81D424E4u), rdw(0x81D42514u));
                  uint64_t ta[] = {0};
                  uint64_t tr = ks->processor()->Execute(ts, kernel::xboxkrnl::GuideConst(0x8177BF80u), ta, 1);
                GuideHudSlotMark(ks, "after xam pool init 8177BF80");
                  XELOGI("GuideTaskInit: 8177BF80 -> {:08X}; after: [81D42450]={:08X} [81D424A8]={:08X} [81D424E4]={:08X} [81D42514]={:08X}",
                         uint32_t(tr), rdw(0x81D42450u), rdw(0x81D424A8u), rdw(0x81D424E4u), rdw(0x81D42514u));
                }
                GuideHudSlotMark(ks, "before guide_xam_app_task");
                if (cvars::guide_xam_app_task) {
                  // Phase 1056: the app manager's task slot.
                  auto* m = ks->memory();
                  auto rdw = [&](uint32_t a) { return xe::load_and_swap<uint32_t>(m->TranslateVirtual(a)); };
                  uint32_t cur_task = rdw(0x81D43C54u);
                  if (!cur_task) {
                    uint32_t out = m->SystemHeapAlloc(16, 16);
                    if (out) {
                      xe::store_and_swap<uint32_t>(m->TranslateVirtual(out), 0);
                      uint64_t aa[] = {0ull, out};
                      uint64_t ar = ks->processor()->Execute(
                          ts, kernel::xboxkrnl::GuideConst(0x8177C408u), aa, 2);
                      uint32_t task = rdw(out);
                      if (task) {
                        xe::store_and_swap<uint32_t>(m->TranslateVirtual(0x81D43C54u), task);
                      }
                      XELOGI("GuideAppTask: 8177C408 -> {:08X}, task {:08X}; [81D43C50+4] now {:08X} (+0 state {:08X}, +28 {:08X})",
                             uint32_t(ar), task, rdw(0x81D43C54u), rdw(0x81D43C50u), rdw(0x81D43C78u));
                    }
                  } else {
                    XELOGI("GuideAppTask: [81D43C50+4] already {:08X}", cur_task);
                  }
                }
                XELOGI("Guide button: calling device creator {:08X}", create_fn);
                {
                  // Snapshot the ring before the creator: mode 1 reaches
                  // VdInitializeRingBuffer and the title stops swapping.
                  // If the ring is merely repointed, restoring it may let
                  // the title carry on; if it is torn down some other way,
                  // that shows here instead.
                  auto* rgs = ks->emulator()->graphics_system();
                  if (rgs && rgs->command_processor()) {
                    uint32_t rp = 0, rs = 0, rw = 0;
                    rgs->command_processor()->GuideRingState(&rp, &rs, &rw);
                    XELOGI("GuideRing: before creator ptr={:08X} "
                           "size={:08X} wb={:08X}", rp, rs, rw);
                    std::thread([rgs, rp, rs, rw]() mutable {
                      xe::threading::set_name("GuideRingWatch");
                      for (int i = 0; i < 60; ++i) {
                        xe::threading::Sleep(std::chrono::seconds(1));
                        uint32_t p2 = 0, s2 = 0, w2 = 0;
                        rgs->command_processor()->GuideRingState(&p2, &s2,
                                                                &w2);
                        // Phase 610: keep sampling after the change - the
                        // question is no longer whether the ring moves but
                        // whether anything writes to the one it moved to.
                        {
                          uint32_t rd_i = 0, wr_i = 0;
                          rgs->command_processor()->GuideRingPointers(&rd_i,
                                                                      &wr_i);
                          XELOGI("GuideRingPtrs {}s: ring={:08X} rptr={} "
                                 "wptr={}", i + 1, p2, rd_i, wr_i);
                        }
                        if (p2 != rp || s2 != rs || w2 != rw) {
                          XELOGI("GuideRing: CHANGED after {}s ptr {:08X}"
                                 "->{:08X} size {:08X}->{:08X} wb "
                                 "{:08X}->{:08X}",
                                 i + 1, rp, p2, rs, s2, rw, w2);
                          if (cvars::guide_restore_title_ring) {
                            // Isolated test of the core claim: the ring
                            // is re-pointed, not destroyed, so handing
                            // the registers back should let the title
                            // resume. Give mode-1 bring-up a moment to
                            // finish before taking the ring back.
                            xe::threading::Sleep(
                                std::chrono::seconds(3));
                            kernel::xboxkrnl::GuideRestoreTitleRing();
                            return;
                          }
                          // Phase 610: do NOT stop watching here. The
                          // interesting period begins after the handover -
                          // whether anything then writes to the new ring -
                          // and returning on first change is why that was
                          // never observed. Re-baseline and keep sampling.
                          rp = p2;
                          rs = s2;
                          rw = w2;
                        }
                      }
                      XELOGI("GuideRing: unchanged for 60s");
                    }).detach();
                  }
                }
                if (cvars::guide_stall_probe_seconds > 0 && cur) {
                  // CreateDevice may never return, so the probe has to live on
                  // a host thread of its own. Collect raw RIPs first and
                  // resolve them only after resuming - LookupFunction takes the
                  // code cache lock, and holding a suspended thread across that
                  // is a deadlock waiting to happen.
                  void* nh = cur->thread() ? cur->thread()->native_handle()
                                           : nullptr;
                  int delay = cvars::guide_stall_probe_seconds;
                  auto* proc = ks->processor();
                  if (nh) {
                    std::thread([nh, delay, proc, ksp = ks]() {
                      xe::threading::set_name("GuideStallProbe");
                      std::this_thread::sleep_for(std::chrono::seconds(delay));
                      uint64_t rips[8] = {};
                      uint32_t gr[8][4] = {};
                      for (int i = 0; i < 8; ++i) {
                        CONTEXT ctx = {};
                        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                        if (SuspendThread(reinterpret_cast<HANDLE>(nh)) !=
                            static_cast<DWORD>(-1)) {
                          if (GetThreadContext(reinterpret_cast<HANDLE>(nh),
                                               &ctx)) {
                            rips[i] = ctx.Rip;
                            // The x64 backend keeps the PPCContext pointer in
                            // rsi (X64Emitter::GetContextReg) and the guest
                            // membase in rdi. PPCContext is host memory, so
                            // the guest GPRs can be read straight out of it
                            // while the thread is held.
                            auto* gc =
                                reinterpret_cast<cpu::ppc::PPCContext*>(
                                    ctx.Rsi);
                            if (gc) {
                              gr[i][0] = static_cast<uint32_t>(gc->r[3]);
                              gr[i][1] = static_cast<uint32_t>(gc->r[11]);
                              gr[i][2] = static_cast<uint32_t>(gc->r[29]);
                              gr[i][3] = static_cast<uint32_t>(gc->r[31]);
                            }
                          }
                          ResumeThread(reinterpret_cast<HANDLE>(nh));
                        }
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(120));
                      }
                      // 819F4488 polls [[device+2B10]] against [arg+8] with
                      // the 819F3FC8 delay between reads. Sample the polled
                      // word itself so "never advances" is measured, not
                      // assumed. 81D43684 is xam's device slot.
                      {
                        auto* m = ksp->memory();
                        auto rdp = [m](uint32_t a) {
                          return xe::load_and_swap<uint32_t>(
                              m->TranslateVirtual(a));
                        };
                        // Use the object the poll actually holds in r29,
                        // not the device global - they are different objects,
                        // and reading the global is what made an earlier pass
                        // report [2B10]=0 and reject a correct guess.
                        uint32_t dv = gr[0][2] ? gr[0][2] : rdp(kernel::xboxkrnl::XamDeviceSlot());
                        uint32_t frame = gr[0][3];
                        if (frame) {
                          XELOGI("StallProbe: poll arg r31={:08X} [r31+8]={:08X}",
                                 frame, rdp(frame + 8));
                        }
                        uint32_t idp = dv ? rdp(dv + 0x2B10u) : 0;
                        XELOGI("StallProbe: device={:08X} [2B10]={:08X}", dv,
                               idp);
                        if (idp) {
                          uint32_t a = rdp(idp);
                          std::this_thread::sleep_for(
                              std::chrono::milliseconds(1500));
                          uint32_t b = rdp(idp);
                          XELOGI("StallProbe: polled word [{:08X}] = {:08X} "
                                 "then {:08X} ({})",
                                 idp, a, b,
                                 a == b ? "UNCHANGED" : "advanced");
                          if (cvars::guide_fake_gpu_writeback) {
                            auto wr = [m](uint32_t addr, uint32_t v) {
                              xe::store_and_swap<uint32_t>(
                                  m->TranslateVirtual(addr), v);
                            };
                            uint32_t v = b;
                            for (int k = 0; k < 24; ++k) {
                              wr(idp, ++v);
                              std::this_thread::sleep_for(
                                  std::chrono::milliseconds(100));
                              CONTEXT c2 = {};
                              c2.ContextFlags = CONTEXT_CONTROL;
                              uint32_t g2 = 0;
                              if (SuspendThread(reinterpret_cast<HANDLE>(nh)) !=
                                  static_cast<DWORD>(-1)) {
                                if (GetThreadContext(
                                        reinterpret_cast<HANDLE>(nh), &c2)) {
                                  auto* f2 =
                                      proc->backend()->code_cache()
                                          ->LookupFunction(c2.Rip);
                                  g2 = f2 ? f2->MapMachineCodeToGuestAddress(
                                                c2.Rip)
                                          : 0;
                                }
                                ResumeThread(reinterpret_cast<HANDLE>(nh));
                              }
                              bool in_spin =
                                  g2 >= 0x819F3FC8u && g2 <= 0x819F4004u;
                              if (k % 6 == 0 || !in_spin) {
                                XELOGI("FakeWriteback[{}]: wrote {:08X}, guest "
                                       "PC {:08X} {}",
                                       k, v, g2,
                                       in_spin ? "(still spinning)"
                                               : "<-- LEFT THE SPIN");
                              }
                              if (!in_spin) break;
                            }
                          }
                        }
                      }
                      auto* cc = proc->backend()->code_cache();
                      for (int i = 0; i < 8; ++i) {
                        if (!rips[i]) {
                          XELOGI("StallProbe[{}]: no sample", i);
                          continue;
                        }
                        auto* f = cc->LookupFunction(rips[i]);
                        uint32_t g =
                            f ? f->MapMachineCodeToGuestAddress(rips[i]) : 0;
                        XELOGI("StallProbe[{}]: guest {:08X}  r3={:08X} "
                               "r11={:08X} r29={:08X} r31={:08X}{}",
                               i, g, gr[i][0], gr[i][1], gr[i][2], gr[i][3],
                               f ? "" : "  (not guest code)");
                      }
                    }).detach();
                  }
                }
                kernel::xboxkrnl::in_xam_createdevice_scope = true;
                // Phase 643: the creator publishes VdGlobalXamDevice and then
                // does not return, which is what forces the bootstrap to be
                // queued ahead of it and therefore onto the wrong device
                // (phase 642). Record which thread it holds, so a waiter can
                // be placed on one it does not.
                {
                  auto* cth = kernel::XThread::GetCurrentThread();
                  XELOGI("GuideCreator: entering {:08X} on thread '{}' "
                         "(handle {:08X})",
                         create_fn, cth ? cth->name() : "?",
                         cth ? cth->handle() : 0u);
                }
                uint64_t cr = ks->processor()->Execute(ts, create_fn, ca,
                                                       xe::countof(ca));
                {
                  auto* cth = kernel::XThread::GetCurrentThread();
                  XELOGI("GuideCreator: RETURNED {:08X} on thread '{}'",
                         static_cast<uint32_t>(cr),
                         cth ? cth->name() : "?");
                }
                kernel::xboxkrnl::in_xam_createdevice_scope = false;
                if (cvars::guide_system_process_type && cur) {
                  auto* kt = cur->guest_object<kernel::X_KTHREAD>();
                  kt->process_type = saved_pt;
                  kt->process_type_dup = saved_ptd;
                }
                XELOGI("Guide button: xam CreateDevice returned {:08X}, "
                       "device now {:08X}",
                       static_cast<uint32_t>(cr), rd(kernel::xboxkrnl::XamDeviceSlot()));
                // Publish it as VdGlobalXamDevice (kernel global 801E6FC8).
                // Xenia stores 0 there with the comment "Pointer to the XAM
                // D3D device, which we don't have", and KernelState has a
                // matching TODO to run graphics notifications as
                // X_PROCTYPE_SYSTEM when it is non-zero. We have one now.
                uint32_t xam_dev = rd(kernel::xboxkrnl::XamDeviceSlot());
                // Phase 960: the device is created in mode 2, which skips its
                // bring-up (959), so its width/height fields stay zero (958)
                // and the projection matrix is later built as 2/0 (952).
                // Filling them during the paint is too late - the matrix is
                // already cached. Fill them here, immediately after creation,
                // before anything reads them.
                if (cvars::guide_fix_device_dims && xam_dev) {
                  auto* dm = ks->memory();
                  static const struct { uint32_t off, val; } kDims[] = {
                      {0x2908u, 0x44200000u}, {0x290Cu, 0x44200000u},
                      {0x2914u, 0x43B40000u}, {0x3220u, 0x44A00000u},
                      {0x3224u, 0x44340000u}, {0x35BCu, 1280u},
                      {0x35C0u, 720u},        {0x558Cu, 1280u},
                      {0x5590u, 720u},
                  };
                  for (auto& d : kDims) {
                    auto* dh = dm->LookupHeap(xam_dev + d.off);
                    if (!dh) continue;
                    xe::store_and_swap<uint32_t>(
                        dm->TranslateVirtual(xam_dev + d.off), d.val);
                  }
                  XELOGI("GuideFixDimsEarly: filled 9 dimension fields on the "
                         "new device {:08X}", xam_dev);
                }
                if (xam_dev) {
                  xe::store_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(0x801E6FC8u), xam_dev);
                  XELOGI("Guide button: VdGlobalXamDevice = {:08X}",
                         rd(0x801E6FC8u));
                }
              }
              {
                // The present path picks its surface as
                //   [dev+0x32A0] ? [dev+0x32A0] : [dev+0x32B0]
                // and faults because both are zero on the device it uses
                // (40870D00), while the bootstrap binds a different one
                // (407CB880). Survey every device pointer we know about and
                // report which of them actually has a surface - that says
                // which device the present should be looking at, instead of
                // guessing between them.
                struct { const char* name; uint32_t at; } devs[] = {
                    {"VdGlobalDevice   ", 0x801E6FC4u},
                    {"VdGlobalXamDevice", 0x801E6FC8u},
                    // Label was "xam device global 81D43684" - a literal
                    // baked into the text, so on retail it printed dashroot's
                    // address next to a value read from 81AA1B14. Print the
                    // address actually used.
                    {"xam device global", kernel::xboxkrnl::XamDeviceSlot()},
                };
                for (auto& d : devs) {
                  uint32_t dev = rd(d.at);
                  XELOGI("DevSurvey: {} {:08X} -> {:08X}  [32A0]={:08X} "
                         "[32B0]={:08X}",
                         d.name, d.at, dev, dev ? rd(dev + 0x32A0u) : 0,
                         dev ? rd(dev + 0x32B0u) : 0);
                }
              }
              if (cvars::guide_bind_depth_scan) {
                // 819DE94C: lwz r9,36(r11) where r11 = [dev+0x32A0], or
                // [dev+0x32B0] when the first is null. It then unpacks width
                // and height bitfields out of [surface+0x24]. Both slots are
                // zero on the Guide device, so that load faults - which is the
                // real reason clearing the null-render flag crashes, and it is
                // not the command buffer. 0x32A0 has no direct stw writer in
                // xam at all; 0x32B0's only writer is 819F3A24 inside
                // 819F38C8, and every guest route there wants the 124-byte
                // parameter block mode 2 passes as null. So borrow a live
                // surface from the title.
                // Minimal probe: no scanning, three reads. The note on
                // guide_bind_title_rt puts the title's render target at
                // [VdGlobalDevice+0x3AC4]; 819DE94C needs [dev+0x32B0] to be
                // a surface whose [+0x24] unpacks as width/height.
                uint32_t tdev = rd(0x801E6FC4u);
                uint32_t gdev = rd(kernel::xboxkrnl::XamDeviceSlot());
                uint32_t cand = tdev ? rd(tdev + 0x3AC4u) : 0;
                uint32_t fc = cand ? rd(cand + 0x24u) : 0;
                XELOGI("DepthProbe: tdev={:08X} gdev={:08X} [t+3AC4]={:08X} "
                       "[cand+24]={:08X} w~{} h~{}",
                       tdev, gdev, cand, fc, ((fc >> 14) & 0x3FFFu) + 1,
                       ((fc >> 3) & 0x7FFFu) + 1);
                if (gdev && cand && fc) {
                  // Both slots. 819DE94C takes 0x32A0 first and only falls
                  // back to 0x32B0, but the draw emitter at 819F5EC4 reads a
                  // *different* surface's [+0x20] format field (low 6 bits
                  // compared against 0x36/0x37/0x3D) and faults with r14 null
                  // when only the depth slot is filled.
                  // 819F5F54-5C: the render targets are an INDEXED array,
                  // [dev + (idx + 0xCA8)*4] = 0x32A0 + idx*4 for idx 0..3,
                  // with idx==4 special-cased to the depth slot at 0x32B0.
                  // That is also why a plain `stw` search finds no writer for
                  // 0x32A0 - it is written with stwx. Fill all five.
                  for (uint32_t i = 0; i <= 4; ++i) {
                    xe::store_and_swap<uint32_t>(
                        ks->memory()->TranslateVirtual(gdev + 0x32A0u + i * 4u),
                        cand);
                  }
                  // 819F5D50 does `mr r14, r8`: the emitter's SIXTH argument
                  // is the front buffer from [dev+0x3F74], and 819F5EC4 is
                  // `lwz r11,32(r14)` faulting with r14 null. Reuse the same
                  // already-validated pointer rather than reading further into
                  // the title device, which is not mapped that far.
                  xe::store_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(gdev + 0x3F74u), cand);
                  XELOGI("DepthProbe: 32A0={:08X} 32B0={:08X} 3F74={:08X}",
                         rd(gdev + 0x32A0u), rd(gdev + 0x32B0u),
                         rd(gdev + 0x3F74u));
                }
              }
              if (cvars::guide_bootstrap_on_title_thread &&
                  !cvars::guide_bootstrap_before_device) {
                kernel::xboxkrnl::QueueGuideBootstrap(
                    hud_base, obj, cvars::guide_use_title_device,
                    skin_mod);
                // xam's UI startup and everything under it check that the
                // caller is xam's recorded UI thread (81D42520). Spoofing that
                // check satisfied the comparison but not the thread's own
                // state, so queue the startup as an APC on the real thread.
                if (cvars::guide_xam_ui_startup) {
                  uint32_t ui_thread = xe::load_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(0x81D42520u));
                  auto threads =
                      ks->object_table()->GetObjectsByType<kernel::XThread>(
                          kernel::XObject::Type::Thread);
                  bool queued = false;
                  for (auto& th : threads) {
                    if (th->guest_object() == ui_thread) {
                      XELOGI("Guide button: queueing xam UI startup {:08X} as "
                             "an APC on xam's UI thread {:08X}",
                             uint32_t(cvars::guide_xam_ui_startup), ui_thread);
                      th->EnqueueApc(cvars::guide_xam_ui_startup, 0, 0, 0);
                      queued = true;
                      break;
                    }
                  }
                  if (!queued) {
                    XELOGW("Guide button: xam UI thread {:08X} not found among "
                           "{} threads",
                           ui_thread, threads.size());
                  }
                }
                {
                  // Second sample of the XUI context. It is well-formed right
                  // after XuiInit - [ctx+0x0C] holds 8178DBD8, a real .text
                  // pointer - but by the time the device context dereferences
                  // it the slot holds 006E0065, two UTF-16 code units. Sample
                  // it again here to narrow when it is overwritten.
                  // 81D6C978 is a dashroot-xam constant; on another build it
                  // is outside the image and reading it host-faults. Bound it
                  // by the loaded module's extent (phase 416).
                  uint32_t xc = XamConstOk(0x81D6C978u, 4)
                                    ? xe::load_and_swap<uint32_t>(
                                          ks->memory()->TranslateVirtual(
                                              0x81D6C978u))
                                    : 0;
                  if (xc) {
                    std::string cw;
                    for (uint32_t i = 0; i < 8; ++i) {
                      cw += fmt::format(
                          "{:08X} ", xe::load_and_swap<uint32_t>(
                                         ks->memory()->TranslateVirtual(
                                             xc + i * 4)));
                    }
                    XELOGI("Guide button: XUI ctx @{:08X} at queue time: {}",
                           xc, cw);
                  }
                }
                // The context is well-formed here and text by the time the
                // device context calls through [ctx+0x0C], so the overwrite
                // happens inside the queued bootstrap. Poll the slot from a
                // host thread and log the transition: that pins the moment
                // against the surrounding log lines without perturbing the
                // guest, the same trick XamTextWatch uses. Reads the context
                // pointer fresh each time so a moved context is not missed.
                if (std::getenv("XENIA_XUICTX_WATCH")) {
                  auto* wmem = ks->memory();
                  std::thread([wmem]() {
                    xe::threading::set_name("XuiCtxWatch");
                    // Watch the context pointer itself as well as the slot.
                    // The first version skipped the iteration when the global
                    // read zero, so a context being destroyed - which clears
                    // it - looked exactly like nothing happening. That false
                    // negative cost a full round of analysis: the answer was a
                    // use-after-free, and the watcher stayed silent through it.
                    uint32_t last = 0, last_ctx = 0;
                    bool primed = false, lent = false;
                    // Same bound for the watcher loop.
                    bool wctx_ok = XamConstOk(0x81D6C978u, 4);
                    if (!wctx_ok) {
                      XELOGW("XuiCtxWatch: 81D6C978 outside this xam image - "
                             "watcher disabled");
                    }
                    for (int i = 0; i < 240000; ++i) {
                      uint32_t c = wctx_ok ? xe::load_and_swap<uint32_t>(
                                       wmem->TranslateVirtual(0x81D6C978u))
                                           : 0;
                      if (primed && c != last_ctx) {
                        XELOGE("XuiCtxWatch: ctx pointer {:08X} -> {:08X}{}",
                               last_ctx, c, c ? "" : "  (CLEARED)");
                      }
                      uint32_t v = 0;
                      if (c) {
                        v = xe::load_and_swap<uint32_t>(
                            wmem->TranslateVirtual(c + 0x0C));
                        if (primed && c == last_ctx && v != last) {
                          XELOGE(
                              "XuiCtxWatch: [{:08X}+0C] changed {:08X} -> "
                              "{:08X}",
                              c, last, v);
                        }
                      }
                      // Lend the title's front buffer to the Guide's device
                      // as soon as that device exists. Doing it in the
                      // composite-draw path is too late: XuiRenderBegin runs
                      // dc->vtable[20] during DC CONSTRUCTION, well before the
                      // first draw, and with [dc+134] cleared that path faults
                      // at 819DE94C (r3=0, deref +0x24). GuidePreDraw still
                      // read [3F74]=00000000 at the crash, which is what
                      // "arrived too late" looks like.
                      if (cvars::guide_borrow_front_buffer && !lent) {
                        auto rdv = [wmem](uint32_t a) {
                      if (a >= 0x81000000u && a < 0x82000000u &&
                          !XamConstOk(a, 4)) {
                        return uint32_t(0);
                      }
                          return a ? xe::load_and_swap<uint32_t>(
                                         wmem->TranslateVirtual(a))
                                   : 0u;
                        };
                        uint32_t wrap = rdv(c + 0x08u);
                        uint32_t gdev = rdv(wrap + 0x0Cu);
                        uint32_t tdev = rdv(0x801E6FC4u);
                        uint32_t fb = rdv(tdev + 0x3F74u);
                        if (gdev && fb && !rdv(gdev + 0x3F74u)) {
                          lent = true;
                          xe::store_and_swap<uint32_t>(
                              wmem->TranslateVirtual(gdev + 0x3F74u), fb);
                          XELOGI(
                              "Guide: lent front buffer {:08X} to guide device "
                              "{:08X} EARLY (from XuiCtxWatch, before DC "
                              "construction); [3F74] now {:08X}",
                              fb, gdev, rdv(gdev + 0x3F74u));
                        }
                      }
                      last = v;
                      last_ctx = c;
                      primed = true;
                      std::this_thread::sleep_for(
                          std::chrono::microseconds(250));
                    }
                  }).detach();
                  XELOGI("XuiCtxWatch: polling [xui_ctx+0C]");
                }
                XELOGI("Guide button: queued XUI bootstrap for the title "
                       "thread (hud {:08X}, obj {:08X})",
                       hud_base, obj);
                if (cvars::guide_scene_off_thread) {
                  // Wait for the title thread to finish the device-touching
                  // part, then build the scene here. XUI scene loading is
                  // asynchronous, so it must not run while we hold the
                  // renderer.
                  for (int i = 0; i < 400; ++i) {
                    if (kernel::xboxkrnl::GuideBootstrapReady()) {
                      break;
                    }
                    xe::threading::Sleep(std::chrono::milliseconds(10));
                  }
                  if (!kernel::xboxkrnl::GuideBootstrapReady()) {
                    XELOGW("Guide button: bootstrap never became ready");
                    return 0;
                  }
                  uint32_t ovt = xe::load_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(obj));
                  uint32_t sfn =
                      ovt ? xe::load_and_swap<uint32_t>(
                                ks->memory()->TranslateVirtual(ovt + 27 * 4))
                          : 0;
                  // Step the scene creator's own sequence instead of calling
                  // it whole, so the blocking sub-call is identifiable:
                  // init -> XamEnableSystemAppInput -> ... -> XuiSceneCreate.
                  auto rdm = [&](uint32_t addr) {
                    return xe::load_and_swap<uint32_t>(
                        ks->memory()->TranslateVirtual(addr));
                  };
                  // Synchronous CPU readings bracket the known-good steps,
                  // so the probe has a control: if the thread accrues time
                  // across steps 1-3 the instrument detects work, and a flat
                  // reading during step 4 then means something.
                  auto cpu_ms = [](void* nh) -> std::pair<uint64_t, uint64_t> {
                    FILETIME c0, e0, k0, u0;
                    if (!nh || !GetThreadTimes(reinterpret_cast<HANDLE>(nh),
                                               &c0, &e0, &k0, &u0)) {
                      return {0, 0};
                    }
                    uint64_t k =
                        (uint64_t(k0.dwHighDateTime) << 32) | k0.dwLowDateTime;
                    uint64_t u =
                        (uint64_t(u0.dwHighDateTime) << 32) | u0.dwLowDateTime;
                    return {k / 10000, u / 10000};
                  };
                  void* nh_self =
                      kernel::XThread::GetCurrentThread()->thread()
                          ? kernel::XThread::GetCurrentThread()
                                ->thread()
                                ->native_handle()
                          : nullptr;
                  if (cvars::guide_step_scene) {
                    auto t0 = cpu_ms(nh_self);
                    XELOGI("CpuMark before steps: kernel={}ms user={}ms",
                           t0.first, t0.second);
                    // CONTROL: start the watchdog before the known-good steps.
                    // Xenia keeps guest registers in host registers while
                    // running, so a "frozen" context may only mean the context
                    // is not written back. If it also looks frozen during
                    // steps 1-3, which demonstrably complete, then sampling it
                    // proves nothing about whether the thread is executing.
                    {
                      // Per-thread CPU time. Unlike sampling the PPC context
                      // (stale during JIT execution) this distinguishes a
                      // spinning thread from a blocked one, and the samples
                      // during steps 1-3 act as the control: they must show
                      // time accruing while work is demonstrably happening.
                      auto* wt0 = kernel::XThread::GetCurrentThread();
                      void* nh = wt0->thread() ? wt0->thread()->native_handle()
                                               : nullptr;
                      std::thread([nh]() {
                        for (int i = 0; i < 30; ++i) {
                          xe::threading::Sleep(std::chrono::milliseconds(500));
                          if (!nh) {
                            continue;
                          }
                          // Is the thread blocked, or dead? A terminated
                          // thread also shows flat CPU, no kernel calls, no
                          // waits and no faults - and would leave xam's XUI
                          // critical section held forever, which is what the
                          // title thread then blocks on.
                          // Host instruction pointer of the stuck thread.
                          // Suspending briefly to read RIP is safe here (the
                          // thread is making no progress) and gives a direct
                          // answer instead of probing candidate locks one at a
                          // time. Symbolize offline as RIP - module_base
                          // against the PDB.
                          {
                            HANDLE hh = reinterpret_cast<HANDLE>(nh);
                            if (SuspendThread(hh) != (DWORD)-1) {
                              CONTEXT ctx;
                              ctx.ContextFlags = CONTEXT_CONTROL;
                              if (GetThreadContext(hh, &ctx)) {
                                auto base = reinterpret_cast<uint64_t>(
                                    GetModuleHandleW(nullptr));
                                // Which module is RIP in?
                                wchar_t modname[MAX_PATH] = {};
                                HMODULE hm = nullptr;
                                if (GetModuleHandleExW(
                                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        reinterpret_cast<LPCWSTR>(ctx.Rip),
                                        &hm)) {
                                  GetModuleFileNameW(hm, modname, MAX_PATH);
                                }
                                XELOGI("HostRip {}: rip={:X} in '{}' +{:X} "
                                       "rsp={:X}",
                                       i, ctx.Rip,
                                       xe::to_utf8(std::u16string(
                                           reinterpret_cast<const char16_t*>(
                                               modname))),
                                       hm ? ctx.Rip -
                                                reinterpret_cast<uint64_t>(hm)
                                          : 0,
                                       ctx.Rsp);
                                // Poor man's stack walk: scan the stack for
                                // return addresses inside the exe, which names
                                // the Xenia code that called into the DLL.
                                if (i == 0) {
                                  // Symbolize with dbghelp so the frames are
                                  // names rather than offsets.
                                  static bool sym_ready = false;
                                  if (!sym_ready) {
                                    SymSetOptions(SYMOPT_UNDNAME |
                                                  SYMOPT_DEFERRED_LOADS);
                                    sym_ready = SymInitialize(
                                                    GetCurrentProcess(),
                                                    nullptr, TRUE) != FALSE;
                                  }
                                  auto sym_name = [](uint64_t addr) {
                                    char buf[sizeof(SYMBOL_INFO) + 512] = {};
                                    auto* si =
                                        reinterpret_cast<SYMBOL_INFO*>(buf);
                                    si->SizeOfStruct = sizeof(SYMBOL_INFO);
                                    si->MaxNameLen = 500;
                                    DWORD64 disp = 0;
                                    if (SymFromAddr(GetCurrentProcess(), addr,
                                                    &disp, si)) {
                                      return std::string(si->Name) + "+" +
                                             std::to_string(disp);
                                    }
                                    return std::string("<no symbol>");
                                  };
                                  XELOGI("  rip sym: {}", sym_name(ctx.Rip));
                                  auto* sp = reinterpret_cast<uint64_t*>(
                                      ctx.Rsp);
                                  int found = 0;
                                  for (int w = 0; w < 96 && found < 6; ++w) {
                                    uint64_t v = sp[w];
                                    if (v > base && v < base + 0x4000000) {
                                      XELOGI("  stack[{}] exe+{:X}  {}", w,
                                             v - base, sym_name(v));
                                      ++found;
                                    }
                                  }
                                }
                              }
                              ResumeThread(hh);
                            }
                          }
                          // Is Xenia's global critical region held while the
                          // Guide thread is stuck? If TryAcquire succeeds the
                          // thread is not blocked on it, which rules out the
                          // most likely host lock.
                          {
                            auto probe =
                                xe::global_critical_region::TryAcquire();
                            XELOGI("GlobalLock {}: {}", i,
                                   probe.owns_lock() ? "FREE" : "HELD");
                          }
                          DWORD exit_code = 0;
                          if (GetExitCodeThread(reinterpret_cast<HANDLE>(nh),
                                                &exit_code)) {
                            XELOGI("ThreadState {}: {}", i,
                                   exit_code == STILL_ACTIVE
                                       ? "STILL_ACTIVE"
                                       : fmt::format("EXITED code={}",
                                                     exit_code));
                          }
                          FILETIME c0, e0, k0, u0;
                          if (GetThreadTimes(reinterpret_cast<HANDLE>(nh), &c0,
                                             &e0, &k0, &u0)) {
                            uint64_t k = (uint64_t(k0.dwHighDateTime) << 32) |
                                         k0.dwLowDateTime;
                            uint64_t u = (uint64_t(u0.dwHighDateTime) << 32) |
                                         u0.dwLowDateTime;
                            XELOGI("CpuProbe {}: kernel={}ms user={}ms", i,
                                   k / 10000, u / 10000);
                          }
                        }
                      }).detach();
                    }
                    XELOGI("Guide button: step 1 init(render_obj,0)");
                    uint64_t ia2[] = {obj + 16, 0};
                    uint64_t ir2 = ks->processor()->Execute(
                        ts, (g_hud_xuiinit ? g_hud_xuiinit : hud_base + 0xA898u), ia2, xe::countof(ia2));
                    XELOGI("Guide button: step 1 init -> {:08X}",
                           static_cast<uint32_t>(ir2));
                    uint32_t inp = rdm(obj + 72);
                    XELOGI("Guide button: step 2 XamEnableSystemAppInput({:08X}"
                           ", 1)",
                           inp);
                    uint64_t ea[] = {inp, 1};
                    uint64_t er = ks->processor()->Execute(
                        ts, 0x913FE724u, ea, xe::countof(ea));
                    XELOGI("Guide button: step 2 -> {:08X}",
                           static_cast<uint32_t>(er));
                    // Step 3: build the resource locator exactly as hud
                    // does - XamBuildResourceLocator([guide+4], "hud",
                    // [91400168] = "strings.xus", buf, 128).
                    uint32_t pbuf = ks->memory()->SystemHeapAlloc(256, 16);
                    uint32_t outh = ks->memory()->SystemHeapAlloc(16, 16);
                    uint64_t ba[] = {rdm(obj + 4), 0x913E1B24u,
                                     rdm(0x91400168u), pbuf, 128};
                    uint64_t br = ks->processor()->Execute(
                        ts, 0x913FE8C4u, ba, xe::countof(ba));
                    std::string loc;
                    for (int i = 0; i < 80; ++i) {
                      uint16_t ch = xe::load_and_swap<uint16_t>(
                          ks->memory()->TranslateVirtual(pbuf + i * 2));
                      if (!ch) break;
                      loc.push_back(static_cast<char>(ch & 0x7F));
                    }
                    XELOGI("Guide button: step 3 locator -> {:08X} '{}'",
                           static_cast<uint32_t>(br), loc);
                    // Step 4: XuiSceneCreate(basePath, sceneFile, 0, &out)
                    auto t3 = cpu_ms(nh_self);
                    XELOGI("CpuMark after steps 1-3: kernel={}ms user={}ms",
                           t3.first, t3.second);
                    XELOGI("Guide button: step 4 XuiSceneCreate calling");
                    // Watchdog: sample this thread's guest context from a host
                    // thread. If lr/r1 move, guest code is still executing (a
                    // loop); if they are frozen, the thread is not running at
                    // all. That is the distinction the CPU and kernel-call
                    // measurements could not make.
                    {
                      auto* wt = kernel::XThread::GetCurrentThread();
                      std::thread([wt]() {
                        for (int i = 0; i < 12; ++i) {
                          xe::threading::Sleep(std::chrono::seconds(2));
                          auto* c = wt->thread_state()->context();
                          XELOGI("Watchdog {}: lr={:08X} r1={:08X} r3={:08X} "
                                 "r4={:08X}",
                                 i, static_cast<uint32_t>(c->lr),
                                 static_cast<uint32_t>(c->r[1]),
                                 static_cast<uint32_t>(c->r[3]),
                                 static_cast<uint32_t>(c->r[4]));
                        }
                      }).detach();
                    }
                    uint64_t sca[] = {pbuf, rdm(0x91400170u), 0, outh};
                    uint64_t scr = ks->processor()->Execute(
                        ts, 0x913FE6D4u, sca, xe::countof(sca));
                    XELOGI("Guide button: step 4 XuiSceneCreate -> {:08X} "
                           "scene={:08X}",
                           static_cast<uint32_t>(scr), rdm(outh));
                    XELOGI("Guide button: steps done");
                    return 0;
                  }
                  XELOGI("Guide button: scene creator {:08X} off-thread", sfn);
                  if (sfn) {
                    uint64_t sa[] = {obj, 0, 0};
                    uint64_t sr =
                        ks->processor()->Execute(ts, sfn, sa, xe::countof(sa));
                    XELOGI("Guide button: off-thread scene creator -> {:08X}",
                           static_cast<uint32_t>(sr));
                  }
                }
                return 0;
              }
              if (cvars::guide_use_title_device) {
                uint32_t title_dev = rd(0x801E6FC4u);
                if (title_dev) {
                  xe::store_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(kernel::xboxkrnl::XamDeviceSlot()), title_dev);
                  XELOGI("Guide button: xam device global -> title device "
                         "{:08X}",
                         title_dev);
                }
              }
              if (cvars::guide_call_render_host) {
                XELOGI("Guide button: D3D device global 81D43684 = {:08X}",
                       rd(kernel::xboxkrnl::XamDeviceSlot()));
                uint64_t ha[] = {0};
                uint64_t hr = ks->processor()->Execute(ts, kernel::xboxkrnl::GuideConst(0x8178DC58u), ha,
                                                       xe::countof(ha));
                XELOGI("Guide button: render host returned {:08X}, "
                       "XUI ctx now {:08X}",
                       static_cast<uint32_t>(hr), rd(0x81D6C978u));
                uint32_t xctx = rd(0x81D6C978u);
                if (xctx) {
                  uint32_t cvt = rd(xctx);
                  XELOGI("Guide button: XUI ctx {:08X} vtable {:08X}", xctx,
                         cvt);
                  for (int i = 0; i < 8; ++i) {
                    XELOGI("Guide button: ctxvt[{}] = {:08X}", i,
                           rd(cvt + i * 4));
                  }
                }
              }
              {
                // Call xam's XuiRenderCreateDC (R 818FB038) directly with our
                // own out-pointer, so its return value is attributable to it
                // rather than to hud's init wrapper.
                uint32_t dcp = ks->memory()->SystemHeapAlloc(16, 16);
                uint64_t da2[] = {dcp};
                uint64_t dr2 = ks->processor()->Execute(ts, kernel::xboxkrnl::GuideConst(0x818FB038u), da2,
                                                        xe::countof(da2));
                XELOGI("Guide button: direct XuiRenderCreateDC -> {:08X}, "
                       "dc={:08X}",
                       static_cast<uint32_t>(dr2), rd(dcp));
              }
              if (cvars::guide_force_render_gate) {
                xe::store_and_swap<uint32_t>(
                    ks->memory()->TranslateVirtual(obj + 16 + 20), 1u);
              }
              uint64_t ia[] = {obj + 16, 0};
              uint64_t ir = ks->processor()->Execute(ts, (g_hud_xuiinit ? g_hud_xuiinit : hud_base + 0xA898u),
                                                     ia, xe::countof(ia));
              if (cvars::guide_create_scene) {
                uint32_t cvt2 = rd(obj);
                uint32_t scene_fn = cvt2 ? rd(cvt2 + 27 * 4) : 0;
                XELOGI("Guide button: scene fn (vtable[27]) = {:08X}",
                       scene_fn);
                if (scene_fn) {
                  uint64_t sa[] = {obj};
                  uint64_t sr = ks->processor()->Execute(ts, scene_fn, sa,
                                                         xe::countof(sa));
                  XELOGI("Guide button: scene create -> {:08X}, +8 now {:08X}",
                         static_cast<uint32_t>(sr), rd(obj + 16 + 8));
                }
              }
              XELOGI("Guide button: post-init +8={:08X} +12={:08X} "
                     "+20={:08X}",
                     rd(obj + 16 + 8), rd(obj + 16 + 12),
                     rd(obj + 16 + 20));
              XELOGI("Guide button: XUI init returned {:08X}",
                     static_cast<uint32_t>(ir));
              // Hand the draw to the graphics-notification path so it runs
              // on the title's render thread. The title's D3D device is
              // thread-affine, so drawing it from this thread is refused by
              // the guest D3D runtime.
              kernel::xboxkrnl::SetGuideDrawHook((g_hud_render ? g_hud_render : hud_base + 0xAB28u), obj + 16);
              XELOGI("Guide button: draw hook installed ({:08X}, {:08X})",
                     (g_hud_render ? g_hud_render : hud_base + 0xAB28u), obj + 16);
              for (int frame = 0; frame < 0; ++frame) {
                uint64_t da[] = {obj + 16};
                uint64_t dr = ks->processor()->Execute(
                    ts, (g_hud_render ? g_hud_render : hud_base + 0xAB28u), da, xe::countof(da));
                if (frame < 3) {
                  XELOGI("Guide button: draw frame {} returned {:08X}", frame,
                         static_cast<uint32_t>(dr));
                }
                xe::threading::Sleep(std::chrono::milliseconds(16));
              }
              XELOGI("Guide button: draw loop ended");
            } else {
              XELOGW("Guide button: no Guide object at 91400690");
            }
          }
          return 0;
        },
        ks->GetSystemProcess()));
    t->set_name("Guide button dispatch");
    if (XFAILED(t->Create())) {
      XELOGE("Guide button: failed to create dispatch thread");
    }
  }

  // Report what the Guide press can and cannot do in this build, so the
  // button is no longer silently swallowed. Opening the real Guide needs
  // hud.xex hosted by xam - see research/FINDINGS.md.
  XELOGI("Guide button: user={} lle_xam={} hud={}", user_index,
         cvars::lle_xam.empty() ? "off" : "on",
         cvars::guide_hud_path.empty() ? "not loaded" : "loaded");
  // XENIA_PROGRESS_WATCH=1: follow the counter the mode-1 stall waits on.
  // 819F4488 polls [[device+0x2B10]] and gives up after 5s without change;
  // under mode 1 it never changes and CreateDevice never returns. Five
  // explanations have been eliminated by measuring around this loop
  // (system command buffer, ring ownership, wait structures, absent
  // notifications, notification frequency), so watch the counter itself.
  // The device is reached the way the draw path reaches it:
  //   [81D6C978] -> xui ctx -> [+0x08] wrapper -> [+0x0C] device
  // and each link is re-read every pass so a late-built chain is not missed.
  if (std::getenv("XENIA_PROGRESS_WATCH")) {
    auto* pmem = memory();
    std::thread([pmem]() {
      xe::threading::set_name("ProgressWatch");
      auto rd32 = [pmem](uint32_t a) -> uint32_t {
                      if (a >= 0x81000000u && a < 0x82000000u &&
                          !XamConstOk(a, 4)) {
                        return uint32_t(0);
                      }
        if (!a) return 0;
        auto* hp = pmem->LookupHeap(a);
        if (!hp || hp->QueryRangeAccess(a, a + 3) ==
                       xe::memory::PageAccess::kNoAccess) {
          return 0;
        }
        return xe::load_and_swap<uint32_t>(pmem->TranslateVirtual(a));
      };
      // Also watch the device globals. Mode 1 builds a device the wrapper
      // never receives - the wrapper is bound around log line 5026 and mode 1
      // runs at 21509 - and the creator never returns, so there is no pointer
      // to hand the setter 8191BAC8. If the new device is published to one of
      // these globals, that is where to get it.
      uint32_t last_g1 = 0, last_g2 = 0;
      uint32_t last = 0, last_dev = 0;
      bool primed = false;
      for (int i = 0; i < 240000; ++i) {
        uint32_t g1 = rd32(kernel::xboxkrnl::XamDeviceSlot());
        uint32_t g2 = rd32(0x801E6FC8u);
        if (g1 != last_g1) {
          XELOGE("ProgressWatch: xam dev global {:08X} -> {:08X}"
                 " ([2B10]={:08X} [3F74]={:08X})",
                 last_g1, g1, rd32(g1 + 0x2B10u), rd32(g1 + 0x3F74u));
          last_g1 = g1;
        }
        if (g2 != last_g2) {
          XELOGE("ProgressWatch: VdGlobalXamDevice {:08X} -> {:08X}"
                 " ([2B10]={:08X} [3F74]={:08X})",
                 last_g2, g2, rd32(g2 + 0x2B10u), rd32(g2 + 0x3F74u));
          last_g2 = g2;
        }
        uint32_t ctx = rd32(0x81D6C978u);
        uint32_t wrap = rd32(ctx + 0x08u);
        uint32_t dev = rd32(wrap + 0x0Cu);
        uint32_t cptr = rd32(dev + 0x2B10u);
        uint32_t val = rd32(cptr);
        if (dev != last_dev) {
          XELOGE("ProgressWatch: ctx={:08X} wrap={:08X} dev={:08X} "
                 "counter_ptr={:08X}",
                 ctx, wrap, dev, cptr);
          last_dev = dev;
          primed = false;
        }
        if (primed && val != last) {
          XELOGE("ProgressWatch: [{:08X}] {:08X} -> {:08X}", cptr, last, val);
        }
        last = val;
        primed = true;
        // Once, well after the creator has run: find the device mode 1 built.
        // It is never published to either global, but it is recognisable -
        // 81A0FE48 gives it a front buffer at +0x3F74 and a progress counter
        // pointer at +0x2B10, and the wrapper's device has both null. Scan the
        // guest heap the devices live in for that signature.
        // 2ms per iteration, so this fires ~8s after the Guide press. It was
        // 30000 (60s), which never arrived inside the harness window - the
        // watcher starts at the press and the run ends well before then.
        if (i == 4000) {
          // The first version of this signature - two plausible pointers at
          // +0x3F74 and +0x2B10 - matched 8 unrelated objects immediately,
          // spaced 0xC0 apart with non-pointer [32A0]. Require a vtable in
          // xam's .rdata at +0 as well, which every real device has, and that
          // [32A0] itself looks like a surface pointer.
          int found = 0, examined = 0;
          for (uint32_t a = 0x40000000u; a < 0x41000000u && found < 8;
               a += 0x40u) {
            uint32_t vt = rd32(a);
            if (vt < 0x815F0000u || vt >= 0x816F5200u) continue;
            uint32_t fb = rd32(a + 0x3F74u);
            if (fb < 0x10000000u || fb >= 0x50000000u) continue;
            uint32_t cp = rd32(a + 0x2B10u);
            if (cp < 0x10000000u || cp >= 0x50000000u) continue;
            // The [32A0] filter rejected the single object that passed
            // everything else, so it was too strict - RT0 can legitimately
            // hold a non-pointer here (the junk 0x60 this file records). Keep
            // the vtable and the two field tests, report [32A0] rather than
            // filtering on it.
            ++examined;
            ++found;
            XELOGE("DevScan: candidate {:08X}  vt={:08X} [3F74]={:08X} "
                   "[2B10]={:08X} [32A0]={:08X}",
                   a, vt, fb, cp, rd32(a + 0x32A0u));
          }
          XELOGE(
              "DevScan: {} candidate(s) ({} passed the vtable+fields test); "
              "wrapper's device is {:08X}",
              found, examined, dev);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }).detach();
    XELOGI("ProgressWatch: polling the async-call progress counter");
  }
  if (cvars::guide_hud_path.empty()) {
    XELOGI(
        "Guide button: no hud.xex configured - set guide_hud_path to load it");
  }
}

void Emulator::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // Don't hold the lock on this (so any waits follow through)
  graphics_system_->Pause();
  audio_system_->Pause();

  auto lock = global_critical_region::AcquireDirect();
  auto threads =
      kernel_state()->object_table()->GetObjectsByType<kernel::XThread>(
          kernel::XObject::Type::Thread);
  auto current_thread = kernel::XThread::IsInThread()
                            ? kernel::XThread::GetCurrentThread()
                            : nullptr;
  for (auto thread : threads) {
    // Don't pause ourself or host threads.
    if (thread == current_thread || !thread->can_debugger_suspend()) {
      continue;
    }

    if (thread->is_running()) {
      thread->thread()->Suspend(nullptr);
    }
  }

  XELOGD("! EMULATOR PAUSED !");
}

void Emulator::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;
  XELOGD("! EMULATOR RESUMED !");

  graphics_system_->Resume();
  audio_system_->Resume();

  auto threads =
      kernel_state()->object_table()->GetObjectsByType<kernel::XThread>(
          kernel::XObject::Type::Thread);
  for (auto thread : threads) {
    if (!thread->can_debugger_suspend()) {
      // Don't pause host threads.
      continue;
    }

    if (!thread->is_running()) {
      thread->thread()->Resume(nullptr);
    }
  }
}

bool Emulator::SaveToFile(const std::filesystem::path& path) {
  Pause();

  filesystem::CreateEmptyFile(path);
  auto map = MappedMemory::Open(path, MappedMemory::Mode::kReadWrite, 0, 2_GiB);
  if (!map) {
    return false;
  }

  // Save the emulator state to a file
  ByteStream stream(map->data(), map->size());
  stream.Write(kEmulatorSaveSignature);
  stream.Write(title_id_.has_value());
  if (title_id_.has_value()) {
    stream.Write(title_id_.value());
  }

  // It's important we don't hold the global lock here! XThreads need to step
  // forward (possibly through guarded regions) without worry!
  processor_->Save(&stream);
  graphics_system_->Save(&stream);
  audio_system_->Save(&stream);
  kernel_state_->Save(&stream);
  memory_->Save(&stream);
  map->Close(stream.offset());

  Resume();
  return true;
}

bool Emulator::RestoreFromFile(const std::filesystem::path& path) {
  // Restore the emulator state from a file
  auto map = MappedMemory::Open(path, MappedMemory::Mode::kReadWrite);
  if (!map) {
    return false;
  }

  restoring_ = true;

  // Terminate any loaded titles.
  Pause();
  kernel_state_->TerminateTitle();

  auto lock = global_critical_region::AcquireDirect();
  ByteStream stream(map->data(), map->size());
  if (stream.Read<uint32_t>() != kEmulatorSaveSignature) {
    return false;
  }

  auto has_title_id = stream.Read<bool>();
  std::optional<uint32_t> title_id;
  if (!has_title_id) {
    title_id = {};
  } else {
    title_id = stream.Read<uint32_t>();
  }
  if (title_id_.has_value() != title_id.has_value() ||
      title_id_.value() != title_id.value()) {
    // Swapping between titles is unsupported at the moment.
    assert_always();
    return false;
  }

  if (!processor_->Restore(&stream)) {
    XELOGE("Could not restore processor!");
    return false;
  }
  if (!graphics_system_->Restore(&stream)) {
    XELOGE("Could not restore graphics system!");
    return false;
  }
  if (!audio_system_->Restore(&stream)) {
    XELOGE("Could not restore audio system!");
    return false;
  }
  if (!kernel_state_->Restore(&stream)) {
    XELOGE("Could not restore kernel state!");
    return false;
  }
  if (!memory_->Restore(&stream)) {
    XELOGE("Could not restore memory!");
    return false;
  }

  // Update the main thread.
  auto threads =
      kernel_state_->object_table()->GetObjectsByType<kernel::XThread>();
  for (auto thread : threads) {
    if (thread->main_thread()) {
      main_thread_ = thread;
      break;
    }
  }

  Resume();

  restore_fence_.Signal();
  restoring_ = false;

  return true;
}

const std::filesystem::path Emulator::GetNewDiscPath(
    std::string window_message) {
  std::filesystem::path path = "";

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(false);
  file_picker->set_title(!window_message.empty() ? window_message
                                                 : "Select Content Package");
  file_picker->set_extensions({
      {"Supported Files", "*.iso;*.xex;*.xcp;*.*"},
      {"Disc Image (*.iso)", "*.iso"},
      {"Xbox Executable (*.xex)", "*.xex"},
      {"All Files (*.*)", "*.*"},
  });

  if (file_picker->Show()) {
    auto selected_files = file_picker->selected_files();
    if (!selected_files.empty()) {
      path = selected_files[0];
    }
  }
  return path;
}

bool Emulator::IsTrayOpen() const {
  if (!kernel_state_ || !kernel_state_->smc()) {
    return false;
  }
  return kernel_state_->smc()->GetTrayState() == X_DVD_TRAY_STATE::OPEN;
}

bool Emulator::SetTrayDisc(const std::filesystem::path& path) {
  if (!IsTrayOpen()) {
    XELOGW("Change Disc: the tray is not open");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(tray_disc_mutex_);
    tray_disc_path_ = path;
  }
  XELOGI("Change Disc: {} is in the open tray (loads when the tray closes)",
         xe::path_to_utf8(path));
  return true;
}

std::filesystem::path Emulator::GetTrayDisc() const {
  std::lock_guard<std::mutex> lock(tray_disc_mutex_);
  return tray_disc_path_;
}

bool Emulator::ExceptionCallbackThunk(Exception* ex, void* data) {
  return reinterpret_cast<Emulator*>(data)->ExceptionCallback(ex);
}

// Phase 1097zd: a ONE-SHOT write-watch on a guest word, built because three
// static searches for the writer of [81D43C50] all failed (1097zc) and the
// writer holds its base in a register loaded from memory, which no scan of the
// image can see. Protect the page read-only, let the guest's own store fault,
// resolve the faulting host RIP to a guest PC with the same two lines the crash
// reporter already uses, then unprotect and let the store retry. Reads are
// unaffected - read-only faults on writes only - so the polling of this word
// that goes on constantly costs nothing.
static std::atomic<uint32_t> g_ww_page{0};
static uint32_t g_ww_addr = 0;
static void* g_ww_host = nullptr;
// Phase 1097zh: RE-ARMABLE. One shot answered "is there a write" and then
// spent itself on whichever store happened to be first - which for
// [81D43C50] was the null branch storing 0, leaving the writes that actually
// carry 1, 2 and 4 unseen. Re-arming after each catch turns one run into the
// whole sequence. The re-protect is deferred to a helper thread with a short
// sleep rather than done inline, because the faulting store has to retire
// first; that leaves a small blind window, so the hit counter below is the
// honest record of how many writes were SEEN, not how many happened.
static std::atomic<uint32_t> g_ww_hits{0};
static uint32_t g_ww_max = 0;
// Phase 1097zj: set while the faulting store is being single-stepped over.
// The page is unprotected for exactly that one instruction instead of the 2 ms
// the timed re-arm needed, which on this page was ~1500 windows totalling
// about three seconds of blindness in a 90 s run.
static std::atomic<uint32_t> g_ww_stepping{0};

static void ArmGuideWriteWatch(xe::kernel::KernelState* ks, uint32_t addr) {
  if (!addr || g_ww_page.load()) return;
  const uint32_t page = addr & ~0xFFFu;
  auto* host = ks->memory()->TranslateVirtual(page);
  if (!host) return;
  xe::memory::PageAccess old = xe::memory::PageAccess::kNoAccess;
  if (!xe::memory::Protect(host, 0x1000, xe::memory::PageAccess::kReadOnly,
                           &old)) {
    XELOGW("GuideWriteWatch: could not protect page {:08X}", page);
    return;
  }
  g_ww_addr = addr;
  g_ww_host = host;
  g_ww_max = cvars::guide_watch_write_hits;
  g_ww_hits.store(0);
  g_ww_page.store(page);
  XELOGI("GuideWriteWatch: armed on {:08X} (page {:08X}, host {:X}, was {}), "
         "up to {} catches",
         addr, page, reinterpret_cast<uint64_t>(host), static_cast<int>(old),
         g_ww_max);
}

bool Emulator::ExceptionCallback(Exception* ex) {
  // Check to see if the exception occurred in guest code.
  auto code_cache = processor()->backend()->code_cache();
  auto code_base = code_cache->execute_base_address();
  auto code_end = code_base + code_cache->total_size();

  // Re-arm one instruction after the store that tripped the watch.
  if (ex->code() == Exception::Code::kSingleStep && g_ww_stepping.load()) {
    auto* sctx = ex->thread_context();
    if (sctx) sctx->eflags &= ~0x100u;  // clear TF
    const uint32_t page = g_ww_stepping.exchange(0);
    if (g_ww_hits.load() < g_ww_max) {
      xe::memory::PageAccess o2 = xe::memory::PageAccess::kNoAccess;
      if (xe::memory::Protect(g_ww_host, 0x1000,
                              xe::memory::PageAccess::kReadOnly, &o2)) {
        g_ww_page.store(page);
      }
    }
    return true;
  }

  // The write-watch fires before anything else: this is an expected fault, not
  // a crash, and it must not reach the crash reporter below.
  {
    const uint32_t wp = g_ww_page.load();
    if (wp && ex->code() == Exception::Code::kAccessViolation) {
      const uint64_t fa = ex->fault_address();
      auto* wmem = memory();
      const uint64_t hb = reinterpret_cast<uint64_t>(
          wmem ? wmem->TranslateVirtual(wp) : nullptr);
      if (hb && fa >= hb && fa < hb + 0x1000ull) {
        auto* wfn = code_cache->LookupFunction(ex->pc());
        const uint32_t wpc =
            wfn ? wfn->MapMachineCodeToGuestAddress(ex->pc()) : 0;
        auto* wth = kernel::XThread::GetCurrentThread();
        // MapMachineCodeToGuestAddress returns 0 when the faulting RIP is in a
        // JIT helper rather than a translated function body, which is what
        // happened on the first run of this watch. The guest LR is the
        // fallback that actually names the code: it is the return address of
        // whatever called the writer.
        auto* wctx = wth ? wth->thread_state()->context() : nullptr;
        // Phase 1097ze: log WHICH WORD IN THE PAGE faulted. The first run of
        // this watch omitted it and spent its single shot on a neighbour -
        // the exact failure mode the flag's own doc warned about - and
        // without the fault address that was indistinguishable from a hit on
        // the watched word itself.
        const uint32_t fault_guest = wp + uint32_t(fa - hb);
        const bool on_watched =
            fault_guest >= g_ww_addr && fault_guest < g_ww_addr + 4u;
        // Phase 1097zs: rate-limit the neighbour line. This page faults
        // millions of times a run - 4,102,375 in the 120 s run of 1097zr -
        // and logging each one produced a 2 GB log. The watched word is
        // always logged; neighbours get the first 16 and then a counter.
        static std::atomic<uint32_t> nb_logged{0};
        const bool log_this =
            on_watched || (++nb_logged <= 16u) || (nb_logged % 1000000u == 0u);
        if (log_this)
        XELOGI("GuideWriteWatch {}#{}: page {:08X} fault at GUEST {:08X} "
               "(watching {:08X}) from guest PC {:08X} (host {:X}) lr={:08X} "
               "r3={:08X} r11={:08X} r31={:08X} by thread '{}' - watched value "
               "before {:08X}",
               (fault_guest >= g_ww_addr && fault_guest < g_ww_addr + 4u)
                   ? "WATCHED " : "neighbour ",
               g_ww_hits.load() + 1,
               wp, fault_guest, g_ww_addr,
               wpc, ex->pc(),
               wctx ? uint32_t(wctx->lr) : 0,
               wctx ? uint32_t(wctx->r[3]) : 0,
               wctx ? uint32_t(wctx->r[11]) : 0,
               wctx ? uint32_t(wctx->r[31]) : 0,
               wth ? wth->name() : std::string("<none>"),
               xe::load_and_swap<uint32_t>(
                   wmem->TranslateVirtual(g_ww_addr)));
        // Phase 1097zf: say WHOSE code did it. The guest PC came back 0, which
        // has two very different causes - a guest tail branch into code the
        // function lookup does not cover, or the emulator itself storing into
        // guest memory on this thread. The discriminator is whether the
        // faulting RIP lies in xenia's own image or in the JIT code cache, and
        // the crash reporter already prints host frames this way.
        if (log_this) {
          const uint64_t exe_base =
              reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
          const uint64_t cb = code_base, ce = code_end;
          const char* whose =
              (ex->pc() >= cb && ex->pc() < ce) ? "JIT code cache (GUEST)"
              : (ex->pc() >= exe_base && ex->pc() < exe_base + 0x2000000ull)
                  ? "xenia's own image (HOST)"
                  : "neither";
          std::string wbt;
          void* wframes[24];
          USHORT wn = RtlCaptureStackBackTrace(0, 24, wframes, nullptr);
          for (USHORT i = 0; i < wn; ++i) {
            const uint64_t fa2 = reinterpret_cast<uint64_t>(wframes[i]);
            if (fa2 >= exe_base && fa2 < exe_base + 0x2000000ull) {
              wbt += fmt::format("exe+{:X} ", fa2 - exe_base);
            } else if (fa2 >= cb && fa2 < ce) {
              wbt += fmt::format("jit+{:X} ", fa2 - cb);
            } else {
              wbt += fmt::format("{:X} ", fa2);
            }
          }
          XELOGI("GuideWriteWatch: the store is in {} (rip {:X}, exe_base "
                 "{:X}, jit {:X}..{:X})",
                 whose, ex->pc(), exe_base, cb, ce);
          XELOGI("GuideWriteWatch: host frames: {}", wbt);
        }
        // Restore write access so the faulting store retires, then re-arm
        // from a helper thread once it has.
        xe::memory::PageAccess prev = xe::memory::PageAccess::kNoAccess;
        xe::memory::Protect(g_ww_host, 0x1000,
                            xe::memory::PageAccess::kReadWrite, &prev);
        g_ww_page.store(0);
        // Phase 1097zi: only writes to the WATCHED WORD spend the budget. The
        // page is 4 KB and its neighbours are written constantly - in the
        // first re-armable run five of six catches were neighbours - so
        // counting them exhausts the budget before the word's own writes are
        // ever seen. Neighbours still have to be let through (the store must
        // retire), so the blind window is unchanged; what changes is that a
        // long run now keeps looking instead of giving up.
        const uint32_t hits = on_watched ? ++g_ww_hits : g_ww_hits.load();
        if (hits < g_ww_max) {
          // Step over the store and re-protect in the single-step handler.
          auto* actx = ex->thread_context();
          if (actx) {
            actx->eflags |= 0x100u;  // TF
            g_ww_stepping.store(wp);
          }
        } else {
          XELOGI("GuideWriteWatch: reached the {}-catch limit; disarmed",
                 g_ww_max);
        }
        return true;
      }
    }
  }

  if (!processor()->is_debugger_attached() && debugging::IsDebuggerAttached()) {
    // If Xenia's debugger isn't attached but another one is, pass it to that
    // debugger.
    return false;
  } else if (processor()->is_debugger_attached()) {
    // Let the debugger handle this exception. It may decide to continue past
    // it (if it was a stepping breakpoint, etc).
    return processor()->OnUnhandledException(ex);
  }

  if (!(ex->pc() >= code_base && ex->pc() < code_end)) {
    // Didn't occur in guest code. Let it pass - but say so first. A host-side
    // fault otherwise produces no log line at all: the only trace is Xenia's
    // modal "Unhandled Exception" dialog, which reports a bare module+offset
    // and no caller. Raw addresses are logged rather than resolved here
    // because resolving would take the loader lock, which is not safe from an
    // exception handler. Feed exe-relative frames to tools/sym.ps1 offline.
    static std::atomic<uint32_t> host_faults{0};
    if (++host_faults <= 4) {
      void* frames[32] = {};
      USHORT n = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
      uint64_t exe_base =
          reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
      std::string bt;
      for (USHORT i = 0; i < n; ++i) {
        uint64_t a = reinterpret_cast<uint64_t>(frames[i]);
        if (a >= exe_base && a < exe_base + 0x2000000ull) {
          bt += fmt::format("exe+{:X} ", a - exe_base);
        } else {
          bt += fmt::format("{:X} ", a);
        }
      }
      XELOGE("HOST FAULT: pc={:X} (exe+{:X}) fault_addr={:X} exe_base={:X}",
             ex->pc(),
             ex->pc() >= exe_base ? ex->pc() - exe_base : 0ull,
             ex->code() == Exception::Code::kAccessViolation
                 ? ex->fault_address()
                 : 0,
             exe_base);
      XELOGE("HOST FAULT: frames: {}", bt);
    }
    return false;
  }

  // Log the essentials BEFORE pausing. Pause() waits for the graphics system
  // and command processor to acknowledge, and if either is blocked - which is
  // likely when a guest thread has just faulted mid-frame - it never returns
  // and the crash dump below is never written. That turns a diagnosable crash
  // into a silent freeze.
  {
    auto* early_thread = kernel::XThread::GetCurrentThread();
    auto* early_fn = code_cache->LookupFunction(ex->pc());
    uint32_t guest_pc =
        early_fn ? early_fn->MapMachineCodeToGuestAddress(ex->pc()) : 0;
    const char* code_str =
        ex->code() == Exception::Code::kAccessViolation ? "access violation"
        : ex->code() == Exception::Code::kIllegalInstruction
            ? "illegal instruction"
            : "other";
    auto* ectx =
        early_thread ? early_thread->thread_state()->context() : nullptr;
    XELOGE(
        "GUEST CRASH: {} at guest PC {:08X} (host {:X}), thread '{}', "
        "fault_addr {:016X}",
        code_str, guest_pc, ex->pc(),
        early_thread ? early_thread->name() : std::string("<none>"),
        ex->code() == Exception::Code::kAccessViolation ? ex->fault_address()
                                                        : 0);
    if (ectx) {
      // LR identifies the caller, which matters when several call sites reach
      // the same function - picking one by "it was compiled just before" is
      // not evidence.
      XELOGE("GUEST CRASH: lr={:08X} r3={:016X} r4={:016X} r5={:016X}",
             static_cast<uint32_t>(ectx->lr), ectx->r[3], ectx->r[4],
             ectx->r[5]);
      // Phase 578: the comment below has said since it was written that the
      // callee-saved registers are what identify the faulting object, and only
      // r3-r5 were ever printed. Phases 575-577 each named a wrong faulting
      // instruction because the register holding the bad pointer was not
      // visible, and each was refuted by sampling state before the call - which
      // says nothing about state during it. Print the registers a faulting
      // `lwz rX, off(rY)` actually uses.
      kernel::xboxkrnl::GuideEmitCoverageNow();
      XELOGE("GUEST CRASH: r11={:08X} r28={:08X} r29={:08X} r30={:08X} "
             "r31={:08X} ctr={:08X}",
             static_cast<uint32_t>(ectx->r[11]),
             static_cast<uint32_t>(ectx->r[28]),
             static_cast<uint32_t>(ectx->r[29]),
             static_cast<uint32_t>(ectx->r[30]),
             static_cast<uint32_t>(ectx->r[31]),
             static_cast<uint32_t>(ectx->ctr));
      // Xenon MSVC keeps "this" and the other long-lived pointers in the
      // callee-saved range, and by the time a load faults r3-r5 are usually
      // already clobbered. Without these it is not possible to tell which
      // object a faulting "lwz rX,off(rY)" was reading from - which is
      // exactly the question a null deref raises.
      // Poor-man's backtrace: scan the guest stack for words that look like
      // xam .text addresses. Breakpoints would give an exact caller, but
      // installing one changes scheduling enough that the code path under
      // investigation stops being taken - so the crash path can only be
      // observed without them.
      {
        auto* mm = kernel_state() ? kernel_state()->memory() : nullptr;
        uint32_t sp = static_cast<uint32_t>(ectx->r[1]);
        if (mm && sp) {
          // Every read below walks addresses derived from a *faulted*
          // thread's stack, so none of them can be assumed mapped. Reading
          // off the end of the stack region faulted inside this handler,
          // which replaced the real crash report with a second exception and
          // destroyed the evidence for the first.
          auto readable = [&](uint32_t a, uint32_t len) {
            auto* hp = mm->LookupHeap(a);
            return hp && hp->QueryRangeAccess(a, a + len - 1) !=
                             xe::memory::PageAccess::kNoAccess;
          };
          // Proper unwind first. PPC keeps a back chain at [sp], and these
          // prologues save LR with "stw r12,-8(r1)" before the stwu, so a
          // frame's return address sits at [caller_sp - 8]. Walking that is
          // exact, unlike the scan below, which cannot tell a live frame from
          // a stale word left by an earlier deeper call.
          {
            std::string bt;
            uint32_t cur = sp;
            for (int f = 0; f < 12 && cur; ++f) {
              if (!readable(cur, 4)) break;
              uint32_t caller_sp =
                  xe::load_and_swap<uint32_t>(mm->TranslateVirtual(cur));
              if (caller_sp <= cur || caller_sp - cur > 0x10000) break;
              if (caller_sp < 8 || !readable(caller_sp - 8, 4)) break;
              uint32_t ra = xe::load_and_swap<uint32_t>(
                  mm->TranslateVirtual(caller_sp - 8));
              if (ra < 0x81000000u || ra >= 0x92000000u) break;
              bt += fmt::format("{:08X} ", ra);
              cur = caller_sp;
            }
            if (!bt.empty()) {
              XELOGE("GUEST CRASH: unwind (back chain): {}", bt);
            }
          }
          std::string line;
          int shown = 0;
          // Cover several frames. 96 words was too short by eight for
          // 819F5D18 alone, whose prologue is stwu r1,-0x1A0(r1) - 104 words -
          // so the direct caller's return address fell outside the window and
          // its absence was misread as proof the call was indirect.
          for (uint32_t i = 0; i < 400 && shown < 24; ++i) {
            uint32_t a = sp + i * 4;
            if (!readable(a, 4)) break;
            uint32_t v = xe::load_and_swap<uint32_t>(mm->TranslateVirtual(a));
            if (v >= 0x81700000u && v < 0x81E00000u) {
              line += fmt::format("{:08X}(+{:X}) ", v, i * 4);
              ++shown;
            }
          }
          if (!line.empty()) {
            XELOGE("GUEST CRASH: stack code refs: {}", line);
          }
        }
      }
      // All 32 GPRs. Picking a subset means the one register the faulting
      // instruction actually used is the one that is missing - which is
      // exactly what happened with an "lwz r11,32(r14)" fault when only
      // r27-r31 were dumped.
      for (int base = 0; base < 32; base += 8) {
        XELOGE("GUEST CRASH: r{:<2}-r{:<2} {:08X} {:08X} {:08X} {:08X} "
               "{:08X} {:08X} {:08X} {:08X}",
               base, base + 7, static_cast<uint32_t>(ectx->r[base + 0]),
               static_cast<uint32_t>(ectx->r[base + 1]),
               static_cast<uint32_t>(ectx->r[base + 2]),
               static_cast<uint32_t>(ectx->r[base + 3]),
               static_cast<uint32_t>(ectx->r[base + 4]),
               static_cast<uint32_t>(ectx->r[base + 5]),
               static_cast<uint32_t>(ectx->r[base + 6]),
               static_cast<uint32_t>(ectx->r[base + 7]));
      }
      // Phase 1096gm: one dashboard run produced five distinct guest crashes
      // that all reduce to "a field of an object held a value that is not a
      // pointer" - a vtable slot reading 00000000, a field reading 000000E8,
      // a slot holding an instruction word. Answering *which* field each time
      // meant a rebuild per crash to aim XENIA_CRASH_PEEK. Dump it instead:
      // for every GPR holding a plausible guest address, print the object it
      // points at, and when its first word looks like a table of code
      // pointers, print that table too.
      {
        auto* mmv = kernel_state() ? kernel_state()->memory() : nullptr;
        auto ok = [&](uint32_t a, uint32_t len) {
          if (!mmv || !a || !len) return false;
          auto* hp = mmv->LookupHeap(a);
          return hp && hp->QueryRangeAccess(a, a + len - 1) !=
                           xe::memory::PageAccess::kNoAccess;
        };
        auto words = [&](uint32_t a, uint32_t n) {
          std::string s2;
          for (uint32_t i = 0; i < n; ++i) {
            s2 += fmt::format("{:08X} ", xe::load_and_swap<uint32_t>(
                                             mmv->TranslateVirtual(a + i * 4)));
          }
          return s2;
        };
        // Only registers that could be a guest pointer at all. Guest data
        // lives well above zero and xam/hud end below 92000000; anything
        // outside that range is a count, a flag or a handle, and dumping it
        // would bury the fields that matter.
        for (int r = 3; r < 32 && mmv; ++r) {
          const uint32_t v = static_cast<uint32_t>(ectx->r[r]);
          if (v < 0x00010000u || v >= 0x92000000u) continue;
          if (!ok(v, 32)) continue;
          XELOGE("GUEST CRASH: [r{}={:08X}] = {}", r, v, words(v, 8));
          const uint32_t vt =
              xe::load_and_swap<uint32_t>(mmv->TranslateVirtual(v));
          if (vt >= 0x81000000u && vt < 0x92000000u && ok(vt, 64)) {
            const uint32_t slot0 =
                xe::load_and_swap<uint32_t>(mmv->TranslateVirtual(vt));
            if (slot0 >= 0x81000000u && slot0 < 0x92000000u) {
              XELOGE("GUEST CRASH:   vtable [r{}] -> {:08X}: {}", r, vt,
                     words(vt, 16));
            }
          }
        }
      }
      // Optional peek at a register-relative address, for when the interesting
      // value is a field of an object a register points at rather than the
      // register itself. XENIA_CRASH_PEEK="29,4E8,8" dumps 8 words starting at
      // r29 + 0x4E8. Off unless the variable is set.
      // XENIA_CRASH_PEEK_DEREF="31,1C8,8" reads the pointer at rN+off and
      // dumps `count` words from *there*. The plain peek shows the pointer;
      // this shows what it points at, which is what distinguishes "the field
      // holds the wrong address" from "the address is right and its contents
      // are wrong". Reasoning across those two without the data has already
      // produced one withdrawn conclusion here.
      if (const char* pd = std::getenv("XENIA_CRASH_PEEK_DEREF")) {
        uint32_t reg = 0, off = 0, count = 8;
        if (std::sscanf(pd, "%u,%x,%u", &reg, &off, &count) >= 2 && reg < 32) {
          if (count > 32) count = 32;
          auto* mm3 = kernel_state() ? kernel_state()->memory() : nullptr;
          uint32_t pa = static_cast<uint32_t>(ectx->r[reg]) + off;
          auto* hp3 = mm3 ? mm3->LookupHeap(pa) : nullptr;
          if (hp3 && hp3->QueryRangeAccess(pa, pa + 3) !=
                         xe::memory::PageAccess::kNoAccess) {
            uint32_t tgt = xe::load_and_swap<uint32_t>(
                mm3->TranslateVirtual(pa));
            auto* hp4 = mm3->LookupHeap(tgt);
            if (tgt && hp4 &&
                hp4->QueryRangeAccess(tgt, tgt + count * 4 - 1) !=
                    xe::memory::PageAccess::kNoAccess) {
              std::string w2;
              for (uint32_t i = 0; i < count; ++i) {
                w2 += fmt::format("{:08X} ",
                                  xe::load_and_swap<uint32_t>(
                                      mm3->TranslateVirtual(tgt + i * 4)));
              }
              XELOGE("GUEST CRASH: deref r{}+{:X} -> {:08X}: {}", reg, off,
                     tgt, w2);
            } else {
              XELOGE("GUEST CRASH: deref r{}+{:X} -> {:08X}: unmapped", reg,
                     off, tgt);
            }
          }
        }
      }
      if (const char* peek = std::getenv("XENIA_CRASH_PEEK")) {
        uint32_t reg = 0, off = 0, count = 4;
        if (std::sscanf(peek, "%u,%x,%u", &reg, &off, &count) >= 2 &&
            reg < 32) {
          if (count > 32) count = 32;
          uint32_t addr = static_cast<uint32_t>(ectx->r[reg]) + off;
          auto* mm2 = kernel_state() ? kernel_state()->memory() : nullptr;
          auto* hp = mm2 ? mm2->LookupHeap(addr) : nullptr;
          if (hp && hp->QueryRangeAccess(addr, addr + count * 4 - 1) !=
                        xe::memory::PageAccess::kNoAccess) {
            std::string words;
            for (uint32_t i = 0; i < count; ++i) {
              words += fmt::format(
                  "{:08X} ", xe::load_and_swap<uint32_t>(
                                 mm2->TranslateVirtual(addr + i * 4)));
            }
            XELOGE("GUEST CRASH: peek r{}+{:X} = {:08X}: {}", reg, off, addr,
                   words);
          } else {
            XELOGE("GUEST CRASH: peek r{}+{:X} = {:08X}: unmapped", reg, off,
                   addr);
          }
        }
      }
      // LR here is the function's own __savegprlr return, not the caller.
      // That helper stores the real LR at [r1-8] of the caller's frame before
      // the stwu, so with a 0xC0 frame it is at r1+0xB8. Scan a window in case
      // the frame size differs.
      // A crash reporter must not itself crash. 81D3F924 is a dashroot-xam
      // global and need not be mapped on another build; reading it unguarded
      // host-faulted here and swallowed the guest crash it was reporting.
      // Same correction: for the xam image use its extent. Stack addresses
      // are still queried, since those pages ARE tracked correctly.
      uint32_t cr_lo = 0, cr_hi = 0;
      if (lle_xam_module_ && lle_xam_module_->xex_module()) {
        cr_lo = lle_xam_module_->xex_module()->base_address();
        cr_hi = cr_lo + lle_xam_module_->xex_module()->image_size();
      }
      auto crash_mapped = [this, cr_lo, cr_hi](uint32_t a,
                                               uint32_t len) -> bool {
        if (cr_lo && a >= cr_lo && a + len <= cr_hi) return true;
        auto* m = memory();
        if (!m || a < 0x1000u) return false;
        auto* hp = m->LookupHeap(a);
        return hp && hp->QueryRangeAccess(a, a + len) !=
                         xe::memory::PageAccess::kNoAccess;
      };
      if (crash_mapped(0x81D3F924u, 4)) {
        XELOGE("GUEST CRASH: [81D3F924] = {:08X}",
               xe::load_and_swap<uint32_t>(
                   memory()->TranslateVirtual(0x81D3F924u)));
      }
      uint32_t sp = static_cast<uint32_t>(ectx->r[1]);
      for (uint32_t off = 0xA0; off <= 0xE0; off += 8) {
        if (!crash_mapped(sp + off, 4)) continue;
        uint32_t v = xe::load_and_swap<uint32_t>(
            memory()->TranslateVirtual(sp + off));
        if (v >= 0x81000000 && v < 0x82000000) {
          XELOGE("GUEST CRASH: saved lr candidate [r1+{:X}] = {:08X}", off, v);
        }
      }
    }
  }

  // Within range. Pause the emulator and eat the exception.
  Pause();

  // Dump information into the log.
  auto current_thread = kernel::XThread::GetCurrentThread();
  assert_not_null(current_thread);

  auto guest_function = code_cache->LookupFunction(ex->pc());
  assert_not_null(guest_function);

  auto context = current_thread->thread_state()->context();

  std::string crash_msg;
  crash_msg.append("==== CRASH DUMP ====\n");
  crash_msg.append(fmt::format("Thread ID (Host: 0x{:08X} / Guest: 0x{:08X})\n",
                               current_thread->thread()->system_id(),
                               current_thread->thread_id()));
  crash_msg.append(
      fmt::format("Thread Handle: 0x{:08X}\n", current_thread->handle()));
  crash_msg.append(
      fmt::format("PC: 0x{:08X}\n",
                  guest_function->MapMachineCodeToGuestAddress(ex->pc())));
  if (ex->code() == Exception::Code::kAccessViolation) {
    const char* op_str = "unknown";
    if (ex->access_violation_operation() ==
        Exception::AccessViolationOperation::kRead) {
      op_str = "read";
    } else if (ex->access_violation_operation() ==
               Exception::AccessViolationOperation::kWrite) {
      op_str = "write";
    }
    crash_msg.append(fmt::format("Access Violation: {} at 0x{:016X}\n", op_str,
                                 ex->fault_address()));
  } else if (ex->code() == Exception::Code::kIllegalInstruction) {
    crash_msg.append("Illegal Instruction\n");
  }
  crash_msg.append("Registers:\n");
  for (int i = 0; i < 32; i++) {
    crash_msg.append(fmt::format(" r{:<3} = {:016X}\n", i, context->r[i]));
  }
  for (int i = 0; i < 32; i++) {
    crash_msg.append(fmt::format(" f{:<3} = {:016X} = (double){} = (float){}\n",
                                 i,
                                 *reinterpret_cast<uint64_t*>(&context->f[i]),
                                 context->f[i], *(float*)&context->f[i]));
  }
  for (int i = 0; i < 128; i++) {
    crash_msg.append(
        fmt::format(" v{:<3} = [0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X}]\n", i,
                    context->v[i].u32[0], context->v[i].u32[1],
                    context->v[i].u32[2], context->v[i].u32[3]));
  }
  XELOGE("{}", crash_msg);
  std::string crash_dlg = fmt::format(
      "The guest has crashed.\n\n"
      "Xenia has now paused itself.\n\n"
      "{}",
      crash_msg);
  // Display a dialog telling the user the guest has crashed.
  if (display_window_ && imgui_drawer_) {
    display_window_->app_context().CallInUIThreadSynchronous([this,
                                                              &crash_dlg]() {
      xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_, "Uh-oh!", crash_dlg);
    });
  }

  // Now suspend ourself (we should be a guest thread).
  current_thread->Suspend(nullptr);

  // We should not arrive here!
  assert_always();
  return false;
}

void Emulator::WaitUntilExit() {
  while (true) {
    if (main_thread_) {
      xe::threading::Wait(main_thread_->thread(), false);
    }

    if (restoring_) {
      restore_fence_.Wait();
    } else {
      // Not restoring and the thread exited. We're finished.
      break;
    }
  }

  on_exit();
}

void Emulator::AddGameConfigLoadCallback(GameConfigLoadCallback* callback) {
  assert_not_null(callback);
  // Game config load callbacks handling is entirely in the UI thread.
  assert_true(!display_window_ ||
              display_window_->app_context().IsInUIThread());
  // Check if already added.
  if (std::ranges::find(std::as_const(game_config_load_callbacks_), callback) !=
      game_config_load_callbacks_.cend()) {
    return;
  }
  game_config_load_callbacks_.push_back(callback);
}

void Emulator::RemoveGameConfigLoadCallback(GameConfigLoadCallback* callback) {
  assert_not_null(callback);
  // Game config load callbacks handling is entirely in the UI thread.
  assert_true(!display_window_ ||
              display_window_->app_context().IsInUIThread());
  auto it =
      std::ranges::find(std::as_const(game_config_load_callbacks_), callback);
  if (it == game_config_load_callbacks_.cend()) {
    return;
  }
  if (game_config_load_callback_loop_next_index_ != SIZE_MAX) {
    // Actualize the next callback index after the erasure from the vector.
    size_t existing_index =
        size_t(std::distance(game_config_load_callbacks_.cbegin(), it));
    if (game_config_load_callback_loop_next_index_ > existing_index) {
      --game_config_load_callback_loop_next_index_;
    }
  }
  game_config_load_callbacks_.erase(it);
}

std::string Emulator::FindLaunchModule() {
  std::string path(fmt::format("{}\\", kDefaultGameSymbolicLink));

  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");

  if (!xam->loader_data().launch_path.empty()) {
    std::string symbolic_link_path;
    if (kernel_state_->file_system()->FindSymbolicLink(kDefaultGameSymbolicLink,
                                                       symbolic_link_path)) {
      std::filesystem::path file_path = symbolic_link_path;
      // Remove previous symbolic links.
      // Some titles can provide root within specific directory.
      kernel_state_->file_system()->UnregisterSymbolicLink(
          kDefaultPartitionSymbolicLink);
      kernel_state_->file_system()->UnregisterSymbolicLink(
          kDefaultGameSymbolicLink);

      file_path /= std::filesystem::path(xam->loader_data().launch_path);

      kernel_state_->file_system()->RegisterSymbolicLink(
          kDefaultPartitionSymbolicLink,
          xe::path_to_utf8(file_path.parent_path()));
      kernel_state_->file_system()->RegisterSymbolicLink(
          kDefaultGameSymbolicLink, xe::path_to_utf8(file_path.parent_path()));

      return xe::path_to_utf8(file_path);
    }
  }

  if (!cvars::launch_module.empty()) {
    return path + cvars::launch_module;
  }

  return path + "default.xex";
}

static std::string format_version(xex2_version version) {
  // fmt::format doesn't like bit fields we use + to bypass it
  return fmt::format("{}.{}.{}.{}", +version.major, +version.minor,
                     +version.build, +version.qfe);
}

// Installing these at the Guide button press misses anything that happens
// during xam and hud initialisation - XUI class registration among it - so
// arm them as soon as the title is loaded instead.
// Sample every guest thread's PC. Split into two passes on purpose: the raw
// registers are logged before anything touches the code cache, because
// LookupFunction takes the code cache lock and that is exactly the lock a
// JIT/loader deadlock is likely to be holding. Resolving first would hang the
// probe and lose the only evidence.
// Report how much of xam's .text reads back as zero. Called at more than one
// point: a page that is populated at load and zero later means something is
// clobbering the image, which is a different bug from it never being loaded.
static void ReportXamTextPopulation(Memory* memory, const char* when) {
  // xam's .text is 81720000..81D13CE0 - taken from the section table of the
  // decrypted image, where a section's bytes live at file offset
  // (VA - ImageBase). The bounds used here before were 81770000..81D60000,
  // which is wrong at BOTH ends and invalidated everything this scan has ever
  // reported:
  //   * it began 320 KB into .text, so it never looked at 81720000-81770000 -
  //     the region containing 81747A00, the address the boot actually dies on;
  //   * it ran 304 KB past the end of .text, into the inter-section gap and
  //     into .data.
  // Every "zero page" it found lived in that overrun: 81D14000-81D1FFFF is the
  // padding between .text and .data, and 81D3D000-81D5FFFF is zero-initialised
  // .data. Both are supposed to be zero. The long-standing "47 of 1520 pages
  // of xam .text are entirely zero" result was therefore measuring alignment
  // padding and .bss, not missing code.
  const uint32_t kTextStart = 0x81720000u;
  const uint32_t kTextEnd = 0x81D14000u;
  uint32_t zero_pages = 0, total_pages = 0, run = 0, best_run = 0;
  uint32_t best_start = 0, first_zero = 0, last_zero = 0;
  // A count says how much is missing but never *what*, and "what" is the
  // question: guest 81747A00 reads as zero here while the firmware image on
  // disk holds a real function there (mfspr prologue, its own .pdata record,
  // two callers). Logging the ranges says whether the same pages are lost
  // every run - which decides whether this is a race or a fixed hole - and
  // makes each one directly checkable against the image.
  uint32_t run_start = 0;
  std::vector<std::pair<uint32_t, uint32_t>> zero_runs;
  for (uint32_t pg = kTextStart; pg < kTextEnd; pg += 0x1000) {
    auto* hp = memory->LookupHeap(pg);
    if (!hp || hp->QueryRangeAccess(pg, pg + 0xFFF) ==
                   xe::memory::PageAccess::kNoAccess) {
      continue;
    }
    ++total_pages;
    const uint32_t* w = memory->TranslateVirtual<const uint32_t*>(pg);
    bool all_zero = true;
    for (uint32_t i = 0; i < 0x1000 / 4; ++i) {
      if (w[i]) {
        all_zero = false;
        break;
      }
    }
    if (all_zero) {
      ++zero_pages;
      if (!first_zero) first_zero = pg;
      last_zero = pg;
      if (!run) run_start = pg;
      if (++run > best_run) {
        best_run = run;
        best_start = run_start;
      }
    } else {
      if (run) zero_runs.emplace_back(run_start, pg - 0x1000);
      run = 0;
    }
  }
  if (run) zero_runs.emplace_back(run_start, kTextEnd - 0x1000);
  if (!zero_runs.empty()) {
    std::string ranges;
    size_t shown = 0;
    for (auto& r : zero_runs) {
      if (shown++ == 24) break;
      ranges += fmt::format("{:08X}-{:08X}({}) ", r.first, r.second + 0xFFF,
                            (r.second - r.first) / 0x1000 + 1);
    }
    XELOGI("xam .text zero ranges ({}): {} run(s){}: {}", when,
           zero_runs.size(),
           zero_runs.size() > 24 ? " [first 24]" : "", ranges);
  }
  // Whole-page counting misses a hole inside an otherwise populated page,
  // which is exactly what a bogus function start landing in inter-function
  // padding would look like. Dump the specific addresses the scanner has
  // tripped on so they can be diffed against the image on disk.
  // 815FA1E0 is xam's feature table (13 x 32-byte records, key at +8,
// names PRELOADED_HUD/MESSENGER/XMP/...). 81747A00 searches it and
// returns NULL on a miss; 81747D70 then asserts and, with the assert
// suppressed, faults dereferencing NULL+0x10. The failing lookup asked
// for key 6 (XSTUDIO), which IS in the table on disk - so the table
// itself must be reading wrong. rec0 should show key 1 at +8 and rec5
// key 6, which is what these two probes check. Note this is .rdata,
// not .text: if it is also affected, the transient-zero phenomenon is
// not confined to the code section.
// Phase 1096gm adds four more. A guest crash dumped [81687324] as
// "00020010 00100010 00100020 00000000..." where the image on disk has the
// vtable "81AB1980 81AB8700 81AB00F0 817D5870...", and [8168C820] as PPC
// instructions where the image has the string "LiveQosHistory". Both sit in
// the RODATA page 81680000-81690000. 81E15B20 is the control: its runtime
// contents matched the image exactly in the same dump, so if it also reads
// wrong here the probe is at fault rather than the page.
for (uint32_t probe_addr : {0x8186E528u, 0x818936B8u, 0x81747D70u,
                            0x81747A00u, 0x815FA1E0u, 0x815FA280u,
                            0x81D3F8A0u, 0x81680000u, 0x81687324u,
                            0x8168C820u, 0x81E15B20u,
                            // Phase 1096gw: dash.xex polls
                            // ObReferenceObjectByName('\Device\HdDvdRom')
                            // 33.5M times per 100 s run from lr 9226784C and
                            // does not stop on NO_SUCH_DEVICE. There is no
                            // dumped image of dash to disassemble, so read the
                            // loop out of the loaded title instead - these six
                            // cover 922677C0..92267880.
                            0x922677C0u, 0x922677E0u, 0x92267800u,
                            0x92267820u, 0x92267840u, 0x92267860u}) {
    auto* hp = memory->LookupHeap(probe_addr);
    if (!hp || hp->QueryRangeAccess(probe_addr, probe_addr + 31) ==
                   xe::memory::PageAccess::kNoAccess) {
      XELOGI("xam probe {:08X} ({}): unmapped", probe_addr, when);
      continue;
    }
    std::string words;
    for (uint32_t i = 0; i < 8; ++i) {
      words += fmt::format(
          "{:08X} ", xe::load_and_swap<uint32_t>(
                         memory->TranslateVirtual(probe_addr + i * 4)));
    }
    XELOGI("xam probe {:08X} ({}): {}", probe_addr, when, words);
  }
  XELOGI(
      "xam .text population ({}): {} of {} mapped pages are entirely zero "
      "({:.1f}%); longest zero run {} pages at {:08X}; first {:08X} "
      "last {:08X}",
      when, zero_pages, total_pages,
      total_pages ? 100.0 * zero_pages / total_pages : 0.0, best_run,
      best_start, first_zero, last_zero);
}

// Give each E_FAIL construction site its own HRESULT so a failing call can
// say where its error came from. Every site builds 0x80004005 with an
// "ori rX,rX,0x4005"; rewriting just the immediate leaves the code layout
// untouched, which matters because breakpointing these paths stops them being
// taken at all. Sites are split into 4 groups across the range and tagged
// 0x80004010 + group, so one run identifies a quarter; narrow and repeat.
// Match the ori alone - requiring an adjacent "lis rX,0x8000" misses 201 of
// xam's 886 sites, since the compiler schedules instructions between them.
static void TagEFailSites(Memory* memory, const char* spec, const char* what) {
  uint32_t lo = 0, hi = 0;
  const char* dash = std::strchr(spec, '-');
  if (!dash) return;
  lo = uint32_t(std::strtoul(spec, nullptr, 16));
  hi = uint32_t(std::strtoul(dash + 1, nullptr, 16));
  if (hi <= lo) return;
  uint32_t span = hi - lo;
  uint32_t counts[4] = {};
  for (uint32_t a = lo; a + 4 <= hi; a += 4) {
    auto* hp = memory->LookupHeap(a);
    if (!hp || hp->QueryRangeAccess(a, a + 3) ==
                   xe::memory::PageAccess::kNoAccess) {
      continue;
    }
    auto* w = memory->TranslateVirtual<uint32_t*>(a);
    uint32_t w2 = xe::load_and_swap<uint32_t>(w);
    if ((w2 & 0xFC00FFFFu) != 0x60004005u) continue;
    uint32_t group = uint32_t((uint64_t(a - lo) * 4) / span);
    if (group > 3) group = 3;
    void* pg = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(w) &
                                       ~uintptr_t(0xFFF));
    xe::memory::PageAccess old_access = xe::memory::PageAccess::kReadOnly;
    if (xe::memory::Protect(pg, 0x1000, xe::memory::PageAccess::kReadWrite,
                            &old_access)) {
      xe::store_and_swap<uint32_t>(w, (w2 & 0xFFFF0000u) | (0x4010u + group));
      xe::memory::Protect(pg, 0x1000, old_access, nullptr);
      ++counts[group];
      // Log the address with its group. Deriving which site a tag maps to by
      // hand from the range arithmetic is error-prone; let the tool say it.
      XELOGI("EFailTag[{}]: tagged {:08X} -> HRESULT 8000{:04X}", what, a,
             0x4010u + group);
    }
  }
  XELOGI(
      "EFailTag[{}]: range {:08X}-{:08X}; tagged g0={} g1={} g2={} g3={} "
      "(HRESULT 0x80004010 + group)",
      what, lo, hi, counts[0], counts[1], counts[2], counts[3]);
}

static void ArmGuideThreadProbe(xe::kernel::KernelState* ks, int pdelay) {
  // Two threads, because enumerating the object table is exactly what a
  // freeze blocks on. The cacher keeps a fresh list of thread objects and
  // will itself wedge on the object table lock once the hang hits - that is
  // expected. The sampler only ever touches the cached references, so it can
  // still read thread contexts after everything else is stuck. Holding
  // object_refs keeps those threads alive, so the handles stay valid.
  struct Shared {
    std::mutex mu;
    std::vector<kernel::object_ref<kernel::XThread>> threads;
    uint32_t generation = 0;
  };
  auto shared = std::make_shared<Shared>();

  std::thread([shared, ksp = ks]() {
    xe::threading::set_name("GuideProbeCache");
    for (;;) {
      auto ths = ksp->object_table()->GetObjectsByType<kernel::XThread>(
          kernel::XObject::Type::Thread);
      {
        std::lock_guard<std::mutex> lk(shared->mu);
        shared->threads = std::move(ths);
        shared->generation++;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }).detach();

  auto* pproc = ks->processor();
  std::thread([pdelay, pproc, shared, ks]() {
    xe::threading::set_name("GuideThreadProbe");
    std::this_thread::sleep_for(std::chrono::seconds(pdelay));
    // Compare against the same scan taken at load: a page populated then and
    // zero now means the image is being clobbered after loading, which is a
    // different bug from it never being loaded.
    // Plain file, not XELOGI: if the logger were wedged these markers would
    // be the only evidence that the probe ran at all.
    FILE* pf = fopen("probe.txt", "w");
    std::vector<kernel::object_ref<kernel::XThread>> ths;
    uint32_t gen = 0;
    {
      std::lock_guard<std::mutex> lk(shared->mu);
      ths = shared->threads;
      gen = shared->generation;
    }
    if (pf) {
      fprintf(pf, "probe: awake, snapshot gen=%u threads=%zu%s", gen,
              ths.size(), "\n");
      // Who is sitting on the global critical region? It is supposed to be
      // held only for very short bursts, so a stable owner here is the hang.
      fprintf(pf, "probe: GCR owner_tid=%lu recursion=%u%s",
              static_cast<unsigned long>(
                  xe::global_critical_region::mutex().owner_thread_id()),
              xe::global_critical_region::mutex().recursion_count(),
              "\n");
      // The GCR owner is an OS thread id, and the holder is often a thread
      // created after the cached snapshot. Enumerate the process's threads
      // and read their names straight from the OS, which needs no kernel
      // lock at all.
      DWORD gcr_owner =
          xe::global_critical_region::mutex().owner_thread_id();
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        DWORD pid = GetCurrentProcessId();
        if (Thread32First(snap, &te)) {
          do {
            if (te.th32OwnerProcessID != pid) continue;
            std::string nm;
            HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                   te.th32ThreadID);
            if (th) {
              PWSTR desc = nullptr;
              if (SUCCEEDED(GetThreadDescription(th, &desc)) && desc) {
                for (PWSTR q = desc; *q; ++q) {
                  nm += (*q < 128) ? char(*q) : '?';
                }
                LocalFree(desc);
              }
              CloseHandle(th);
            }
            fprintf(pf, "os-thread tid=%-6lu %-28s%s%s",
                    static_cast<unsigned long>(te.th32ThreadID), nm.c_str(),
                    te.th32ThreadID == gcr_owner ? "  <== GCR OWNER" : "",
                    "\n");
          } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
        fflush(pf);
      }
      // Map host addresses to loaded modules, so the owner's rip and return
      // addresses name a DLL instead of a bare number.
      struct Mod {
        uint64_t base, size;
        std::string name;
      };
      std::vector<Mod> mods;
      HANDLE msnap = CreateToolhelp32Snapshot(
          TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
      if (msnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me;
        me.dwSize = sizeof(me);
        if (Module32FirstW(msnap, &me)) {
          do {
            std::string nm;
            for (PWSTR q = me.szModule; *q; ++q) {
              nm += (*q < 128) ? char(*q) : '?';
            }
            mods.push_back({reinterpret_cast<uint64_t>(me.modBaseAddr),
                            me.modBaseSize, nm});
          } while (Module32NextW(msnap, &me));
        }
        CloseHandle(msnap);
      }
      auto where = [&mods](uint64_t a) -> std::string {
        for (auto& m : mods) {
          if (a >= m.base && a < m.base + m.size) {
            return fmt::format("{}+{:X}", m.name, a - m.base);
          }
        }
        return "?";
      };

      // Sample the GCR owner directly by tid. It is typically a guest thread
      // created after the cached snapshot, so this is the only way to see
      // where it stopped.
      if (gcr_owner) {
        HANDLE oh = OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
            FALSE, gcr_owner);
        if (oh) {
          CONTEXT oc = {};
          oc.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
          if (SuspendThread(oh) != static_cast<DWORD>(-1)) {
            if (GetThreadContext(oh, &oc)) {
              fprintf(pf, "GCR owner rip=%016llX  %s%s",
                      static_cast<unsigned long long>(oc.Rip),
                      where(oc.Rip).c_str(), "\n");
              fflush(pf);
              // Walk a little of its stack for return addresses; the guest
              // frame that took the lock should be in here somewhere.
              for (int d = 0; d < 48; ++d) {
                uint64_t slot = 0;
                SIZE_T got = 0;
                if (!ReadProcessMemory(
                        GetCurrentProcess(),
                        reinterpret_cast<LPCVOID>(oc.Rsp + d * 8), &slot,
                        sizeof(slot), &got) ||
                    got != sizeof(slot)) {
                  break;
                }
                if (slot > 0x10000) {
                  std::string w = where(slot);
                  if (w != "?") {
                    fprintf(pf, "GCR owner stack[%02d]=%016llX  %s%s", d,
                            static_cast<unsigned long long>(slot), w.c_str(),
                            "\n");
                  }
                }
              }
              fflush(pf);
            }
            ResumeThread(oh);
          }
          CloseHandle(oh);
        }
      }
    }
    struct Row {
      uint32_t h;
      std::string nm;
      uint64_t rip;
      uint32_t lr, r3;
      uint32_t tid;
      uint32_t r1 = 0;
      uint64_t rsp = 0;
    };
    std::vector<Row> rows;
    for (auto& th : ths) {
      void* nh2 = th->thread() ? th->thread()->native_handle() : nullptr;
      if (!nh2) continue;
      uint64_t rip = 0, rsp = 0;
      uint32_t lr = 0, r3 = 0;
      CONTEXT c2 = {};
      c2.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      if (SuspendThread(reinterpret_cast<HANDLE>(nh2)) !=
          static_cast<DWORD>(-1)) {
        if (GetThreadContext(reinterpret_cast<HANDLE>(nh2), &c2)) {
          rip = c2.Rip;
          rsp = c2.Rsp;
        }
        ResumeThread(reinterpret_cast<HANDLE>(nh2));
      }
      auto* tc = th->thread_state() ? th->thread_state()->context() : nullptr;
      if (tc) {
        lr = static_cast<uint32_t>(tc->lr);
        r3 = static_cast<uint32_t>(tc->r[3]);
      }
      rows.push_back({th->handle(), th->thread_name(), rip, lr, r3,
                      th->thread() ? th->thread()->system_id() : 0,
                      tc ? static_cast<uint32_t>(tc->r[1]) : 0u, rsp});
    }
    if (pf) {
      for (auto& r : rows) {
        fprintf(pf, "%08X %-26s tid=%-6u rip=%016llX lr=%08X r3=%08X\n",
                r.h, r.nm.c_str(), r.tid,
                static_cast<unsigned long long>(r.rip), r.lr, r.r3);
        // Phase 1099z56: host return addresses inside the exe (symbolize
        // with research/symbolize.py) - a freeze that wedges the logger is
        // only visible from here. The stack is sampled after the thread
        // resumed, so it is a hint, not an exact trace.
        const uint64_t exe_lo =
            reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe_lo);
        const auto* nt =
            reinterpret_cast<IMAGE_NT_HEADERS*>(exe_lo + dos->e_lfanew);
        const uint64_t exe_hi = exe_lo + nt->OptionalHeader.SizeOfImage;
        int shown = 0;
        for (int d = 0; r.rsp && d < 400 && shown < 14; ++d) {
          uint64_t slot = 0;
          SIZE_T got = 0;
          if (!ReadProcessMemory(GetCurrentProcess(),
                                 reinterpret_cast<LPCVOID>(r.rsp + d * 8),
                                 &slot, sizeof(slot), &got) ||
              got != sizeof(slot)) {
            break;
          }
          if (slot >= exe_lo && slot < exe_hi) {
            fprintf(pf, "   host+%llX\n",
                    static_cast<unsigned long long>(slot - exe_lo));
            ++shown;
          }
        }
      }
      fprintf(pf, "probe: sampled %zu threads\n", rows.size());
      fflush(pf);
    }
    // Resolution last: LookupFunction takes the code cache lock and may never
    // return under a deadlock. Everything above is already on disk by now.
    auto* cc2 = pproc->backend()->code_cache();
    for (auto& r : rows) {
      auto* f2 = r.rip ? cc2->LookupFunction(r.rip) : nullptr;
      uint32_t g2 = f2 ? f2->MapMachineCodeToGuestAddress(r.rip) : 0;
      if (pf) {
        fprintf(pf, "resolved %08X guest %08X%s", r.h, g2, "\n");
        fflush(pf);
      }
      // Phase 1099v: the guest back chain from r1, so a thread parked in a
      // kernel call several guest frames deep says WHERE it is waiting.
      std::string chain;
      {
        auto* mm = ks ? ks->memory() : nullptr;
        uint32_t cur = r.r1;
        for (int f = 0; mm && f < 10 && cur; ++f) {
          auto* hp = mm->LookupHeap(cur);
          if (!hp || hp->QueryRangeAccess(cur, cur + 3) ==
                         xe::memory::PageAccess::kNoAccess) {
            break;
          }
          uint32_t caller_sp =
              xe::load_and_swap<uint32_t>(mm->TranslateVirtual(cur));
          if (caller_sp <= cur || caller_sp - cur > 0x20000 || caller_sp < 8) {
            break;
          }
          auto* hp2 = mm->LookupHeap(caller_sp - 8);
          if (!hp2 || hp2->QueryRangeAccess(caller_sp - 8, caller_sp - 5) ==
                          xe::memory::PageAccess::kNoAccess) {
            break;
          }
          uint32_t ra =
              xe::load_and_swap<uint32_t>(mm->TranslateVirtual(caller_sp - 8));
          chain += fmt::format(" {:08X}", ra);
          cur = caller_sp;
        }
      }
      if (pf) {
        fprintf(pf, "chain %08X%s\n", r.h, chain.c_str());
        fflush(pf);
      }
      XELOGI("ThreadProbe: {:08X} {:26} guest {:08X} lr={:08X} r3={:08X}{} |"
             " chain{}",
             r.h, r.nm, g2, r.lr, r.r3,
             f2 ? "" : "  (host: in a kernel call)", chain);
    }
    ReportXamTextPopulation(pproc->memory(), "at probe time");
  }).detach();
  XELOGI("ThreadProbe: armed for {}s", pdelay);
}

// Phase 1099z60: dash Lua diagnostics for the Game Details page that never
// shows. Offsets are Lua 5.1 (32-bit big-endian) as embedded in dash.xex:
// lua_State top +0x08, ci +0x14; CallInfo 0x18 bytes {base, func, top,
// savedpc, ...}; LClosure p +0x10; Proto code +0x0C, lineinfo +0x14, source
// +0x20; TString text +0x10; TValue 16 bytes, tt at +8 (4 = string).
static bool GuideLuaRead(Memory* mm, uint32_t addr, uint32_t len) {
  if (addr < 0x10000u) return false;
  auto* hp = mm->LookupHeap(addr);
  return hp && ((addr >= 0x80000000u && addr < 0xA0000000u) ||
                hp->QueryRangeAccess(addr, addr + len - 1) !=
                    xe::memory::PageAccess::kNoAccess);
}
static uint32_t GuideLuaU32(Memory* mm, uint32_t addr) {
  return GuideLuaRead(mm, addr, 4)
             ? xe::load_and_swap<uint32_t>(mm->TranslateVirtual(addr))
             : 0;
}
static std::string GuideLuaTString(Memory* mm, uint32_t ts) {
  std::string s;
  if (!ts) return s;
  uint32_t len = GuideLuaU32(mm, ts + 0xC);
  if (len > 160) len = 160;
  if (!GuideLuaRead(mm, ts + 0x10, len ? len : 1)) return s;
  auto* p = mm->TranslateVirtual<const char*>(ts + 0x10);
  for (uint32_t i = 0; i < len; ++i) {
    s += (p[i] >= 0x20 && p[i] < 0x7F) ? p[i] : '?';
  }
  return s;
}
// "source:line" of the Lua function that called the C function running on L.
static std::string GuideLuaCaller(Memory* mm, uint32_t L) {
  const uint32_t ci = GuideLuaU32(mm, L + 0x14);
  if (ci < 0x18) return "?";
  const uint32_t caller = ci - 0x18;
  const uint32_t func = GuideLuaU32(mm, caller + 4);
  const uint32_t cl = GuideLuaU32(mm, func);
  if (!cl || !GuideLuaRead(mm, cl + 6, 1) ||
      *mm->TranslateVirtual<uint8_t*>(cl + 6)) {
    return "?(C caller)";
  }
  const uint32_t proto = GuideLuaU32(mm, cl + 0x10);
  const uint32_t code = GuideLuaU32(mm, proto + 0xC);
  const uint32_t savedpc = GuideLuaU32(mm, caller + 0xC);
  const uint32_t lineinfo = GuideLuaU32(mm, proto + 0x14);
  uint32_t line = 0;
  if (savedpc > code && lineinfo) {
    line = GuideLuaU32(mm, lineinfo + ((savedpc - code) / 4 - 1) * 4);
  }
  return fmt::format("{}:{}", GuideLuaTString(mm, GuideLuaU32(mm, proto + 0x20)),
                     line);
}

// Phase 1099z74: signin.xex status scene (status.xur) state machine with host
// timestamps - which states the Guide's sign-in popup passes through and how
// long each lasts. Addresses from the 1099z73 static RE of signin.xex.
static void InstallGuideSigninTraces(xe::kernel::KernelState* ks) {
  static const auto t0 = std::chrono::steady_clock::now();
  auto ms = []() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
  };
  auto* proc = ks->processor();
  // Update 9010CCD0, at the state switch: r31 = scene, [r31+0x30] = state.
  proc->AddGuestHook(0x9010CD14, [ks, ms](cpu::ppc::PPCContext* c) {
    static std::atomic<int32_t> last{-12345};
    const uint32_t st = GuideLuaU32(ks->memory(), uint32_t(c->r[31]) + 0x30);
    if (int32_t(st) != last.exchange(int32_t(st))) {
      XELOGI("GuideSignin: t={}ms status scene {:08X} state {}", ms(),
             uint32_t(c->r[31]), st);
    }
  });
  // Message handler 9010D8E8(scene, msg, ...): [msg+4] = message id.
  proc->AddGuestHook(0x9010D8E8, [ks, ms](cpu::ppc::PPCContext* c) {
    static std::atomic<uint32_t> n{0};
    if (++n > 400) return;
    XELOGI("GuideSignin: t={}ms status msg {:08X} scene {:08X}", ms(),
           GuideLuaU32(ks->memory(), uint32_t(c->r[4]) + 4), uint32_t(c->r[3]));
  });
  // Phase 1099z75: the sign-in flow end to end (addresses from the 1099z73/74
  // static RE). kind: 0 = log registers, 1 = log [r3+0x110], 2 = counter
  // (log first 3 and every 60th; f1 summed as dt ms).
  struct Site {
    uint32_t addr;
    const char* name;
    int kind;
  };
  static const Site sites[] = {
      {0x81BED140, "xam XamUserLogonTaskProc entry", 0},
      {0x81AB6E58, "xam LogonContinue entry", 0},
      {0x81AB6F08, "xam logon hr stored (r10)", 0},
      {0x81780598, "xam launcher dispatch", 0},
      {0x8178C7FC, "xam XamAppLoad(signin.xex)", 0},
      {0x81791138, "xam XamShowMessageBox entry", 0},
      {0x901096C8, "signin mode dispatch", 1},
      {0x901097D4, "signin request 2 posted", 0},
      {0x90115540, "signin state 7 stored", 0},
      {0x901150E0, "signin create status scene result", 0},
      {0x9010D7E0, "signin StatusScene init", 0},
      {0x901182D0, "signin logon task entry", 0},
      {0x90118774, "signin logon call (r4 flags)", 0},
      {0x90118778, "signin logon result (r3)", 0},
      {0x90119798, "signin error box (r4 hr, r5 string)", 0},
      {0x90115C58, "signin UI wait start", 0},
      {0x90115C5C, "signin UI wait end", 0},
      {0x90115748, "signin msgbox result", 0},
      // Phase 1099z80: profile package open behind the xgirt link warning
      // (xam 81B35168 builds "xgirt%08x"; 81739BA8 creates the link at
      // 81739F98 only if 817366C0 succeeds; 81732CFC prints the warning).
      {0x81B35168, "xam xgirt name builder entry", 3},
      {0x81739BA8, "xam content create entry", 3},
      {0x81739E5C, "xam package open+verify result (r3)", 0},
      {0x817367F8, "xam post-mount check result (r3)", 0},
      {0x81736408, "xam device-ID verdict -> PV03 cmd 2", 0},
      {0x81739F98, "xam ObCreateSymbolicLink(new package)", 0},
      {0x81739EA8, "xam create: after verify (r19 mode, r30 flags)", 4},
      {0x8173A0D4, "xam create: r19 != 1 path", 0},
      {0x8173A024, "xam create: flags 0x40 path", 0},
      {0x8173A05C, "xam create: error path (r31)", 4},
      {0x8173A00C, "xam create: skip-link path", 0},
      {0x817ABBA0, "xam symbolic link helper entry", 5},
      {0x81732CFC, "xam xgirt link warning", 3},
      // Phase 1099z98: dash disc title-info reader (9225C9F8 "Get Title Info").
      {0x922672AC, "dash disc NtOpenFile default.xex result (r3)", 0},
      {0x922675A8, "dash disc XexTransformImageKey result (r3)", 0},
      {0x92267614, "dash disc decrypt/decompress result (r3)", 0},
      {0x92267648, "dash disc page-hash verify result (r3)", 0},
      {0x9225C5C0, "dash disc resource lookup result (r3)", 0},
      {0x9225C67C, "dash disc XMsg title info result (r3)", 0},
      {0x9225C6D0, "dash disc XamLoaderSetGameInfo (r30)", 4},
      {0x913E66C8, "hud XamLoaderGetGameInfo result (r3)", 0},
      // Phase 1099z124: XNotify toast pipeline (agent RE, xam 17559).
      {0x81792E44, "xnotify pump call (CNotifyApp fields)", 6},
      {0x81769BD8, "xnotify XNotifyQueueUI (r3 type, r6 text)", 7},
      {0x817A6C90, "xnotify sign-in change handler (r4 flags)", 0},
      {0x817A70CC, "xnotify sign-in toast queue site", 0},
      // Phase 1099z136: which caller issues each logon and which one changes
      // the signed-in mask (agent RE of the 0x20 no-popup flag).
      {0x81BEF8D0, "XamUserLogonEx entry (r3 xuids, r4 flags)", 3},
      {0x817A6FF0, "xnotify sign-in masks", 10},
      {0x817A7D08, "xam auto sign-in builder", 3},
      // Phase 1099z139: xam's launch fade (graphics notification 5 thread).
      {0x8179166C, "fade: notification 5 entry", 0},
      {0x817917BC, "fade: notification 4 stop", 0},
      {0x8178E9F0, "fade: thread entry", 0},
      {0x8178EC74, "fade: gate passed", 0},
      {0x8178EC6C, "fade: gate skipped", 0},
      {0x8178EDC4, "fade: SetGammaRamp step (r29)", 11},
      {0x8178EE14, "fade: stop, black present", 0},
      {0x8176B194, "xnotify dropped by hardware flags", 0},
      {0x8176B20C, "xnotify enqueue", 0},
      {0x8176A4F4, "xnotify dequeue ([r30+8] type)", 9},
      {0x8176A508, "xnotify filter result (r3)", 0},
      {0x81769428, "xnotify scene create hresult (r3)", 0},
      {0x817698C4, "xnotify show check (r3)", 0},
      {0x8176995C, "xnotify NOT showing branch", 0},
      {0x8176A53C, "xnotify now showing", 0},
      {0x8176A588, "xnotify render call", 2},
      {0x8176A808, "xnotify timer dismiss", 0},
      {0x90106370, "signin XuiAnimRun", 2},
      {0x90106468, "signin XuiRenderPresent", 2},
  };
  for (const auto& s : sites) {
    proc->AddGuestHook(s.addr, [ks, ms, s](cpu::ppc::PPCContext* c) {
      auto* th = kernel::XThread::GetCurrentThread();
      const uint32_t tid = th ? th->handle() : 0;
      if (s.kind == 6) {
        static std::atomic<uint32_t> pn{0};
        const uint32_t k = ++pn;
        if (k > 3 && (k % 600) != 0) return;
        auto* mm = ks->memory();
        const uint32_t app = GuideLuaU32(mm, 0x81D42278);
        XELOGI("GuideSignin: t={}ms {} #{} app={:08X} +A0={:08X} +A4={:08X} "
               "+B0={:08X} +58(normal q)={:08X} +80(high q)={:08X} +E8={:08X} "
               "+C4={:08X}",
               ms(), s.name, k, app, GuideLuaU32(mm, app + 0xA0),
               GuideLuaU32(mm, app + 0xA4), GuideLuaU32(mm, app + 0xB0),
               GuideLuaU32(mm, app + 0x58), GuideLuaU32(mm, app + 0x80),
               GuideLuaU32(mm, app + 0xE8), GuideLuaU32(mm, app + 0xC4));
        return;
      }
      if (s.kind == 2) {
        static std::mutex mu;
        static std::map<uint32_t, std::pair<uint64_t, double>> acc;
        std::lock_guard<std::mutex> lk(mu);
        auto& a = acc[s.addr];
        a.first++;
        a.second += c->f[1];
        if (a.first <= 3 || a.first % 60 == 0) {
          XELOGI("GuideSignin: t={}ms {} #{} f1={:.2f} sumf1={:.1f} thread {:08X}",
                 ms(), s.name, a.first, c->f[1], a.second, tid);
        }
        return;
      }
      static std::atomic<uint32_t> total{0};
      if (++total > 2000) return;
      std::string extra;
      if (s.kind == 1) {
        extra = fmt::format(" mode=[r3+110]={:08X}",
                            GuideLuaU32(ks->memory(), uint32_t(c->r[3]) + 0x110));
      } else if (s.kind == 7) {
        std::string text;
        auto* mm = ks->memory();
        const uint32_t p = uint32_t(c->r[6]);
        for (uint32_t i = 0; i < 80 && GuideLuaRead(mm, p + i * 2, 2); ++i) {
          const uint16_t ch =
              xe::load_and_swap<uint16_t>(mm->TranslateVirtual(p + i * 2));
          if (!ch) break;
          text += (ch >= 0x20 && ch < 0x7F) ? char(ch) : '?';
        }
        extra = fmt::format(" r6=\"{}\"", text);
      } else if (s.kind == 9) {
        extra = fmt::format(" [r30+8]={:08X}",
                            GuideLuaU32(ks->memory(), uint32_t(c->r[30]) + 8));
      } else if (s.kind == 11) {
        extra = fmt::format(" r29={:08X}", uint32_t(c->r[29]));
      } else if (s.kind == 10) {
        const uint32_t sp = uint32_t(c->r[1]);
        extra = fmt::format(
            " offline-signin={:08X} online-signin={:08X} signout={:08X} "
            "flags={:08X} show={:08X}",
            GuideLuaU32(ks->memory(), sp + 0x50),
            GuideLuaU32(ks->memory(), sp + 0x54), uint32_t(c->r[22]),
            uint32_t(c->r[18]), GuideLuaU32(ks->memory(), sp + 0x5C));
      } else if (s.kind == 3) {
        extra = " chain " + ks->GuestBackChain(8);
      } else if (s.kind == 4) {
        extra = fmt::format(" r19={:08X} r30={:08X} r31={:08X}",
                            uint32_t(c->r[19]), uint32_t(c->r[30]),
                            uint32_t(c->r[31]));
      } else if (s.kind == 5) {
        // r3/r4 as guest ANSI_STRING pointers (length u16, max u16, buffer).
        auto ansi = [ks](uint32_t p) {
          std::string out;
          auto* mm = ks->memory();
          const uint32_t len =
              GuideLuaRead(mm, p, 8)
                  ? xe::load_and_swap<uint16_t>(mm->TranslateVirtual(p)) : 0;
          const uint32_t buf = GuideLuaU32(mm, p + 4);
          if (len && len < 260 && GuideLuaRead(mm, buf, len)) {
            out.assign(mm->TranslateVirtual<const char*>(buf), len);
          }
          return out;
        };
        extra = fmt::format(" a=\"{}\" b=\"{}\" chain {}",
                            ansi(uint32_t(c->r[3])), ansi(uint32_t(c->r[4])),
                            ks->GuestBackChain(4));
      }
      XELOGI("GuideSignin: t={}ms {} {:08X} thread {:08X} r3={:08X} r4={:08X} "
             "r5={:08X} r10={:08X} lr={:08X}{}",
             ms(), s.name, s.addr, tid, uint32_t(c->r[3]), uint32_t(c->r[4]),
             uint32_t(c->r[5]), uint32_t(c->r[10]), uint32_t(c->lr), extra);
    });
  }
  XELOGI("GuideSignin: hooks added");
}

// Defaults are dash.xex 2.0.17489; --trace_dash_lua passes another build's
// luaD_throw,Sleep,lua_yield and skips the 17489-only hooks after them.
// Phase 1099z170: what the guest does around a title launch - which XUI
// timelines and scene transitions xam (the Guide) and the 17559 dash play,
// and when xam's launcher, the dash's persist and the terminate run relative
// to them. Every site is checked against the instruction it was read from.
// Phase 1099z170: guide_capture_count shots, guide_capture_interval_ms apart,
// written as <prefix>_N.raw next to the exe (research/rawtopng.py).
static void GuideCaptureBurst(xe::gpu::GraphicsSystem* gs, std::string prefix) {
  std::thread([gs, prefix]() {
    xe::threading::set_name("GuideCaptureBurst");
    auto* presenter = gs ? gs->presenter() : nullptr;
    if (!presenter) return;
    const auto t0 = std::chrono::steady_clock::now();
    const int shots = std::max(1, int(cvars::guide_capture_count));
    for (int shot = 0; shot < shots; ++shot) {
      xe::ui::RawImage image;
      if (!presenter->CaptureGuestOutput(image)) {
        XELOGW("GuideCaptureBurst: capture failed at shot {}", shot);
        return;
      }
      auto path = xe::filesystem::GetExecutableFolder() /
                  fmt::format("{}_{:02}.raw", prefix, shot);
      if (FILE* f = xe::filesystem::OpenFile(path, "wb")) {
        uint32_t hdr[3] = {image.width, image.height,
                           static_cast<uint32_t>(image.stride)};
        fwrite(hdr, sizeof(hdr), 1, f);
        fwrite(image.data.data(), 1, image.data.size(), f);
        fclose(f);
      }
      XELOGI("GuideCaptureBurst: {} shot {} at +{} ms", prefix, shot,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - t0)
                 .count());
      xe::threading::Sleep(std::chrono::milliseconds(
          std::max(1, int(cvars::guide_capture_interval_ms))));
    }
  }).detach();
}

static void InstallGuideTransitionTraces(xe::kernel::KernelState* ks) {
  auto* proc = ks->processor();
  auto* mem = ks->memory();
  enum Kind { kPlay, kFind, kPlayNamed, kCreate, kPlain, kLaunch };
  struct Site {
    uint32_t addr;
    uint32_t word;
    Kind kind;
    const char* name;
  };
  static const Site kSites[] = {
      {0x817BDC78u, 0x7D8802A6u, kPlay, "xam PlayTimeline"},
      {0x817BDDE8u, 0x7D8802A6u, kFind, "xam FindNamedFrame"},
      {0x817BDCE0u, 0x7D8802A6u, kPlayNamed, "xam PlayNamedFrames"},
      {0x817C4D90u, 0x7D8802A6u, kPlain, "xam ScenePlayBackFromTransition"},
      {0x817C4DE8u, 0x7D8802A6u, kPlain, "xam ScenePlayBackToTransition"},
      {0x817C4E98u, 0x7D8802A6u, kPlain, "xam ScenePlayFromTransition"},
      {0x817C4E40u, 0x7D8802A6u, kPlain, "xam ScenePlayToTransition"},
      {0x817C0B70u, 0x7D8802A6u, kPlain, "xam SceneInterruptTransitions"},
      {0x817C56E0u, 0x7D8802A6u, kPlain, "xam SceneNavigateBack"},
      {0x817C54D0u, 0x7D8802A6u, kPlain, "xam SceneNavigateForward"},
      {0x817C5390u, 0x7D8802A6u, kPlain, "xam SceneNavigateFirst"},
      {0x817BCBE8u, 0x7D8802A6u, kPlain, "xam ElementUnlink"},
      {0x817C4A58u, 0x7CC83378u, kCreate, "xam SceneCreate"},
      {0x8169E120u, 0x7D8802A6u, kLaunch, "xam launch request"},
      {0x816A0798u, 0x7D8802A6u, kPlain, "xam launcher"},
      {0x8169E5D8u, 0x7D8802A6u, kPlain, "xam terminate title"},
      {0x921DFDB0u, 0x7D8802A6u, kPlay, "dash PlayTimeline"},
      {0x921DFFD0u, 0x7D8802A6u, kFind, "dash FindNamedFrame"},
      {0x921DFE18u, 0x7D8802A6u, kPlayNamed, "dash PlayNamedFrames"},
      {0x921E7AE0u, 0x7D8802A6u, kPlain, "dash ScenePlayBackFromTransition"},
      {0x921E7B38u, 0x7D8802A6u, kPlain, "dash ScenePlayBackToTransition"},
      {0x921E7BE8u, 0x7D8802A6u, kPlain, "dash ScenePlayFromTransition"},
      {0x921E7B90u, 0x7D8802A6u, kPlain, "dash ScenePlayToTransition"},
      {0x921E8430u, 0x7D8802A6u, kPlain, "dash SceneNavigateBack"},
      {0x921E8220u, 0x7D8802A6u, kPlain, "dash SceneNavigateForward"},
      {0x921E80E0u, 0x7D8802A6u, kPlain, "dash SceneNavigateFirst"},
      {0x921DEBD0u, 0x7D8802A6u, kPlain, "dash ElementUnlink"},
      {0x921E77A8u, 0x7CC83378u, kCreate, "dash SceneCreate"},
      {0x92183B18u, 0x7D8802A6u, kPlain, "dash launch callback"},
      {0x92182120u, 0x7D8802A6u, kPlain, "dash persist display"},
  };
  static std::atomic<uint32_t> lines{0};
  for (const Site& s : kSites) {
    proc->AddGuestHook(s.addr, [mem, ks, s](cpu::ppc::PPCContext* c) {
      const uint8_t* ip = mem->TranslateVirtual(s.addr);
      if (!ip || xe::load_and_swap<uint32_t>(ip) != s.word) return;
      if (lines.fetch_add(1) > 200000) return;
      auto wstr = [mem](uint64_t a) -> std::string {
        const uint32_t ga = static_cast<uint32_t>(a);
        if (ga < 0x10000 || ga >= 0xE0000000u) return {};
        const uint8_t* p = mem->TranslateVirtual(ga);
        if (!p) return {};
        std::string out;
        for (int i = 0; i < 96; ++i) {
          const uint16_t ch = xe::load_and_swap<uint16_t>(p + i * 2);
          if (!ch) break;
          out.push_back(ch < 0x80 ? char(ch) : '?');
        }
        return out;
      };
      const uint32_t lr = static_cast<uint32_t>(c->lr);
      const uint32_t th = xe::kernel::XThread::GetCurrentThreadHandle();
      const uint32_t r3 = static_cast<uint32_t>(c->r[3]);
      switch (s.kind) {
        case kPlay:
          XELOGI("Transition: {} elem {:08X} start {} init {} end {} loop {} "
                 "recurse {} lr {:08X} th {:08X}",
                 s.name, r3, int32_t(c->r[4]), int32_t(c->r[5]),
                 int32_t(c->r[6]), uint32_t(c->r[7]), uint32_t(c->r[8]), lr,
                 th);
          break;
        case kFind:
          XELOGI("Transition: {} elem {:08X} '{}' lr {:08X} th {:08X}", s.name,
                 r3, wstr(c->r[4]), lr, th);
          break;
        case kPlayNamed:
          XELOGI("Transition: {} elem {:08X} '{}' '{}' '{}' lr {:08X} th "
                 "{:08X}",
                 s.name, r3, wstr(c->r[4]), wstr(c->r[5]), wstr(c->r[6]), lr,
                 th);
          break;
        case kCreate:
          XELOGI("Transition: {} '{}' '{}' lr {:08X} th {:08X}", s.name,
                 wstr(c->r[3]), wstr(c->r[4]), lr, th);
          break;
        case kLaunch: {
          const int64_t now_ms =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
          ks->transition_trace_until_ms.store(now_ms + 5000);
          static std::atomic<int32_t> launches{0};
          const int32_t nth = ++launches;
          if (cvars::guide_capture_on_launch > 0 &&
              nth == cvars::guide_capture_on_launch) {
            GuideCaptureBurst(ks->emulator()->graphics_system(),
                              fmt::format("launch{}", nth));
          }
          XELOGI("Transition: {} r3 {:08X} r4 {:08X} r5 {:08X} lr {:08X} th "
                 "{:08X}",
                 s.name, r3, uint32_t(c->r[4]), uint32_t(c->r[5]), lr, th);
          break;
        }
        default:
          XELOGI("Transition: {} r3 {:08X} r4 {:08X} r5 {:08X} lr {:08X} th "
                 "{:08X}",
                 s.name, r3, uint32_t(c->r[4]), uint32_t(c->r[5]), lr, th);
          break;
      }
    });
  }
  XELOGI("Transition: {} trace sites installed", std::size(kSites));
}

// 2026-09-16, HOST-SIDE (guide_game_library_cover_art): My Games box art on
// dash 2.0.17559. The dash's ConvertToLibraryItem (Marketplace.GameLibrary
// .lua) builds every installed game's library item with boxart = '' (pc10
// `LOADK r6 ''`, pc104 `SETTABLE item.boxart r6`), so the LibrarySlot tile
// (MarketplaceSlot.xui, img_0=boxart) only ever shows its built-in
// placeholder. Two changes, both checked against the exact 17559 bytes:
//  1. pc10 becomes `GETTABLE r6 r1 'boxart'` (content.boxart; r1 is the
//     native title record, never nil there).
//  2. The record's __index getter (928ADF90) has no "boxart" property; its
//     last key compare (928AE778) is redirected for that key only, and the
//     string it pushes (928AE0E0, r4 = UTF-16BE) becomes
//     file://HDD:/BoxArt/<titleid>/largeboxart.jpg when the game library
//     saved one, or NULL (pushes '') so the placeholder stays.
// The cover files are written by app/game_library.cc. "HDD:" is the
// \??\HDD: -> Partition1 link the dash itself creates elsewhere.
namespace {
constexpr uint32_t kCoverArtPatchPc10 = 0x92FB7943;
constexpr uint32_t kCoverArtCheckPc104 = 0x92FB7ABB;
constexpr uint32_t kCoverArtCheckPc108 = 0x92FB7ACB;
constexpr uint32_t kCoverArtLastCompare = 0x928AE778;
constexpr uint32_t kCoverArtPushString = 0x928AE0E0;
std::atomic<bool> cover_art_patched{false};
thread_local bool cover_art_pending = false;
thread_local uint32_t cover_art_title_id = 0;
}  // namespace

static void InstallCoverArtHooks(xe::kernel::KernelState* ks) {
  auto* mem = ks->memory();
  auto* proc = ks->processor();
  const auto read_be32 = [mem](uint32_t address) {
    return xe::load_and_swap<uint32_t>(mem->TranslateVirtual(address));
  };

  ks->user_module_loaded_hook = [mem, read_be32](
                                    xe::kernel::UserModule* module) {
    const auto is_dash = [](const std::string& name) {
      return xe::utf8::equal_case(name, "dash") ||
             xe::utf8::equal_case(name, "dash.xex");
    };
    if (!module ||
        !(is_dash(module->name()) || is_dash(module->bounding_filename()))) {
      return;
    }
    // Older dashboards (e.g. 2.0.13604) are smaller: these addresses are not
    // mapped and reading them faulted the launch. Check before reading.
    const auto readable = [mem](uint32_t address) {
      auto* heap = mem->LookupHeap(address);
      uint32_t protect = 0;
      return heap && heap->QueryProtect(address, &protect) &&
             (protect & kMemoryProtectRead);
    };
    if (!readable(kCoverArtPatchPc10) || !readable(kCoverArtCheckPc104) ||
        !readable(kCoverArtCheckPc108)) {
      XELOGW("CoverArt: {} does not map dash 2.0.17559's GameLibrary "
             "bytecode; My Games box art left off",
             module->name());
      cover_art_patched = false;
      return;
    }
    const uint32_t pc10 = read_be32(kCoverArtPatchPc10);
    const uint32_t pc104 = read_be32(kCoverArtCheckPc104);
    const uint32_t pc108 = read_be32(kCoverArtCheckPc108);
    if (pc10 == 0x00C7C186u) {
      cover_art_patched = true;  // A second load of the same image.
      return;
    }
    if (pc10 != 0x00014181u || pc104 != 0x8F818449u || pc108 != 0x91048449u) {
      XELOGW("CoverArt: {} is not dash 2.0.17559's GameLibrary bytecode "
             "({:08X} {:08X} {:08X}); My Games box art left off",
             module->name(), pc10, pc104, pc108);
      cover_art_patched = false;
      return;
    }
    // Image pages are read-only; lift that for the write, as the patcher does.
    auto* heap = mem->LookupHeap(kCoverArtPatchPc10);
    uint32_t old_protect = 0;
    if (!heap || !heap->QueryProtect(kCoverArtPatchPc10, &old_protect)) {
      XELOGW("CoverArt: cannot query the dash image protection");
      return;
    }
    heap->Protect(kCoverArtPatchPc10, 4,
                  kMemoryProtectRead | kMemoryProtectWrite);
    xe::store_and_swap<uint32_t>(mem->TranslateVirtual(kCoverArtPatchPc10),
                                 0x00C7C186u);
    heap->Protect(kCoverArtPatchPc10, 4, old_protect);
    cover_art_patched = true;
    XELOGI("CoverArt: patched ConvertToLibraryItem pc10 (HOST-SIDE)");
  };

  proc->AddGuestHook(kCoverArtLastCompare, [mem, read_be32](
                                               cpu::ppc::PPCContext* c) {
    if (!cover_art_patched) {
      return;
    }
    const uint32_t key = uint32_t(c->r[30]);
    if (!key) {
      return;
    }
    const char* text = mem->TranslateVirtual<const char*>(key);
    if (!xe::utf8::equal_case(std::string_view(text, strnlen(text, 16)),
                              "boxart")) {
      return;
    }
    const uint32_t record = read_be32(uint32_t(c->r[31]) + 0xC);
    cover_art_title_id = record ? read_be32(record + 8) : 0;
    cover_art_pending = true;
    c->r[3] = 0;  // "matched": take this compare's branch to the push.
  });

  auto paths = std::make_shared<std::map<uint32_t, uint32_t>>();
  auto paths_mutex = std::make_shared<std::mutex>();
  proc->AddGuestHook(kCoverArtPushString, [mem, paths, paths_mutex](
                                              cpu::ppc::PPCContext* c) {
    if (!cover_art_pending) {
      return;
    }
    cover_art_pending = false;
    const uint32_t title_id = cover_art_title_id;
    uint32_t guest_path = 0;
    if (title_id && !cvars::guide_hdd_path.empty()) {
      const std::string name = fmt::format("{:08x}", title_id);
      std::error_code ec;
      const bool exists = std::filesystem::exists(
          std::filesystem::path(cvars::guide_hdd_path) / "BoxArt" / name /
              "largeboxart.jpg",
          ec);
      if (exists) {
        std::lock_guard<std::mutex> lock(*paths_mutex);
        auto it = paths->find(title_id);
        if (it == paths->end()) {
          const std::string url =
              "file://HDD:/BoxArt/" + name + "/largeboxart.jpg";
          const uint32_t size = uint32_t(url.size() + 1) * 2;
          const uint32_t address = mem->SystemHeapAlloc(size);
          if (address) {
            auto* out = mem->TranslateVirtual<uint8_t*>(address);
            for (size_t i = 0; i <= url.size(); ++i) {
              const char16_t ch = i < url.size() ? char16_t(url[i]) : 0;
              xe::store_and_swap<uint16_t>(out + i * 2, uint16_t(ch));
            }
            it = paths->emplace(title_id, address).first;
            XELOGI("CoverArt: {:08X} -> {}", title_id, url);
          }
        }
        if (it != paths->end()) {
          guest_path = it->second;
        }
      }
    }
    c->r[4] = guest_path;  // NULL pushes '': the tile keeps its placeholder.
  });
}

static void InstallGuideLuaTraces(xe::kernel::KernelState* ks,
                                  uint32_t throw_pc = 0x928E4838,
                                  uint32_t sleep_pc = 0x928F6C58,
                                  uint32_t yield_pc = 0x928E4608,
                                  bool generic_only = false) {
  static bool installed = false;
  if (installed) return;
  installed = true;
  auto* proc = ks->processor();
  // luaD_throw(L, errcode)
  proc->AddGuestHook(throw_pc, [ks](cpu::ppc::PPCContext* c) {
    static std::atomic<uint32_t> n{0};
    const uint32_t k = ++n;
    if (k > 200) return;
    auto* mm = ks->memory();
    const uint32_t L = uint32_t(c->r[3]);
    const uint32_t top = GuideLuaU32(mm, L + 8);
    std::string msg;
    if (top >= 16 && GuideLuaU32(mm, top - 16 + 8) == 4) {
      msg = GuideLuaTString(mm, GuideLuaU32(mm, top - 16));
    }
    XELOGI("GuideLua: throw #{} L={:08X} err={} lr={:08X} at {} msg \"{}\" "
           "chain{}",
           k, L, uint32_t(c->r[4]), uint32_t(c->lr), GuideLuaCaller(mm, L),
           msg, ks->GuestBackChain(8));
  });
  // Lua Sleep: log each distinct Lua call site once, then every 20000 calls
  // the counts, so a coroutine stuck in a wait shows up as a growing site.
  proc->AddGuestHook(sleep_pc, [ks, generic_only](cpu::ppc::PPCContext* c) {
    auto* mm = ks->memory();
    const std::string site = GuideLuaCaller(mm, uint32_t(c->r[3]));
    static std::mutex mu;
    static std::map<std::string, uint64_t> counts;
    static uint64_t total = 0;
    std::lock_guard<std::mutex> lk(mu);
    if (++counts[site] == 1) {
      XELOGI("GuideLua: new Sleep site {}", site);
    }
    if (++total % (generic_only ? 2000 : 20000) == 0) {
      std::string s;
      for (auto& kv : counts) s += fmt::format(" {}={}", kv.first, kv.second);
      XELOGI("GuideLua: Sleep counts after {}:{}", total, s);
    }
  });
  // lua_yield(L, nresults): where coroutines park. Distinct (site, lr) once.
  proc->AddGuestHook(yield_pc, [ks](cpu::ppc::PPCContext* c) {
    auto* mm = ks->memory();
    const std::string key = fmt::format(
        "{} lr={:08X}", GuideLuaCaller(mm, uint32_t(c->r[3])), uint32_t(c->lr));
    static std::mutex mu;
    static std::map<std::string, uint64_t> seen;
    std::lock_guard<std::mutex> lk(mu);
    if (++seen[key] == 1) {
      XELOGI("GuideLua: new yield site {} L={:08X} chain{}", key,
             uint32_t(c->r[3]), ks->GuestBackChain(6));
    }
  });
  if (generic_only) {
    // luaD_precall(L, func, nresults), if a 4th pc was given: each distinct
    // (callee, calling Lua line) once - Lua callees as source:linedefined,
    // C callees by address. Proto 5.1: +0x20 source, +0x3C linedefined.
    uint32_t pcs[4] = {};
    if (std::sscanf(cvars::trace_dash_lua.c_str(), "%x,%x,%x,%x", &pcs[0],
                    &pcs[1], &pcs[2], &pcs[3]) != 4) {
      return;
    }
    proc->AddGuestHook(pcs[3], [ks](cpu::ppc::PPCContext* c) {
      auto* mm = ks->memory();
      const uint32_t func = uint32_t(c->r[4]);
      if (GuideLuaU32(mm, func + 8) != 6) return;
      const uint32_t cl = GuideLuaU32(mm, func);
      if (!cl || !GuideLuaRead(mm, cl + 6, 1)) return;
      std::string callee;
      if (*mm->TranslateVirtual<uint8_t*>(cl + 6)) {
        callee = fmt::format("C:{:08X}", GuideLuaU32(mm, cl + 0x10));
      } else {
        const uint32_t proto = GuideLuaU32(mm, cl + 0x10);
        callee = fmt::format("{}:{}",
                             GuideLuaTString(mm, GuideLuaU32(mm, proto + 0x20)),
                             GuideLuaU32(mm, proto + 0x3C));
      }
      std::string key =
          callee + " from " + GuideLuaCaller(mm, uint32_t(c->r[3]));
      static std::mutex mu;
      static std::set<std::string> seen;
      static const auto t0 = std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> lk(mu);
      // Inside --kernel_sample_from_ms..to_ms every call is logged (a
      // stack of events, not a set).
      const int64_t now_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0)
              .count();
      const bool window = cvars::kernel_sample_to_ms > 0 &&
                          now_ms >= cvars::kernel_sample_from_ms &&
                          now_ms <= cvars::kernel_sample_to_ms;
      if (window) {
        XELOGI("GuideLuaCall* {}", key);
      } else if (seen.size() < 20000 && seen.insert(key).second) {
        XELOGI("GuideLuaCall: {}", key);
      }
    });
    return;
  }
  // Phase 1099z66: the LoadGameLibrary op's completion poll and the content
  // manager (static 929695F8) that has to finish a scan first.
  auto site_hook = [ks, proc](uint32_t addr, const char* what, bool cm_state) {
    proc->AddGuestHook(addr, [ks, addr, what, cm_state](cpu::ppc::PPCContext* c) {
      static std::mutex mu;
      static std::map<uint32_t, uint64_t> hits;
      uint64_t n;
      {
        std::lock_guard<std::mutex> lk(mu);
        n = ++hits[addr];
      }
      auto* mm = ks->memory();
      // The LoadGameLibrary op (vtable 921269FC) is always interesting.
      bool lib_op = addr == 0x928F6BB0 &&
                    GuideLuaU32(mm, GuideLuaU32(mm, uint32_t(c->r[31]) + 0x18)) ==
                        0x921269FCu;
      static std::atomic<uint32_t> lib_op_logs{0};
      if (lib_op && ++lib_op_logs > 30) lib_op = false;
      if (!lib_op && n > 6 && n % 500 != 0) return;
      std::string extra;
      if (addr == 0x928AB0B8) {
        const uint32_t op = uint32_t(c->r[3]);
        const uint32_t lib = GuideLuaU32(mm, 0x92A7B99C);
        extra = fmt::format(
            " lr={:08X} op={:08X} notified={:02X} cancelled={:08X} lib={:08X} "
            "[lib+44]={:08X} [lib+4FC]={:08X}",
            uint32_t(c->lr), op,
            GuideLuaRead(mm, op + 0x10, 1)
                ? *mm->TranslateVirtual<uint8_t*>(op + 0x10) : 0,
            GuideLuaU32(mm, op + 0x14), lib, GuideLuaU32(mm, lib + 0x44),
            GuideLuaU32(mm, lib + 0x4FC));
      } else if (addr == 0x928F6BB0) {
        const uint32_t sched = uint32_t(c->r[30]);
        extra = fmt::format(
            " IsComplete->{:08X} waiter={:08X} op={:08X} sched={:08X} "
            "[sched+8]={:02X}",
            uint32_t(c->r[3]), uint32_t(c->r[31]),
            GuideLuaU32(mm, uint32_t(c->r[31]) + 0x18), sched,
            GuideLuaRead(mm, sched + 8, 1)
                ? *mm->TranslateVirtual<uint8_t*>(sched + 8) : 0);
      } else if (addr == 0x922BED90) {
        extra = fmt::format(" r3={:08X} [r3+4FC]={:08X}", uint32_t(c->r[3]),
                            GuideLuaU32(mm, uint32_t(c->r[3]) + 0x4FC));
      } else {
        extra = fmt::format(" r3={:08X} r11={:08X}", uint32_t(c->r[3]),
                            uint32_t(c->r[11]));
      }
      if (cm_state) {
        extra += fmt::format(" CM+110={:08X} CM+4={:08X} CM+E8={:08X} "
                             "CM+8/C/10={:08X}/{:08X}/{:08X}",
                             GuideLuaU32(mm, 0x929695F8 + 0x110),
                             GuideLuaU32(mm, 0x929695F8 + 0x4),
                             GuideLuaU32(mm, 0x929695F8 + 0xE8),
                             GuideLuaU32(mm, 0x929695F8 + 0x8),
                             GuideLuaU32(mm, 0x929695F8 + 0xC),
                             GuideLuaU32(mm, 0x929695F8 + 0x10));
      }
      XELOGI("GuideLib: {} {:08X} hit #{}{}", what, addr, n, extra);
    });
  };
  // Follow the one LoadGameLibrary op through the resume path.
  static std::atomic<uint32_t> g_lib_op{0};
  proc->AddGuestHook(0x928ABFEC, [](cpu::ppc::PPCContext* c) {
    g_lib_op = uint32_t(c->r[4]);  // 928F6A60(L, op)
    XELOGI("GuideLib: LoadGameLibrary op {:08X}", g_lib_op.load());
  });
  auto op_hook = [ks, proc](uint32_t addr, const char* what, int waiter_reg) {
    proc->AddGuestHook(addr, [ks, addr, what, waiter_reg](cpu::ppc::PPCContext* c) {
      auto* mm = ks->memory();
      const uint32_t waiter = uint32_t(c->r[waiter_reg]);
      if (!g_lib_op || GuideLuaU32(mm, waiter + 0x18) != g_lib_op) return;
      XELOGI("GuideLib: [op {:08X}] {} {:08X} waiter={:08X} r3={:08X} r4={:08X} "
             "r5={:08X} co L={:08X}",
             g_lib_op.load(), what, addr, waiter, uint32_t(c->r[3]),
             uint32_t(c->r[4]), uint32_t(c->r[5]),
             GuideLuaU32(mm, waiter + 0x14));
    });
  };
  op_hook(0x928F6BB0, "scan: IsComplete returned", 31);
  op_hook(0x928F6BF0, "scan: resume queued", 31);
  op_hook(0x928F6674, "resume: GetResults returned", 30);
  op_hook(0x928F66AC, "resume: pcall(results) returned", 30);
  op_hook(0x928F6704, "resume: lua_resume(co, n)", 30);
  op_hook(0x928F6708, "resume: lua_resume returned", 30);
  site_hook(0x928AB0B8, "op IsComplete", true);
  site_hook(0x922BED90, "library pump", false);
  site_hook(0x92262F30, "CM RequestRescan", true);
  site_hook(0x92264D2C, "CM scan begins", true);
  site_hook(0x92264DC0, "CM state write", true);
  site_hook(0x92264E68, "CM worker wait returned", true);
  // Positive control: the dash worker's 45 ms poll loop (thread probe
  // 1099z40), which runs constantly.
  proc->AddGuestHook(0x922C0AC4, [](cpu::ppc::PPCContext*) {
    static std::atomic<uint32_t> n{0};
    const uint32_t k = ++n;
    if (k <= 3) XELOGI("GuideLua: control hook hit #{}", k);
  });
  XELOGI("GuideLua: guest hooks added (luaD_throw, Sleep, control)");
}

static void InstallGuideStoreTraces(xe::kernel::KernelState* ks) {
  static std::vector<std::unique_ptr<cpu::Breakpoint>> st_bps;
  if (!cvars::guide_trace_stores.empty() && st_bps.empty()) {
    std::string spec = cvars::guide_trace_stores;
    size_t pos = 0;
    while (pos <= spec.size()) {
      size_t comma = spec.find(',', pos);
      std::string tok = spec.substr(
          pos, comma == std::string::npos ? std::string::npos
                                          : comma - pos);
      if (!tok.empty()) {
        uint32_t addr =
            uint32_t(std::strtoul(tok.c_str(), nullptr, 16));
        if (addr) {
          auto bp = std::make_unique<cpu::Breakpoint>(
              ks->processor(),
              cpu::Breakpoint::AddressType::kGuest,
              uint64_t(addr),
              [](cpu::Breakpoint* bp,
                 cpu::ThreadDebugInfo* ti, uint64_t hpc) {
                auto* th = kernel::XThread::GetCurrentThread();
                auto* c =
                    th ? th->thread_state()->context() : nullptr;
                static std::atomic<uint32_t> sn{0};
                uint32_t s = ++sn;
                if (s > 60) return;
                // Dump the UTF-16 string at r3 as well:
                // these sites pass resource locators, and
                // the locator is what decides whether a
                // scene loads any visual content.
                std::string wstr;
                auto* mm = th ? th->kernel_state()->memory()
                              : nullptr;
                uint32_t sp = c ? uint32_t(c->r[3]) : 0;
                if (mm && sp > 0x1000u) {
                  auto* hp = mm->LookupHeap(sp);
                  if (hp && hp->QueryRangeAccess(sp, sp + 64) !=
                                xe::memory::PageAccess::kNoAccess) {
                    for (uint32_t w = 0; w < 48; ++w) {
                      uint16_t ch = xe::load_and_swap<uint16_t>(
                          mm->TranslateVirtual(sp + w * 2));
                      if (!ch) break;
                      wstr += (ch >= 0x20 && ch < 0x7F)
                                  ? char(ch) : '?';
                    }
                  }
                }
                std::string wstr2;
                uint32_t sp2 = c ? uint32_t(c->r[4]) : 0;
                if (mm && sp2 > 0x1000u) {
                  auto* hp2 = mm->LookupHeap(sp2);
                  if (hp2 &&
                      hp2->QueryRangeAccess(sp2, sp2 + 64) !=
                          xe::memory::PageAccess::kNoAccess) {
                    for (uint32_t w = 0; w < 48; ++w) {
                      uint16_t ch = xe::load_and_swap<uint16_t>(
                          mm->TranslateVirtual(sp2 + w * 2));
                      if (!ch) break;
                      wstr2 += (ch >= 0x20 && ch < 0x7F)
                                   ? char(ch) : '?';
                    }
                  }
                }
                XELOGI("StoreTrace {:08X} #{}: r3={:08X} "
                       "\"{}\"  r4={:08X} \"{}\"  lr={:08X}",
                       bp->guest_address(), s, sp, wstr,
                       sp2, wstr2, c ? uint32_t(c->lr) : 0);
              });
          ks->processor()->AddBreakpoint(bp.get());
          st_bps.push_back(std::move(bp));
        }
      }
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    XELOGI("StoreTrace: installed {} breakpoints",
           st_bps.size());
  }
}

X_STATUS Emulator::CompleteLaunch(const std::filesystem::path& path,
                                  const std::string_view module_path) {
  // Making changes to the UI (setting the icon) and executing game config
  // load callbacks which expect to be called from the UI thread.
  // If not on UI thread, dispatch to it synchronously.
  if (!display_window_->app_context().IsInUIThread()) {
    X_STATUS result = X_STATUS_UNSUCCESSFUL;
    display_window_->app_context().CallInUIThreadSynchronous(
        [this, &path, &module_path, &result]() {
          result = CompleteLaunch(path, module_path);
        });
    return result;
  }

  // Setup NullDevices for raw HDD partition accesses
  // Cache/STFC code baked into games tries reading/writing to these
  // By using a NullDevice that just returns success to all IO requests it
  // should allow games to believe cache/raw disk was accessed successfully

  // NOTE: this should probably be moved to xenia_main.cc, but right now we
  // need to register the \Device\Harddisk0\ NullDevice _after_ the
  // \Device\Harddisk0\Partition1 HostPathDevice, otherwise requests to
  // Partition1 will go to this. Registering during CompleteLaunch allows us
  // to make sure any HostPathDevices are ready beforehand. (see comment above
  // cache:\ device registration for more info about why)
  // Phase 1099z159: the hard drive's system auxiliary partition, which a
  // system update fills with the avatar asset pack (xam 17559 reads
  // \Device\Harddisk0\SystemAuxPartition\ and "%s\AvatarAssetPack"). Served
  // read-only from the update package the importer extracted.
  if (!cvars::kernel_system_aux_path.empty()) {
    auto aux = std::make_unique<vfs::HostPathDevice>(
        "\\Device\\Harddisk0\\SystemAuxPartition",
        cvars::kernel_system_aux_path, true);
    if (aux->Initialize() && file_system_->RegisterDevice(std::move(aux))) {
      XELOGI("SystemAux: \\Device\\Harddisk0\\SystemAuxPartition -> {}",
             xe::path_to_utf8(cvars::kernel_system_aux_path));
    }
  }
  // Phase 1099z159: the system extended partition (the update's versioned
  // system files, "SystemExtPartition\%08X\dash.xex" in xam 17559).
  if (!cvars::kernel_system_ext_path.empty()) {
    auto ext = std::make_unique<vfs::HostPathDevice>(
        "\\Device\\Harddisk0\\SystemExtPartition",
        cvars::kernel_system_ext_path, true);
    if (ext->Initialize() && file_system_->RegisterDevice(std::move(ext))) {
      XELOGI("SystemExt: \\Device\\Harddisk0\\SystemExtPartition -> {}",
             xe::path_to_utf8(cvars::kernel_system_ext_path));
    }
  }
  auto null_paths = {std::string("\\Partition0"), std::string("\\Cache0"),
                     std::string("\\Cache1")};
  auto null_device =
      std::make_unique<vfs::NullDevice>("\\Device\\Harddisk0", null_paths);
  if (null_device->Initialize()) {
    file_system_->RegisterDevice(std::move(null_device));
  }
  if (cvars::kernel_device_auth) {
    // 1099z17559-5: the kernel's accessory-authentication device. Opening it
    // is all xam needs from the file system; NtDeviceIoControlFile handles its
    // requests (xboxkrnl_io.cc).
    auto auth = std::make_unique<vfs::NullDevice>(
        "\\Device\\DeviceAuth", std::initializer_list<std::string>{});
    if (auth->Initialize()) {
      file_system_->RegisterDevice(std::move(auth));
    }
  }

  // Reset state.
  title_id_ = std::nullopt;
  title_name_ = "";
  title_version_ = "";
  display_window_->SetIcon(nullptr, 0);

  // Allow xam to request module loads.
  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");

  // Register \SystemRoot before the title runs. LaunchXexFile registers it
  // only after CompleteLaunch returns, by which time the guest has already
  // queried it - the dashboard looks for \SystemRoot\systemupdate.xex during
  // startup and gets "device not found". Doing it here makes the link exist
  // when the title first asks.
  if (cvars::system_root_early && !module_path.empty()) {
    // Prefer the SYS: mount when guide_system_root is set. \SystemRoot is
    // where xam looks for huduiskin.xex (the skin loader at 81795548 opens
    // \SystemRoot\huduiskin.xex, and it is the only route to
    // XuiVisualRegister). Aliasing it to the title's own base makes that a
    // lookup on GAME:, which is the dashboard folder only when the dashboard
    // is the title - launch a real disc and GAME: is the disc, the skin is not
    // on it, and the visual registry stays empty. Measured on the PvZ harness:
    // "Early SystemRoot -> 'GAME:'" then "File not found:
    // \SystemRoot\huduiskin.xex", so the Guide had no visuals to draw.
    std::string base;
    if (!cvars::guide_system_root.empty()) {
      base = "SYS:";
    } else {
      base = utf8::find_base_guest_path(module_path);
    }
    if (!base.empty()) {
      file_system_->RegisterSymbolicLink("\\SystemRoot", base);
      XELOGI("Early SystemRoot -> '{}' (guide_system_root='{}')", base,
             cvars::guide_system_root);
    }
  }

  // Phase 1095x: let the EMULATOR extract the system update. Xenia already
  // recognises PIRS/CON/LIVE and builds an XContentContainerDevice for them
  // (see the FileSignatureType switch above), so point it at the update
  // directory and list what each package holds. The reason this matters:
  // nothing in this environment imports xam ordinal 2798 - the export thunk
  // 817BFB60 that tail-branches to xam's RegisterDevice at 81731818 - so the
  // only device that ever registers is xam's internal HDD entry at 81731BE0
  // (kind 1) and no kind-3 device exists, which is why 817319F4's use-path
  // walks an unlinked list head and faults at 817286C0.
  if (!cvars::guide_mount_update.empty()) {
    const auto updir = xe::to_path(cvars::guide_mount_update);
    std::vector<std::filesystem::path> pkgs;
    std::error_code ec;
    for (auto& de : std::filesystem::directory_iterator(updir, ec)) {
      if (!de.is_regular_file(ec)) continue;
      pkgs.push_back(de.path());
    }
    std::sort(pkgs.begin(), pkgs.end());
    XELOGI("GuideUpdate: scanning {} ({} file(s))",
           cvars::guide_mount_update, pkgs.size());
    uint32_t idx = 0;
    for (auto& pkg : pkgs) {
      const auto sig = GetFileSignature(pkg);
      if (sig != FileSignatureType::PIRS &&
          sig != FileSignatureType::CON &&
          sig != FileSignatureType::LIVE) {
        continue;
      }
      const std::string mount = fmt::format("\UPD{}", idx);
      auto dev = vfs::XContentContainerDevice::CreateContentDevice(mount, pkg);
      if (!dev || !dev->Initialize()) {
        XELOGE("GuideUpdate: {} - container device failed",
               xe::path_to_utf8(pkg.filename()));
        continue;
      }
      if (!file_system_->RegisterDevice(std::move(dev))) {
        XELOGE("GuideUpdate: {} - RegisterDevice failed",
               xe::path_to_utf8(pkg.filename()));
        continue;
      }
      file_system_->RegisterSymbolicLink(fmt::format("UPD{}:", idx), mount);
      size_t n_entries = 0;
      if (auto* root = file_system_->ResolvePath(mount)) {
        for (size_t i = 0; i < root->children().size(); ++i) {
          auto* c = root->children()[i].get();
          if (!c) continue;
          ++n_entries;
          if (n_entries <= 64) {
            XELOGI("GuideUpdate:     {} ({} bytes)", c->name(), c->size());
          }
        }
      }
      XELOGI("GuideUpdate: {} mounted as {} - {} entr(ies)",
             xe::path_to_utf8(pkg.filename()), mount, n_entries);
      if (!cvars::guide_extract_update.empty()) {
        const auto outdir = xe::to_path(cvars::guide_extract_update) /
                            pkg.filename();
        std::error_code oec;
        std::filesystem::create_directories(outdir, oec);
        size_t wrote = 0, failed = 0;
        if (auto* root = file_system_->ResolvePath(mount)) {
          for (size_t i = 0; i < root->children().size(); ++i) {
            auto* c = root->children()[i].get();
            if (!c || c->attributes() & vfs::kFileAttributeDirectory) continue;
            vfs::File* f = nullptr;
            if (c->Open(vfs::FileAccess::kGenericRead, &f) != X_STATUS_SUCCESS ||
                !f) {
              ++failed;
              continue;
            }
            std::vector<uint8_t> buf(c->size());
            size_t got = 0;
            if (!buf.empty()) {
              f->ReadSync(std::span<uint8_t>(buf.data(), buf.size()), 0, &got);
            }
            f->Destroy();
            auto of = xe::filesystem::OpenFile(outdir / xe::to_path(c->name()),
                                               "wb");
            if (of) {
              if (got) fwrite(buf.data(), 1, got, of);
              fclose(of);
              ++wrote;
            } else {
              ++failed;
            }
          }
        }
        XELOGI("GuideUpdate: extracted {} file(s) from {} to {} ({} failed)",
               wrote, xe::path_to_utf8(pkg.filename()),
               xe::path_to_utf8(outdir), failed);
      }
      ++idx;
    }
  }

  // Phase 1095z: give xam a real \Device\Flash. The run log has always shown
  // ResolvePath(\Device\Flash\xstudio.xex) failing "device not found", and a
  // Guide item failing ResolvePath(createprofile.xex) since 1091w, because no
  // Flash device existed. These are the guest's own modules, extracted from the
  // user's system update by guide_extract_update.
  if (!cvars::guide_flash_root.empty()) {
    auto flash_dev = std::make_unique<vfs::HostPathDevice>(
        "\\Device\\Flash", xe::to_path(cvars::guide_flash_root), true);
    if (flash_dev->Initialize() &&
        file_system_->RegisterDevice(std::move(flash_dev))) {
      file_system_->RegisterSymbolicLink("flash:", "\\Device\\Flash");
      XELOGI("Guide: mounted {} as the Flash device (and flash:)",
             cvars::guide_flash_root);
    } else {
      XELOGE("Guide: failed to mount flash root {}", cvars::guide_flash_root);
    }
  }

  // LLE xam bootstrap: load the real xam.xex as a guest module before the main
  // module, so the main module's xam imports bind against xam's real export
  // table instead of Xenia's HLE xam. See XexModule::SetupLibraryImports.
  if (!cvars::guide_system_root.empty()) {
    // xam.xex and hud.xex normally have to sit on the title's own GAME:
    // device, which only works when the title is the dashboard folder.
    // Mount them separately so a real game disc can be the title.
    auto sys_device = std::make_unique<vfs::HostPathDevice>(
        "\\SYS", xe::to_path(cvars::guide_system_root), true);
    if (sys_device->Initialize() &&
        file_system_->RegisterDevice(std::move(sys_device))) {
      file_system_->RegisterSymbolicLink("SYS:", "\\SYS");
      XELOGI("Guide: mounted system root {} as SYS:",
             cvars::guide_system_root);
      if (cvars::guide_media_link) {
        file_system_->RegisterSymbolicLink("media:", "\\SYS");
        XELOGI("Guide: linked media: to the system root (xam's font files)");
      }
    } else {
      XELOGE("Guide: failed to mount system root {}",
             cvars::guide_system_root);
    }
  }
  // Phase 1099z125 cold boot: the 17489 kernel's phase-1 init starts the boot
  // animation (progress 0x72, AniStartBootAnimation(0) at 80081858) before it
  // loads xam (0x79) and does not wait for it; xam waits in
  // AniBlockOnAnimation.
  if (cvars::guide_cold_boot && !cvars::lle_xam.empty()) {
    const uint32_t ani = kernel::xboxkrnl::AniStartBootAnimationHost(false);
    XELOGI("Cold boot: AniStartBootAnimation(0) = {:08X}", ani);
  }
  if (!cvars::lle_xam.empty()) {
    XELOGI("LLE xam: loading guest xam from {}", cvars::lle_xam);
    lle_xam_module_ = kernel_state_->LoadUserModule(cvars::lle_xam, false);
    auto xam_module = lle_xam_module_;
    if (!xam_module) {
      XELOGE("LLE xam: failed to load {}", cvars::lle_xam);
      return X_STATUS_NOT_FOUND;
    }
    // 2026-09-16: a pre-2010 update's xam is the base 2.0.1888 image plus
    // xam.xexp beside it (kernel_system_flash_patches); every other module
    // loader applies patches before finishing, and so must this one.
    if (cvars::kernel_system_flash_patches &&
        file_system_->ResolvePath(cvars::lle_xam + 'p')) {
      kernel_state_->ApplyTitleUpdate(xam_module);
    }
    // call_entry=false: DllMain is run later on a real guest thread by the
    // bootstrap below. CompleteLaunch runs on the UI thread, which has no
    // guest thread state, so executing guest code here is not valid.
    X_RESULT xam_result =
        kernel_state_->FinishLoadingUserModule(xam_module, false);
    if (XFAILED(xam_result)) {
      XELOGE("LLE xam: failed to finish loading {}", cvars::lle_xam);
      return xam_result;
    }
    XELOGI("LLE xam: loaded at {:08X}", xam_module->hmodule_ptr());
    GuideHudSlotMark(kernel_state_.get(), "xam loaded, before DllMain");
    if (xam_module->xex_module()) {
      g_xam_img_lo = xam_module->xex_module()->base_address();
      g_xam_img_hi = g_xam_img_lo + xam_module->xex_module()->image_size();
      // Publish it to the Guide code in xboxkrnl_video.cc, which reads its own
      // set of dashroot-derived constants.
      kernel::xboxkrnl::SetXamImageExtent(g_xam_img_lo, g_xam_img_hi);
      XELOGI("LLE xam: image extent {:08X}..{:08X}", g_xam_img_lo,
             g_xam_img_hi);
    }
    // Dump the loaded xam image, if asked. This has to happen here rather than
    // in the draw path: on a non-dashroot build the Guide never binds a
    // command buffer, so a dump placed there never fires - and that image is
    // exactly what is needed to re-establish addresses for such a build.
    if (!cvars::guide_dump_xam_path.empty()) {
      auto* dm = memory();
      // Use the module's own image size, not dashroot's 0x8C0000: the retail
      // xam image is smaller, so a fixed extent failed the range check and
      // dumped nothing precisely on the build the dump exists for.
      uint32_t dbase = xam_module->xex_module()
                           ? xam_module->xex_module()->base_address()
                           : 0x815F0000u;
      uint32_t dsize = xam_module->xex_module()
                           ? xam_module->xex_module()->image_size()
                           : 0u;
      if (!dsize) dsize = 0x8C0000u;
      // The image is not contiguously mapped over its whole extent, so a
      // single range check rejects it. Walk it in 4K pages, writing what is
      // mapped and zero-filling what is not: the scratchpad tooling wants a
      // flat VA-indexed image, and a hole reads as zeroes there anyway.
      FILE* f = std::fopen(cvars::guide_dump_xam_path.c_str(), "wb");
      if (!f) {
        XELOGW("DumpXam: could not open {}", cvars::guide_dump_xam_path);
      } else {
        static const uint8_t kZero[0x1000] = {0};
        uint32_t mapped = 0, holes = 0;
        for (uint32_t off = 0; off < dsize; off += 0x1000u) {
          uint32_t a = dbase + off;
          uint32_t n = std::min<uint32_t>(0x1000u, dsize - off);
          auto* hp = dm ? dm->LookupHeap(a) : nullptr;
          bool ok = hp && hp->QueryRangeAccess(a, a + n) !=
                              xe::memory::PageAccess::kNoAccess;
          if (ok) {
            std::fwrite(dm->TranslateVirtual(a), 1, n, f);
            ++mapped;
          } else {
            std::fwrite(kZero, 1, n, f);
            ++holes;
          }
        }
        std::fclose(f);
        XELOGI("DumpXam: {:08X}+{:X} -> {} ({} pages mapped, {} zero-filled)",
               dbase, dsize, cvars::guide_dump_xam_path, mapped, holes);
      }
    }
    if (cvars::guide_patch_cmdbuf_reset) {
      // 81A01464  stw r30,0x2B4C(r31)  ; zeroes the cmdbuf write cursor
      const uint32_t kRAddr = 0x81A01464u;
      const uint32_t kROrig = 0x93DF2B4Cu;
      auto* rw = memory()->TranslateVirtual<uint32_t*>(kRAddr);
      uint32_t rcur = xe::load_and_swap<uint32_t>(rw);
      if (rcur == kROrig) {
        void* rpage = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(rw) & ~uintptr_t(0xFFF));
        xe::memory::PageAccess rold = xe::memory::PageAccess::kReadOnly;
        if (xe::memory::Protect(rpage, 0x1000,
                                xe::memory::PageAccess::kReadWrite,
                                &rold)) {
          xe::store_and_swap<uint32_t>(rw, 0x60000000u);
          xe::memory::Protect(rpage, 0x1000, rold, nullptr);
          XELOGI("Guide: patched {:08X} {:08X} -> 60000000 (cmdbuf "
                 "cursor reset removed)",
                 kRAddr, rcur);
        }
      } else {
        XELOGW("Guide: NOT patching {:08X}: found {:08X}, expected {:08X}",
               kRAddr, rcur, kROrig);
      }
    }
    ReportXamTextPopulation(memory(), "after xam load");
    // Phase 1097zs: arming at Guide-press time cannot see anything written
    // during xam's own boot, which is exactly the window that matters for
    // "is [81D426C8] ever written and then cleared, or never written at all"
    // (1097zr). This arms it here instead, right after xam is loaded.
    if (cvars::guide_watch_write_addr && cvars::guide_watch_write_early) {
      ArmGuideWriteWatch(kernel_state_.get(), cvars::guide_watch_write_addr);
    }
    // Phase 1099z44: arm after a delay instead, for heap pages that are not
    // committed yet at xam load.
    if (cvars::guide_watch_write_addr &&
        cvars::guide_watch_write_delay_seconds > 0) {
      auto* ks_ww = kernel_state_.get();
      std::thread([ks_ww]() {
        for (int i = 0; i < cvars::guide_watch_write_delay_seconds; ++i) {
          xe::threading::Sleep(std::chrono::seconds(1));
        }
        ArmGuideWriteWatch(ks_ww, cvars::guide_watch_write_addr);
      }).detach();
    }
    // These addresses hold correct code here and read back as zero later in
    // the run, then are correct again by 30s. Poll them so the transition is
    // timestamped against the surrounding log rather than inferred.
    {
      Memory* wmem = memory();
      ++g_memory_pollers;
      std::thread([wmem]() {
        xe::threading::set_name("XamTextWatch");
        // 81D3F8A0 is xam's 64-bit feature-enable bitmask (bit id-1 per the
        // table at 815FA1E0). It reads zero on disk and zero at both load
        // checkpoints, which should mean XSTUDIO (id 6, bit 5) is disabled and
        // that 817CE3C8 returns 80004005 without ever calling 81747D70 - yet
        // the crash unwind shows it did call it. Either the mask is set
        // between the checkpoints and the call, or that reasoning is wrong.
        // Watching it says which, and nothing in xam writes it through any
        // lis+displacement store, so a writer would be worth seeing.
        // 815FA1E0/815FA280 are the feature table in .rdata - rec0 (key 1)
        // and rec5 (key 6, XSTUDIO). The four .text addresses above are known
        // to flip to zero and back together; 81D3F8A0 in .data has been
        // measured never to change. Whether .rdata flips is the open question,
        // and it matters directly: if the table reads zero at the instant
        // 81747A00 searches it, the lookup misses a key that is present, which
        // is exactly the failure seen at 81747DDC.
        const uint32_t addrs[] = {0x8186E528u, 0x818936B8u, 0x81747D70u,
                                  0x818AE538u, 0x81D3F8A0u,
                                  0x815FA1E0u, 0x815FA280u,
                                  // 81D43C78 is [skin context + 0x28], the
                                  // callback manager the skin loader calls
                                  // through. Nothing in xam writes it - no
                                  // store to that offset exists anywhere in
                                  // .text - so either something outside the
                                  // module creates it, or it is populated
                                  // later than our bootstrap runs the loader.
                                  // Watching it on the *default* path (skin
                                  // init off) distinguishes the two: if it
                                  // ever becomes non-null, the loader is
                                  // simply being called too early.
                                  0x81D43C78u,
                                  // Phase 1096gm: both read correct at the
                                  // two load checkpoints and wrong inside a
                                  // crash dump later in the same run -
                                  // 81687324 is a real vtable on disk
                                  // (81AB1980 81AB8700 ...) but reads
                                  // "00020010 00100010 00100020 0..." at the
                                  // faulting bctr that consumes it, and
                                  // 8168C820 is the string "LiveQosHistory"
                                  // on disk but reads as PPC instructions
                                  // that appear nowhere in the image. Both
                                  // sit in the 64k RODATA page 81680000. The
                                  // protect= field logged on a change is the
                                  // discriminator: still read-only means the
                                  // mapping was replaced, writable means
                                  // something stored through this view.
                                  0x81687324u, 0x8168C820u,
                                  // Phase 1096hb: 81D43CF8 is [81D43C50+0xA8],
                                  // the single field measured to gate BOTH
                                  // xam's sys-app launcher (via the accessor
                                  // 8178D678) and the HUD-manager loop body at
                                  // 81794C7C. It has been 0 in every run so
                                  // far. Now that xam's image is no longer
                                  // corrupted and the dashboard is
                                  // deterministic, watch it directly rather
                                  // than inferring its state from what does
                                  // not happen.
                                  0x81D43CF8u,
                                  // Phase 1097zs: the Guide app record
                                  // pointer. 1097zr proved nothing writes it
                                  // after the Guide press (0 catches against
                                  // 4.1 million on the same page), but the
                                  // page-protect watch cannot cover xam's own
                                  // boot without changing the timing of the
                                  // thing it watches - it crashed the guest
                                  // when tried. Polling costs nothing and
                                  // answers the remaining half: is this word
                                  // EVER non-zero, at any point in the run.
                                  0x81D426C8u,
                                  // Phase 1097zx: [81D43C50+0x40]. Ordinal
                                  // 0x299 publishes the HUD state as
                                  // [[block+0x40]+0x90] - an ARBITRARY value
                                  // out of a sub-object - after checking
                                  // [+0xB8], [+0xB4] and [+0xA8] (the gate
                                  // this phase opened). So the state CAN be
                                  // 0x10; the "setter has two callers passing
                                  // 4 and 0x40" argument never covered this
                                  // store. The one catch of it had r11 = 0
                                  // after the +0x40 load, i.e. the chain was
                                  // null. Poll it: if it is never non-null,
                                  // that is the blocker; if it becomes
                                  // non-null, read [+0x90].
                                  0x81D43C90u,
                                  // Phase 1097zz: [81D43C50+0x44], the SECOND
                                  // and independent gate on writing +0x40.
                                  // 81795030..8179504C requires it non-zero
                                  // with bit 0 set (and not equal to 1) to
                                  // reach 81795064, whose block writes +0x40
                                  // at 8179510C - a path that does NOT depend
                                  // on the HUD state, so it is the one way out
                                  // of the 1097zx cycle. A static scan found no
                                  // writer, but this phase's scans have missed
                                  // writers three times where a watch then
                                  // found them, so poll it rather than trust
                                  // the scan.
                                  0x81D43C94u,
                                  // Phase 1098d: 401C09D0 is the object the
                                  // HUD-manager loop polls through
                                  // 8174FDA0 -> 8174F680 ([[81D43C50+0x80]+0x38],
                                  // measured 34M times, always this pointer),
                                  // and its outstanding-work count +0x14 is
                                  // stuck at exactly 1 all run. Its VTABLE
                                  // POINTER at +0 decides whose class it is:
                                  // the constructor 8174E170 installs
                                  // 815FAD34, while the "Guide Background
                                  // transition" vtable whose slot 7 clears
                                  // +0x14 is 815FAFBC. 1098d identified the
                                  // class from the second one WITHOUT checking
                                  // this word, and the ticker 8174C7C8 was
                                  // measured to run only on 401EA240/401BF070/
                                  // 408C4280 - never on this object - so the
                                  // identification is unconfirmed. Read +0 and
                                  // +0x10 and settle it.
                                  0x401C09D0u,
                                  0x401C09E0u,
                                  // Phase 1098e: the completion test is
                                  // 8174FFD0 (vtable slot 1 of CHUDBkgndScene,
                                  // a LEAF function - the mflr backscan
                                  // misfiled it under 8174FF38): it clears
                                  // [obj+0x14] only when [obj+4] == [obj+8].
                                  // So +4 is "loads finished" and +8 is "loads
                                  // expected". The page watch already caught
                                  // +4 being written (lr 8174C7AC x2, lr
                                  // 8194A070 x2), so it moves - it just never
                                  // reaches +8. Read both.
                                  0x401C09D4u,
                                  0x401C09D8u,
                                  // Phase 1098h: the HUD state word itself, and
                                  // the scene's pending flag. The page
                                  // write-watch SAMPLES and missed the state-8
                                  // write that [81D43C90] proved happened, so
                                  // read these by poll instead - it is
                                  // non-perturbing and reports every change it
                                  // observes.
                                  0x81D43C50u,
                                  0x401C09E4u,
                                  // Phase 1098h: the Guide app record that
                                  // [81D426C8] now points at (401C2720), and
                                  // its handler slot +0xC - the field
                                  // XamRegisterSysApp (8177F240, at 8177F2CC)
                                  // fills in. If the handler is non-null, xam
                                  // can dispatch to hud; if it stays 0, that is
                                  // the next blocker.
                                  0x401C2720u,
                                  0x401C272Cu,
                                  // Phase 1097: [81D4F610] is xam's Xenon
                                  // button context (the event 817C1FF8 sets)
                                  // and [81D4F614] is the enable flag that
                                  // 817C23A8 tests FIRST - measured 0 at the
                                  // moment the registered SysReq callback was
                                  // called, so the press was refused by xam
                                  // itself. Three sites set it to 1
                                  // (8174EC40, 8175005C, 8175DDE4), each
                                  // guarded on bit 0x4000 of
                                  // XboxHardwareInfo.flags being CLEAR, and
                                  // one site inside the message dispatcher
                                  // (817807BC) sets it back to 0. Watching it
                                  // says which of "never set" and "set then
                                  // cleared" is true.
                                  // (left out of the live list: it has already
                                  // answered - see 1097 - and every extra
                                  // entry costs two heap queries per watch
                                  // iteration on a 120000-iteration loop.)
                                  // Phase 1096ho: dash has a 176-entry C++
                                  // static-initializer table at
                                  // 9293F414..9293F6D0. Each entry constructs
                                  // one static object, and every constructor
                                  // of this shape starts by writing the
                                  // object's own address into [obj] as a
                                  // self-linked list head. So [obj] != 0 means
                                  // "its initializer ran". These are the
                                  // objects of entries #1, #2, #40, #46 and
                                  // #175 - spread across the whole table, so
                                  // the pattern of which change says whether
                                  // the walk never happened or stopped
                                  // partway. Entry #46's object is 92A40C98,
                                  // whose event at +0x14 is one of the nine
                                  // that a wait cannot resolve.
                                  0x9296D710u, 0x929695F8u, 0x929973C0u,
                                  0x92A40C98u, 0x92A7BFB8u,
                                  // Phase 1096hp: dash's _initterm at 92198760
                                  // walks a 4-entry PRE-init table first
                                  // (9293F6D8..9293F6E8) and returns
                                  // immediately if any entry yields non-zero -
                                  // which SKIPS the 176-entry C++ constructor
                                  // table at 9293F410..9293F6D4 entirely. The
                                  // only entry that can fail is 9219FD70, the
                                  // CRT file-descriptor table init: it callocs
                                  // via 9219BC28, stores the pointer at
                                  // 92A7C0FC and the count at 92A7C100, and on
                                  // a second failure returns 0x1A. So
                                  // [92A7C0FC] non-zero means the allocation
                                  // succeeded and the abort is elsewhere;
                                  // staying zero means this is the root cause.
                                  0x92A7C0FCu, 0x92A7C100u,
                                  // Phase 1096hq: brackets dash's entry point.
                                  // 92196660 stores -1 to both of these at
                                  // 9219668C/90, before any of the calls it
                                  // makes, so if they never read FFFFFFFF the
                                  // entry itself never ran and main is being
                                  // reached some other way; if they do, the
                                  // failure is between there and the
                                  // _initterm call at 921966C0.
                                  0x92A7C224u, 0x92A7C228u,
                                  // Phase 1096hr POSITIVE CONTROL for the
                                  // watch itself: 92196660 is dash's entry
                                  // point and its first word is the constant
                                  // 7D8802A6 (mflr r12). If the watch reports
                                  // 00000000 for this, the watch cannot read
                                  // dash image memory and every dash result it
                                  // has produced is an artefact.
                                  0x92196660u,
                                  // Control: measured correct in the same
                                  // crash dump, so it must never change.
                                  0x81E15B20u,
                                  // Phase 1099z46: xam task object whose
                                  // vtable word becomes 01003CB4 before the
                                  // Guide-over-Sonic crash (81778558).
                                  0x401AFFA0u, 0x401AFFA8u, 0x401AFFB0u};
        constexpr size_t kWatchCount = xe::countof(addrs);
        uint32_t last[kWatchCount] = {};
        bool skip_reported[kWatchCount] = {};
        bool first_reported[kWatchCount] = {};
        bool primed = false;
        for (int iter = 0; iter < 120000 && !g_memory_pollers_stop; ++iter) {
          for (size_t i = 0; i < kWatchCount; ++i) {
            auto* hp = wmem->LookupHeap(addrs[i]);
            // Phase 1096hr: this guard silently skipped every dash address.
            // The comment on the probe list in ReportXamTextPopulation already
            // records that QueryRangeAccess "reports kNoAccess for readable
            // image addresses", and that is exactly what happened here: the
            // watch never read 92xxxxxx at all, so "no change was reported"
            // meant "never looked", not "never changed". Report the skip
            // instead of hiding it, and report the first value actually read
            // for every address so the instrument proves itself.
            if (!hp) {
              if (!skip_reported[i]) {
                skip_reported[i] = true;
                XELOGW("XamTextWatch: {:08X} has NO HEAP - not watched",
                       addrs[i]);
              }
              continue;
            }
            const bool queryable =
                hp->QueryRangeAccess(addrs[i], addrs[i] + 3) !=
                xe::memory::PageAccess::kNoAccess;
            // Reading regardless of the query cost 4 host faults - an
            // uncommitted page still faults no matter how unreliable the
            // access query is for image addresses. Ask for the region's
            // COMMIT state instead, which is what the heap scan in
            // GuideLauncherScan had to switch to for the same reason.
            bool committed = false;
            {
              HeapAllocationInfo info = {};
              if (hp->QueryRegionInfo(addrs[i] & ~0xFFFu, &info)) {
                committed = (info.state & kMemoryAllocationCommit) != 0;
              }
            }
            if (!queryable && !committed) {
              if (!skip_reported[i]) {
                skip_reported[i] = true;
                XELOGW("XamTextWatch: {:08X} not readable yet (query says "
                       "kNoAccess and the region is not committed) - skipping "
                       "until it is",
                       addrs[i]);
              }
              continue;
            }
            if (!queryable && committed && !skip_reported[i]) {
              skip_reported[i] = true;
              XELOGW("XamTextWatch: {:08X} QueryRangeAccess says kNoAccess but "
                     "the region IS committed - reading it. That query is "
                     "unreliable for image addresses, which is why every dash "
                     "address was silently skipped before this.",
                     addrs[i]);
            }
            uint32_t v = xe::load_and_swap<uint32_t>(
                wmem->TranslateVirtual(addrs[i]));
            if (!first_reported[i]) {
              first_reported[i] = true;
              // Phase 1096ht: without a timestamp, "first read = 00000000"
              // cannot be ordered against the guest thread that would have
              // written it, so it cannot distinguish "the store never ran"
              // from "we looked before it ran". Stamp it.
              XELOGI("XamTextWatch: first read of {:08X} = {:08X} @{}ms",
                     addrs[i], v,
                     xe::Clock::QueryHostUptimeMillis());
            }
            if (primed && v != last[i]) {
              // Ask the OS what the page actually is at the moment it
              // changes. If the guard is still PAGE_READONLY then nothing
              // wrote through this view; if the protection has been reset,
              // the mapping was replaced rather than written.
              MEMORY_BASIC_INFORMATION mbi = {};
              void* hostp = wmem->TranslateVirtual(addrs[i]);
              SIZE_T got = VirtualQuery(hostp, &mbi, sizeof(mbi));
              XELOGE(
                  "XamTextWatch: {:08X} changed {:08X} -> {:08X} @{}ms "
                  "(host={} protect={:X} state={:X} type={:X} allocbase={} "
                  "regionsize={:X})",
                  addrs[i], last[i], v,
                  xe::Clock::QueryHostUptimeMillis(), hostp,
                  got ? mbi.Protect : 0u, got ? mbi.State : 0u,
                  got ? mbi.Type : 0u, got ? mbi.AllocationBase : nullptr,
                  got ? static_cast<uint64_t>(mbi.RegionSize) : 0ull);
            }
            last[i] = v;
          }
          primed = true;
          // Phase 1096ht: the watch had no liveness signal, so "no change was
          // reported after time T" could not be told apart from "the thread
          // ended at T". The last event in the previous run was 15 SECONDS
          // before the title even launched, which makes that distinction the
          // whole question. Emit a heartbeat with the iteration count and the
          // current value of the two entry-point markers.
          if ((iter % 2000) == 0) {
            uint32_t m0 = 0, m1 = 0;
            for (size_t k = 0; k < kWatchCount; ++k) {
              if (addrs[k] == 0x92A7C224u) m0 = last[k];
              if (addrs[k] == 0x92A7C228u) m1 = last[k];
            }
            XELOGI("XamTextWatch: alive, iter {} @{}ms | 92A7C224={:08X} "
                   "92A7C228={:08X}",
                   iter, xe::Clock::QueryHostUptimeMillis(), m0, m1);
          }
          std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        --g_memory_pollers;
      }).detach();
      XELOGI("XamTextWatch: polling 8 xam addresses");
      // Opt-in (XENIA_XAM_RO=1): make xam's .text read-only at the host level
      // so whatever writes zeros over it traps instead of succeeding. The
      // existing fault logging then names the writer. Off by default because
      // it deliberately turns a silent corruption into a crash.
      if (std::getenv("XENIA_XAM_RO")) {
        auto* host = memory()->TranslateVirtual(0x81740000u);
        xe::memory::PageAccess old_access = xe::memory::PageAccess::kReadWrite;
        bool ok = xe::memory::Protect(host, 0x818C0000u - 0x81740000u,
                                      xe::memory::PageAccess::kReadOnly,
                                      &old_access);
        XELOGI("XamTextWatch: read-only guard over 81740000-818C0000 -> {}",
               ok ? "armed" : "FAILED");
      }
    }
    if (const char* spec = std::getenv("XENIA_EFAIL_TAG")) {
      TagEFailSites(memory(), spec, "xam");
    }
    // XENIA_TAG26=1: the six places in xam that build 0x80300026, the code
    // XuiSendMessage returns when message 9 is refused. Give each its own low
    // byte so the returned HRESULT names the site, the same trick that found
    // the scene E_FAIL in hud.
    if (std::getenv("XENIA_TAG26")) {
      // Eight, not six: requiring the "lis rX,0x8030" to sit immediately
      // before the ori missed 819362CC and 81959304, the same adjacency trap
      // that hid 201 of xam's 886 E_FAIL sites earlier.
      static const uint32_t kSites[] = {0x819330BCu, 0x81934FBCu, 0x81936100u,
                                        0x819362A8u, 0x819362CCu, 0x8193889Cu,
                                        0x81957998u, 0x81959304u};
      for (uint32_t i = 0; i < xe::countof(kSites); ++i) {
        auto* w = memory()->TranslateVirtual<uint32_t*>(kSites[i]);
        uint32_t cur = xe::load_and_swap<uint32_t>(w);
        if ((cur & 0xFC00FFFFu) != 0x60000026u) {
          XELOGW("Tag26: NOT patching {:08X}: found {:08X}", kSites[i], cur);
          continue;
        }
        void* pg = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(w) &
                                           ~uintptr_t(0xFFF));
        xe::memory::PageAccess old_access = xe::memory::PageAccess::kReadOnly;
        if (xe::memory::Protect(pg, 0x1000,
                                xe::memory::PageAccess::kReadWrite,
                                &old_access)) {
          xe::store_and_swap<uint32_t>(w, (cur & 0xFFFF0000u) | (0x30u + i));
          xe::memory::Protect(pg, 0x1000, old_access, nullptr);
          XELOGI("Tag26: {:08X} -> 803000{:02X}", kSites[i], 0x30u + i);
        }
      }
    }
    if (cvars::guide_patch_null_render) {
      // 818FDEF0  lwz r11,0x1C(r27)   ; XUI context's null-render flag
      // 818FDF14  stw r11,0x134(r30)  ; over the device context's copy
      const uint32_t kAddr = 0x818FDF14u;
      const uint32_t kOrig = 0x917E0134u;
      XELOGI("Guide: patch step 1, memory()={}",
             static_cast<const void*>(memory()));
      auto* pw = memory()->TranslateVirtual<uint32_t*>(kAddr);
      XELOGI("Guide: patch step 2, host ptr={}", static_cast<void*>(pw));
      uint32_t cur = xe::load_and_swap<uint32_t>(pw);
      XELOGI("Guide: patch step 3, cur={:08X}", cur);
      if (cur == kOrig) {
        // Guest code pages are mapped without write access, so the store
        // faults unless the page is temporarily made writable.
        void* page = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pw) &
                                             ~uintptr_t(0xFFF));
        xe::memory::PageAccess old_access = xe::memory::PageAccess::kReadOnly;
        bool unprotected = xe::memory::Protect(
            page, 0x1000, xe::memory::PageAccess::kReadWrite, &old_access);
        if (!unprotected) {
          XELOGE("Guide: could not unprotect {:08X} for patching", kAddr);
          return X_STATUS_UNSUCCESSFUL;
        }
        xe::store_and_swap<uint32_t>(pw, 0x60000000u);  // nop
        xe::memory::Protect(page, 0x1000, old_access, nullptr);
        XELOGI("Guide: patched {:08X} {:08X} -> 60000000 (null-render copy "
               "removed)",
               kAddr, cur);
      } else {
        XELOGW("Guide: NOT patching {:08X}: found {:08X}, expected {:08X}",
               kAddr, cur, kOrig);
      }
    }
    if (cvars::guide_patch_present_gate) {
      // XuiRenderPresent (dc->vtable[21], runtime 818F9290):
      //   818F92DC  lwz   r11,0x134(r31)   ; the null-render flag
      //   818F92E0  cmpwi cr6,r11,0
      //   818F92E4  bne   cr6,+0x38        ; non-zero -> skip the real present
      //   818F92E8  lwz   r3,0x1CC(r31)    ; else fall through to the wrapper
      //   ...       lwz   r11,0x60(r11)    ; vtable[24]
      //             bctr                   ; the actual present
      //
      // Nop ONLY that branch. Both existing flags clear the FIELD, so they
      // change XuiRenderBegin too - and Begin then runs vtable[20], which
      // crashes at 819DE94C or hangs the draw call at 2645 depending on
      // timing. Leaving the field non-zero keeps Begin skipping that broken
      // setup (which is what yields 2100+ composite draws) while making
      // Present actually present.
      const uint32_t kPAddr = 0x818F92E4u;
      const uint32_t kPOrig = 0x409A0038u;
      auto* pp = memory()->TranslateVirtual<uint32_t*>(kPAddr);
      uint32_t pcur = pp ? xe::load_and_swap<uint32_t>(pp) : 0;
      if (pcur == kPOrig) {
        void* ppage = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(pp) & ~uintptr_t(0xFFF));
        xe::memory::PageAccess pold = xe::memory::PageAccess::kReadOnly;
        if (xe::memory::Protect(ppage, 0x1000,
                                xe::memory::PageAccess::kReadWrite, &pold)) {
          xe::store_and_swap<uint32_t>(pp, 0x60000000u);  // nop
          xe::memory::Protect(ppage, 0x1000, pold, nullptr);
          XELOGI("Guide: patched {:08X} {:08X} -> 60000000 (present gate "
                 "removed; [dc+134] left intact for XuiRenderBegin)",
                 kPAddr, pcur);
        } else {
          XELOGE("Guide: could not unprotect {:08X} for patching", kPAddr);
        }
      } else {
        XELOGW("Guide: NOT patching {:08X}: found {:08X}, expected {:08X}",
               kPAddr, pcur, kPOrig);
      }
    }
  }

  // Lend the title's front buffer to the Guide's device the moment that
  // device exists. This needs its own thread: the first attempt lived in the
  // composite-draw path (far too late - vtable[20] runs during DC
  // CONSTRUCTION) and the second lived in XuiCtxWatch, which never starts
  // unless XENIA_XUICTX_WATCH is set in the environment, so it silently never
  // ran. A flag should not depend on an unrelated env var to take effect.
  if (cvars::guide_borrow_front_buffer) {
    auto* lmem = memory();
    std::thread([lmem]() {
      xe::threading::set_name("GuideLendFB");
      // Every read here must be guarded. This thread starts before the title
      // does, and the chain walks pointers read out of guest memory, so an
      // unmapped or garbage address is normal rather than exceptional -
      // TranslateVirtual happily returns a host pointer into reserved-but-
      // uncommitted space and the load faults the HOST process. The first
      // version had no guard and took the emulator down in GuideLendFB before
      // the title started.
      auto rdv = [lmem](uint32_t a) -> uint32_t {
                      if (a >= 0x81000000u && a < 0x82000000u &&
                          !XamConstOk(a, 4)) {
                        return uint32_t(0);
                      }
        if (a < 0x1000u || (a & 3u)) return 0u;
        auto* heap = lmem->LookupHeap(a);
        if (!heap) return 0u;
        // QueryProtect is the right check for pointers read out of guest
        // memory, but it rejects 0x801E6FC4 - VdGlobalDevice, a fixed global
        // in xam's loaded image that the draw path reads unguarded every
        // frame. That false negative is what made the poller sit for a whole
        // run with tdev=00000000 while the device it wanted was plainly
        // there. Module/kernel image addresses are mapped once xam is loaded,
        // and LookupHeap already established the address is backed, so allow
        // them through when QueryProtect declines to answer.
        uint32_t protect = 0;
        if (!heap->QueryProtect(a, &protect) || !protect) {
          if (a < 0x80000000u || a >= 0x90000000u) return 0u;
        }
        auto* host = lmem->TranslateVirtual(a);
        return host ? xe::load_and_swap<uint32_t>(host) : 0u;
      };
      for (int i = 0; i < 400000; ++i) {
        uint32_t ctx = rdv(0x81D6C978u);
        uint32_t wrap = rdv(ctx + 0x08u);
        uint32_t gdev = rdv(wrap + 0x0Cu);
        // Watch [wrapper+0x0C]. With [dc+0x134]=0 and no breakpoints, the
        // guest faults at 819DE94C with r3=0, and r3 traces back through
        // 819DEA70's callee-saved r29 to `lwz r3,12(r31)` in 8191AFD0 - i.e.
        // that slot is null at DC construction. It cannot be observed with a
        // cpu::Breakpoint: hitting one on the title thread wedges it (see the
        // correction above), so watch it from here instead, where the only
        // cost is a read.
        static uint32_t last_gdev = 0;
        if (gdev) last_gdev = gdev;
        if (wrap && !rdv(wrap + 0x0Cu) && last_gdev) {
          static int nulls = 0;
          if (++nulls <= 6) {
            XELOGW("GuideLendFB: [wrapper {:08X} +0C] is NULL at iteration {} "
                   "(last known device {:08X})",
                   wrap, i, last_gdev);
          }
          if (cvars::guide_repair_wrapper_device) {
            if (auto* ws = lmem->TranslateVirtual(wrap + 0x0Cu)) {
              xe::store_and_swap<uint32_t>(ws, last_gdev);
            }
          }
        }
        uint32_t tdev = rdv(0x801E6FC4u);
        // VdGlobalDevice holds garbage until dash publishes its device: the
        // first poll read tdev=FFCAE000, and lending from that produced a
        // front buffer of 98409940 instead of dash's A240A380. Racing ahead
        // of the title is a real hazard for any early poller - require the
        // pointer to look like a guest heap object before trusting it.
        if (tdev < 0x40000000u || tdev >= 0x50000000u) tdev = 0;
        uint32_t fb = rdv(tdev + 0x3F74u);
        // The previous run polled the whole time without the condition ever
        // becoming true, while GuidePreDraw showed the device plainly present
        // at 40870D00 - so one of these links is null and guessing which
        // would repeat the mistake this file keeps recording. Print them.
        // GuidePreDraw reaches the device through the DC
        // ([[dc+0x1CC]+0x0C]), NOT through the ctx global, so the ctx->+08
        // chain is the part most likely to be wrong.
        if ((i % 4000) == 0 && i < 160000) {
          XELOGI(
              "GuideLendFB[{}]: ctx={:08X} wrap={:08X} gdev={:08X} "
              "gdev[3F74]={:08X} tdev={:08X} tdev[3F74]={:08X}",
              i, ctx, wrap, gdev, rdv(gdev + 0x3F74u), tdev, fb);
        }
        // Keep the field in SYNC rather than snapshotting once. The first
        // version lent whatever VdGlobalDevice pointed at ~60ms in
        // (98409940) and never revisited it; dash publishes 40952400 /
        // A240A380 later. That did not matter while the present was gated
        // off, but with guide_patch_present_gate the real present actually
        // dereferences the buffer - and faulted at 98409940+0x20. A stale
        // pointer only becomes a bug once something follows it.
        // Validate the VALUE, not just the source. Syncing blindly walked
        // A240A380 -> 01C001C0 -> 03C003C0 -> 06000600 -> 08C00900: packed
        // width/height pairs (0x01C0=448, 0x03C0=960), not pointers.
        // VdGlobalDevice does not always point at a device, so [+0x3F74] is
        // not always a front buffer. Real buffers observed are 98409940 and
        // A240A380 - both above 0x80000000.
        if (fb < 0x80000000u) fb = 0;
        uint32_t cur_fb = rdv(gdev + 0x3F74u);
        if (gdev && fb && cur_fb != fb) {
          auto* slot = lmem->TranslateVirtual(gdev + 0x3F74u);
          if (!slot) continue;
          xe::store_and_swap<uint32_t>(slot, fb);
          static int lends = 0;
          if (++lends <= 6) {
            XELOGI(
                "Guide: lent front buffer {:08X} (was {:08X}) to guide device "
                "{:08X} (GuideLendFB, iteration {})",
                fb, cur_fb, gdev, i);
          }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(250));
      }
      XELOGW("GuideLendFB: gave up; never saw a guide device with a null "
             "front buffer while the title had one");
    }).detach();
    XELOGI("GuideLendFB: watching for the guide device");
  }

  // Phase 540: capture the presented frame to a file so the "is the Guide
  // visible" question can be answered here instead of handed to a person.
  // Xenia's own screenshot path lives on EmulatorWindow, but the capture it
  // uses - Presenter::CaptureGuestOutput - is reachable from the graphics
  // system, and RawImage is plain R8G8B8X8, so the frame can be dumped raw and
  // turned into a PNG afterwards.
  if (cvars::guide_capture_seconds > 0 || cvars::guide_capture_on_burst) {
    int cap_delay = cvars::guide_capture_seconds;
    std::thread([this, cap_delay]() {
      xe::threading::set_name("GuideCapture");
      // Phase 997: the burst emits draws on exactly one frame (996), and this
      // thread has always slept a fixed number of seconds - so every capture
      // in this investigation has photographed a frame the Guide did not draw
      // in. Wait for the event instead, when asked to.
      if (cvars::guide_capture_on_burst) {
        uint32_t waited_ms = 0;
        while (xe::gpu::g_guide_bursts_drawn.load(std::memory_order_acquire) ==
                   0 &&
               waited_ms < 120000) {
          xe::threading::Sleep(std::chrono::milliseconds(2));
          waited_ms += 2;
        }
        XELOGI("GuideCapture: burst-drawn seen after {} ms ({} burst(s))",
               waited_ms,
               xe::gpu::g_guide_bursts_drawn.load(std::memory_order_acquire));
      } else {
        xe::threading::Sleep(std::chrono::seconds(cap_delay));
      }
      auto* gs = graphics_system();
      auto* presenter = gs ? gs->presenter() : nullptr;
      if (!presenter) {
        XELOGW("GuideCapture: no presenter");
        return;
      }
      // Phase 719: N consecutive frames, not one. A single capture cannot
      // distinguish content that never appears from content that appears in
      // the buffer the display is not currently scanning out.
      const int shots = std::max(1, int(cvars::guide_capture_count));
      for (int shot = 0; shot < shots; ++shot) {
        xe::ui::RawImage image;
        if (!presenter->CaptureGuestOutput(image)) {
          XELOGW("GuideCapture: CaptureGuestOutput failed at shot {}", shot);
          return;
        }
        auto path = xe::filesystem::GetExecutableFolder() /
                    fmt::format("guide_capture_{}.raw", shot);
        FILE* f = xe::filesystem::OpenFile(path, "wb");
        if (!f) {
          XELOGW("GuideCapture: cannot open {}", xe::path_to_utf8(path));
          return;
        }
        uint32_t hdr[3] = {image.width, image.height,
                           static_cast<uint32_t>(image.stride)};
        fwrite(hdr, sizeof(hdr), 1, f);
        fwrite(image.data.data(), 1, image.data.size(), f);
        fclose(f);
        XELOGI("GuideCapture: shot {} wrote {}x{} to {}", shot, image.width,
               image.height, xe::path_to_utf8(path));
        if (shot + 1 < shots) {
          xe::threading::Sleep(std::chrono::milliseconds(
              std::max(1, int(cvars::guide_capture_interval_ms))));
        }
      }
    }).detach();
    XELOGI("GuideCapture: armed for {}s", cap_delay);
  }

  if (!cvars::guide_script.empty()) {
    // Phase 1054 tabs: the test plan runs inside the emulator - keystrokes go
    // straight into the Guide's input poll and frames come from the presenter
    // - so a run needs neither the window in the foreground nor the keyboard
    // (research/guidetest.ps1 raised the window and typed with keybd_event,
    // which typed into whatever the user had in front, and the user's typing
    // landed in xenia).
    std::thread([this]() {
      xe::threading::set_name("GuideScript");
      uint32_t waited = 0;
      while (kernel::xboxkrnl::GuideShowCount() == 0 && waited < 600000) {
        xe::threading::Sleep(std::chrono::milliseconds(5));
        waited += 5;
      }
      if (kernel::xboxkrnl::GuideShowCount() == 0) {
        XELOGW("GuideScript: no show in {} ms; plan skipped", waited);
        return;
      }
      auto* gs = graphics_system();
      auto* presenter = gs ? gs->presenter() : nullptr;
      const auto t0 = std::chrono::steady_clock::now();
      auto ms_since = [&]() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      };
      XELOGI("GuideScript: first show after {} ms; plan: {}", waited, cvars::guide_script);
      auto write_raw = [&](const std::string& name, const xe::ui::RawImage& image) {
        auto path = xe::filesystem::GetExecutableFolder() /
                    fmt::format("{}_{}.raw", cvars::guide_script_tag, name);
        FILE* fp = xe::filesystem::OpenFile(path, "wb");
        if (!fp) {
          XELOGW("GuideScript: cannot open {}", xe::path_to_utf8(path));
          return;
        }
        uint32_t hdr[3] = {image.width, image.height, static_cast<uint32_t>(image.stride)};
        fwrite(hdr, sizeof(hdr), 1, fp);
        fwrite(image.data.data(), 1, image.data.size(), fp);
        fclose(fp);
      };
      auto shot = [&](const std::string& name) {
        xe::ui::RawImage image;
        if (!presenter || !presenter->CaptureGuestOutput(image)) {
          XELOGW("GuideScript: capture failed for {}", name);
          return;
        }
        write_raw(name, image);
        XELOGI("GuideScript: shot {} at +{:.0f} ms", name, ms_since());
      };
      static const std::pair<const char*, uint16_t> kKeys[] = {
          {"up", 0x5810}, {"down", 0x5811}, {"left", 0x5812}, {"right", 0x5813},
          {"a", 0x5800},  {"b", 0x5801},    {"x", 0x5802},    {"y", 0x5803},
          {"guide", 0x5838}, {"start", 0x5814}, {"back", 0x5815}, {"lb", 0x5805}, {"rb", 0x5804}};
      std::vector<std::string> steps;
      {
        std::string cur;
        for (char ch : cvars::guide_script) {
          if (ch == ',') {
            if (!cur.empty()) steps.push_back(cur);
            cur.clear();
          } else {
            cur.push_back(ch);
          }
        }
        if (!cur.empty()) steps.push_back(cur);
      }
      for (const auto& step : steps) {
        std::vector<std::string> parts;
        {
          std::string cur;
          for (char ch : step) {
            if (ch == ':') {
              parts.push_back(cur);
              cur.clear();
            } else {
              cur.push_back(ch);
            }
          }
          parts.push_back(cur);
        }
        const std::string& verb = parts[0];
        auto num = [&](size_t i, int def) {
          return (i < parts.size() && !parts[i].empty()) ? std::atoi(parts[i].c_str()) : def;
        };
        if (verb == "wait") {
          xe::threading::Sleep(std::chrono::milliseconds(num(1, 0)));
        } else if (verb == "key" && parts.size() >= 2) {
          uint16_t vk = 0;
          for (const auto& k : kKeys) {
            if (parts[1] == k.first) vk = k.second;
          }
          if (!vk) {
            XELOGW("GuideScript: unknown key {}", parts[1]);
            continue;
          }
          int hold = num(2, 60);
          XELOGI("GuideScript: key {} ({:04X}) at +{:.0f} ms", parts[1], vk, ms_since());
          if (vk == 0x5838) {
            // the Guide button: the swap-time toggle's edge (the keystroke
            // poll only runs while the Guide is up, so a queued key could
            // close it but never reopen it)
            kernel::xboxkrnl::GuideScriptGuideButton();
          } else {
            kernel::xboxkrnl::GuideScriptPushKey(vk, 1);
            xe::threading::Sleep(std::chrono::milliseconds(hold));
            kernel::xboxkrnl::GuideScriptPushKey(vk, 2);
          }
        } else if (verb == "shot" && parts.size() >= 2) {
          shot(parts[1]);
        } else if (verb == "film" && parts.size() >= 4) {
          int count = num(2, 10), interval = num(3, 33);
          const auto f0 = std::chrono::steady_clock::now();
          std::vector<std::pair<std::string, xe::ui::RawImage>> frames;
          frames.reserve(size_t(std::max(count, 1)));
          for (int i = 0; i < count; ++i) {
            auto target = f0 + std::chrono::milliseconds(i * interval);
            auto now = std::chrono::steady_clock::now();
            if (target > now) {
              xe::threading::Sleep(std::chrono::duration_cast<std::chrono::milliseconds>(target - now));
            }
            double el = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
            xe::ui::RawImage image;
            if (!presenter || !presenter->CaptureGuestOutput(image)) {
              XELOGW("GuideScript: capture failed in film {}", parts[1]);
              break;
            }
            frames.emplace_back(fmt::format("{}_{:02d}_{}ms", parts[1], i, int(el)), std::move(image));
          }
          for (auto& fr : frames) write_raw(fr.first, fr.second);
          XELOGI("GuideScript: film {} {} frames at +{:.0f} ms", parts[1], frames.size(), ms_since());
        } else if (verb == "watch") {
          XELOGI("GuideScript: watch at +{:.0f} ms", ms_since());
          kernel::xboxkrnl::GuideScriptWatch();
        } else if (verb == "dump") {
          XELOGI("GuideScript: dump at +{:.0f} ms", ms_since());
          kernel::xboxkrnl::GuideScriptDump();
        } else if (verb == "quit") {
          // Phase 1055 bugs: a clean exit through the window (the runner
          // otherwise kills the process), so the shutdown path is exercised.
          XELOGI("GuideScript: quit at +{:.0f} ms", ms_since());
          if (display_window_) {
            display_window_->app_context().CallInUIThread([this]() { display_window_->RequestClose(); });
          }
          break;
        } else {
          XELOGW("GuideScript: unknown step {}", step);
        }
      }
      XELOGI("GuideScript: plan done at +{:.0f} ms", ms_since());
    }).detach();
    XELOGI("GuideScript: armed");
  }

  // Phase 1099d: GIVE xam A CONTROLLER, THROUGH ITS OWN API.
  //
  // hud polls XamInputGetKeystrokeHud 1008 times a run and xam answers
  // ERROR_DEVICE_NOT_CONNECTED (0x48F) to every one, because its input stack
  // looks for a USB device at [81D3C728 + user*4] and Xenia emulates no USB for
  // it to enumerate. The Guide therefore draws but cannot be navigated.
  //
  // xam already has the answer built in. XAutomationpBindController (ordinal
  // 0x3D5) marks a user's slot present, and XAutomationpInputSetState (0x3D9)
  // copies a 12-byte X_INPUT_GAMEPAD into xam's own table - the automation
  // path the console's own test tooling uses. So the host hands over button
  // state and xam does every other part itself: no device object is fabricated
  // here, no address is hardcoded (both are resolved from xam's export table by
  // ordinal), and nothing is written into guest memory except the gamepad
  // struct the guest asked for.
  if (cvars::guide_automation_input) {
    std::thread([this]() {
      xe::threading::set_name("GuideAutomationInput");
      auto* ks = kernel_state_.get();
      // Wait for xam to be loaded and for its exports to resolve.
      uint32_t bind = 0, setstate = 0, press = 0;
      for (int i = 0; i < 600 && !setstate; ++i) {
        xe::threading::Sleep(std::chrono::milliseconds(100));
        auto xm = ks->GetModule("xam.xex", true);
        if (!xm) continue;
        bind = xm->GetProcAddressByOrdinal(0x3D5);
        setstate = xm->GetProcAddressByOrdinal(0x3D9);
        press = xm->GetProcAddressByOrdinal(0x3D8);
      }
      if (!bind || !setstate) {
        XELOGW("GuideInput: xam does not export XAutomationpBindController "
               "(0x3D5)/XAutomationpInputSetState (0x3D9) - bind={:08X} "
               "set={:08X}",
               bind, setstate);
        return;
      }
      XELOGI("GuideInput: BindController={:08X} InputSetState={:08X} "
             "InputPress={:08X}",
             bind, setstate, press);

      // Phase 1099d: xam is loaded BEFORE the title (lle_xam), so resolving its
      // exports says nothing about whether the kernel is ready to host a guest
      // thread. Creating the pump at that point faulted before the thread body
      // ever ran. Wait for the executable module to exist, and give its own
      // start-up a moment, before creating anything.
      for (int i = 0; i < 900; ++i) {
        if (ks->GetExecutableModule()) break;
        xe::threading::Sleep(std::chrono::milliseconds(100));
      }
      if (!ks->GetExecutableModule()) {
        XELOGW("GuideInput: no executable module - not arming the pump");
        return;
      }
      xe::threading::Sleep(std::chrono::seconds(8));
      XELOGI("GuideInput: title is up, creating the pump");

      // Parse the optional script once, before touching the guest.
      struct Step { uint32_t delay_ms; uint16_t buttons; std::string name; };
      std::vector<Step> script;
      {
        // Phase 1099p: "@<path>" reads the script from a file (a recording).
        std::string sc = cvars::guide_input_script;
        if (!sc.empty() && sc[0] == '@') {
          std::string body;
          if (FILE* sf = xe::filesystem::OpenFile(sc.substr(1), "rb")) {
            char chunk[4096];
            size_t n;
            while ((n = fread(chunk, 1, sizeof(chunk), sf)) > 0) {
              body.append(chunk, n);
            }
            fclose(sf);
          } else {
            XELOGW("GuideInput: cannot read script file {}", sc.substr(1));
          }
          sc = body;
        }
        size_t i = 0;
        while (i < sc.size()) {
          size_t comma = sc.find(',', i);
          if (comma == std::string::npos) comma = sc.size();
          std::string item = sc.substr(i, comma - i);
          i = comma + 1;
          size_t colon = item.find(':');
          if (colon == std::string::npos) continue;
          uint32_t d = uint32_t(std::strtoul(item.substr(0, colon).c_str(),
                                             nullptr, 10));
          std::string b = item.substr(colon + 1);
          // Phase 1099e: these are XInput VK_PAD_* virtual keys, not gamepad
          // bitmask bits. XAutomationpInputPress takes a CODE and maps it
          // through xam's own table at 81612AF0 (codes 0x5800..0x5837), then
          // posts a real input event - which is what actually produces a
          // keystroke. Setting the gamepad bitmask with InputSetState marks the
          // pad present but generates no keystroke, so the menu never moved.
          // Phase 1099p: names (and raw "0x5824" hex) come from the shared
          // kGuideVkNames table, which the recorder writes with - so every
          // press a recording contains can be replayed. "guide" (0xFFFF) is
          // xam's automation Guide button; "hostguide" (0xFFFE) is the host's
          // Guide-button path, which a physical/keyboard press takes.
          const uint16_t bits = GuideVkFromName(b);
          if (!bits) {
            XELOGW("GuideInput: unknown button '{}' in guide_input_script", b);
            continue;
          }
          script.push_back({d, bits, b});
        }
        if (!script.empty()) {
          XELOGI("GuideInput: script has {} step(s)", script.size());
        }
      }

      auto t = kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          ks, 256 * 1024, 0,
          [this, ks, bind, setstate, press, script]() -> int {
            auto* ts = kernel::XThread::GetCurrentThread()->thread_state();
            XELOGI("GuideInput: pump thread entered");
            GuideRecordStart();
            const uint32_t pad = ks->memory()->SystemHeapAlloc(16);
            XELOGI("GuideInput: gamepad struct at {:08X}", pad);
            if (!pad) {
              XELOGE("GuideInput: could not allocate the gamepad struct");
              return 0;
            }
            uint64_t ba[] = {0};
            uint64_t br =
                ks->processor()->Execute(ts, bind, ba, xe::countof(ba));
            XELOGI("GuideInput: BindController(0) -> {:08X}",
                   static_cast<uint32_t>(br));

            auto push = [&](const hid::X_INPUT_GAMEPAD& src) {
              auto* gp = ks->memory()->TranslateVirtual<hid::X_INPUT_GAMEPAD*>(
                  pad);
              *gp = src;
              uint64_t sa[] = {0, pad};
              ks->processor()->Execute(ts, setstate, sa, xe::countof(sa));
            };

            // Phase 1099k: a REAL pad only ever reached xam through push()
            // (InputSetState), which 1099d/e measured as moving nothing - it
            // marks buttons held but posts no event, so no keystroke. The
            // script path works because it calls InputPress. Do the same for
            // the real pad: on each NEWLY pressed button, InputPress its XInput
            // VK_PAD code (xam maps it through its own table at 81612AF0).
            // Triggers and the left stick use XInput's documented thresholds
            // (XINPUT_GAMEPAD_TRIGGER_THRESHOLD 30, LEFT_THUMB_DEADZONE 7849).
            struct PadCode { uint32_t bit; uint16_t vk; const char* name; };
            static const PadCode kPadCodes[] = {
                {0x1000, 0x5800, "a"},      {0x2000, 0x5801, "b"},
                {0x4000, 0x5802, "x"},      {0x8000, 0x5803, "y"},
                {0x0200, 0x5804, "rb"},     {0x0100, 0x5805, "lb"},
                {0x10000, 0x5806, "lt"},    {0x20000, 0x5807, "rt"},
                {0x0001, 0x5810, "up"},     {0x0002, 0x5811, "down"},
                {0x0004, 0x5812, "left"},   {0x0008, 0x5813, "right"},
                {0x0010, 0x5814, "start"},  {0x0020, 0x5815, "back"},
                {0x0040, 0x5816, "ls"},     {0x0080, 0x5817, "rs"},
                {0x40000, 0x5820, "ls_up"}, {0x80000, 0x5821, "ls_down"},
                {0x100000, 0x5822, "ls_right"},
                {0x200000, 0x5823, "ls_left"}};
            auto virtual_bits = [](const hid::X_INPUT_GAMEPAD& g) -> uint32_t {
              uint32_t v = uint16_t(g.buttons);
              if (g.left_trigger > 30) v |= 0x10000;
              if (g.right_trigger > 30) v |= 0x20000;
              const int lx = int16_t(g.thumb_lx), ly = int16_t(g.thumb_ly);
              if (ly > 7849) v |= 0x40000;
              if (ly < -7849) v |= 0x80000;
              if (lx > 7849) v |= 0x100000;
              if (lx < -7849) v |= 0x200000;
              return v;
            };
            uint32_t prev_bits = 0;

            // Phase 1099k: guide_input_fake_pad stands in for the DEVICE READ
            // only ("delay_ms:hexbits,..." held for guide_input_hold_ms), so
            // the real-pad path below can be verified without a person at the
            // controller. Everything after the read is the real path.
            struct FakeStep { uint32_t delay_ms; uint16_t bits; };
            std::vector<FakeStep> fake;
            {
              const std::string& fs = cvars::guide_input_fake_pad;
              size_t i = 0;
              while (i < fs.size()) {
                size_t comma = fs.find(',', i);
                if (comma == std::string::npos) comma = fs.size();
                std::string item = fs.substr(i, comma - i);
                i = comma + 1;
                size_t colon = item.find(':');
                if (colon == std::string::npos) continue;
                fake.push_back(
                    {uint32_t(std::strtoul(item.substr(0, colon).c_str(),
                                           nullptr, 10)),
                     uint16_t(std::strtoul(item.substr(colon + 1).c_str(),
                                           nullptr, 16))});
              }
            }
            size_t fake_step = 0;
            uint32_t fake_since = 0, fake_hold_left = 0;
            uint16_t fake_bits = 0;

            // Run the script first, if there is one, so a verification run is
            // deterministic; then fall through to pumping the real pad.
            size_t step = 0;
            const uint32_t hold =
                uint32_t(std::max(16, int(cvars::guide_input_hold_ms)));
            uint32_t since = 0;
            int64_t since_origin = 0;
            uint16_t held = 0;
            uint32_t held_left = 0;
            while (!guide_input_stop_.load(std::memory_order_acquire)) {
              uint16_t buttons = 0;
              if (step < script.size()) {
                // Phase 1099p: real clock, so a recorded session replays at the
                // speed it was played (16 ms loop ticks drift slower).
                const auto now_ms = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (!since) since_origin = now_ms;
                since = uint32_t(std::max<int64_t>(1, now_ms - since_origin));
                if (since >= script[step].delay_ms) {
                  if (script[step].buttons == 0xFFFE) {
                    XELOGI("GuideInput: host Guide-button path");
                    on_guide_button_pressed(0);
                  } else if (press) {
                    uint64_t pa[] = {0, script[step].buttons};
                    uint64_t pr = ks->processor()->Execute(ts, press, pa,
                                                           xe::countof(pa));
                    XELOGI("GuideInput: InputPress('{}' = {:04X}) -> {:08X}",
                           script[step].name, script[step].buttons,
                           static_cast<uint32_t>(pr));
                  }
                  ++step;
                  since = 0;
                }
                xe::threading::Sleep(std::chrono::milliseconds(16));
                continue;
              }
              // Real controller, routed through the same guest API.
              hid::X_INPUT_GAMEPAD gpad = {};
              if (!fake.empty()) {
                if (fake_hold_left) {
                  fake_hold_left = fake_hold_left > 16 ? fake_hold_left - 16 : 0;
                  if (!fake_hold_left) fake_bits = 0;
                } else if (fake_step < fake.size()) {
                  fake_since += 16;
                  if (fake_since >= fake[fake_step].delay_ms) {
                    fake_bits = fake[fake_step].bits;
                    fake_hold_left = hold;
                    fake_since = 0;
                    ++fake_step;
                    XELOGI("GuideInput: fake pad read {:04X}", fake_bits);
                  }
                }
                gpad.buttons = fake_bits;
              } else if (auto* is = cvars::guide_input_host_off
                                           ? nullptr
                                           : input_system()) {
                hid::X_INPUT_STATE st = {};
                // Phase 1099l: the flags argument SELECTS drivers - InputSystem
                // keeps a driver only if (flags & its InputType) != 0, and the
                // types are Controller 1 / Keyboard 2 / Other 4. Passing 0 (as
                // this did) selected no driver at all, so every read returned
                // DEVICE_NOT_CONNECTED and the pad was read as all zeros - which
                // is exactly what "pumped N times, buttons 0000" showed while
                // the user was pressing buttons.
                const X_RESULT rr = is->GetState(
                    0,
                    hid::InputType::Controller | hid::InputType::Keyboard |
                        hid::InputType::Other,
                    &st);
                static X_RESULT last_rr = 0xFFFFFFFF;
                if (rr != last_rr) {
                  last_rr = rr;
                  XELOGI("GuideInput: controller read for user 0 -> {:08X} ({})",
                         rr, rr == X_ERROR_SUCCESS ? "connected" : "no device");
                }
                if (rr == X_ERROR_SUCCESS) {
                  gpad = st.gamepad;
                }
              }
              const uint32_t bits = virtual_bits(gpad);
              const uint32_t pressed = bits & ~prev_bits;
              prev_bits = bits;
              // The Guide button: same host handler the keyboard key and the
              // ImGui path use (it opens via the kernel callback and closes via
              // xam's automation button). Kept out of the held state so xam
              // never sees it twice.
              if (pressed & hid::X_INPUT_GAMEPAD_GUIDE) {
                XELOGI("GuideInput: pad Guide button");
                on_guide_button_pressed(0);
              }
              gpad.buttons = uint16_t(gpad.buttons) &
                             uint16_t(~hid::X_INPUT_GAMEPAD_GUIDE);
              push(gpad);
              // Phase 1099m: for the REAL device, presses come from the host's
              // own keystroke queue (XInputGetKeystroke via InputSystem), not
              // from thresholding the state above. Measured in the user's hands
              // run (hands1099l): the threshold version fired ls_right 23 /
              // ls_up 23 / ls_left 18 / ls_down 12 - an off-axis stick crossed
              // two thresholds and wobble re-crossed them. XInputGetKeystroke
              // already turns a stick into ONE dominant or diagonal VK_PAD code
              // (0x5820..0x5827) and supplies held-key REPEAT, so nothing about
              // direction, deadzone or repeat timing is decided here.
              if (fake.empty() && !cvars::guide_input_host_off) {
                if (auto* is = input_system(); is && press) {
                  hid::X_INPUT_KEYSTROKE k = {};
                  for (int drain = 0; drain < 16; ++drain) {
                    if (is->GetKeystroke(0,
                                         hid::InputType::Controller |
                                             hid::InputType::Keyboard |
                                             hid::InputType::Other,
                                         &k) != X_ERROR_SUCCESS) {
                      break;
                    }
                    const uint16_t vk = uint16_t(k.virtual_key);
                    const uint16_t fl = uint16_t(k.flags);
                    if (vk < 0x5800 || vk > 0x5837) continue;
                    if (!(fl & (hid::X_INPUT_KEYSTROKE_KEYDOWN |
                                hid::X_INPUT_KEYSTROKE_REPEAT))) {
                      continue;
                    }
                    GuideRecordPress(vk);
                    uint64_t pa[] = {0, vk};
                    uint64_t pr = ks->processor()->Execute(ts, press, pa,
                                                           xe::countof(pa));
                    static uint32_t klog = 0;
                    if (klog++ < 400) {
                      XELOGI("GuideInput: keystroke {:04X} flags {:X} -> "
                             "InputPress -> {:08X}",
                             vk, fl, static_cast<uint32_t>(pr));
                    }
                  }
                }
              }
              if (!fake.empty() && pressed && press) {
                for (const auto& pc : kPadCodes) {
                  if (!(pressed & pc.bit)) continue;
                  uint64_t pa[] = {0, pc.vk};
                  uint64_t pr =
                      ks->processor()->Execute(ts, press, pa, xe::countof(pa));
                  static uint32_t plog = 0;
                  if (plog++ < 200) {
                    XELOGI("GuideInput: pad '{}' -> InputPress({:04X}) -> {:08X}",
                           pc.name, pc.vk, static_cast<uint32_t>(pr));
                  }
                }
              }
              buttons = uint16_t(gpad.buttons);
              static uint32_t pn = 0;
              if ((pn++ % 300) == 0) {
                XELOGI("GuideInput: pumped {} times, buttons {:04X}", pn,
                       buttons);
              }
              xe::threading::Sleep(std::chrono::milliseconds(16));
            }
            return 0;
          }));
      t->set_name("GuideInputPump");
      if (XFAILED(t->Create())) {
        XELOGE("GuideInput: could not create the pump thread");
        return;
      }
      t->Wait(0, 0, 0, nullptr);
    }).detach();
    XELOGI("GuideInput: automation input armed");
  }

  // Phase 1099n: research probe. Dump xam's storage-device table (20 slots of
  // 0xA0 at 81D3E1A8, layout decoded from 81731188: +0x00 "<name>\", +0x40 type
  // string, +0x78 device id, +0x7C kind, +0x80 state; state 3 = usable) at
  // guide_storage_dump_seconds and again 15 s later.
  if (cvars::guide_storage_dump_seconds > 0) {
    int delay = cvars::guide_storage_dump_seconds;
    std::thread([this, delay]() {
      xe::threading::set_name("GuideStorageDump");
      for (int pass = 0; pass < 2; ++pass) {
        xe::threading::Sleep(std::chrono::seconds(pass ? 15 : delay));
        auto* mem = memory();
        if (!mem) return;
        const uint8_t* t = mem->TranslateVirtual<const uint8_t*>(0x81D3E1A8u);
        XELOGI("GuideStorageDump pass {}: count [81D3E1A4]={:08X}", pass,
               xe::load_and_swap<uint32_t>(
                   mem->TranslateVirtual<const uint8_t*>(0x81D3E1A4u)));
        for (uint32_t i = 0; i < 20; ++i) {
          const uint8_t* e = t + i * 0xA0;
          if (!e[0]) continue;
          std::string name(reinterpret_cast<const char*>(e),
                           strnlen(reinterpret_cast<const char*>(e), 0x40));
          std::string type(reinterpret_cast<const char*>(e + 0x40),
                           strnlen(reinterpret_cast<const char*>(e + 0x40), 0x38));
          XELOGI("GuideStorageDump   slot {}: '{}' type '{}' id={:08X} kind={:08X} "
                 "state={:08X}",
                 i, name, type, xe::load_and_swap<uint32_t>(e + 0x78),
                 xe::load_and_swap<uint32_t>(e + 0x7C),
                 xe::load_and_swap<uint32_t>(e + 0x80));
        }
      }
    }).detach();
  }

  if (cvars::guide_auto_press_seconds > 0) {
    int delay = cvars::guide_auto_press_seconds;
    int again = cvars::guide_auto_press_again_seconds;
    std::thread([this, delay, again]() {
      xe::threading::set_name("GuideAutoPress");
      xe::threading::Sleep(std::chrono::seconds(delay));
      XELOGI("Guide button: auto-press firing after {}s", delay);
      on_guide_button_pressed(0);
      if (again > 0) {
        xe::threading::Sleep(std::chrono::seconds(again));
        XELOGI("Guide button: second auto-press firing after {}s more", again);
        on_guide_button_pressed(0);
      }
    }).detach();
    XELOGI("Guide button: auto-press armed for {}s", delay);
  }
  // Arm the thread probe here rather than only from the Guide button path.
  // The button handler is gated on a handler that a freeze during hud load
  // never publishes, so a probe armed there can never fire on the very hang
  // it exists to diagnose.
  if (cvars::guide_probe_threads_seconds > 0) {
    ArmGuideThreadProbe(kernel_state_.get(),
                        cvars::guide_probe_threads_seconds);
  }
  if (!cvars::guide_test_tray_seconds.empty()) {
    // TEST-ONLY (HOST-SIDE): the eject button, pressed at two times.
    auto* ks = kernel_state_.get();
    int open_s = -1, close_s = -1;
    std::sscanf(cvars::guide_test_tray_seconds.c_str(), "%d,%d", &open_s,
                &close_s);
    std::thread([ks, open_s, close_s]() {
      xe::threading::set_name("TestTray");
      const auto t0 = std::chrono::steady_clock::now();
      auto at = [&](int s, bool open) {
        if (s < 0) return;
        std::this_thread::sleep_until(t0 + std::chrono::seconds(s));
        XELOGI("TestTray: {} the tray", open ? "opening" : "closing");
        ks->smc()->MoveTray(open);
      };
      at(open_s, true);
      at(close_s, false);
    }).detach();
  }
  if (cvars::guide_test_notify_seconds > 0) {
    // DIAGNOSTIC (HOST-SIDE, not console behaviour): N seconds after start,
    // call the real xam.xex's XNotifyQueueUI (ordinal 0x290) on a guest
    // thread, so the notification toast path can be verified in an automated
    // run without a game unlocking an achievement.
    // XNotifyQueueUI(type, user index, areas (64-bit), text, reserved).
    auto* ks = kernel_state_.get();
    const int delay = cvars::guide_test_notify_seconds;
    std::thread([ks, delay]() {
      xe::threading::set_name("TestNotify");
      std::this_thread::sleep_for(std::chrono::seconds(delay));
      auto real_xam = ks->GetModule("xam.xex", true);
      const uint32_t fn =
          real_xam ? real_xam->GetProcAddressByOrdinal(0x290) : 0;
      if (!fn) {
        XELOGW("TestNotify: real xam XNotifyQueueUI not found");
        return;
      }
      static const char16_t kText[] = u"Toast test from the emulator";
      const uint32_t text_len = uint32_t(xe::countof(kText));
      const uint32_t text_ptr = ks->memory()->SystemHeapAlloc(text_len * 2);
      for (uint32_t i = 0; i < text_len; ++i) {
        xe::store_and_swap<uint16_t>(
            ks->memory()->TranslateVirtual(text_ptr + i * 2),
            uint16_t(kText[i]));
      }
      const uint32_t first_type = uint32_t(cvars::guide_test_notify_type);
      auto t = kernel::object_ref<kernel::XHostThread>(
          new kernel::XHostThread(ks, 256 * 1024, 0,
                                  [ks, fn, text_ptr, first_type]() -> int {
            auto* ts = kernel::XThread::GetCurrentThread()->thread_state();
            // Try argument combinations until xam accepts one (0).
            const uint32_t types[] = {first_type, 12, 0, 1};
            const uint32_t users[] = {0xFF, 0};
            const uint64_t areas[] = {0xFFFFFFFFFFFFFFFFull, 0};
            for (uint32_t type : types) {
              for (uint32_t user : users) {
                for (uint64_t area : areas) {
                  uint64_t a[] = {type, user, area, text_ptr, 0};
                  uint64_t r =
                      ks->processor()->Execute(ts, fn, a, xe::countof(a));
                  XELOGI("TestNotify: XNotifyQueueUI({:08X}) type {} user {:X} "
                         "areas {:X} -> {:08X}",
                         fn, type, user, area, uint32_t(r));
                  if (uint32_t(r) == 0) return 0;
                }
              }
            }
            return 0;
          }));
      t->set_name("TestNotify");
      t->Create();
    }).detach();
  }
  if (cvars::kernel_probe_on_request) {
    // DIAGNOSTIC: take the thread probe (probe.txt) on demand. Creating a file
    // named probe_now next to the exe triggers it ~2 s later (the probe's
    // thread cache needs one refresh); the file is deleted so it can be
    // requested again. For hangs whose timing can't be predicted at launch.
    auto* ks = kernel_state_.get();
    std::thread([ks]() {
      xe::threading::set_name("ProbeOnRequest");
      const auto trigger = xe::filesystem::GetExecutableFolder() / "probe_now";
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::error_code ec;
        if (std::filesystem::exists(trigger, ec)) {
          std::filesystem::remove(trigger, ec);
          XELOGI("ProbeOnRequest: probe_now found, probing in 2 s");
          ArmGuideThreadProbe(ks, 2);
        }
      }
    }).detach();
  }
  if (cvars::guide_trace_signin) {
    InstallGuideSigninTraces(kernel_state_.get());
  }
  if (cvars::guide_trace_transitions) {
    InstallGuideTransitionTraces(kernel_state_.get());
  }
  if (cvars::guide_game_library_cover_art) {
    InstallCoverArtHooks(kernel_state_.get());
  }
  if (cvars::guide_trace_dash_lua) {
    // Guest hooks apply to code translated afterwards: add them before the
    // title module loads.
    InstallGuideLuaTraces(kernel_state_.get());
  } else if (!cvars::trace_dash_lua.empty()) {
    uint32_t pcs[3] = {};
    if (std::sscanf(cvars::trace_dash_lua.c_str(), "%x,%x,%x", &pcs[0],
                    &pcs[1], &pcs[2]) == 3) {
      InstallGuideLuaTraces(kernel_state_.get(), pcs[0], pcs[1], pcs[2], true);
    } else {
      XELOGW("trace_dash_lua: expected throw,sleep,yield hex pcs");
    }
  }
  if (cvars::guide_probe_threads_at_terminate > 0 &&
      !kernel_state_->title_terminate_hook) {
    // Phase 1099z52: probe N seconds after the Mth title termination
    // (--guide_probe_threads_at_terminate_index, default 2: the first is
    // usually the dash leaving for the game). 0 = every termination.
    auto* ks = kernel_state_.get();
    auto count = std::make_shared<int>(0);
    kernel_state_->title_terminate_hook = [ks, count]() {
      const int32_t want = cvars::guide_probe_threads_at_terminate_index;
      if (++*count == want || want == 0) {
        ArmGuideThreadProbe(ks, cvars::guide_probe_threads_at_terminate);
      }
    };
  }
  // Phase 1099z168: FABLE III POST-SAVE GUEST CRASH AT 821DBAEC.
  //
  // Disassembled (image dumped by XexTool, base 82000000):
  //   821DBAE0  the crashing function: lwz r11,4(r3) with r3 = 0x3E8
  //   82258F84  addi r3, r30, 0x3e8 ; bl 821DBAE0     -> r30 is NULL
  //   821BC5C0  mr r11,r4 / mr r4,r3 / mr r3,r11 / b 82258F00  (arg-swap thunk,
  //             vtable slot +0x34 of the draw item) -> 82258F00(scene, item)
  //   821F1900  bctrl vtable[+0x34](item=r29, scene=r27)       -> r27 is NULL
  //   82302FDC / 82303030  lwzx r5, idx*4, [8355F454]          -> the NULL
  // So the render-view slot the draw names is empty. Slots are handed out by
  // 82C4F1A8, which on a failed 0x5A0 `new` takes the branch at 82C4F228,
  // prints the title's own wide string "Out Of Memory allocating %u in %s"
  // (82077 3E8) and stores NULL in the slot anyway. These hooks say which of
  // those it is. RESEARCH ONLY - they read registers and log, nothing else.
  if (cvars::guide_trace_fable_views) {
    enum class FV {
      kAllocRet,
      kOom,
      kDraw,
      kSetId,
      kNewB,
      kStoreB,
      kMove,
      kStoreA,
      kDestroy,
      kBindEnter,
      kBindPick,
      kThreadCmp
    };
    struct FVSite {
      uint32_t addr, word;
      FV kind;
      const char* name;
    };
    static const FVSite kFV[] = {
        // Fable III retail default.xex, load address 82000000.
        {0x82C4F334u, 0x7F83E378u, FV::kAllocRet, "AllocView"},
        {0x82C4F228u, 0x3D608207u, FV::kOom, "AllocView-OOM"},
        {0x82302FE0u, 0x4BEEE7D1u, FV::kDraw, "Draw(mode2)"},
        {0x82303034u, 0x4BEEE77Du, FV::kDraw, "Draw(mode1)"},
        {0x82300C24u, 0x93DF0000u, FV::kSetId, "SetViewId"},
        // 822A21A8 is a SECOND creator for the same slots (reached from
        // 82C4FDE0, the per-draw "which thread am I" helper). It allocates the
        // same 0x5A0 object and, like 82C4F1A8, publishes NULL into the slot if
        // that allocation fails.
        {0x822A2E70u, 0x2B030000u, FV::kNewB, "NewB-alloc"},
        {0x822A2F00u, 0x7F9DF12Eu, FV::kStoreB, "StoreB"},
        // The vector's own move loop clears a destination slot before writing
        // it; it runs whenever the vector is resized.
        {0x82C52DBCu, 0x937F0000u, FV::kMove, "VectorMove"},
        {0x82C4F2C0u, 0x7FBEF92Eu, FV::kStoreA, "StoreA"},
        // 82C4F358 is the registry's DestroyView(id): it releases the object,
        // stores NULL in the slot (82C4F4B0/82C4F4B8) and pushes the id back on
        // the free list. Nothing else empties a slot.
        {0x82C4F358u, 0x7D8802A6u, FV::kDestroy, "DestroyView"},
        // 82C4FCE8 runs between the draw's "read the view id" and its "read the
        // registry slot" (82303004 lwz r30,0(r31) / 82303008 bl / 82303030
        // lwzx). It binds the calling thread to a scratch view and, through
        // 822A21A8, can DESTROY one first.
        {0x82C4FD0Cu, 0x7F035840u, FV::kThreadCmp, "ThreadCmp"},
        {0x822A21A8u, 0x7D8802A6u, FV::kBindEnter, "Bind"},
        {0x822A2FF8u, 0x7C68482Eu, FV::kBindPick, "BindPick"},
    };
    static std::atomic<uint32_t> fv_counts[std::size(kFV)] = {};
    auto* fvmem = memory_.get();
    auto* fvks = kernel_state_.get();
    for (size_t i = 0; i < std::size(kFV); ++i) {
      const FVSite s = kFV[i];
      processor_->AddGuestHook(s.addr, [fvmem, fvks, s,
                                        i](cpu::ppc::PPCContext* c) {
        // The addresses are Fable-specific: refuse to report anything if the
        // code at the site is not the instruction they were read from.
        const uint8_t* ip = fvmem->TranslateVirtual(s.addr);
        if (!ip || xe::load_and_swap<uint32_t>(ip) != s.word) return;
        const uint32_t n = ++fv_counts[i];
        switch (s.kind) {
          case FV::kAllocRet: {
            const uint32_t id = static_cast<uint32_t>(c->r[28]);
            const uint32_t obj = static_cast<uint32_t>(c->r[29]);
            if (obj == 0 || n <= 64) {
              XELOGI("FableView: {} #{} -> id {} object {:08X}{}", s.name, n, id,
                     obj, obj ? "" : "  <-- NULL SLOT");
            }
            break;
          }
          case FV::kOom:
            XELOGE(
                "FableView: {} #{} - the title's own allocator returned NULL "
                "for 0x5A0 bytes; the slot it is about to publish will be "
                "NULL (lr {:08X})",
                s.name, n, static_cast<uint32_t>(c->lr));
            break;
          case FV::kDraw: {
            // r5 = the slot contents, r30 = the slot id. Only the empty ones
            // matter; this runs several times per frame.
            if (static_cast<uint32_t>(c->r[5]) != 0) break;
            const uint32_t id = static_cast<uint32_t>(c->r[30]);
            const uint32_t base = static_cast<uint32_t>(c->r[11]);
            // The registry's own header (8355F450: +4 begin, +8 end) and the
            // slots either side, so a zeroed neighbourhood (a fresh buffer) is
            // told apart from one cleared slot.
            std::string around;
            const uint8_t* hdr = fvmem->TranslateVirtual(0x8355F450u);
            const uint32_t begin =
                hdr ? xe::load_and_swap<uint32_t>(hdr + 4) : 0u;
            const uint32_t end = hdr ? xe::load_and_swap<uint32_t>(hdr + 8) : 0u;
            const uint32_t count =
                (end > begin) ? std::min<uint32_t>((end - begin) / 4u, 512u) : 0;
            uint32_t nonzero = 0;
            for (uint32_t sl = 0; sl < count; ++sl) {
              const uint8_t* p = fvmem->TranslateVirtual(begin + sl * 4u);
              const uint32_t v = p ? xe::load_and_swap<uint32_t>(p) : 0u;
              if (!v) continue;
              ++nonzero;
              if (nonzero <= 24) around += fmt::format(" [{}]={:08X}", sl, v);
            }
            around += fmt::format("  ({} of {} slots occupied)", nonzero, count);
            XELOGE(
                "FableView: {} #{} with EMPTY slot id {} (vector {:08X}) - this "
                "is the 821DBAEC crash; header begin {:08X} end {:08X};{}",
                s.name, n, id, base, begin, end, around);
            break;
          }
          case FV::kStoreA: {
            const uint32_t id = static_cast<uint32_t>(c->r[30]) >> 2;
            const uint32_t obj = static_cast<uint32_t>(c->r[29]);
            const uint32_t base = static_cast<uint32_t>(c->r[31]);
            XELOGI("FableView: {} #{} slot {} <- {:08X} (vector {:08X}){}",
                   s.name, n, id, obj, base,
                   obj ? "" : "  <-- CLEARS THE SLOT");
            // From the second publish on, protect the page the slot lives on
            // and report the GUEST PC of everything that writes it. The slot
            // written here reads back as zero ~9 s later and no registry writer
            // clears it, so the culprit is not in this registry at all.
            // --guide_watch_write_addr=0xFFFFFFFF asks for it; the page
            // protection perturbs timing, so it is not on by default.
            if (n >= 2 && cvars::guide_watch_write_addr == 0xFFFFFFFFu) {
              ArmGuideWriteWatch(fvks, base + id * 4u);
            }
            break;
          }
          case FV::kSetId:
            if (n <= 64) {
              XELOGI("FableView: {} #{} object {:08X} id {}", s.name, n,
                     static_cast<uint32_t>(c->r[31]),
                     static_cast<uint32_t>(c->r[30]));
            }
            break;
          case FV::kNewB: {
            const uint32_t p = static_cast<uint32_t>(c->r[3]);
            if (p == 0) {
              XELOGE(
                  "FableView: {} #{} - operator new(0x5A0) returned NULL; the "
                  "slot this publishes will be NULL",
                  s.name, n);
            } else if (n <= 64) {
              XELOGI("FableView: {} #{} -> {:08X}", s.name, n, p);
            }
            break;
          }
          case FV::kStoreB: {
            const uint32_t obj = static_cast<uint32_t>(c->r[28]);
            const uint32_t id = static_cast<uint32_t>(c->r[29]) >> 2;
            if (obj == 0 || n <= 64) {
              XELOGI("FableView: {} #{} slot {} <- {:08X} (vector {:08X}){}",
                     s.name, n, id, obj, static_cast<uint32_t>(c->r[30]),
                     obj ? "" : "  <-- CLEARS THE SLOT");
            }
            break;
          }
          case FV::kDestroy:
            XELOGI("FableView: {} #{} id {} lr {:08X}", s.name, n,
                   static_cast<uint32_t>(c->r[3]),
                   static_cast<uint32_t>(c->lr));
            break;
          case FV::kThreadCmp:
            if (n <= 8) {
              XELOGI(
                  "FableView: {} #{} this thread id {:08X} vs the registered "
                  "one {:08X} ({})",
                  s.name, n, static_cast<uint32_t>(c->r[3]),
                  static_cast<uint32_t>(c->r[11]),
                  static_cast<uint32_t>(c->r[3]) ==
                          static_cast<uint32_t>(c->r[11])
                      ? "same - early out"
                      : "DIFFERENT - takes the bind path");
            }
            break;
          case FV::kBindEnter:
            if (n <= 24) {
              XELOGI("FableView: {} #{} r3 {:08X} r4 {:08X} r5 {:08X} lr {:08X}",
                     s.name, n, static_cast<uint32_t>(c->r[3]),
                     static_cast<uint32_t>(c->r[4]),
                     static_cast<uint32_t>(c->r[5]),
                     static_cast<uint32_t>(c->lr));
            }
            break;
          case FV::kBindPick: {
            // lwzx r3, r8, r9 has not run yet: read the same word here, so the
            // id this is about to tear down is on record.
            const uint32_t at = static_cast<uint32_t>(c->r[9]) +
                                static_cast<uint32_t>(c->r[8]);
            const uint8_t* p = fvmem->TranslateVirtual(at);
            const uint32_t picked = p ? xe::load_and_swap<uint32_t>(p) : 0u;
            if (n <= 24 || picked >= 100u) {
              XELOGI(
                  "FableView: {} #{} record {:08X} ids@{:08X} slot*4 {:08X} -> "
                  "id {}",
                  s.name, n, static_cast<uint32_t>(c->r[31]),
                  static_cast<uint32_t>(c->r[9]),
                  static_cast<uint32_t>(c->r[8]), int32_t(picked));
            }
            break;
          }
          case FV::kMove:
            if (n <= 64) {
              XELOGI("FableView: {} #{} at {:08X}", s.name, n,
                     static_cast<uint32_t>(c->r[31]));
            }
            break;
        }
      });
      XELOGI("FableView: arming {:08X} {}", s.addr, s.name);
    }
  }
  // Phase 1099z165: WHERE DO xam's LIVE AUTH EXPORTS LAND?
  //
  // All seven exist as real guest code in 17559 (Xenia implements none of them,
  // and with blank lle_xam_scope every importer binds to real xam, so the
  // guest's own implementations are what run). Resolved from xam's export table
  // at 81A73D34. Hooks must be registered BEFORE the title module loads, and
  // only affect code translated afterwards - see the guest-hooks note.
  if (cvars::guide_trace_live_auth) {
    struct LiveSite {
      uint32_t addr;
      const char* name;
    };
    static const LiveSite kLive[] = {
        {0x81689AE0u, "XamIsCSVDrainageSupported"},
        {0x8168EB50u, "XamSetKerbTimeouts"},
        {0x8168EB88u, "XamGetKerbTimeouts"},
        {0x8168EBC0u, "XamSetMacsTimeouts"},
        {0x8168EBF8u, "XamGetMacsTimeouts"},
        {0x816DEF20u, "XamProfileHasWindowsLiveCredentials"},
        {0x816FE538u, "XamGetTLSConfig"},
        // POSITIVE CONTROLS. These two are called constantly on any boot. If
        // they do not fire, the hooks did not install and a zero for the seven
        // above says nothing at all.
        {0x816DD118u, "XamUserGetSigninState [CONTROL]"},
        {0x816F14E8u, "XamInputGetState [CONTROL]"},
    };
    static std::atomic<uint32_t> live_counts[std::size(kLive)] = {};
    auto* lmem = memory_.get();
    for (size_t i = 0; i < std::size(kLive); ++i) {
      const LiveSite s = kLive[i];
      // Record what is actually AT each address, so a wrong build shows up as
      // nonsense rather than as a quiet zero.
      const uint8_t* ip = lmem->TranslateVirtual(s.addr);
      XELOGI("LiveAuth: arming {:08X} {} (first word {:08X})", s.addr, s.name,
             ip ? xe::load_and_swap<uint32_t>(ip) : 0u);
      processor_->AddGuestHook(s.addr, [s, i](cpu::ppc::PPCContext* c) {
        const uint32_t n = ++live_counts[i];
        if (n <= 4 || (n % 500) == 0) {
          XELOGI("LiveAuth: {} #{} lr={:08X} r3={:08X} r4={:08X} r5={:08X}",
                 s.name, n, static_cast<uint32_t>(c->lr),
                 static_cast<uint32_t>(c->r[3]), static_cast<uint32_t>(c->r[4]),
                 static_cast<uint32_t>(c->r[5]));
        }
      });
    }
    XELOGI("LiveAuth: {} hooks armed", std::size(kLive));
  }

  XELOGI("Loading module {}", module_path);
  auto module = kernel_state_->LoadUserModule(module_path);
  // xam code that was correct right after xam loaded reads back as zero later
  // in the run. Bracket the title load, which is the largest thing that
  // happens in between.
  ReportXamTextPopulation(memory(), "after title load");
  // Phase 1096gw: same flat VA-indexed dump guide_dump_xam_path makes for xam,
  // for the title. dash.xex's HdDvdRom retry loop is four frames above the
  // call site (back chain 92262090 9226234C 922623CC 92198B0C) and there is no
  // image of it to disassemble; reading it six words at a time through the xam
  // probe list costs a rebuild per address.
  if (module && !cvars::guide_dump_title_path.empty() &&
      module->xex_module()) {
    auto* tm = memory();
    const uint32_t tbase = module->xex_module()->base_address();
    const uint32_t tsize = module->xex_module()->image_size();
    FILE* tf = std::fopen(cvars::guide_dump_title_path.c_str(), "wb");
    if (!tf) {
      XELOGW("DumpTitle: could not open {}", cvars::guide_dump_title_path);
    } else {
      static const uint8_t kTitleZero[0x1000] = {0};
      uint32_t mapped = 0, holes = 0;
      for (uint32_t off = 0; off < tsize; off += 0x1000u) {
        const uint32_t a = tbase + off;
        const uint32_t n = std::min<uint32_t>(0x1000u, tsize - off);
        auto* hp = tm ? tm->LookupHeap(a) : nullptr;
        const bool ok = hp && hp->QueryRangeAccess(a, a + n - 1) !=
                                  xe::memory::PageAccess::kNoAccess;
        if (ok) {
          std::fwrite(tm->TranslateVirtual(a), 1, n, tf);
          ++mapped;
        } else {
          std::fwrite(kTitleZero, 1, n, tf);
          ++holes;
        }
      }
      std::fclose(tf);
      XELOGI("DumpTitle: {:08X}+{:08X} -> {} ({} pages mapped, {} zero-filled)",
             tbase, tsize, cvars::guide_dump_title_path, mapped, holes);
    }
  }
  if (!module) {
    XELOGE("Failed to load user module {}", path);
    return X_STATUS_NOT_FOUND;
  }

  if (!module->is_executable()) {
    if (!cvars::allow_dll_module_launch) {
      kernel_state_->UnloadUserModule(module, false);
      XELOGE("Failed to load user module {}", path);
      return X_STATUS_NOT_SUPPORTED;
    }
    // System DLL modules (hud.xex, xam.xex, ...) are normally loaded by xam
    // rather than booted. Allow launching them directly so their imports can
    // be resolved and reported.
    XELOGW("Launching non-executable (DLL) module {}", path);
  }

  X_RESULT result = kernel_state_->ApplyTitleUpdate(module);
  if (XFAILED(result)) {
    XELOGE("Failed to apply title update! Cannot run module {}", path);
    return result;
  }

  result = kernel_state_->FinishLoadingUserModule(module);
  if (XFAILED(result)) {
    XELOGE("Failed to initialize user module {}", path);
    return result;
  }
  // Grab the current title ID.
  xex2_opt_execution_info* info = nullptr;
  uint32_t workspace_address = 0;
  module->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &info);

  // The title workspace (/XEXWORKSPACE) is addressed by the guest as the
  // region immediately following the module image, not as an arbitrary
  // allocation. xam derives its heap base from the image end, so placing this
  // anywhere else leaves xam's heaps uninitialized and every xam allocation
  // fails. Try the fixed address first and only fall back to a floating
  // allocation if that range is unavailable.
  if (module->xex_module()) {
    workspace_address = xe::round_up(
        module->xex_module()->base_address() + module->xex_module()->image_size(),
        0x1000);
    auto* heap = kernel_state_->memory()->LookupHeap(workspace_address);
    if (!heap ||
        !heap->AllocFixed(workspace_address, module->workspace_size(), 0x1000,
                          kMemoryAllocationReserve | kMemoryAllocationCommit,
                          kMemoryProtectRead | kMemoryProtectWrite)) {
      XELOGW("Title workspace: could not reserve {} bytes at {:08X}",
             module->workspace_size(), workspace_address);
      workspace_address = 0;
    } else {
      XELOGI("Title workspace: {:08X}-{:08X} ({} bytes)", workspace_address,
             workspace_address + module->workspace_size(),
             module->workspace_size());
    }
  }
  if (!workspace_address) {
    kernel_state_->memory()
        ->LookupHeapByType(false, 0x1000)
        ->Alloc(module->workspace_size(), 0x1000,
                kMemoryAllocationReserve | kMemoryAllocationCommit,
                kMemoryProtectRead | kMemoryProtectWrite, false,
                &workspace_address);
  }

  if (!info) {
    title_id_ = 0;
  } else {
    title_id_ = info->title_id;
    auto title_version = info->version();
    if (title_version.value != 0) {
      title_version_ = format_version(title_version);
    }
  }

  // Try and load the resource database (xex only).
  if (module->title_id()) {
    auto title_id = fmt::format("{:08X}", module->title_id());

    // Load the per-game configuration file and make sure updates are handled
    // by the callbacks.
    config::LoadGameConfig(title_id);
    assert_true(game_config_load_callback_loop_next_index_ == SIZE_MAX);
    game_config_load_callback_loop_next_index_ = 0;
    while (game_config_load_callback_loop_next_index_ <
           game_config_load_callbacks_.size()) {
      game_config_load_callbacks_[game_config_load_callback_loop_next_index_++]
          ->PostGameConfigLoad();
    }
    game_config_load_callback_loop_next_index_ = SIZE_MAX;

    const auto db = kernel_state_->module_xdbf(module);

    game_info_database_ =
        std::make_unique<kernel::util::GameInfoDatabase>(db.get());
    kernel_state_->xam_state()->LoadSpaInfo(db.get());

    kernel_state_->xam_state()->user_tracker()->AddTitleToPlayedList();

    if (game_info_database_->IsValid()) {
      title_name_ = game_info_database_->GetTitleName(static_cast<XLanguage>(
          kernel_state_->xconfig()->ReadSetting<uint32_t>(
              kernel::XCONFIG_USER_CATEGORY, kernel::XCONFIG_USER_LANGUAGE)));
      XELOGI("Title name: {}", title_name_);

      // Show achievments data
      tabulate::Table table;
      table.format().multi_byte_characters(true);
      table.add_row({"ID", "Title", "Description", "Type", "Gamerscore"});

      const std::vector<kernel::util::GameInfoDatabase::Achievement>
          achievement_list = game_info_database_->GetAchievements();
      for (const kernel::util::GameInfoDatabase::Achievement& entry :
           achievement_list) {
        const std::string type = GetAchievementTypeName(
            kernel::xam::GetAchievementType(entry.flags));

        table.add_row({fmt::format("{}", entry.id), entry.label,
                       entry.description, type,
                       fmt::format("{}", entry.gamerscore)});
      }
      XELOGI("\n-------------------- ACHIEVEMENTS --------------------\n{}",
             table.str());

      const std::vector<kernel::util::GameInfoDatabase::Property>
          properties_list = game_info_database_->GetProperties();

      // 4D5307DC SPA contains a lot of properties, limit properties to log.
      const auto properties_list_limit =
          properties_list | std::views::take(150);

      table = tabulate::Table();
      table.format().multi_byte_characters(true);
      table.add_row({"ID", "Name", "Matchmaking", "Data Size"});

      for (const kernel::util::GameInfoDatabase::Property& entry :
           properties_list_limit) {
        std::string label =
            string_util::remove_eol(string_util::trim(entry.description));

        table.add_row({fmt::format("{:08X}", entry.id), label,
                       entry.is_matchmaking ? "True" : "False",
                       fmt::format("{}", entry.data_size)});
      }

      std::string properties_totals;

      if (properties_list.size() > properties_list_limit.size()) {
        properties_totals =
            fmt::format("\nProperties: {}/{}", properties_list_limit.size(),
                        properties_list.size());
      }

      XELOGI("\n-------------------- PROPERTIES --------------------{}\n{}",
             properties_totals.c_str(), table.str());

      const std::vector<kernel::util::GameInfoDatabase::Context> contexts_list =
          game_info_database_->GetContexts();

      table = tabulate::Table();
      table.format().multi_byte_characters(true);
      table.add_row(
          {"ID", "Name", "Matchmaking", "Default Value", "Max Value"});

      for (const kernel::util::GameInfoDatabase::Context& entry :
           contexts_list) {
        std::string label =
            string_util::remove_eol(string_util::trim(entry.description));

        table.add_row({fmt::format("{:08X}", entry.id), label,
                       entry.is_matchmaking ? "True" : "False",
                       fmt::format("{}", entry.default_value),
                       fmt::format("{}", entry.max_value)});
      }
      XELOGI("\n-------------------- CONTEXTS --------------------\n{}",
             table.str());

      const std::vector<kernel::util::GameInfoDatabase::StatsView> stats_views =
          game_info_database_->GetStatsViews();

      // 4D5307EA SPA contains a lot of stats, limit views to log.
      const auto stats_views_limit = stats_views | std::views::take(100);

      table = tabulate::Table();
      table.format().multi_byte_characters(true);
      table.add_row({"ID", "View Type", "Name", "Skilled", "Arbitrated",
                     "Hidden", "Team View", "Online Only"});

      for (const kernel::util::GameInfoDatabase::StatsView& entry :
           stats_views_limit) {
        const std::string name =
            string_util::remove_eol(string_util::trim(entry.view.name));

        const std::string view_type =
            kernel::xam::GetViewTypeName(entry.view.view_type);

        table.add_row({fmt::format("{:08X}", entry.view.id), view_type, name,
                       entry.view.skilled ? "True" : "False",
                       entry.view.arbitrated ? "True" : "False",
                       entry.view.hidden ? "True" : "False",
                       entry.view.team_view ? "True" : "False",
                       entry.view.online_only ? "True" : "False"});
      }

      std::string stats_view_totals;

      if (stats_views.size() > stats_views_limit.size()) {
        stats_view_totals = fmt::format(
            "\nViews: {}/{}", stats_views_limit.size(), stats_views.size());
      }
      XELOGI("\n-------------------- STATS VIEWS --------------------{}\n{}",
             stats_view_totals.c_str(), table.str());

      const std::vector<kernel::util::GameInfoDatabase::PresenceMode>
          presence_modes = game_info_database_->GetPresenceModes();

      table = tabulate::Table();
      table.format().multi_byte_characters(true);
      table.add_row({"Context Value", "Contexts Count", "Properties Count"});

      for (const kernel::util::GameInfoDatabase::PresenceMode& entry :
           presence_modes) {
        table.add_row(
            {fmt::format("{}", entry.context_value),
             fmt::format("{}", entry.property_bag.contexts.size()),
             fmt::format("{}", entry.property_bag.properties.size())});
      }
      XELOGI("\n-------------------- PRESENCE MODES --------------------\n{}",
             table.str());

      auto icon_block = game_info_database_->GetIcon();
      if (!icon_block.empty()) {
        display_window_->SetIcon(icon_block.data(), icon_block.size());
      }
    }
  }

  // Initialize shader storage asynchronously - pipeline compilation happens in
  // background while the game goes through its normal startup (loading screens,
  // intro videos, etc.). With async_shader_compilation enabled, draws are
  // skipped until pipelines are ready, so this is safe. By the time actual
  // gameplay starts, most cached pipelines should be compiled.
  if (graphics_system_) {
    on_shader_storage_initialization(true);
    graphics_system_->InitializeShaderStorage(
        cache_root_, title_id_.value(), false,
        [this]() { on_shader_storage_initialization(false); });
  }

  // If a real xam was loaded, its DllMain must run on a guest thread before
  // the title starts, otherwise the title spins waiting for an uninitialized
  // xam. Do this synchronously so xam is ready before the main thread exists.
  if (module->is_executable() && lle_xam_module_ &&
      lle_xam_module_->entry_point()) {
    auto* ks = kernel_state_.get();
    auto xam_mod = lle_xam_module_;
    // SetExecutableModule first: InitializeGuestObject acquires the title
    // process thread_list_spinlock, which is only valid once initialized.
    kernel_state_->SetExecutableModule(module);
    // Neutralise the heap selector's app-id trap (Ghidra 817BAE38, runtime
    // 817B3C38: xam .text is shifted by 0x7200 because Xenia maps the XEX
    // basefile flat while Ghidra honours PE raw offsets). The trap fires when
    // the current app id does not match the requested one; the value the
    // selector actually returns is derived from flag bits further down, so
    // branching onto the OK path yields what a matching context would have.
    // This must happen before DllMain runs, or the JIT may already have cached
    // the untranslated block.
    // 817BAE38 is the app-id mismatch trap; 817BADFC terminates the 20-entry
    // flag-mask table scan that runs before it. Both sit on the path to a real
    // heap id, so probe them together.
    for (uint32_t ghidra_trap :
         {0x817BAE38u, 0x817BADFCu}) {
      if (!cvars::lle_xam_heap_patch) {
        break;
      }
      const uint32_t trap_addr = ghidra_trap - 0x7200u;
      auto* p = memory()->TranslateVirtual(trap_addr);
      // Guest code pages are mapped read-only, so the store faults unless the
      // page is temporarily made writable first.
      auto* heap = memory()->LookupHeap(trap_addr);
      uint32_t old_protect = 0;
      bool unprotected =
          heap && heap->Protect(trap_addr, 4,
                                kMemoryProtectRead | kMemoryProtectWrite,
                                &old_protect);
      XELOGI("LLE xam: trap {:08X} was {:08X}, unprotect={}", ghidra_trap,
             xe::load_and_swap<uint32_t>(p), unprotected);
      if (unprotected) {
        xe::store_and_swap<uint32_t>(p, 0x48000004u);
        XELOGI("LLE xam: trap {:08X} now {:08X}", ghidra_trap,
               xe::load_and_swap<uint32_t>(p));
        heap->Protect(trap_addr, 4, old_protect, nullptr);
      } else {
        XELOGE("LLE xam: could not unprotect {:08X}", ghidra_trap);
      }
    }
    // xam selects its heap from the "current app id", which its getter
    // derives from KeGetCurrentProcessType when there is no per-thread app
    // context: SYSTEM (2) yields 0xFE, anything else 0xEE. On hardware xam
    // initialises inside the system process, so run its DllMain there or the
    // heap selector traps on an app-id mismatch.
    auto xam_boot =
        kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
            ks, 1024 * 1024, 0, [ks, xam_mod]() -> int {
              auto* ts = kernel::XThread::GetCurrentThread()->thread_state();
              // Phase 1096bl: create xam's D3D device BEFORE its DllMain, so
              // xam's own boot sees the device gate set.
              //
              // xam reaches its skin init from 81751220 only past
              // [[815F048C]] & 0x200 at 817511EC. The host's existing device
              // creation lives in the GUIDE BUTTON path, which runs long after
              // xam has booted, so xam reads that gate CLEAR, skips the init,
              // and 8177B0A8 can never allocate the skin-callback manager -
              // which is why the host had to hand [81D43C50+0x28] a stand-in
              // (1096bg/bk). Xenia's own comment says what hardware does:
              // 8178E9F0's mode-1 device is something "nothing inside xam ever
              // calls, so on hardware it comes from the system boot".
              //
              // So do what the system boot does, at the time it does it. This
              // is an ORDERING change, not a value: nothing is fabricated, xam
              // creates its own device with its own code.
              //
              // lle_xam_device_init was defined in kernel_flags.cc with its
              // implementation lost ("the declaration survived but the
              // definition did not") and referenced nowhere. This is that
              // definition. Defaults off.
              if (cvars::lle_xam_device_init) {
                uint8_t sv_pt = 0, sv_ptd = 0;
                auto* dcur = kernel::XThread::GetCurrentThread();
                if (dcur) {
                  auto* kt = dcur->guest_object<kernel::X_KTHREAD>();
                  sv_pt = kt->process_type;
                  sv_ptd = kt->process_type_dup;
                  // Mode 1 asserts unless it is running as SYSTEM: it checks
                  // VdGlobalXamDevice (empty) rather than VdGlobalDevice (the
                  // title's, already set).
                  kt->process_type = kernel::X_PROCTYPE_SYSTEM;
                  kt->process_type_dup = kernel::X_PROCTYPE_SYSTEM;
                }
                uint64_t dca[] = {0};
                XELOGI("LLE xam: creating xam's device BEFORE DllMain "
                       "(8178E9F0, mode 1 - the one the system boot makes)");
                uint64_t dcr = ks->processor()->Execute(
                    ts, kernel::xboxkrnl::GuideConst(0x8178E9F0u), dca,
                    xe::countof(dca));
                if (dcur) {
                  auto* kt = dcur->guest_object<kernel::X_KTHREAD>();
                  kt->process_type = sv_pt;
                  kt->process_type_dup = sv_ptd;
                }
                auto rdg = [&](uint32_t a) {
                  return xe::load_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(a));
                };
                const uint32_t gp = rdg(0x815F048Cu);
                const uint32_t gv = gp ? rdg(gp) : 0;
                XELOGI("LLE xam: early device creation returned {:08X} | gate "
                       "[815F048C]={:08X} [*]={:08X} bit200={}",
                       static_cast<uint32_t>(dcr), gp, gv,
                       (gv & 0x200) ? "SET" : "clear");
              }
              // Phase 1096br: xam is a SYSTEM module on hardware. Its own
              // allocator 8177B0A8 opens with
              //   bl KeGetCurrentProcessType ; cmpwi r3,2 ; bne -> return null
              // so it refuses to allocate for anything that is not
              // X_PROCTYPE_SYSTEM(2). That is why 8177BFC8 - the skin-callback
              // manager's constructor - returns null (1096bk) and why the host
              // had to substitute a stand-in at [81D43C50+0x28] (1096bg).
              // Tell the truth about which process this is and the guest builds
              // its own object; nothing is fabricated.
              uint8_t sv_ppt = 0, sv_pptd = 0;
              kernel::XThread* sysca = nullptr;
              if (cvars::lle_xam_system_process) {
                sysca = kernel::XThread::GetCurrentThread();
                if (sysca) {
                  auto* kt = sysca->guest_object<kernel::X_KTHREAD>();
                  sv_ppt = kt->process_type;
                  sv_pptd = kt->process_type_dup;
                  kt->process_type = kernel::X_PROCTYPE_SYSTEM;
                  kt->process_type_dup = kernel::X_PROCTYPE_SYSTEM;
                  XELOGI("LLE xam: boot thread process type {} -> SYSTEM for "
                         "DllMain (xam is a system module; 8177B0A8 refuses to "
                         "allocate otherwise)", sv_ppt);
                }
              }
              uint64_t args[] = {xam_mod->handle(), 1 /* PROCESS_ATTACH */, 0};
              XELOGI("LLE xam: DllMain entry={:08X}", xam_mod->entry_point());
              ks->processor()->Execute(ts, xam_mod->entry_point(), args,
                                       xe::countof(args));
              XELOGI("LLE xam: DllMain returned");
              if (sysca) {
                auto* kt = sysca->guest_object<kernel::X_KTHREAD>();
                kt->process_type = sv_ppt;
                kt->process_type_dup = sv_pptd;
              }
              GuideHudSlotMark(ks, "after xam DllMain");
              // xam's heap descriptors (array at 0x81D4E1B0, 10 x 408 bytes)
              // are still all-zero after DllMain: the "Unable to commit %d
              // bytes for heap." path never runs, so creation is not failing,
              // it is never attempted. 817BBD70 (runtime 817B4B70) is the
              // routine that builds them - it calls the single-heap creator
              // five times - and it has no callers inside xam, so on hardware
              // something outside the module drives it. Take one argument.
              if (cvars::lle_xam_heap_init) {
                uint64_t hargs[] = {0};
                XELOGI("LLE xam: calling heap init 817B4B70");
                ks->processor()->Execute(ts, kernel::xboxkrnl::GuideConst(0x817B4B70u), hargs,
                                         xe::countof(hargs));
                XELOGI("LLE xam: heap init returned");
              }
              // Same shape of gap, and the one that matters for rendering:
              // 81795548 is xam's skin loader. Its string constants are
              // "\SystemRoot\huduiskin.xex", L"skin.xur", L"skin" and
              // L"skin://", and it is the only route that reaches
              // XuiVisualRegister (via 8193D4B8). A demand-JIT trace shows it
              // never runs under our bootstrap, which is exactly why the
              // visual registry is empty and every control reports a null
              // visual (80300017) while laying out correctly. It has no
              // callers inside xam and takes no arguments, so drive it here.
              // Phase 1091r: THE VISUALS WITHOUT THE ORPHAN'S TAIL.
              // 81795548's useful work is done well before the point that
              // needs the fabricated manager. Reading it end to end:
              //   817956B8 r3 = 816080F4 (L"\SystemRoot\huduiskin.xex")
              //   817956B0 r4 = 0x40000008, r5 = 817AB9E8(), r6 = &handle
              //   817956C4 bl 81D0F42C            <- XexLoadImage
              //   817956EC bl 8178E340(handle, 81608850 L"skin",
              //                        81608B24 L"skin.xur", &buf, 0x80)
              //   817956F8 bl 8193D4B8(&buf, 0)   <- XuiVisualRegister(uri, 0)
              //   ... and only at 8179593C does it read [81D43C50+0x28] and
              //       register the skin-change callback, which is the part
              //       that faults without a stand-in (the harness's own
              //       comment says the visuals are already registered by then).
              // 81608274/81608298 confirm the URI shape: "memory://%.*ws" and
              // "section://%X,strtable#strings.xus".
              // So do the first half only, with EVERY value taken from xam -
              // its own path string, its own flags, its own format strings,
              // its own version call - and never reach the tail, so nothing
              // has to be fabricated. The two SystemHeapAllocs here are
              // out-parameter scratch, not stand-ins for guest objects.
              if (cvars::guide_skin_visuals_only) {
                auto* m = ks->memory();
                auto rd32 = [&](uint32_t a) {
                  return xe::load_and_swap<uint32_t>(m->TranslateVirtual(a));
                };
                uint32_t hbuf = m->SystemHeapAlloc(16, 16);
                uint32_t ubuf = m->SystemHeapAlloc(0x100, 16);
                if (!hbuf || !ubuf) {
                  XELOGW("SkinVisuals: could not allocate scratch - nothing done");
                } else {
                  std::memset(m->TranslateVirtual(hbuf), 0, 16);
                  std::memset(m->TranslateVirtual(ubuf), 0, 0x100);
                  // Phase 1091t: THE MISSING PIECE 1091r's 8000FFFF WAS.
                  // 8178DC58 installs xam's XUI PROVIDERS - no arguments,
                  // asserts the UI thread, and registers the object at
                  // 81D22A54 with its interfaces 81D22A58 / 81D22A5C
                  // (8178DD64 bl 8193F8E0, 8178DD78 bl 8196EF30). That is
                  // where 817923A0 - the visual registrar working off the LIVE
                  // [81D43C50+0x80] - actually lives. Its ONLY caller in the
                  // image is 81795638, inside the orphan skin loader. So the
                  // loader is not merely a skin loader: it is the
                  // initialisation for the whole XUI provider subsystem, and
                  // skipping it is exactly why XuiVisualRegister answered
                  // 8000FFFF last time. 817900C0 is the call immediately
                  // before it whose result the loader checks, so drive that
                  // first and do not proceed if it fails.
                  uint64_t pa[] = {0};
                  uint32_t pre = uint32_t(ks->processor()->Execute(
                      ts, kernel::xboxkrnl::GuideConst(0x817900C0u), pa, 1));
                  uint32_t prov = 0xDEADu;
                  if (int32_t(pre) >= 0) {
                    uint64_t da[] = {0};
                    prov = uint32_t(ks->processor()->Execute(
                        ts, kernel::xboxkrnl::GuideConst(0x8178DC58u), da, 1));
                  }
                  XELOGI("SkinVisuals: 817900C0 -> {:08X}; provider install "
                         "8178DC58 -> {:08X} (DEAD = skipped, the first call failed)",
                         pre, prov);
                  uint64_t va[] = {0};
                  uint32_t ver = uint32_t(ks->processor()->Execute(
                      ts, kernel::xboxkrnl::GuideConst(0x817AB9E8u), va, 1));
                  uint64_t la[] = {0x816080F4ull, 0x40000008ull, ver, hbuf};
                  uint32_t lr32 = uint32_t(ks->processor()->Execute(
                      ts, kernel::xboxkrnl::GuideConst(0x81D0F42Cu), la, 4));
                  uint32_t hmod = rd32(hbuf);
                  XELOGI("SkinVisuals: XexLoadImage(816080F4, 40000008, ver {:08X}) "
                         "-> {:08X}, module handle {:08X}", ver, lr32, hmod);
                  if (int32_t(lr32) < 0 || !hmod) {
                    XELOGW("SkinVisuals: the skin module did not load - stopping "
                           "here rather than registering anything");
                  } else {
                    uint64_t fa[] = {hmod, 0x81608850ull, 0x81608B24ull, ubuf, 0x80ull};
                    uint32_t fr = uint32_t(ks->processor()->Execute(
                        ts, kernel::xboxkrnl::GuideConst(0x8178E340u), fa, 5));
                    std::string uri;
                    for (uint32_t i = 0; i < 96; ++i) {
                      uint16_t u = xe::load_and_swap<uint16_t>(
                          m->TranslateVirtual(ubuf + i * 2u));
                      if (!u) break;
                      uri += (u >= 32 && u < 127) ? char(u) : '?';
                    }
                    uint64_t ra[] = {ubuf, 0};
                    uint32_t rr = uint32_t(ks->processor()->Execute(
                        ts, kernel::xboxkrnl::GuideConst(0x8193D4B8u), ra, 2));
                    XELOGI("SkinVisuals: 8178E340 -> {:08X}, uri \"{}\"; "
                           "XuiVisualRegister(8193D4B8) -> {:08X}", fr, uri, rr);
                  }
                }
              }
              if (cvars::lle_xam_skin_init) {
                // The loader's tail loads [81D43C50+0x28] and calls through
                // it to register a skin-change callback. Nothing in xam ever
                // writes that field - no store to it exists anywhere in
                // .text - so the call gets null and faults storing at +0x30,
                // *after* the visuals have already been registered. Give it a
                // zeroed block to write into so the loader can finish and
                // return, instead of dying two instructions from the end.
                //
                // This is deliberately a stand-in, not the real object: the
                // callee only stores the callback at +0x30/+0x34, and since
                // nothing else in xam creates or reads this manager, nothing
                // else can be misled by it.
                // Phase 1091m: ASK THE GUEST FOR IT FIRST. The stand-in below
                // is a host-authored object sitting in a guest field, and it
                // cost five phases (1091e-1091k) of reading OUR OWN block as
                // if it were xam's. xam has a constructor for what this slot
                // wants: 8177BFC8 takes no arguments, calls 8177B0A8 and
                // returns its result ORed with 1 - and that tag bit is exactly
                // what 8177BFB0 asserts on the value it reads out of the slot
                // (clrlwi. r3,0x1f / bne / twui), which a raw host pointer can
                // never satisfy. 8177B0A8 asserts KeGetCurrentProcessType == 2,
                // calls 81778CD8, then 8177B058 for the object, and addrefs it
                // at [obj+4].
                // NO SILENT FALLBACK (the 1089 rule): if the guest returns 0
                // the harness says so and leaves the slot alone rather than
                // putting a number of its own back.
                // Phase 1091n: WHY xam has no object to give - MEASURED, and
                // the first attempt at this probe is why it is a plain read now.
                // 8177BFC8 -> 8177B0A8 -> 8177B058, and 8177B058 is
                // `817795A8(0,&out); return out ? [out+0x0C] : 0` - the current
                // worker's current task ([worker+0x0C] is the slot 1074 named).
                // CALLING 817795A8 HERE FAULTS (run worker1: access violation at
                // guest PC 81779604): both it and 81778CD8 open by loading a TLS
                // INDEX from [81D227F0], asserting it is not -1, asserting
                // KeGetCurrentProcessType()==2, and then calling KeTlsGetValue
                // (81D0FEFC) and dereferencing the result. On this bootstrap
                // thread that slot is empty, the asserts are silenced by
                // ignore_trap_instructions, and the dereference is the crash.
                // So read the index instead of driving the chain.
                if (cvars::guide_bkgnd_watch) {
                  uint32_t tls_index = xe::load_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(0x81D227F0u));
                  XELOGI("LLE xam: xam's per-thread context is a TLS slot, index "
                         "[81D227F0] = {:08X}{}. 8177B058 reads it through "
                         "KeTlsGetValue and returns [worker+0x0C]; on this thread "
                         "there is no worker, so 8177BFC8 has no skin-callback "
                         "manager to return and the harness must stand one in.",
                         tls_index,
                         tls_index == 0xFFFFFFFFu ? " (-1: xam never allocated it)"
                                                  : "");
                }
                if (cvars::guide_hud_app_init) {
                  // Phase 1095d: the device gate at 817511EC guards FOUR calls,
                  // not one, and xam's own error strings name them:
                  //   817ABBA0(the media: and SystemRoot path strings)
                  //        815FB5D4 "Failed to create media symbolic link"
                  //   81795970  815FB5A8 "Failed to initialize UI Thread"
                  //   81BE5D18
                  //   8176C588
                  // Driving only 81795970 (1095c) left the other three undone
                  // and the skin loader faulted in a different place each run.
                  // Run the block in xam's own order and report each result.
                  auto rdw2 = [&](uint32_t a) {
                    return xe::load_and_swap<uint32_t>(
                        ks->memory()->TranslateVirtual(a));
                  };
                  // Phase 1095e: make this a thread xam owns, using xam's
                  // own installer, before running anything it gates on.
                  uint32_t xam_tls_block = 0;
                  if (cvars::guide_hud_app_init_tls) {
                    xam_tls_block = ks->memory()->SystemHeapAlloc(64, 16);
                    if (xam_tls_block) {
                      std::memset(
                          ks->memory()->TranslateVirtual(xam_tls_block), 0, 64);
                      uint64_t ba2[] = {xam_tls_block};
                      uint64_t br2 = ks->processor()->Execute(
                          ts, kernel::xboxkrnl::GuideConst(0x81778D38u), ba2, 1);
                      XELOGI("LLE xam: installed xam per-thread block {:08X} "
                             "via 81778D38 -> {:08X}; 8177BFC8 now -> {:08X}",
                             xam_tls_block, uint32_t(br2), [&] {
                               uint64_t qa[] = {0};
                               return uint32_t(ks->processor()->Execute(
                                   ts, kernel::xboxkrnl::GuideConst(0x8177BFC8u),
                                   qa, 1));
                             }());
                    }
                  }
                  {
                    uint64_t la[] = {0x815FB608ull, 0x815FB61Cull};
                    uint64_t lr2 = ks->processor()->Execute(
                        ts, kernel::xboxkrnl::GuideConst(0x817ABBA0u), la, 2);
                    XELOGI("LLE xam: media symlink 817ABBA0 -> {:08X}",
                           uint32_t(lr2));
                  }
                  uint32_t before = rdw2(0x81D43C78u);
                  uint64_t ia[] = {0};
                  uint64_t ir = ks->processor()->Execute(
                      ts, kernel::xboxkrnl::GuideConst(0x81795970u), ia, 1);
                  XELOGI("LLE xam: HUD app init 81795970 -> {:08X} | "
                         "[81D43C78] {:08X} -> {:08X} | [81D43C50] {:08X} | "
                         "[81D43684] {:08X} (8178F748 via xam boot 817517FC; "
                         "8178DCB4 passes it to 818FF2C8 and the skin loader "
                         "faults if the object behind it is unconstructed) | "
                         "[81D42970] {:08X} [81D42974] {:08X}",
                         uint32_t(ir), before, rdw2(0x81D43C78u),
                         rdw2(0x81D43C50u), rdw2(0x81D43684u),
                         rdw2(0x81D42970u), rdw2(0x81D42974u));
                  for (uint32_t fn : {0x81BE5D18u, 0x8176C588u}) {
                    uint64_t fa[] = {0};
                    uint64_t fr = ks->processor()->Execute(
                        ts, kernel::xboxkrnl::GuideConst(fn), fa, 1);
                    XELOGI("LLE xam: gated init {:08X} -> {:08X}", fn,
                           uint32_t(fr));
                  }
                  if (xam_tls_block) {
                    uint64_t ea[] = {0};
                    ks->processor()->Execute(
                        ts, kernel::xboxkrnl::GuideConst(0x81778DA8u), ea, 1);
                    XELOGI("LLE xam: cleared xam per-thread block (81778DA8)");
                  }
                  // Phase 1095f: 8177BF38 QUEUES the skin loader (tail call to
                  // 81779920), so 81795548 runs on a pool worker while this
                  // thread would otherwise carry straight on into device
                  // creation, XUI init and scene navigation - both sides
                  // touching XUI state, which is what a fault site that moves
                  // every run looks like. Wait on the loader's OWN completion
                  // flag: 81795948 writes [81D43C50+0xB4] = 1 at its tail.
                  if (cvars::guide_hud_app_init_wait_ms > 0) {
                    const auto t0 = std::chrono::steady_clock::now();
                    const auto deadline =
                        t0 + std::chrono::milliseconds(
                                 cvars::guide_hud_app_init_wait_ms);
                    uint32_t done = 0;
                    while (std::chrono::steady_clock::now() < deadline) {
                      done = rdw2(0x81D43C50u + 0xB4u);
                      if (done == 1u) break;
                      std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    const auto ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
                    if (done == 1u) {
                      XELOGI("LLE xam: skin loader 81795548 completed after "
                             "{} ms ([81D43C50+B4]=1, [+B8]={:08X})",
                             ms, rdw2(0x81D43C50u + 0xB8u));
                    } else {
                      XELOGE("GUIDE FAULT: skin loader 81795548 did NOT "
                             "complete in {} ms ([81D43C50+B4]={:08X}) - the "
                             "Guide bootstrap is about to race it",
                             ms, done);
                    }
                  }
                }
                uint32_t mgr = 0;
                if (cvars::guide_real_skin_mgr) {
                  uint64_t ma[] = {0};
                  uint32_t before = xe::load_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(0x81D43C78u));
                  uint64_t mr = ks->processor()->Execute(
                      ts, kernel::xboxkrnl::GuideConst(0x8177BFC8u), ma, 1);
                  mgr = uint32_t(mr);
                  XELOGI("LLE xam: skin callback manager - asked xam (8177BFC8)"
                         " -> {:08X} | tag bit {} | slot was {:08X}",
                         mgr, (mgr & 1u) ? "SET" : "CLEAR", before);
                  // 8177BFC8 ORs the tag bit in UNCONDITIONALLY (8177BFDC
                  // `ori r3, r3, 1` sits before the null test), so a failure
                  // returns 00000001, not 0. Testing truthiness stored that 1,
                  // 8177BFB0 cleared the tag back to 0, and 8177B2C0 wrote the
                  // procedure to [0+0x30] - an access violation at 8177B2F0,
                  // which is the exact fault 1079's XamAppLoad guard was
                  // written to avoid. The object is the value ABOVE the tag.
                  if (mgr & ~1u) {
                    xe::store_and_swap<uint32_t>(
                        ks->memory()->TranslateVirtual(0x81D43C78u), mgr);
                    XELOGI("LLE xam: skin callback manager is XAM'S OWN {:08X}",
                           mgr);
                  } else {
                    XELOGW("LLE xam: xam returned NO skin callback manager "
                           "(8177BFC8 -> {:08X}, i.e. 8177B0A8 gave null and only "
                           "the tag bit survives) - the slot is LEFT ALONE, no "
                           "stand-in substituted", mgr);
                  }
                } else if (uint32_t have = xe::load_and_swap<uint32_t>(
                               ks->memory()->TranslateVirtual(0x81D43C78u))) {
                  // Phase 1095f: the guest already put its OWN manager here
                  // (81795970 writes it - see guide_hud_app_init). Never
                  // overwrite it with a stand-in; that is the host authoring
                  // over the guest, and it silently undid the whole point of
                  // driving xam's init. 1095e also settled that asking
                  // 8177BFC8 is the wrong question - it is a PER-THREAD
                  // accessor, not a getter for this global slot.
                  mgr = have;
                  XELOGI("LLE xam: skin callback manager is XAM'S OWN {:08X} "
                         "(already in the slot - no stand-in)", mgr);
                } else {
                  mgr = ks->memory()->SystemHeapAlloc(0x100, 16);
                  if (mgr) {
                    std::memset(ks->memory()->TranslateVirtual(mgr), 0, 0x100);
                    xe::store_and_swap<uint32_t>(
                        ks->memory()->TranslateVirtual(0x81D43C78u), mgr);
                    XELOGI("LLE xam: skin callback manager stand-in at {:08X} "
                           "(HOST-AUTHORED - the slot was empty; run "
                           "guide_hud_app_init and the guest fills it itself)",
                           mgr);
                  }
                }
                uint64_t sargs[] = {0};
                // Phase 1054: XUIFONT::Init sizes the glyph atlas by point
                // size (81913A44: < 12 -> 128, < 64 -> 256, else 512). The
                // harness replays one paint's stream later, so any glyph cell
                // recycled within a paint shows as a missing or wrong letter.
                // The patch must land before 8178DE50 translates the
                // function (p1054e: patched after it, no effect). Words are
                // "li r11, N" immediates (0x39600000 | N).
                if (cvars::guide_font_atlas > 0) {
                  uint32_t fa = uint32_t(cvars::guide_font_atlas) & 0xFFFFu;
                  kernel::xboxkrnl::GuidePatchWord(0x81913A48u, 0x39600080u,
                                                   0x39600000u | fa,
                                                   "FontAtlas128");
                  kernel::xboxkrnl::GuidePatchWord(0x81913A58u, 0x39600100u,
                                                   0x39600000u | fa,
                                                   "FontAtlas256");
                  kernel::xboxkrnl::GuidePatchWord(0x81913A64u, 0x39600200u,
                                                   0x39600000u | fa,
                                                   "FontAtlas512");
                  // The two thresholds the compares read at run time (12.0
                  // and 64.0 points): -1.0 sends every size down the 512
                  // arm whether or not the code patch beat the translator.
                  kernel::xboxkrnl::GuidePatchWord(0x8163FA8Cu, 0x41400000u,
                                                   0xBF800000u,
                                                   "FontAtlasThresh12");
                  kernel::xboxkrnl::GuidePatchWord(0x8163FA88u, 0x42800000u,
                                                   0xBF800000u,
                                                   "FontAtlasThresh64");
                }
                if (cvars::lle_xam_font_init) {
                  uint64_t fargs[] = {0};
                  uint64_t fr = ks->processor()->Execute(
                      ts, kernel::xboxkrnl::GuideConst(0x8178DE50u), fargs, 0);
                  XELOGI("LLE xam: font subsystem init 8178DE50 -> {:08X}",
                         static_cast<uint32_t>(fr));
                }
                if (cvars::lle_xam_render_host_first) {
                  auto rdw = [&](uint32_t a) {
                    return a ? xe::load_and_swap<uint32_t>(
                                   ks->memory()->TranslateVirtual(a))
                             : 0u;
                  };
                  uint32_t rhost = kernel::xboxkrnl::XamRenderHost();
                  uint32_t ctx_before = rdw(0x81D6C978u);
                  uint64_t hargs[] = {0};
                  uint64_t hr = rhost ? ks->processor()->Execute(ts, rhost, hargs, 0)
                                      : 0xDEADull;
                  uint32_t ctx_after = rdw(0x81D6C978u);
                  XELOGI("LLE xam: render host {:08X} before skin init -> hr={:08X} "
                         "ctx {:08X} -> {:08X} [+C]={:08X} [+1C]={:08X} "
                         "texdev[81D6C980]={:08X}",
                         rhost, static_cast<uint32_t>(hr), ctx_before, ctx_after,
                         ctx_after ? rdw(ctx_after + 0x0Cu) : 0u,
                         ctx_after ? rdw(ctx_after + 0x1Cu) : 0u,
                         rdw(0x81D6C980u));
                }
                // Phase 517: the loader dies at 81901EAC, a bctrl through
                // [[obj+0x1C8]+0x0C] where that slot holds 006E0065 - UTF-16
                // "en" - so the field points at locale text, not an interface
                // table. Driving 81795548 directly skips whatever its normal
                // caller initialises. The site has a clean null path (81901E88
                // bne -> 81901E8C returns E_FAIL), so nop the branch to take it
                // unconditionally and let the loader continue.
                //
                // This must happen HERE, not in GuideBootstrap: the crash is
                // during this call, and the bootstrap's patch site is never
                // reached (RtUnbindPatch does not log in this configuration).
                // Phase 1046: the nop is separable from the stand-ins below.
                // Once the JIT translates 81901E40 with it, every image load
                // returns E_FAIL for the session; see guide_skin_dispatch_nop.
                if (cvars::guide_patch_skin_dispatch &&
                    cvars::guide_skin_dispatch_nop) {
                  kernel::xboxkrnl::GuidePatchWord(0x81901E88u, 0x409A0010u,
                                                   0x60000000u,
                                                   "SkinDispatchPatch");
                } else if (cvars::guide_patch_skin_dispatch) {
                  XELOGI("LLE xam: SkinDispatchPatch skipped "
                         "(guide_skin_dispatch_nop=false)");
                }
                // Phase 1096ci: this direct Execute is a SECOND invocation.
                // 81795970 has already armed the task at [81D43C50+0x28] with
                // proc 81795548 and flags 5 (proc | ENQUEUE) and kicked the
                // pool at 81795A20. When the pool is live the guest dispatches
                // it itself (measured: skin-loader caller history carries
                // 81779D54, the dispatcher's bctrl on task[+0x30]), so calling
                // it here as well makes it run twice and races its own tail.
                if (cvars::guide_guest_dispatch_skin_loader) {
                  XELOGI("LLE xam: NOT calling skin loader 81795548 - the "
                         "guest queued it at 81795A18/81795A20; leaving the "
                         "dispatch to xam's own task pool");
                  // Phase 1096ck: removing the host's call alone reordered the
                  // block - 81780A28's pool task ran first and faulted at
                  // 81747DE8. The host still has to WAIT where it used to
                  // block inside Execute, but waiting is not driving: poll
                  // xam's own completion flag, the one 81795948 writes at the
                  // loader's tail ([81D43C50+0xB4] = 1), which is set only
                  // after 8179593C has re-armed the shared task with the
                  // HUD-manager loop. Nothing is written to the guest.
                  if (cvars::guide_guest_dispatch_wait_ms > 0) {
                    auto rdw3 = [&](uint32_t a) {
                      return a ? xe::load_and_swap<uint32_t>(
                                     ks->memory()->TranslateVirtual(a))
                               : 0u;
                    };
                    const auto t0 = std::chrono::steady_clock::now();
                    const auto deadline =
                        t0 + std::chrono::milliseconds(
                                 cvars::guide_guest_dispatch_wait_ms);
                    uint32_t done = 0;
                    while (std::chrono::steady_clock::now() < deadline) {
                      done = rdw3(0x81D43C50u + 0xB4u);
                      if (done == 1u) break;
                      std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    const auto ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
                    const uint32_t slot = rdw3(0x81D43C50u + 0x28u);
                    const uint32_t task = slot & ~1u;
                    XELOGI("LLE xam: guest-dispatched skin loader {} after "
                           "{} ms ([81D43C50+B4]={:08X}); slot={:08X} "
                           "proc[+30]={:08X}",
                           done == 1u ? "COMPLETED" : "DID NOT COMPLETE", ms,
                           done, slot, task ? rdw3(task + 0x30u) : 0u);
                    // Phase 1096dc: press the Guide button through xam's own
                    // automation API. See guide_guest_xenon_button.
                    if (cvars::guide_guest_xenon_button && done == 1u) {
                      const uint32_t dev_before = rdw3(0x81D3C728u);
                      uint64_t bargs[] = {0};
                      uint64_t br = ks->processor()->Execute(
                          ts, kernel::xboxkrnl::GuideConst(0x81723E08u), bargs,
                          1);
                      XELOGI("GuideXenonButton: 81723E08(0) -> {:08X} | "
                             "[81D3C728] {:08X} -> {:08X} | state {:08X}",
                             static_cast<uint32_t>(br), dev_before,
                             rdw3(0x81D3C728u), rdw3(0x81D43C50u));
                      // Phase 1096dc: and deliver the press itself through
                      // xam ordinal 0x3D9, XAutomationpInputSetState
                      // (81723ED0). It memcpy's 12 bytes - an XINPUT_GAMEPAD -
                      // into [81D3C74C + user*12] (81723F00 mulli 0xC,
                      // 81723F10 add, 81723F14 bl 8180D9F0 with r5=0xC). The
                      // only value written is the button the user actually
                      // pressed; xam decides what to do with it.
                      const uint32_t pad =
                          ks->memory()->SystemHeapAlloc(16u);
                      if (pad) {
                        auto* padp = ks->memory()->TranslateVirtual(pad);
                        std::memset(padp, 0, 16);
                        xe::store_and_swap<uint16_t>(
                            padp, uint16_t(xe::hid::X_INPUT_GAMEPAD_GUIDE));
                        uint64_t sargs2[] = {0, pad};
                        uint64_t sr2 = ks->processor()->Execute(
                            ts, kernel::xboxkrnl::GuideConst(0x81723ED0u),
                            sargs2, 2);
                        XELOGI("GuideXenonButton: 81723ED0(0, {:08X}) -> "
                               "{:08X} | state {:08X}",
                               pad, static_cast<uint32_t>(sr2),
                               rdw3(0x81D43C50u));
                        // Phase 1096dh: and the press itself, through xam
                        // ordinal 0x3D8 XAutomationpInputPress (817243A8),
                        // which takes (user, button mask) - 817243B4/B8 keep
                        // r3/r4, 817243F4 compares the mask against 0xFFFF.
                        // The only value passed is the button the user pressed.
                        // Phase 1096dh: the mask 0xFFFF is xam's OWN sentinel
                        // for the Guide button - 81724444 compares against it
                        // and 81724454 `ori r11, r11, 0x400` sets
                        // X_INPUT_GAMEPAD_GUIDE only on that path. Any other
                        // mask goes through the validator at 817C4758, which
                        // rejects 0x400 (measured: the call returned 0).
                        uint64_t pargs[] = {0, 0xFFFFull};
                        uint64_t pr = ks->processor()->Execute(
                            ts, kernel::xboxkrnl::GuideConst(0x817243A8u),
                            pargs, 2);
                        XELOGI("GuideXenonButton: 817243A8(0, GUIDE) -> {:08X} "
                               "| state {:08X}",
                               static_cast<uint32_t>(pr), rdw3(0x81D43C50u));
                        // Phase 1096dl: a watcher wants an EDGE, and one
                        // press cannot produce one if nothing sampled the
                        // released state first. Drive press/release pairs and
                        // let the pump's argument capture say whether ANY new
                        // message id appears. If none does, the automation
                        // press notifies nothing and the Guide watcher is not
                        // on this path at all - which is the open question
                        // 1096dk withdrew to.
                        for (int rep = 0; rep < 8; ++rep) {
                          std::memset(padp, 0, 16);
                          uint64_t z[] = {0, pad};
                          ks->processor()->Execute(
                              ts, kernel::xboxkrnl::GuideConst(0x81723ED0u), z,
                              2);
                          xe::threading::Sleep(std::chrono::milliseconds(40));
                          uint64_t p2[] = {0, 0xFFFFull};
                          ks->processor()->Execute(
                              ts, kernel::xboxkrnl::GuideConst(0x817243A8u), p2,
                              2);
                          xe::threading::Sleep(std::chrono::milliseconds(40));
                        }
                        XELOGI("GuideXenonButton: 8 press/release pairs done | "
                               "state {:08X}",
                               rdw3(0x81D43C50u));
                      }
                    }
                    // Phase 1096fq: DIAGNOSTIC, see guide_diag_call_launcher.
                    if (cvars::guide_diag_call_launcher && done == 1u) {
                      const uint32_t rec_before = rdw3(0x81D426C8u);
                      uint64_t la[] = {0, 0, 0};
                      uint64_t lr2 = ks->processor()->Execute(
                          ts, kernel::xboxkrnl::GuideConst(0x817C2480u), la, 3);
                      XELOGI("GuideDiagLauncher: 817C2480(0,0,0) -> {:08X} | "
                             "[81D426C8] {:08X} -> {:08X} | state {:08X}",
                             static_cast<uint32_t>(lr2), rec_before,
                             rdw3(0x81D426C8u), rdw3(0x81D43C50u));
                    }
                    // Phase 1096fh: DIAGNOSTIC, see guide_diag_call_ordinal_580.
                    if (cvars::guide_diag_call_ordinal_580 && done == 1u) {
                      const uint32_t gate_before =
                          rdw3(0x81D43C50u + 0xA8u);
                      uint64_t o5[] = {0, 0, 0, 0, 0, 0};
                      uint64_t or5 = ks->processor()->Execute(
                          ts, kernel::xboxkrnl::GuideConst(0x81793AA0u), o5, 6);
                      XELOGI("GuideDiag580: ordinal 0x244 (81793AA0) with six "
                             "zeros -> {:08X} | [81D43C50+A8] {:08X} -> {:08X} "
                             "| state {:08X}",
                             static_cast<uint32_t>(or5), gate_before,
                             rdw3(0x81D43C50u + 0xA8u), rdw3(0x81D43C50u));
                    }
                    // Phase 1096cs: call xam's OWN exported show entry
                    // (ordinal 581) so xam sets its own HUD state and its own
                    // loop reaches ShowHud. See guide_guest_show_via_xam.
                    if (cvars::guide_guest_show_via_xam && done == 1u) {
                      const uint32_t st_before = rdw3(0x81D43C50u);
                      uint64_t nargs[] = {0};
                      uint64_t xr = ks->processor()->Execute(
                          ts, kernel::xboxkrnl::GuideConst(0x8178E240u), nargs,
                          0);
                      XELOGI("GuideGuestShow: xam ordinal 581 (8178E240) -> "
                             "{:08X} | [81D43C50] {:08X} -> {:08X}",
                             static_cast<uint32_t>(xr), st_before,
                             rdw3(0x81D43C50u));
                    }
                    // Phase 1096cp: DIAGNOSTIC, see guide_diag_kick_hud_loop.
                    if (cvars::guide_diag_kick_hud_loop && done == 1u && slot) {
                      // Phase 1096cu: call 81790758, NOT the raw kick 8177BF38.
                      // The loop's exit path CASes [81D43C50+0x24] and loops
                      // back to 81794C64 only while that counter is non-zero
                      // (81795460 cmpwi r10,0 / 81795464 bne -> 81794C64).
                      // 81790758 is xam's own post: it atomically increments
                      // [+0x24] AND kicks on the 0->1 transition (817907BC).
                      // The raw kick alone dispatched a loop with no pending
                      // work, which is why it exited immediately.
                      const uint32_t proc = task ? rdw3(task + 0x30u) : 0u;
                      const uint32_t cnt_before = rdw3(0x81D43C50u + 0x24u);
                      uint64_t kargs[] = {0};
                      uint64_t kr = ks->processor()->Execute(
                          ts, kernel::xboxkrnl::GuideConst(0x81790758u), kargs,
                          1);
                      XELOGI("GuideDiagKick: 81790758(0) with proc[+30]={:08X} "
                             "-> {:08X} | [+24] {:08X} -> {:08X} | state {:08X}",
                             proc, static_cast<uint32_t>(kr), cnt_before,
                             rdw3(0x81D43C50u + 0x24u), rdw3(0x81D43C50u));
                    }
                  }
                } else {
                  XELOGI("LLE xam: calling skin loader 81795548");
                  uint64_t sr = ks->processor()->Execute(
                      ts, kernel::xboxkrnl::GuideConst(0x81795548u), sargs, 0);
                  XELOGI("LLE xam: skin loader returned {:08X}",
                         static_cast<uint32_t>(sr));
                }
                // Phase 518: the Guide bootstrap then dies at 819138F0, on a
                // different thread and long after skin init:
                //
                //   819138D8  addi r25, r11, -0x363b   r25 = 0x81D6C9C5
                //   819138EC  lwz  r3, 3(r25)          r3 = [0x81D6C9C8] -> null
                //   819138F0  lwz  r11, 0(r3)          fault at guest 0
                //   819138F4  lwz  r11, 0xc(r11)       vtable slot 3
                //   819138FC  bctrl
                //   81913900  or. r27, r3, r3
                //   81913904  blt -> error exit
                //
                // Installed AFTER the loader returns, not before: setting the
                // global up front diverts the loader itself into a crash at
                // 8191083C two instructions after the call. The global is
                // null-and-harmless during skin init and null-and-fatal later,
                // so the stand-in has to appear in between.
                //
                // Another uninitialised global dereferenced as an object, the
                // same shape as the [81D43C50+0x28] gap above. There is no null
                // check to patch here, so supply the object instead: a block
                // whose word 0 points at a vtable whose slot 3 is the `blr`
                // stub GuideConst already uses. The stub returns with r3 still
                // holding the object pointer, which is non-negative, so the
                // `blt` is not taken and the caller proceeds.
                //
                // Like the manager above this is a stand-in, not the real
                // object - the call it replaces does not happen. It buys the
                // bootstrap past this point so the draw hook can install.
                // Phase 1047: [81D6C9C8] is the XUI font renderer. XUIFONT::Init
                // (819138EC) calls its vtable slot 3 to build the typeface, and
                // the render host's XuiInit path (81911D30 -> 819106F8) installs
                // the static renderer 81D6CA00 there. Installing a blr-stub
                // stand-in over a value xam has already set is what left every
                // element typeface with no font behind it, so read the slot
                // first and leave a non-null value alone.
                uint32_t cur_fr = xe::load_and_swap<uint32_t>(
                    ks->memory()->TranslateVirtual(0x81D6C9C8u));
                XELOGI("LLE xam: [81D6C9C8] after skin loader = {:08X}{}", cur_fr,
                       cur_fr ? " (xam's own; not overwriting)" : "");
                if (cvars::guide_patch_skin_dispatch && !cur_fr) {
                  uint32_t blk = ks->memory()->SystemHeapAlloc(0x80, 16);
                  uint32_t nopfn = kernel::xboxkrnl::GuideNopFn();
                  if (cvars::guide_skin_dispatch_real) {
                    // Phase 603: this is the EARLIEST installer, and the one
                    // that decides what 819106F8 sees at render-host time.
                    // Patching only the later sites left the fabricated block
                    // in place for the call that matters.
                    auto* m = ks->memory();
                    xe::store_and_swap<uint32_t>(
                        m->TranslateVirtual(0x81D6C9C8u), 0x81D6CA00u);
                    XELOGI("LLE xam: [81D6C9C8] <- real object 81D6CA00");
                  } else if (blk && nopfn) {
                    auto* m = ks->memory();
                    std::memset(m->TranslateVirtual(blk), 0, 0x80);
                    uint32_t vt = blk + 0x40u;
                    xe::store_and_swap<uint32_t>(m->TranslateVirtual(blk), vt);
                    // Fill EVERY slot, not just the one the crash named. With
                    // only +0x0C set, the next fault was 81914250 - a bctrl on
                    // slot 1 (Release) of this very object, jumping to zero. A
                    // stand-in vtable with holes just relocates the crash.
                    for (uint32_t sl = 0; sl < 16; ++sl) {
                      xe::store_and_swap<uint32_t>(
                          m->TranslateVirtual(vt + sl * 4u), nopfn);
                    }
                    xe::store_and_swap<uint32_t>(
                        m->TranslateVirtual(0x81D6C9C8u), blk);
                    XELOGI("LLE xam: [81D6C9C8] stand-in obj={:08X} vtable={:08X} "
                           "16 slots -> {:08X}", blk, vt, nopfn);
                  } else {
                    XELOGW("LLE xam: could not build [81D6C9C8] stand-in "
                           "(blk={:08X} nopfn={:08X})", blk, nopfn);
                  }
                }

              }
              return 0;
            },
            ks->GetSystemProcess()));
    xam_boot->set_name("LLE xam init");
    if (XSUCCEEDED(xam_boot->Create())) {
      XELOGI("LLE xam: waiting for init thread");
      if (cvars::lle_xam_skin_init) {
        // The skin loader's tail loads [81D43C50+0x28] and calls through it.
        // Nothing in xam ever writes that field under our bootstrap, so the
        // call gets a null object and the thread dies there - after the
        // registrations, but before returning. Waiting forever then wedges
        // everything downstream: the Guide never receives a handler and the
        // auto-press fires with handler=0. Bound the wait so the rest of the
        // bootstrap still runs and what did get registered can be measured.
        uint64_t timeout = static_cast<uint64_t>(-150000000LL);  // 15s
        X_STATUS wait_result = xam_boot->Wait(0, 0, 0, &timeout);
        if (wait_result == X_STATUS_TIMEOUT) {
          XELOGW(
              "LLE xam: init thread still running after 15s (the skin "
              "loader's tail faults on a null); continuing anyway");
        }
      } else {
        xam_boot->Wait(0, 0, 0, nullptr);
      }
      XELOGI("LLE xam: init complete");
      // Did the skin's registrations actually land? XuiVisualRegister
      // (8193D238) takes a lock at 81D6CE5C and inserts into a registry at
      // 81D6CF50 - both constants built in its own prologue. That is the
      // collection XuiVisualCreateInstance searches, and it is NOT the class
      // registry at 81D6D508 (48 slots, 38 non-null), which is a different
      // table entirely. Dump it here so a skin run can be compared against a
      // default one without inferring anything from scene behaviour.
      {
        // 81D6CF50 is a dashroot-xam address. On any other build it need not
        // be mapped, and reading it unguarded host-faults during xam init -
        // which killed the run before the title ever launched and made both
        // alternative-xam tests look like properties of those builds
        // (phases 403, 406). Check the range first; the project's own method
        // note says never to read a guest pointer without doing so.
        auto* vm = memory();
        uint32_t vlo = 0, vhi = 0;
        if (lle_xam_module_ && lle_xam_module_->xex_module()) {
          vlo = lle_xam_module_->xex_module()->base_address();
          vhi = vlo + lle_xam_module_->xex_module()->image_size();
        }
        if (vm && vlo && 0x81D6CF50u >= vlo && 0x81D6CF50u + 48u <= vhi) {
          std::string vr;
          for (uint32_t i = 0; i < 12; ++i) {
            vr += fmt::format("{:08X} ", xe::load_and_swap<uint32_t>(
                                             vm->TranslateVirtual(
                                                 0x81D6CF50u + i * 4)));
          }
          XELOGI("LLE xam: visual registry @81D6CF50: {}", vr);
        } else {
          XELOGW("LLE xam: visual registry @81D6CF50 not mapped - skipping "
                 "(expected on a non-dashroot xam)");
        }
      }
      // The tag->index mapper (817BA1D8) returns a single flag bit as the
      // index for the 0x10000000 request class, so requests like 0x18100000
      // land on heap[0] deterministically - and heap[0] is the 0xCCCC
      // placeholder. Copy heap[1]'s descriptor over it to test whether that
      // is the whole story: if so the failures go away while xam stays up.
      // Locate the heap-descriptor array by content, for builds where
      // 81D4E1B0 does not apply. Signature per entry, from dashroot's runtime
      // dump: +0 is a small index (0..15), +4 and +8 are equal and hold the
      // heap base (0x4xxxxxxx or 0x8Cxxxxxx). The indices are written during
      // init, so this must run after DllMain, not against the on-disk image.
      // Locate the heap-descriptor array for THIS build. dashroot's is at
      // 81D4E1B0 with stride 408; retail 17559's is at 81AABFF8 with stride
      // 24 (phase 418). Deriving both at runtime removes the last hardcoded
      // dependency from the heap workaround.
      uint32_t heap_desc_base = 0, heap_desc_stride = 0;
      if ((cvars::lle_xam_find_heaps || cvars::lle_xam_heap0_alias) &&
          lle_xam_module_ && lle_xam_module_->xex_module()) {
        uint32_t xb = lle_xam_module_->xex_module()->base_address();
        uint32_t xs = lle_xam_module_->xex_module()->image_size();
        auto* fm = memory();
        std::vector<uint32_t> hits;
        // No page query here either - it reports kNoAccess for readable
        // image addresses (phase 416). Staying inside [base, base+size) is
        // the correct and sufficient bound.
        for (uint32_t o = 0; fm && xs && o + 12 < xs; o += 4) {
          uint32_t a = xb + o;
          uint32_t v0 = xe::load_and_swap<uint32_t>(fm->TranslateVirtual(a));
          if (v0 > 15) continue;
          uint32_t v4 =
              xe::load_and_swap<uint32_t>(fm->TranslateVirtual(a + 4));
          uint32_t v8 =
              xe::load_and_swap<uint32_t>(fm->TranslateVirtual(a + 8));
          // dashroot's heap bases are 4000_0000, 401F_0000, 405A_0000,
          // 408D_0000, 408F_0000, 4091_0000 and 8C00_0000. The earlier bound
          // (< 0x90000000) also admitted 0x81xxxxxx image pointers, which
          // buried the signal in 90-odd false candidates.
          bool base_ok = (v4 >= 0x40000000u && v4 < 0x50000000u) ||
                         (v4 >= 0x8C000000u && v4 < 0x8D000000u);
          if (v4 != v8 || !base_ok) continue;
          hits.push_back(a);
        }
        XELOGI("FindHeaps: {} candidate descriptors in {:08X}+{:X}",
               hits.size(), xb, xs);
        // Derive the stride from two candidates whose indices differ by one,
        // then project back to entry 0. Index-aware rather than
        // spacing-aware: idx 0 and idx 3 are absent on BOTH builds (the
        // 0000CCCC placeholder, and an entry with +4 != +8), so consecutive
        // hits are not necessarily adjacent entries. A fixed minimum stride
        // was also wrong - dashroot's is 408, retail's is 24.
        auto idx_of = [&](uint32_t a) {
          return xe::load_and_swap<uint32_t>(fm->TranslateVirtual(a));
        };
        for (size_t i = 0; i + 1 < hits.size() && !heap_desc_base; ++i) {
          for (size_t j = i + 1; j < hits.size(); ++j) {
            uint32_t ia = idx_of(hits[i]), ja = idx_of(hits[j]);
            if (ja <= ia || ja - ia > 8 || hits[j] <= hits[i]) continue;
            uint32_t span = hits[j] - hits[i];
            if (span % (ja - ia)) continue;
            uint32_t st = span / (ja - ia);
            if (st < 8 || st > 0x600 || hits[i] < st * ia) continue;
            heap_desc_stride = st;
            heap_desc_base = hits[i] - st * ia;
            XELOGI("FindHeaps: array at {:08X} stride {} ({:X}) from idx {} "
                   "@{:08X} and idx {} @{:08X}",
                   heap_desc_base, st, st, ia, hits[i], ja, hits[j]);
            break;
          }
        }
        for (size_t i = 0; i < hits.size() && i < 12; ++i) {
          XELOGI("FindHeaps:   cand {:08X} idx={} base={:08X}", hits[i],
                 xe::load_and_swap<uint32_t>(fm->TranslateVirtual(hits[i])),
                 xe::load_and_swap<uint32_t>(
                     fm->TranslateVirtual(hits[i] + 4)));
        }
      }
      // Every address in this block is a constant lifted from dashroot's xam.
      // On another build they need not be mapped, and reading them unguarded
      // host-faults during init - which is what made phases 403 and 406 read
      // as facts about those xams instead of about this code.
      // QueryRangeAccess is NOT a readability test for the xam image: it
      // reports kNoAccess for addresses that read back fine (measured -
      // [81D4E1B0] returns 0000CCCC while the query says kNoAccess), because
      // the page table does not track protection for the mapped image. Using
      // it here rejected valid dashroot addresses and silently disabled the
      // heap alias, which made dashroot fail exactly like retail.
      //
      // The question these guards actually need to answer is "is this
      // dashroot-derived constant inside THIS build's xam image", so ask that
      // directly against the loaded module's extent.
      uint32_t xam_img_lo = 0, xam_img_hi = 0;
      if (lle_xam_module_ && lle_xam_module_->xex_module()) {
        xam_img_lo = lle_xam_module_->xex_module()->base_address();
        xam_img_hi = xam_img_lo + lle_xam_module_->xex_module()->image_size();
      }
      auto xam_mapped = [&xam_img_lo, &xam_img_hi](uint32_t a,
                                                   uint32_t len) -> bool {
        return xam_img_lo && a >= xam_img_lo && a + len <= xam_img_hi;
      };
      // Use the located array rather than dashroot's constants, so this
      // applies on any build. Falls back to the dashroot values only if the
      // scan found nothing and they are inside this image.
      uint32_t alias_base = heap_desc_base, alias_stride = heap_desc_stride;
      if (!alias_base && xam_mapped(0x81D4E1B0u, 2 * 408u)) {
        alias_base = 0x81D4E1B0u;
        alias_stride = 408u;
      }
      if (cvars::lle_xam_heap0_alias && alias_base && alias_stride &&
          xam_mapped(alias_base, 2 * alias_stride)) {
        auto* d0 = memory()->TranslateVirtual(alias_base);
        auto* d1 = memory()->TranslateVirtual(alias_base + alias_stride);
        // Phase 1099w: remember the real heap[0] (the per-title workspace
        // placeholder) so a title switch can put it back. heap[0] is TITLE
        // memory that xam destroys and releases at every title terminate
        // (81750CD8 -> 817B4838 -> 817B3860 + 817B2970); with heap[1]'s
        // descriptor aliased into it, that teardown released xam's own system
        // heap at 40000000 and every system thread crashed.
        kernel_state_->heap0_alias_address = alias_base;
        kernel_state_->heap0_original.assign(d0, d0 + alias_stride);
        std::memcpy(d0, d1, alias_stride);
        xe::store_and_swap<uint32_t>(d0, 0);  // keep id field as index 0
        XELOGI("LLE xam: aliased heap[0] to heap[1] at {:08X} stride {}",
               alias_base, alias_stride);
      } else if (cvars::lle_xam_heap0_alias) {
        XELOGW("LLE xam: heap0 alias skipped - no descriptor array located");
      }
      // Read the heap descriptors straight out of guest memory rather than
      // inferring their state from allocation-failure counts. Array is at
      // 0x81D4E1B0 (a constant embedded in xam's code, so already a flat
      // runtime address), 10 entries of 408 bytes. Field +4 is the "created"
      // flag: every reader in xam early-outs when it is zero.
      for (uint32_t i = 0; i < 10 && alias_base && alias_stride &&
                          xam_mapped(alias_base, 10 * alias_stride);
           ++i) {
        uint32_t desc = alias_base + i * alias_stride;
        auto* dp = memory()->TranslateVirtual(desc);
        XELOGI("LLE xam: heap[{}] @{:08X} +0={:08X} +4={:08X} +8={:08X} "
               "+1C={:08X} +24={:08X}",
               i, desc, xe::load_and_swap<uint32_t>(dp),
               xe::load_and_swap<uint32_t>(dp + 4),
               xe::load_and_swap<uint32_t>(dp + 8),
               xe::load_and_swap<uint32_t>(dp + 0x1C),
               xe::load_and_swap<uint32_t>(dp + 0x24));
      }
      // xam's current-app-id getter (81783270) reads a global sentinel first:
      // when it holds -1 the getter short-circuits to 0xFE (XamApp) instead of
      // falling through to KeGetCurrentProcessType, which yields 0xEE on title
      // threads and makes the heap selector trap. Title code calling into xam
      // therefore lands on heap 0, the zero-sized placeholder.
      if (cvars::lle_xam_appid_sentinel && xam_mapped(0x81D227F0u, 4)) {
        auto* p = memory()->TranslateVirtual(0x81D227F0);
        XELOGI("LLE xam: app-id sentinel was {:08X}, forcing FFFFFFFF",
               xe::load_and_swap<uint32_t>(p));
        xe::store_and_swap<uint32_t>(p, 0xFFFFFFFFu);
      }
    } else {
      XELOGE("LLE xam: failed to create init thread");
    }
  }

  kernel::object_ref<kernel::XThread> main_thread;
  if (!module->is_executable() && cvars::allow_dll_module_launch) {
    // DLL modules have no title entry point to launch, and their DllMain must
    // not run on the UI thread (no guest thread state there). Run the
    // DLL_PROCESS_ATTACH sequence on a real guest thread instead.
    auto* ks = kernel_state_.get();
    auto xam_mod = lle_xam_module_;
    auto dll_mod = module;
    auto boot = kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
        ks, 1024 * 1024, 0, [ks, xam_mod, dll_mod]() -> int {
          auto* ts = kernel::XThread::GetCurrentThread()->thread_state();
          auto attach =
              [&](const kernel::object_ref<kernel::UserModule>& m) {
                if (!m || !m->entry_point()) {
                  return;
                }
                uint64_t args[] = {m->handle(), 1 /* DLL_PROCESS_ATTACH */, 0};
                XELOGI("Bootstrap: DllMain {} entry={:08X}", m->name(),
                       m->entry_point());
                ks->processor()->Execute(ts, m->entry_point(), args,
                                         xe::countof(args));
                XELOGI("Bootstrap: DllMain {} returned", m->name());
              };
          attach(xam_mod);
          attach(dll_mod);
          XELOGI("Bootstrap: attach sequence complete");

          // If a system app registered a message handler (hud.xex registers
          // as app 0xFF), optionally dispatch the Guide open message to it.
          if (cvars::lle_show_guide) {
            uint32_t guide_handler = ks->sys_app_handler(0xFF);
            if (guide_handler) {
              auto* mem = ks->memory();
              // hud copies 0x47C bytes out of the inner struct and writes
              // the result size through the third argument, so that must be
              // a pointer, not a size.
              uint32_t inner = mem->SystemHeapAlloc(0x500, 16);
              uint32_t buf = mem->SystemHeapAlloc(0x40, 16);
              uint32_t out_sz = mem->SystemHeapAlloc(0x10, 16);
              XELOGI("Guide: buffers inner={:08X} buf={:08X} out_sz={:08X}",
                     inner, buf, out_sz);
              std::memset(mem->TranslateVirtual(inner), 0, 0x500);
              std::memset(mem->TranslateVirtual(buf), 0, 0x40);
              std::memset(mem->TranslateVirtual(out_sz), 0, 0x10);
              auto* iw = mem->TranslateVirtual<xe::be<uint32_t>*>(inner);
              iw[2] = static_cast<uint32_t>(cvars::guide_subcommand);
              auto* bw = mem->TranslateVirtual<xe::be<uint32_t>*>(buf);
              bw[0] = 1;
              bw[1] = inner;
              XELOGI("Bootstrap: Guide dispatch msg=80000004 subcmd={} -> {:08X}",
                     int32_t(cvars::guide_subcommand), guide_handler);
              uint64_t gargs[] = {static_cast<uint64_t>(cvars::guide_message), buf, out_sz};
              uint64_t gres = ks->processor()->Execute(ts, guide_handler, gargs,
                                                       xe::countof(gargs));
              XELOGI("Bootstrap: Guide handler returned {:08X}",
                     static_cast<uint32_t>(gres));
            } else {
              XELOGW("Bootstrap: no system app 0xFF handler registered");
            }
          }

          if (cvars::lle_show_guide && xam_mod) {
            // XamShowGuideUI == xam ordinal 0x304.
            uint32_t guide_addr = xam_mod->GetProcAddressByOrdinal(0x304);
            XELOGI("Bootstrap: XamShowGuideUI (ord 0x304) -> {:08X}",
                   guide_addr);
            if (guide_addr) {
              uint64_t guide_args[] = {0};
              uint64_t guide_ret = ks->processor()->Execute(
                  ts, guide_addr, guide_args, xe::countof(guide_args));
              XELOGI("Bootstrap: XamShowGuideUI returned {:08X}",
                     static_cast<uint32_t>(guide_ret));
            } else {
              XELOGE("Bootstrap: could not resolve XamShowGuideUI");
            }
          }
          return 0;
        }));
    boot->set_name("Guide Bootstrap");
    // Must happen before Create(): SetExecutableModule initializes the title
    // X_KPROCESS, and XThread::InitializeGuestObject acquires that process's
    // thread_list_spinlock. Creating a thread first spins on an uninitialized
    // lock forever. This mirrors the ordering in KernelState::LaunchModule.
    kernel_state_->SetExecutableModule(module);
    XELOGI("Bootstrap: creating thread");
    X_STATUS boot_status = boot->Create();
    XELOGI("Bootstrap: Create() returned {:08X}", boot_status);
    if (XFAILED(boot_status)) {
      XELOGE("Failed to create Guide bootstrap thread");
      return X_STATUS_UNSUCCESSFUL;
    }
    main_thread = kernel::object_ref<kernel::XThread>(boot.release());
  } else {
    // Phase 1096ed: see guide_hud_load_before_title. Registration only needs
    // the module loaded and its DllMain run; the overlay's RENDERING is what
    // needs the title up, and that still happens later on the existing thread.
    if (cvars::guide_hud_load_before_title && !cvars::guide_hud_path.empty()) {
      XELOGI("Guide: pre-loading {} BEFORE the title launches",
             std::string(cvars::guide_hud_path));
      auto pre = kernel_state_->LoadUserModule(cvars::guide_hud_path, false);
      if (!pre) {
        XELOGE("Guide: pre-load failed to load");
      } else if (XFAILED(kernel_state_->FinishLoadingUserModule(pre, false))) {
        XELOGE("Guide: pre-load failed to finish loading");
      } else {
        // Phase 1096ee: loading is not what registers. hud's DllMain is - it
        // reaches 913E6B48, the module's ONLY call site of XamRegisterSysApp
        // (measured: 8177F240 callers = 913E6B74, twice). FinishLoadingUserModule
        // with false does not run it, which is why 1096ed's pre-load moved
        // nothing. Run DllMain here, on a guest thread, and WAIT for it, so the
        // registration lands before xam's one-shot sys-app pass.
        auto* ks_pre = kernel_state_.get();
        uint32_t ep = pre->entry_point();
        uint32_t hm = pre->hmodule_ptr();
        auto done = std::make_shared<std::atomic<bool>>(false);
        auto dll_thread =
            kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
                ks_pre, 1024 * 1024, 0, [ks_pre, ep, hm, done]() -> int {
                  auto* ts2 =
                      kernel::XThread::GetCurrentThread()->thread_state();
                  uint64_t a[] = {hm, 1 /* PROCESS_ATTACH */, 0};
                  uint64_t r = ks_pre->processor()->Execute(ts2, ep, a,
                                                            xe::countof(a));
                  XELOGI("Guide: pre-load DllMain({:08X}) returned {:08X}", ep,
                         static_cast<uint32_t>(r));
                  done->store(true);
                  return 0;
                }));
        dll_thread->set_name("Guide PreRegister");
        if (XFAILED(dll_thread->Create())) {
          XELOGE("Guide: pre-load DllMain thread failed to start");
        } else {
          const auto t0 = std::chrono::steady_clock::now();
          while (!done->load() &&
                 std::chrono::steady_clock::now() - t0 <
                     std::chrono::milliseconds(8000)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
          }
          XELOGI("Guide: pre-load DllMain {} after {} ms",
                 done->load() ? "completed" : "TIMED OUT",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0)
                     .count());
        }
        XELOGI("Guide: pre-load complete, base {:08X}",
               pre->xex_module()->base_address());
      }
    }
    // Phase 1099z16: the optical drive's contents. The tray hook runs while
    // the SMC tray moves: opening removes the disc, closing loads
    // guide_tray_disc_path (the host's "put a disc in the tray").
    {
      {
        std::lock_guard<std::mutex> lock(tray_disc_mutex_);
        if (tray_disc_path_.empty()) {
          tray_disc_path_ = cvars::guide_tray_disc_path;
        }
      }
      auto mount_disc = [this]() {
        const std::filesystem::path disc = GetTrayDisc();
        if (disc.empty() || file_system_->ResolvePath("\\Device\\CdRom0\\")) {
          return;
        }
        auto device = CreateVfsDevice(disc, "\\Device\\CdRom0");
        if (!device || !device->Initialize() ||
            !file_system_->RegisterDevice(std::move(device))) {
          XELOGE("Tray: could not load disc image {}", xe::path_to_utf8(disc));
          return;
        }
        XELOGI("Tray: disc {} is in the drive", xe::path_to_utf8(disc));
      };
      kernel_state_->smc()->set_tray_hook([this, mount_disc](bool open) {
        if (open) {
          if (file_system_->UnregisterDevice("\\Device\\CdRom0")) {
            XELOGI("Tray: disc removed");
          }
        } else {
          mount_disc();
        }
        on_tray_state_changed(open);
      });
      if (cvars::guide_tray_disc_at_boot) {
        mount_disc();
      }
    }
    if (cvars::kernel_boot_via_xam && !cvars::guide_xam_boot_launch &&
        lle_xam_module_ && module->is_executable()) {
      // 1099z17559-8: retail xam's loader launches its boot title itself
      // (8169EAB0, launch type 2 -> XexLoadExecutable("\SystemRoot\dash.xex")).
      // Starting the image here as well ran two dashboards (the host-started
      // one was torn down by the XexLoadExecutable bypass and left a main
      // thread behind). Keep the image loaded but unstarted; xam's own
      // XexLoadExecutable adopts it and XexStartExecutable starts it. Nothing
      // calls into xam from the host.
      kernel_state_->boot_launch_module = module;
      main_thread = kernel::object_ref<kernel::XThread>(new kernel::XHostThread(
          kernel_state_.get(), 64 * 1024, 0,
          []() -> int {
            // Stands in for the title's main thread in WaitUntilExit; the
            // console does not exit when a title does.
            for (;;) {
              xe::threading::Sleep(std::chrono::seconds(1));
            }
            return 0;
          },
          kernel_state_->GetSystemProcess()));
      main_thread->set_name("Boot (xam launches the title)");
      if (XFAILED(main_thread->Create())) {
        XELOGE("BootViaXam: thread creation failed");
        main_thread = nullptr;
      }
    } else if (cvars::guide_xam_boot_launch && lle_xam_module_ &&
        module->is_executable()) {
      // Phase 1099z6: a console's xam launches the dashboard through its own
      // launcher, which records the running title (launcher +0x0 state 5,
      // +0x28C title id), runs its per-title start routine and builds the
      // title workspace. Keep the image Xenia just loaded, but hand the
      // launch to xam: XexLoadExecutable adopts this module, then
      // XexStartExecutable starts it.
      kernel_state_->boot_launch_module = module;
      auto* ks = kernel_state_.get();
      auto xam_mod = lle_xam_module_;
      std::string launch_path = cvars::guide_xam_boot_launch_path;
      main_thread = kernel::object_ref<kernel::XThread>(new kernel::XHostThread(
          ks, 1024 * 1024, kernel::X_CREATE_SUSPENDED,
          [ks, xam_mod, launch_path]() -> int {
            ks->RestoreHeap0Alias("BootLaunch");
            uint32_t launch = xam_mod->GetProcAddressByOrdinal(0x1A4);
            auto* mem = ks->memory();
            uint32_t path = mem->SystemHeapAlloc(
                static_cast<uint32_t>(launch_path.size() + 1));
            std::memcpy(mem->TranslateVirtual(path), launch_path.c_str(),
                        launch_path.size() + 1);
            XELOGI("BootLaunch: XamLoaderLaunchTitle({:08X}) '{}'", launch,
                   launch_path);
            auto* ts = kernel::XThread::GetCurrentThread()->thread_state();
            uint64_t args[] = {path, 0};
            uint64_t r =
                ks->processor()->Execute(ts, launch, args, xe::countof(args));
            XELOGI("BootLaunch: XamLoaderLaunchTitle returned {:08X}",
                   static_cast<uint32_t>(r));
            if (cvars::guide_net_probe_seconds > 0) {
              // Research probe: what real xam's network exports report.
              xe::threading::Sleep(
                  std::chrono::seconds(cvars::guide_net_probe_seconds));
              uint32_t link_fn = xam_mod->GetProcAddressByOrdinal(0x4B);
              uint32_t xnaddr_fn = xam_mod->GetProcAddressByOrdinal(0x49);
              uint64_t la[] = {0};
              uint64_t link =
                  link_fn ? ks->processor()->Execute(ts, link_fn, la, 1) : ~0ull;
              uint32_t addr = mem->SystemHeapAlloc(0x40);
              std::memset(mem->TranslateVirtual(addr), 0, 0x40);
              uint64_t xa[] = {0, addr};
              uint64_t xn = xnaddr_fn
                                ? ks->processor()->Execute(ts, xnaddr_fn, xa, 2)
                                : ~0ull;
              auto* w = mem->TranslateVirtual<xe::be<uint32_t>*>(addr);
              XELOGI("NetProbe: XNetGetEthernetLinkStatus({:08X}) -> {:08X}; "
                     "XNetGetTitleXnAddr({:08X}) -> {:08X} ina={:08X} "
                     "inaOnline={:08X}",
                     link_fn, static_cast<uint32_t>(link), xnaddr_fn,
                     static_cast<uint32_t>(xn), uint32_t(w[0]), uint32_t(w[1]));
              uint32_t media_fn = xam_mod->GetProcAddressByOrdinal(0x1A3);
              uint32_t tray_fn = xam_mod->GetProcAddressByOrdinal(0x1AA);
              std::memset(mem->TranslateVirtual(addr), 0, 0x40);
              uint64_t ma[] = {addr, addr + 4};
              uint64_t mr =
                  media_fn ? ks->processor()->Execute(ts, media_fn, ma, 2) : ~0ull;
              uint64_t tr =
                  tray_fn ? ks->processor()->Execute(ts, tray_fn, la, 0) : ~0ull;
              XELOGI("MediaProbe: XamLoaderGetMediaInfo({:08X}) -> {:08X} "
                     "type={:08X} id={:08X}; XamLoaderGetDvdTrayState({:08X}) "
                     "-> {:08X}",
                     media_fn, static_cast<uint32_t>(mr), uint32_t(w[0]),
                     uint32_t(w[1]), tray_fn, static_cast<uint32_t>(tr));
            }
            // This thread stands in for the title's main thread in
            // WaitUntilExit; the console does not "exit" when a title does.
            for (;;) {
              xe::threading::Sleep(std::chrono::seconds(1));
            }
          },
          ks->GetSystemProcess()));
      main_thread->set_name("Boot Launch (xam)");
      if (XFAILED(main_thread->Create())) {
        XELOGE("BootLaunch: thread creation failed");
        main_thread = nullptr;
      }
    } else {
      main_thread = kernel_state_->LaunchModule(module);
    }
  }
  if (!main_thread) {
    return X_STATUS_UNSUCCESSFUL;
  }
  main_thread_ = main_thread;

  // Optionally load the Guide (hud.xex) as a system app alongside the title.
  // It is an overlay: it needs a title running underneath for graphics, so
  // this happens after the main thread exists.
  if (!cvars::guide_hud_path.empty()) {
    auto* ks = kernel_state_.get();
    std::string hud_path = cvars::guide_hud_path;
    auto xam_mod_for_guide = lle_xam_module_;
    auto hud_boot =
        kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
            ks, 1024 * 1024, 0, [this, ks, hud_path, xam_mod_for_guide]() -> int {
              // Give the title time to bring up graphics before overlaying.
              // Phase 1096: was a hardcoded 8 seconds. Behind a flag now so the
              // value is measurable rather than asserted - see
              // guide_hud_load_delay_ms for why it is suspected of losing xam's
              // single message 0x7EC.
              xe::threading::Sleep(std::chrono::milliseconds(
                  std::max(0, cvars::guide_hud_load_delay_ms)));
              XELOGI("Guide: loading {}", hud_path);
              auto hud = ks->LoadUserModule(hud_path, false);
              if (!hud) {
                XELOGE("Guide: failed to load {}", hud_path);
                return 1;
              }
              if (XFAILED(ks->FinishLoadingUserModule(hud, false))) {
                XELOGE("Guide: failed to finish loading");
                return 1;
              }
              // Phase 1055 menus: the fix hooks on hud's xam imports (always)
              // and the call trace (guide_trace_xam_imports).
              GuideInstallXamImportTrace(hud.get());
              auto* ts = kernel::XThread::GetCurrentThread()->thread_state();
              // DllMain's first argument is the module's hmodule, not a
              // kernel object handle. hud keeps it and later hands it to
              // XexGetModuleSection to find its own "hud" resource section;
              // passing the handle made that lookup fail with
              // "no module for hmodule F8000494".
              uint64_t args[] = {hud->hmodule_ptr(), 1 /* PROCESS_ATTACH */,
                                 0};
              // Patch the locator choosers *before* DllMain runs. hud loads
              // its string table during initialisation, so patching after
              // DllMain returned was always too late for that path.
              if (cvars::guide_static_locator) {
                // hud chooses between the static and dynamic locator builders
                // in more than one place, and every one of them looks the
                // same:
                //   lwz r3,8(rX) ; cmpwi cr6,r3,-1 ; bneq -> dynamic
                // [obj+8] is 0 rather than -1 under our bootstrap, so the
                // dynamic branch is taken and the locator comes out as
                // "section://@0,...". Nopping the branch forces the static
                // builder, which takes its module from [obj+4].
                //
                // 913EB994 is the scene path. 913EC75C is the string-table
                // path: it feeds XamBuildDynamicResourceLocator into
                // XuiLoadStringTableFromFile, whose result is assigned to
                // [obj+0x4E8] by a helper that ignores the return value - so
                // a failed load leaves hud's string table null and hud later
                // copies from a null string (crash at 913FA204).
                //
                // Patching hud itself covers the xam-driven path too; setting
                // [obj+8] only works when our own bootstrap runs.
                static const uint32_t kLocatorSites[] = {0x913EB994u,
                                                         0x913EC75Cu};
                const uint32_t kBOrig = 0x409A0020u;
                for (uint32_t addr : kLocatorSites) {
                  auto* bw = memory()->TranslateVirtual<uint32_t*>(addr);
                  uint32_t bcur = xe::load_and_swap<uint32_t>(bw);
                  if (bcur != kBOrig) {
                    XELOGW("Guide: NOT patching hud {:08X}: found {:08X}, "
                           "expected {:08X}",
                           addr, bcur, kBOrig);
                    continue;
                  }
                  void* bp2 = reinterpret_cast<void*>(
                      reinterpret_cast<uintptr_t>(bw) & ~uintptr_t(0xFFF));
                  xe::memory::PageAccess bold =
                      xe::memory::PageAccess::kReadOnly;
                  if (xe::memory::Protect(bp2, 0x1000,
                                          xe::memory::PageAccess::kReadWrite,
                                          &bold)) {
                    xe::store_and_swap<uint32_t>(bw, 0x60000000u);
                    xe::memory::Protect(bp2, 0x1000, bold, nullptr);
                    XELOGI("Guide: patched hud {:08X} {:08X} -> 60000000 "
                           "(force static resource locator)",
                           addr, bcur);
                  }
                }
              }
              XELOGI("Guide: DllMain entry={:08X}", hud->entry_point());
              ks->processor()->Execute(ts, hud->entry_point(), args,
                                       xe::countof(args));
              // Phase 1096: log the RETURN VALUE, not just "returned". hud's
              // entry point (913F9D00) with reason==1 calls its CRT/static
              // initialiser 913F9C88 and, if that returns 0, branches to
              // 913F9D30 and returns 0 WITHOUT reaching 913E6B48 - the only
              // call site of XamRegisterSysApp in the module, which registers
              // hud as sysapp 0xFF with descriptor 913E69C0. A zero here means
              // hud never registered, which is exactly what the sysapp table
              // shows ([81D426C8] == 0, GuideSysAppTable id FF invalid).
              // "DllMain returned" was true and uninformative in both cases.
              XELOGI("Guide: DllMain returned {:08X} (0 = CRT init 913F9C88 "
                     "failed, hud did NOT register sysapp FF)",
                     static_cast<uint32_t>(ts->context()->r[3]));
              if (const char* hspec = std::getenv("XENIA_EFAIL_TAG_HUD")) {
                TagEFailSites(ks->memory(), hspec, "hud");
              }

              // Load hud's XUI skin package. hud asks
              // XamBuildResourceLocator for a locator into this module; with
              // no module the locator comes back empty and no scene loads.
              // hud.xex carries its own XUI skin as a resource section named
              // "hud" (91401000, 167581b in the XEX resource table), which is
              // exactly the container it passes to XamBuildResourceLocator.
              // So the module it wants is itself, not a separate package.
              guide_skin_module_ = hud->hmodule_ptr();
              XELOGI("Guide: hud handle={:08X} hmodule_ptr={:08X}",
                     hud->handle(), hud->hmodule_ptr());
              if (!cvars::guide_skin_path.empty()) {
                auto skin = ks->LoadUserModule(cvars::guide_skin_path, false);
                if (skin) {
                  ks->FinishLoadingUserModule(skin, false);
                  guide_skin_module_ = skin->hmodule_ptr();
                  XELOGI("Guide: skin override {} -> hmodule {:08X}",
                         cvars::guide_skin_path, guide_skin_module_);
                }
              }

              uint32_t h = ks->sys_app_handler(0xFF);
              if (!h) {
                // Under LLE xam, hud's XamRegisterSysApp import binds to the
                // real guest xam export, so the handler lands in xam's own
                // table and never reaches Xenia's HLE map. hud registers app
                // 0xFF with a handler baked into its code as base + 0x69C0
                // (98007960: lis r11,0x913e / addi r5,r11,27072, against a
                // load base of 913E0000). Derive it from the module base.
                // +0x69C0 is dashroot's hud. On the retail 17559 hud the
                // same handler sits at +0x5930, and dispatching a message ID
                // into whatever else lives at +0x69C0 there crashed inside
                // hud (phase 410: r3 held ASCII). The two handlers are
                // instruction-for-instruction identical, so find it by
                // signature instead of by offset:
                //     mfspr / bl / stwu r1,-128(r1) / lis rX,0x8000
                //     mr / ori rY,rX,4 / mr / subf r11,rY,r3
                // The 0x80000004 construction is unique in both images, and
                // sits at function_start + 0xC in both.
                uint32_t hbase = hud->xex_module()->base_address();
                uint32_t hsz = hud->xex_module()->image_size();
                uint32_t found = 0;
                auto* hmem = ks->memory();
                for (uint32_t o = 0; hmem && hsz && o + 16 < hsz && !found;
                     o += 4) {
                  uint32_t a = hbase + o;
                  auto* hh = hmem->LookupHeap(a);
                  if (!hh || hh->QueryRangeAccess(a, a + 16) ==
                                 xe::memory::PageAccess::kNoAccess) {
                    continue;
                  }
                  uint32_t w0 =
                      xe::load_and_swap<uint32_t>(hmem->TranslateVirtual(a));
                  if ((w0 >> 26) != 15 || ((w0 >> 16) & 31) != 0 ||
                      (w0 & 0xFFFF) != 0x8000) {
                    continue;
                  }
                  uint32_t rt = (w0 >> 21) & 31;
                  for (uint32_t j = 1; j <= 3; ++j) {
                    uint32_t wj = xe::load_and_swap<uint32_t>(
                        hmem->TranslateVirtual(a + j * 4));
                    if ((wj >> 26) == 24 && ((wj >> 21) & 31) == rt &&
                        (wj & 0xFFFF) == 0x0004) {
                      found = a - 0xC;
                      break;
                    }
                  }
                }
                if (found) {
                  XELOGI("Guide: handler located by signature at {:08X} "
                         "(base+{:X})", found, found - hbase);
                  // The handler also tells us where it keeps the Guide
                  // object. At +0x34 it builds the high half of that address
                  // and at +0x3C loads through it; the two hud builds are
                  // instruction-identical here, differing only in the
                  // displacement (dashroot 91400690, retail 913FF690). Decode
                  // it rather than hardcoding either.
                  uint32_t wi_hi = xe::load_and_swap<uint32_t>(
                      hmem->TranslateVirtual(found + 0x34u));
                  uint32_t wi_lo = xe::load_and_swap<uint32_t>(
                      hmem->TranslateVirtual(found + 0x3Cu));
                  uint32_t xi = FindHudSig(hmem, hbase, hsz, kHudXuiInitSig,
                                           14);
                  uint32_t rr = FindHudSig(hmem, hbase, hsz, kHudRenderSig, 14);
                  if (xi) g_hud_xuiinit = xi;
                  if (rr) g_hud_render = rr;
                  kernel::xboxkrnl::SetHudEntries(xi, rr);
                  XELOGI("Guide: hud entries by signature - xuiinit {:08X} "
                         "(base+{:X}), render {:08X} (base+{:X})",
                         xi, xi ? xi - hbase : 0, rr, rr ? rr - hbase : 0);
                  if ((wi_hi >> 26) == 15 && (wi_lo >> 26) == 32) {
                    int32_t disp = static_cast<int16_t>(wi_lo & 0xFFFF);
                    uint32_t slot = ((wi_hi & 0xFFFF) << 16) + disp;
                    guide_obj_slot_ = slot;
                    XELOGI("Guide: object slot decoded from handler = {:08X}",
                           slot);
                  }
                }
                h = found ? found : (hbase + 0x69C0);
                XELOGW("Guide: HLE map empty (LLE xam owns the registration); "
                       "using hud base {:08X} + 0x69C0 -> {:08X}",
                       hud->xex_module()->base_address(), h);
              }
              auto* mem = ks->memory();
              // hud copies 0x47C bytes out of the inner struct and writes the
              // result size through the third argument, so that is a pointer,
              // not a size.
              uint32_t inner = mem->SystemHeapAlloc(0x500, 16);
              uint32_t buf = mem->SystemHeapAlloc(0x40, 16);
              uint32_t out_sz = mem->SystemHeapAlloc(0x10, 16);
              XELOGI("Guide: buffers inner={:08X} buf={:08X} out_sz={:08X}",
                     inner, buf, out_sz);

              // Publish these immediately - the XUI registration below takes
              // a while, and a Guide button press during that window would
              // otherwise find no handler recorded.
              guide_handler_ = h;
              guide_hud_base_ = hud->xex_module()->base_address();
              guide_buf_ = buf;
              guide_out_sz_ = out_sz;
              std::memset(mem->TranslateVirtual(inner), 0, 0x500);
              std::memset(mem->TranslateVirtual(buf), 0, 0x40);
              std::memset(mem->TranslateVirtual(out_sz), 0, 0x10);
              auto* iw = mem->TranslateVirtual<xe::be<uint32_t>*>(inner);
              iw[2] = static_cast<uint32_t>(cvars::guide_subcommand);
              auto* bw = mem->TranslateVirtual<xe::be<uint32_t>*>(buf);
              bw[0] = 1;
              bw[1] = inner;
              // 0x80000004 constructs the Guide object and stores it at
              // 91400690. Every other message loads that object and would
              // dereference null if it does not exist yet, so always create
              // first.
              // hud's scenes derive from XUI built-ins ("BaseScene" has parent
              // "XuiScene"), and CBaseScene::Register fails with 0x80300006
              // because the parent is not in xam's class registry. The
              // registrars that populate it have no callers anywhere inside
              // xam - something outside the module drives them on hardware,
              // the same shape as the heap-init routine. Drive them here.
              // xam's core XUI class registrars are unreachable from its own
              // code: no bl, no b, and no pointer table outside .pdata. They
              // build a descriptor with the class name at +84 and the parent
              // at +88 (the hud-side wrappers use +100/+104) and call the
              // register entry at 81956318. Drive them directly. The list is
              // run twice because registration resolves the parent at call
              // time, so classes registered in the first pass unblock the
              // ones that depend on them in the second.
              if (cvars::lle_xam_xui_init) {
                static const uint32_t kXuiCore[] = {
                    0x8194F860u, 0x8194F950u, 0x8194FAC0u, 0x8194FBE8u,
                    0x8194FCD8u, 0x8194FDC8u, 0x8194FEB0u, 0x8194FFA0u,
                    0x81950090u, 0x81950178u, 0x81950268u, 0x81950350u,
                    0x819504C0u, 0x819505B0u, 0x819506A0u, 0x819507D0u,
                    0x819508C0u, 0x819509B0u, 0x81950AA0u, 0x81950B88u,
                    0x81950C78u, 0x81950D68u, 0x81950E58u, 0x81950F48u,
                    0x81951038u, 0x81951128u, 0x81951218u, 0x81951308u,
                    0x819513F8u, 0x81952428u, 0x819524D0u, 0x81952580u,
                    0x81952628u, 0x81953298u, 0x81953338u, 0x819533E8u,
                    0x819536B0u, 0x81970290u};
                int prev_ok = -1;
                for (int pass = 0; pass < 8; ++pass) {
                  int ok = 0, fail = 0;
                  for (uint32_t reg : kXuiCore) {
                    uint64_t rargs[] = {0};
                    uint64_t rr = ks->processor()->Execute(ts, reg, rargs,
                                                           xe::countof(rargs));
                    if (static_cast<uint32_t>(rr) & 0x80000000u) {
                      ++fail;
                    } else {
                      ++ok;
                    }
                  }
                  XELOGI("Guide: XUI core pass {}: {} ok, {} failed", pass, ok,
                         fail);
                  if (ok == prev_ok) {
                    break;  // converged - remaining failures are not ordering
                  }
                  prev_ok = ok;
                }
                for (uint32_t reg : {0x817503E8u, 0x8199BE08u, 0x8176B2C8u}) {
                  uint64_t rargs[] = {0};
                  XELOGI("Guide: XUI registrar {:08X}", reg);
                  uint64_t rr = ks->processor()->Execute(ts, reg, rargs,
                                                         xe::countof(rargs));
                  XELOGI("Guide: registrar {:08X} returned {:08X}", reg,
                         static_cast<uint32_t>(rr));
                }
              }
              // Read xam's XUI class registry directly instead of inferring
              // its state from HRESULTs. 81950D60 takes the critical section
              // at 0x81D6D030 and searches the structure at 0x81D6D508; if
              // that is all zeroes the registry was never initialised, which
              // is a different problem from a class merely being absent.
              {
                auto* mem2 = ks->memory();
                // Both constants are dashroot-xam addresses; on another build
                // they need not be mapped. Reading them unguarded faulted on
                // the Guide Loader thread (phase 407).
                // Image-extent test, not a page query - see the note at the
                // xam_mapped helper: QueryRangeAccess reports kNoAccess for
                // readable image addresses.
                uint32_t m2lo = 0, m2hi = 0;
                if (xam_mod_for_guide && xam_mod_for_guide->xex_module()) {
                  m2lo = xam_mod_for_guide->xex_module()->base_address();
                  m2hi = m2lo + xam_mod_for_guide->xex_module()->image_size();
                }
                auto m2 = [m2lo, m2hi](uint32_t a, uint32_t len) -> bool {
                  return m2lo && a >= m2lo && a + len <= m2hi;
                };
                if (m2(0x81D6D030u, 32) && m2(0x81D6D508u, 32)) {
                  std::string cs, tb;
                  for (int i = 0; i < 8; ++i) {
                    cs += fmt::format("{:08X} ", xe::load_and_swap<uint32_t>(
                        mem2->TranslateVirtual(0x81D6D030u + i * 4)));
                    tb += fmt::format("{:08X} ", xe::load_and_swap<uint32_t>(
                        mem2->TranslateVirtual(0x81D6D508u + i * 4)));
                  }
                  XELOGI("Guide: XUI crit @81D6D030: {}", cs);
                  XELOGI("Guide: XUI registry @81D6D508: {}", tb);
                } else {
                  XELOGW("Guide: XUI crit/registry constants not mapped - "
                         "skipping (expected on a non-dashroot xam)");
                }
              }
              // Prefer xam's own entry point over a hand-built message.
              // Phase 18: the payload layout was guessed, and xam stores
              // the buffer as a typed sub-object and virtual-calls it,
              // which faults. XamShowGuideUI (ordinal 0x304) makes xam
              // construct its own correctly-shaped message.
              if (cvars::lle_show_guide && xam_mod_for_guide) {
                // Nothing inside xam registers its system apps: the
                // static descriptor table at 0x81604368 (ids 0xEF-0xFD)
                // is walked by 8177FE50, reached only from the callerless
                // root 81751428. XamShowGuideUI sends to app 0xFE, which
                // is absent from that table, so drive the root first and
                // see what the table looks like afterwards.
                if (cvars::lle_xam_sysapp_init) {
                  // 81751428 is the outer root, but it spawns a worker and
                  // waits on it, which never completes here. 8177FE50 is the
                  // function that actually walks the descriptor table and
                  // fills in the app entries, so call that directly.
                  uint64_t sa[] = {0};
                  // Walker first (fills 0xEF-0xFD), then the outer root,
                  // which creates XamApp's thread - the only thing that can
                  // register 0xFE. The root blocks, so run it last.
                  XELOGI("Guide: sysapp table walk 8177FE50");
                  ks->processor()->Execute(ts, kernel::xboxkrnl::GuideConst(0x8177FE50u), sa,
                                           xe::countof(sa));
                  // 81751428 turned out to be a teardown path: it runs an
                  // app whose main is the ExTerminateTitleProcess wrapper.
                  // 81A34E78 is the factory that constructs XamApp itself -
                  // it calls XamApp's constructor (81A4E640) three times and
                  // has no callers anywhere in xam.
                  // XamApp's factory runs the app's message pump, so it
                  // never returns - that is what a system app does. Run it on
                  // its own guest thread and carry on, otherwise the Guide
                  // sequence blocks here forever.
                  auto app_thread = kernel::object_ref<kernel::XHostThread>(
                      new kernel::XHostThread(ks, 1024 * 1024, 0, [ks]() -> int {
                        auto* ats =
                            kernel::XThread::GetCurrentThread()->thread_state();
                                                // 81A34E78 is a constructor: it stores through r3.
                        // Give it a real zeroed buffer instead of address 0.
                        // Size it properly: the factory addresses sub-objects
                        // with addis/addi pairs (addis +0x10000 then a negative
                        // addi), so a naive scan of small stw displacements
                        // reported 76 bytes when the real span is 33956
                        // (0x84A4). Allocate 64 KiB - comfortably past that.
                        const uint32_t kSelfSize = 0x10000;
                        uint32_t self =
                            ks->memory()->SystemHeapAlloc(kSelfSize, 128);
                        std::memset(ks->memory()->TranslateVirtual(self), 0,
                                    kSelfSize);
                        XELOGI("Guide: XamApp factory 81A34E78 this={:08X}",
                               self);
                        uint64_t aa[] = {self};
                        ks->processor()->Execute(ats, kernel::xboxkrnl::GuideConst(0x81A34E78u), aa,
                                                 xe::countof(aa));
                        XELOGI("Guide: XamApp factory returned");
                        // The factory constructs three XamApp instances at
                        // this+10832, +17880 and +24928 (the three calls to
                        // the ctor 81A4E640). XamApp's entry 81A4E3D0 reads
                        // this+204/+232/+236, so run it on the first instance
                        // - that is the chain that reaches the 0x1015 handler.
                        for (uint32_t inst : {10832u, 17880u, 24928u}) {
                          uint32_t obj = self + inst;
                          XELOGI("Guide: XamApp entry 81A4E3D0 this={:08X} "
                                 "(+{})", obj, inst);
                          uint64_t ea[] = {obj};
                          uint64_t er = ks->processor()->Execute(
                              ats, kernel::xboxkrnl::GuideConst(0x81A4E3D0u), ea, xe::countof(ea));
                          XELOGI("Guide: XamApp entry returned {:08X}",
                                 static_cast<uint32_t>(er));
                        }

                        // Register app 0xFE by hand. xam locates a system app
                        // at 0x81D4E550 - id*192 (81786078), and the validator
                        // 817863F8 requires BOTH +8 and +16 non-zero, with the
                        // handler at +12. Nothing in xam's own startup runs
                        // here to do this, so point 0xFE straight at xam's real
                        // XamApp message handler 81A5F220 - the dispatcher that
                        // owns the 0x1015 case XamShowGuideUI sends.
                        if (cvars::lle_xam_fake_app_fe) {
                          uint32_t fe = 0x81D4E550u - 0xFEu * 192u;
                          auto* p8 = ks->memory()->TranslateVirtual(fe);
                          uint32_t ctx = self + 10832u;  // first XamApp
                          xe::store_and_swap<uint32_t>(p8 + 8, ctx);
                          xe::store_and_swap<uint32_t>(p8 + 12, 0x81A5F220u);
                          xe::store_and_swap<uint32_t>(p8 + 16, 1u);
                          xe::store_and_swap<uint32_t>(p8 + 24, 0xFEu);
                          XELOGI("Guide: registered app FE @{:08X} "
                                 "handler=81A5F220 ctx={:08X}", fe, ctx);
                          // Same treatment for the apps xam ships in its
                          // static descriptor table at 0x81604368: entries of
                          // {name, appId, handler, flags}, ids 0xEF-0xFD. The
                          // table walker 8177FE50 returns success but leaves
                          // them unregistered, and something polls 0xFC
                          // (XLiveBase) thousands of times a second when it is
                          // missing. Register each entry that has a handler.
                          auto* mem4 = ks->memory();
                          for (uint32_t i = 0; i < 15; ++i) {
                            uint32_t rec = 0x81604368u + i * 16u;
                            uint32_t id = xe::load_and_swap<uint32_t>(
                                mem4->TranslateVirtual(rec + 4));
                            uint32_t handler = xe::load_and_swap<uint32_t>(
                                mem4->TranslateVirtual(rec + 8));
                            if (id < 0xEFu || id > 0xFDu || !handler) {
                              continue;
                            }
                            uint32_t e = 0x81D4E550u - id * 192u;
                            auto* ep = mem4->TranslateVirtual(e);
                            xe::store_and_swap<uint32_t>(ep + 8, ctx);
                            xe::store_and_swap<uint32_t>(ep + 12, handler);
                            xe::store_and_swap<uint32_t>(ep + 16, 1u);
                            xe::store_and_swap<uint32_t>(ep + 24, id);
                            XELOGI("Guide: registered app {:02X} @{:08X} "
                                   "handler={:08X}", id, e, handler);
                          }
                          // 81A5F220 is not merely a dispatcher - it is
                          // XamApp's run function. It initialises (81A63BB8,
                          // 81A63D28, 81A64438) and the 0x1015 comparison sits
                          // inside its message loop, so this is the pump that
                          // would drain what XMsgStartIORequest queues. It
                          // takes this in r3 and a 2060-byte scratch buffer in
                          // r4 (zeroed at entry). Run it on its own thread; it
                          // is a loop and will not return.
                          uint32_t scratch =
                              ks->memory()->SystemHeapAlloc(0x1000, 128);
                          std::memset(ks->memory()->TranslateVirtual(scratch),
                                      0, 0x1000);
                          auto pump = kernel::object_ref<kernel::XHostThread>(
                              new kernel::XHostThread(
                                  ks, 1024 * 1024, 0, [ks, ctx, scratch]() -> int {
                                    auto* pts = kernel::XThread::
                                        GetCurrentThread()->thread_state();
                                    uint64_t pa[] = {ctx, scratch};
                                    XELOGI("Guide: XamApp pump 81A5F220 "
                                           "this={:08X} buf={:08X}", ctx,
                                           scratch);
                                    ks->processor()->Execute(
                                        pts, kernel::xboxkrnl::GuideConst(0x81A5F220u), pa, xe::countof(pa));
                                    XELOGI("Guide: XamApp pump returned");
                                    return 0;
                                  },
                                  ks->GetSystemProcess()));
                          pump->set_name("XamApp pump");
                          if (XFAILED(pump->Create())) {
                            XELOGE("Guide: failed to create pump thread");
                          }
                          xe::threading::Sleep(std::chrono::seconds(3));
                          // Re-read app 0xFE after the pump has had time to
                          // run. If the pump publishes its wait event (or a
                          // queue) into the app entry, it will appear here -
                          // that is the field XMsgStartIORequest would need in
                          // order to wake it, and the one our hand-written
                          // entry omits.
                          {
                            uint32_t fe2 = 0x81D4E550u - 0xFEu * 192u;
                            auto* q = ks->memory()->TranslateVirtual(fe2);
                            std::string d1, d2;
                            for (int i = 0; i < 12; ++i) {
                              d1 += fmt::format("{:08X} ",
                                  xe::load_and_swap<uint32_t>(q + i * 4));
                            }
                            for (int i = 12; i < 24; ++i) {
                              d2 += fmt::format("{:08X} ",
                                  xe::load_and_swap<uint32_t>(q + i * 4));
                            }
                            XELOGI("Guide: app FE after pump [0..47]:  {}", d1);
                            XELOGI("Guide: app FE after pump [48..95]: {}", d2);
                          }
                        }
                        return 0;
                      }, ks->GetSystemProcess()));
                  app_thread->set_name("XamApp");
                  if (XFAILED(app_thread->Create())) {
                    XELOGE("Guide: failed to create XamApp thread");
                  }
                  // Give it time to construct and register before we look.
                  xe::threading::Sleep(std::chrono::seconds(5));
                }
                // xam locates a system app as 0x81D4E550 - appid*192
                // (81786078), then 817863F8 requires fields +8 and +16
                // to be set. XamShowGuideUI sends to app 0xFE, whose
                // entry is 0x81D426D0. Read it before calling.
                {
                  auto* m3 = ks->memory();
                  for (uint32_t id : {0xFEu, 0xFFu}) {
                    uint32_t e = 0x81D4E550u - id * 192u;
                    std::string f;
                    for (int i = 0; i < 8; ++i) {
                      f += fmt::format("{:08X} ", xe::load_and_swap<uint32_t>(
                          m3->TranslateVirtual(e + i * 4)));
                    }
                    XELOGI("Guide: app {:02X} entry @{:08X}: {}", id, e, f);
                  }
                  for (uint32_t ord : {0x24Bu, 0x24Cu, 0x304u}) {
                    XELOGI("Guide: xam ordinal {:X} -> {:08X}", ord,
                           xam_mod_for_guide->GetProcAddressByOrdinal(ord));
                  }
                  XELOGI("Guide: current-app ptr @81D426C8 = {:08X}",
                         xe::load_and_swap<uint32_t>(
                             m3->TranslateVirtual(0x81D426C8u)));
                }
                uint32_t g = xam_mod_for_guide->GetProcAddressByOrdinal(0x304);
                // Xenia declares the whole app lifecycle API in its xam
                // table - XamAppLoad (0x244), XamAppRequestLoad (0x248),
                // XamAppLoadPass2SysApps (0x254), XamAppRequestLoadEx (0x299)
                // - and implements none of it. That API *is* the hosting
                // mechanism hud needs: something has to load and host system
                // apps, and nothing in Xenia does. Real xam has these, so try
                // driving them directly.
                // XamLoadSysApp(0x251) is the specific "load a system app"
                // entry; it takes (id, arg) and does not store through the
                // incoming r3, so it is safe to drive. hud registers as app
                // 0xFF, so try that and the ids in xam's descriptor table.
                // Guest breakpoint on xam's sys-app loader (81786788). Its
                // first argument is compared against 4, but the sys-app main
                // appears to pass a table address - the static reading could
                // not resolve which. A breakpoint reads the real registers.
                static std::unique_ptr<cpu::Breakpoint> loader_bp;
                if (cvars::lle_xam_trace_loader && !loader_bp) {
                  loader_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x913FE6D4ull,
                      [](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                         uint64_t host_pc) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) {
                          XELOGI("LoaderTrace: hit, no thread context");
                          return;
                        }
                        auto* c = th->thread_state()->context();
                        {
                          uint32_t np = static_cast<uint32_t>(c->r[3]);
                          std::string nm;
                          if (np) {
                            auto* mem = th->kernel_state()->memory();
                            for (int i = 0; i < 40; ++i) {
                              uint16_t ch = xe::load_and_swap<uint16_t>(
                                  mem->TranslateVirtual(np + i * 2));
                              if (!ch) break;
                              nm.push_back(static_cast<char>(ch & 0x7F));
                            }
                          }
                          XELOGI("LoaderTrace: FindClass name '{}'", nm);
                        }
                        XELOGI(
                            "LoaderTrace: SCENECREATE 913FE6D4 lr={:08X} r3={:08X} "
                            "r4={:08X} r5={:08X} r6={:08X} r7={:08X} "
                            "r29={:08X} r30={:08X}",
                            static_cast<uint32_t>(c->lr),
                            static_cast<uint32_t>(c->r[3]),
                            static_cast<uint32_t>(c->r[4]),
                            static_cast<uint32_t>(c->r[5]),
                            static_cast<uint32_t>(c->r[6]),
                            static_cast<uint32_t>(c->r[7]),
                            static_cast<uint32_t>(c->r[29]),
                            static_cast<uint32_t>(c->r[30]));
                      });
                  // AddBreakpoint installs it when the processor is running.
                  ks->processor()->AddBreakpoint(loader_bp.get());
                  XELOGI(
                      "LoaderTrace: bp exec_state={} patched={} host={:X}",
                      static_cast<int>(ks->processor()->execution_state()),
                      loader_bp->backend_data().size(),
                      loader_bp->backend_data().empty()
                          ? 0ull
                          : loader_bp->backend_data()[0].first);
                  // Control: the same mechanism on XamShowGuideUI's worker
                  // (81787430), which is definitely executed. If the control
                  // never fires either, breakpoints are not working here and
                  // the loader result means nothing.
                  static std::unique_ptr<cpu::Breakpoint> ctl_bp;
                  ctl_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x81787430ull,
                      [](cpu::Breakpoint*, cpu::ThreadDebugInfo*, uint64_t) {
                        XELOGI("LoaderTrace: CONTROL hit at 81787430");
                      });
                  // Disabled: a breakpoint hit does not resume, so the entry
                  // control kills the thread before the call site is reached.
                  // ks->processor()->AddBreakpoint(ctl_bp.get());
                  // XamShowGuideUI's unconditional callee: ghidra 8178D730,
                  // runtime 8178D730 - 0x7200 = 81786530.
                  static std::unique_ptr<cpu::Breakpoint> work_bp;
                  work_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x81787470ull,
                      [](cpu::Breakpoint*, cpu::ThreadDebugInfo*, uint64_t) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) {
                          XELOGI("LoaderTrace: WORKER hit, no thread");
                          return;
                        }
                        auto* c = th->thread_state()->context();
                        XELOGI(
                            "LoaderTrace: CALLSITE 81787470 r3={:08X} r4={:08X} "
                            "r5={:08X}",
                            static_cast<uint32_t>(c->r[3]),
                            static_cast<uint32_t>(c->r[4]),
                            static_cast<uint32_t>(c->r[5]));
                      });
                  // Disabled for this run: one hit per session (see below).
                  // ks->processor()->AddBreakpoint(work_bp.get());
                  XELOGI("LoaderTrace: 81787470 patched={}",
                         work_bp->backend_data().size());
                  XELOGI("LoaderTrace: 81787430 patched={} host={:X}",
                         ctl_bp->backend_data().size(),
                         ctl_bp->backend_data().empty()
                             ? 0ull
                             : ctl_bp->backend_data()[0].first);
                }
                if (cvars::lle_xam_app_host) {
                  uint32_t load_fn =
                      xam_mod_for_guide->GetProcAddressByOrdinal(0x251);
                  XELOGI("Guide: XamLoadSysApp -> {:08X}", load_fn);
                  if (load_fn) {
                    for (uint32_t id : {0xFFu, 0xFEu, 0xF7u}) {
                      uint64_t la[] = {id, 0};
                      uint64_t lr = ks->processor()->Execute(
                          ts, load_fn, la, xe::countof(la));
                      XELOGI("Guide: XamLoadSysApp({:02X}) -> {:08X}", id,
                             static_cast<uint32_t>(lr));
                    }
                  }
                  uint32_t msg_fn =
                      xam_mod_for_guide->GetProcAddressByOrdinal(0x247);
                  XELOGI("Guide: XamSendMessageToLoadedApps -> {:08X}", msg_fn);
                }
                XELOGI("Guide: XamShowGuideUI (ord 0x304) -> {:08X}", g);
                if (g) {
                  uint64_t ga[] = {0};
                  uint64_t gr = ks->processor()->Execute(ts, g, ga,
                                                         xe::countof(ga));
                  XELOGI("Guide: XamShowGuideUI returned {:08X}",
                         static_cast<uint32_t>(gr));
                } else {
                  XELOGE("Guide: could not resolve ordinal 0x304");
                }
                return 0;
              }
              // Remember these so the Guide button can re-dispatch later.
              guide_handler_ = h;
              guide_buf_ = buf;
              guide_out_sz_ = out_sz;
              XELOGI("Guide: create msg=80000004 subcmd={} -> {:08X}",
                     int32_t(cvars::guide_subcommand), h);
              uint64_t cargs[] = {0x80000004ull, buf, out_sz};
              uint64_t cres = ks->processor()->Execute(ts, h, cargs,
                                                       xe::countof(cargs));
              XELOGI("Guide: create returned {:08X}",
                     static_cast<uint32_t>(cres));
              // hud's handler stores the Guide object at 0x91400690 and every
              // non-create message loads it from there, so a null here means
              // the later dispatch would fault rather than draw.
              XELOGI("Guide: object @91400690 = {:08X}",
                     xe::load_and_swap<uint32_t>(
                         mem->TranslateVirtual(0x91400690u)));
              // hud is a system app: on hardware the system creates its
              // thread and runs its render loop. Nothing here does, which is
              // why every message returns success and nothing draws. hud's
              // own XUI entry points, located by scanning .text in flat
              // address space for calls to the import thunks:
              //   base + 0xA898  calls XuiInit + XuiRenderCreateDC
              //   base + 0xAB28  calls XuiRenderBegin/End/Present
              // Both take `this` in r3 and only read from it. Drive them
              // against the Guide object the create message just produced.
              if (cvars::lle_guide_draw) {
                uint32_t hb = hud->xex_module()->base_address();
                uint32_t slot1 = guide_obj_slot_ ? guide_obj_slot_ : 0x91400690u;
                uint32_t obj = xe::load_and_swap<uint32_t>(
                    mem->TranslateVirtual(slot1));
                // 0x91400690 is where *dashroot's* hud stores the Guide
                // object. On the retail hud that slot holds unrelated data -
                // it read 6E74726F ("ntro"), which was then passed as `this`
                // and crashed inside hud (phases 410, 412). "Non-zero" is not
                // a validity test: require it to be a mapped guest heap
                // pointer before handing it to guest code.
                bool obj_ok = false;
                if (obj >= 0x30000000u && obj < 0x50000000u) {
                  auto* oh = mem->LookupHeap(obj);
                  obj_ok = oh && oh->QueryRangeAccess(obj, obj + 0x20u) !=
                                     xe::memory::PageAccess::kNoAccess;
                }
                if (!obj_ok && obj) {
                  XELOGW("Guide: object slot 91400690 = {:08X} is not a mapped "
                         "heap pointer - not calling hud with it (the create "
                         "returned an error, or this hud stores it elsewhere)",
                         obj);
                  obj = 0;
                }
                if (obj) {
                  uint64_t ia[] = {obj};
                  if (cvars::guide_force_obj14) {
                    xe::store_and_swap<uint32_t>(
                        mem->TranslateVirtual(obj + 0x14u),
                        cvars::guide_force_obj14);
                    XELOGI("Guide: forced [obj+14] = {:08X} so init takes the "
                           "XuiRenderCreateDC branch",
                           uint32_t(cvars::guide_force_obj14));
                  }
                  // Phase 1097r: THIS is the site that actually runs - the
                  // one whose object (401C1CC0) matches the one captured at
                  // hud's init, and whose Execute leaves the BCBCBCBC link
                  // register that identified the call as the host's. The two
                  // sites in on_guide_button_pressed and the one in
                  // GuideBootstrap are not reached in this configuration; an
                  // earlier attempt to gate the bootstrap one measured
                  // nothing because its block never ran (the "NOT calling"
                  // line never appeared - the arming control caught it).
                  uint64_t ir = 0;
                  if (cvars::guide_guest_hud_init) {
                    XELOGI("Guide: NOT calling hud's init {:08X} this={:08X} - "
                           "leaving it to the guest (guide_guest_hud_init)",
                           (g_hud_xuiinit ? g_hud_xuiinit : hb + 0xA898u), obj);
                  } else {
                    XELOGI("Guide: hud XUI init {:08X} this={:08X}", (g_hud_xuiinit ? g_hud_xuiinit : hb + 0xA898u),
                           obj);
                    ir = ks->processor()->Execute(ts, (g_hud_xuiinit ? g_hud_xuiinit : hb + 0xA898u), ia,
                                                  xe::countof(ia));
                    XELOGI("Guide: hud XUI init returned {:08X}",
                           static_cast<uint32_t>(ir));
                  }
                  // [obj+12] is null on this path, so the device context
                  // XuiRenderCreateDC produced is stored elsewhere in the
                  // object. Dump the head of it to find the pointer: a DC
                  // lives in the 0x40000000 heap like the object itself.
                  {
                    std::string ow;
                    for (uint32_t w = 0; w < 32; ++w) {
                      uint32_t v = xe::load_and_swap<uint32_t>(
                          mem->TranslateVirtual(obj + w * 4));
                      ow += fmt::format("{:02X}:{:08X} ", w * 4, v);
                    }
                    XELOGI("Guide: obj {:08X} head {}", obj, ow);
                  }
                  // The device context hangs off [this+12]; [dc+0x134] is
                  // the null-render flag that decides whether XuiRenderBegin
                  // dispatches vtable[20] and the emitter ever builds a
                  // DRAW_INDX. Report it alongside the GPU draw count so a
                  // cleared flag that still produces no draws is
                  // distinguishable from a flag that never cleared.
                  auto* gsd = ks->emulator()->graphics_system();
                  auto* cpd = gsd ? gsd->command_processor() : nullptr;
                  uint32_t prev_draws =
                      cpd ? cpd->guide_draw_count_ : 0u;
                  // Phase 585: the coverage readback sits AFTER this loop,
                  // and 6000 frames outlasts every run (covrun kills at 60s;
                  // the loop reaches ~frame 3000 by then), so the instrument
                  // could never report from here. When coverage is requested,
                  // bound the loop so the readback is actually reached.
                  // Phase 587: coverage on 913EAB28 shows it executes 17 of
                  // 46 instructions on every one of 2000 frames - it loads
                  // [this+0xC], passes it to 913FE874, and returns at
                  // 913EAB50 when that call reports failure. [this+0xC] is
                  // null (the same field this loop reads as obj+12), so the
                  // render never begins. Hand it the one DC known to be
                  // fully constructed.
                  // Must be applied here, not from the xam bootstrap: that
                  // path runs after hud's render has already faulted.
                  // 81A02AD8 resets the reservation window to the device's
                  // own 0x12C0-byte buffer on every call and returns 0, so a
                  // widened window cannot survive to the fit test. Nop the
                  // two stores; the flag and [dev+0x38] stores are left
                  // alone.
                  if (cvars::guide_patch_window_reset) {
                    kernel::xboxkrnl::GuidePatchWord(0x81A02B08u, 0x917F0030u,
                                                     0x60000000u,
                                                     "WindowResetPatch.cur");
                    kernel::xboxkrnl::GuidePatchWord(0x81A02B14u, 0x915F0034u,
                                                     0x60000000u,
                                                     "WindowResetPatch.end");
                  }
                  if (cvars::guide_patch_present_rt) {
                    kernel::xboxkrnl::GuidePatchWord(0x819DE934u, 0x409A00FCu,
                                                     0x480000FCu,
                                                     "PresentRTPatch");
                  }
                  const int kGuideFrames = cvars::guide_coverage_fn ? 2000 : 6000;
                  for (int frame = 0; frame < kGuideFrames; ++frame) {
                    // The bootstrap DC is published on the title's render
                    // thread and is still null when this loop starts (it
                    // appears by ~frame 500), so this cannot be a one-shot
                    // before the loop - retry until it lands.
                    if (cvars::guide_set_render_dc) {
                      uint32_t cur = xe::load_and_swap<uint32_t>(
                          mem->TranslateVirtual(obj + 12));
                      if (!cur) {
                        uint32_t bdc = kernel::xboxkrnl::GuideBootDc();
                        if (bdc) {
                          xe::store_and_swap<uint32_t>(
                              mem->TranslateVirtual(obj + 12), bdc);
                          XELOGI("GuideSetRenderDC: frame {} [{:08X}+0C] "
                                 "-> {:08X}", frame, obj, bdc);
                          // The device hud renders against is not the DC and
                          // not the one XamDeviceSlot reports - it is one hop
                          // out, and both its render-target slots are null.
                          if (cvars::guide_bind_boot_rt) {
                            uint32_t sub = xe::load_and_swap<uint32_t>(
                                mem->TranslateVirtual(bdc + 0x1CCu));
                            uint32_t dev =
                                sub ? xe::load_and_swap<uint32_t>(
                                          mem->TranslateVirtual(sub + 0x0Cu))
                                    : 0u;
                            XELOGI("GuideBindBootRt: dc={:08X} sub={:08X} "
                                   "dev={:08X}", bdc, sub, dev);
                            // Phase 628: XuiRenderPresent -> DC vtable[0x54]
                            // (818F9290) -> wrapper vtable[0x60], and that
                            // last call never returns. Name it.
                            {
                              uint32_t wvt =
                                  sub ? xe::load_and_swap<uint32_t>(
                                            mem->TranslateVirtual(sub))
                                      : 0u;
                              XELOGI("GuidePresentChain: dc={:08X} "
                                     "[dc+134]={:08X} wrapper={:08X} "
                                     "wvt={:08X} wvt[60]={:08X}",
                                     bdc,
                                     xe::load_and_swap<uint32_t>(
                                         mem->TranslateVirtual(bdc + 0x134u)),
                                     sub, wvt,
                                     wvt ? xe::load_and_swap<uint32_t>(
                                               mem->TranslateVirtual(wvt +
                                                                     0x60u))
                                         : 0u);
                            }
                            // Phase 595: 819E2ED0 loads [dev+0x3050] and the
                            // crash shows it holding 5 on the title device.
                            // Compare the same field on both devices - if
                            // hud's own device has a pointer there and the
                            // title's has a small integer, the two are not
                            // layout-compatible and the redirect is wrong in
                            // principle, not merely incomplete.
                            {
                              uint32_t td = xe::load_and_swap<uint32_t>(
                                  mem->TranslateVirtual(0x801E6FC4u));
                              XELOGI("GuideDev3050: hud_dev={:08X} "
                                     "[+3050]={:08X} | title_dev={:08X} "
                                     "[+3050]={:08X}",
                                     dev,
                                     dev ? xe::load_and_swap<uint32_t>(
                                               mem->TranslateVirtual(
                                                   dev + 0x3050u)) : 0u,
                                     td,
                                     td ? xe::load_and_swap<uint32_t>(
                                              mem->TranslateVirtual(
                                                  td + 0x3050u)) : 0u);
                            }
                            if (cvars::guide_render_on_xam_device && sub) {
                              // Phase 641: prefer the device mode 1 actually
                              // built - it owns the ring and the interrupt
                              // handler. XamDeviceSlot reports the *global*,
                              // which in these runs is 40870D00, the same
                              // device the present already uses, so using it
                              // made the redirect a no-op.
                              uint32_t xdev =
                                  kernel::xboxkrnl::GuideMode1Device();
                              if (!xdev) {
                                uint32_t xslot =
                                    kernel::xboxkrnl::XamDeviceSlot();
                                xdev = xslot ? xe::load_and_swap<uint32_t>(
                                                   mem->TranslateVirtual(xslot))
                                             : 0u;
                              }
                              if (xdev && xdev != dev) {
                                xe::store_and_swap<uint32_t>(
                                    mem->TranslateVirtual(sub + 0x0Cu), xdev);
                                XELOGI("GuideXamDev: [{:08X}+0C] {:08X} -> "
                                       "{:08X}", sub, dev, xdev);
                                dev = xdev;
                              }
                            }
                            // hud's device is a third device, distinct from
                            // both xam's and the title's, and only the
                            // title's buffers are ever submitted. Redirect
                            // the wrapper's device pointer at the title's.
                            if (cvars::guide_render_on_title_device && sub) {
                              uint32_t tdev = xe::load_and_swap<uint32_t>(
                                  mem->TranslateVirtual(0x801E6FC4u));
                              if (tdev) {
                                xe::store_and_swap<uint32_t>(
                                    mem->TranslateVirtual(sub + 0x0Cu), tdev);
                                XELOGI("GuideTitleDev: [{:08X}+0C] {:08X} -> "
                                       "{:08X}", sub, dev, tdev);
                                dev = tdev;
                              } else {
                                XELOGW("GuideTitleDev: title device is null");
                              }
                            }
                            // Phase 630: 819FCE50 kicks the GPU through
                            // [dev+0x2B14], which only mode-1 setup writes.
                            // On the device the present path uses it is null,
                            // and the resulting fault truncates the present.
                            // Phase 632: 819FCE50 reads [r31+0x2B14] with
                            // r31 = the TITLE's device (phase 615), not the
                            // one reached from the boot DC, so patch both.
                            if (cvars::guide_fix_kick_ptr) {
                              uint32_t tdev = xe::load_and_swap<uint32_t>(
                                  mem->TranslateVirtual(0x801E6FC4u));
                              if (tdev) {
                                uint32_t tcur = xe::load_and_swap<uint32_t>(
                                    mem->TranslateVirtual(tdev + 0x2B14u));
                                if (!tcur) {
                                  uint32_t tb = mem->SystemHeapAlloc(
                                      0x20, 16, kSystemHeapPhysical);
                                  if (tb) {
                                    std::memset(mem->TranslateVirtual(tb), 0,
                                                0x20);
                                    xe::store_and_swap<uint32_t>(
                                        mem->TranslateVirtual(tdev + 0x2B14u),
                                        tb);
                                    XELOGI("GuideKickPtr: title dev {:08X} "
                                           "[2B14] <- {:08X}", tdev, tb);
                                  }
                                } else {
                                  XELOGI("GuideKickPtr: title dev {:08X} "
                                         "[2B14] already {:08X}", tdev, tcur);
                                }
                              }
                            }
                            if (cvars::guide_fix_kick_ptr && dev) {
                              uint32_t cur = xe::load_and_swap<uint32_t>(
                                  mem->TranslateVirtual(dev + 0x2B14u));
                              uint32_t m1 = kernel::xboxkrnl::GuideMode1Device();
                              uint32_t src =
                                  m1 ? xe::load_and_swap<uint32_t>(
                                           mem->TranslateVirtual(m1 + 0x2B14u))
                                     : 0u;
                              // Phase 642: XamDeviceSlot finds its slot by
                              // scanning the MODE-2 creator, so mode 1 may
                              // publish its device somewhere else. Log both
                              // documented globals alongside the captured
                              // value.
                              XELOGI("GuideDevGlobals: VdGlobalDevice[801E6FC4]"
                                     "={:08X} VdGlobalXamDevice[801E6FC8]="
                                     "{:08X} XamDeviceSlot={:08X} m1={:08X}",
                                     xe::load_and_swap<uint32_t>(
                                         mem->TranslateVirtual(0x801E6FC4u)),
                                     xe::load_and_swap<uint32_t>(
                                         mem->TranslateVirtual(0x801E6FC8u)),
                                     kernel::xboxkrnl::XamDeviceSlot()
                                         ? xe::load_and_swap<uint32_t>(
                                               mem->TranslateVirtual(
                                                   kernel::xboxkrnl::
                                                       XamDeviceSlot()))
                                         : 0u,
                                     kernel::xboxkrnl::GuideMode1Device());
                              XELOGI("GuideKickPtr: dev={:08X} [2B14]={:08X} "
                                     "m1={:08X} m1[2B14]={:08X}",
                                     dev, cur, m1, src);
                              if (!cur && src) {
                                xe::store_and_swap<uint32_t>(
                                    mem->TranslateVirtual(dev + 0x2B14u), src);
                                XELOGI("GuideKickPtr: installed {:08X}", src);
                              } else if (!cur) {
                                // Phase 631: mode 1's device is never
                                // captured here, so there is nothing to copy.
                                // 81A0FF3C stores the result of
                                // 81A0A290(0x20, 5, 1) - a 32-byte block - and
                                // the kick at 819FCE50 only writes [ptr+4].
                                // A zeroed block of the right size turns the
                                // fault into a harmless store. That does not
                                // kick the GPU, but neither does faulting.
                                uint32_t blk =
                                    mem->SystemHeapAlloc(0x20, 16,
                                                         kSystemHeapPhysical);
                                if (blk) {
                                  std::memset(mem->TranslateVirtual(blk), 0,
                                              0x20);
                                  xe::store_and_swap<uint32_t>(
                                      mem->TranslateVirtual(dev + 0x2B14u),
                                      blk);
                                  XELOGI("GuideKickPtr: substituted a zeroed "
                                         "0x20 block at {:08X}", blk);
                                }
                              }
                            }
                            if (cvars::guide_retarget_interrupt && dev) {
                              auto* gsi = ks->emulator()->graphics_system();
                              if (gsi) {
                                uint32_t icb = gsi->interrupt_callback();
                                uint32_t icd = gsi->interrupt_callback_data();
                                if (icb && icd != dev) {
                                  gsi->SetInterruptCallback(icb, dev);
                                  XELOGI("GuideRetargetInt: cb={:08X} data "
                                         "{:08X} -> {:08X}", icb, icd, dev);
                                }
                              }
                            }
                            if (cvars::guide_transplant_ring && dev) {
                              uint32_t m1t =
                                  kernel::xboxkrnl::GuideMode1Device();
                              if (m1t && m1t != dev) {
                                // The command-buffer block (2B48/2B4C/2B50/
                                // 2B58), the kick pointer (2B14) and the
                                // submit/service counters.
                                static const uint32_t kFields[] = {
                                    0x2B14u, 0x2B48u, 0x2B4Cu, 0x2B50u,
                                    0x2B58u, 0x462Cu, 0x4634u, 0x4644u,
                                    0x46C8u, 0x46CCu};
                                std::string moved;
                                for (uint32_t f : kFields) {
                                  uint32_t v = xe::load_and_swap<uint32_t>(
                                      mem->TranslateVirtual(m1t + f));
                                  xe::store_and_swap<uint32_t>(
                                      mem->TranslateVirtual(dev + f), v);
                                  moved += fmt::format("{:04X}={:08X} ", f, v);
                                }
                                XELOGI("GuideTransplant: {:08X} -> {:08X}: {}",
                                       m1t, dev, moved);
                              }
                            }
                            kernel::xboxkrnl::GuideBindDeviceRt(dev, ts);
                            if (cvars::guide_bind_boot_cmdbuf_kb) {
                              kernel::xboxkrnl::GuideBindDeviceCmdbuf(
                                  dev, ts,
                                  uint32_t(cvars::guide_bind_boot_cmdbuf_kb));
                            }
                          }
                          // Phase 588: the fault in 819DE94C is on device
                          // 40870D00, which is NOT the device the bootstrap
                          // probe reported (407CB880) - so the device must
                          // be identified here, at render time. Dump the xam
                          // device slot and the head of the DC; whichever
                          // holds 40870D00 is the path to the render-target
                          // slots that need binding.
                          uint32_t xds = kernel::xboxkrnl::XamDeviceSlot();
                          uint32_t xdev = xds
                              ? xe::load_and_swap<uint32_t>(
                                    mem->TranslateVirtual(xds))
                              : 0u;
                          XELOGI("GuideRTProbe: xam_dev={:08X} "
                                 "[32A0]={:08X} [32B0]={:08X}",
                                 xdev,
                                 xdev ? xe::load_and_swap<uint32_t>(
                                            mem->TranslateVirtual(
                                                xdev + 0x32A0u)) : 0u,
                                 xdev ? xe::load_and_swap<uint32_t>(
                                            mem->TranslateVirtual(
                                                xdev + 0x32B0u)) : 0u);
                          // 819DE8F8 does `mr r31, r3`, so the device is the
                          // caller's first argument, not a global we can
                          // read. Scan the DC for heap-pointer-shaped words
                          // and correlate the offsets against the r31 the
                          // crash dump prints for the same run.
                          std::string dh;
                          for (uint32_t w = 0; w < 128; ++w) {
                            uint32_t v = xe::load_and_swap<uint32_t>(
                                mem->TranslateVirtual(bdc + w * 4));
                            if ((v & 0xFF000000u) == 0x40000000u) {
                              dh += fmt::format("{:03X}:{:08X} ", w * 4, v);
                            }
                          }
                          XELOGI("GuideDCPtrs: {:08X} {}", bdc, dh);
                          // The device (40870D00 in the crash dump) is not
                          // in the DC itself, but [dc+0x1C8]/[dc+0x1CC] point
                          // into the same 4087xxxx region. Walk one hop and
                          // print their pointer-shaped words so the device
                          // can be reached from something we hold.
                          for (uint32_t off : {0x1C8u, 0x1CCu}) {
                            uint32_t sub = xe::load_and_swap<uint32_t>(
                                mem->TranslateVirtual(bdc + off));
                            if (!sub) continue;
                            std::string sp;
                            for (uint32_t w = 0; w < 96; ++w) {
                              uint32_t v = xe::load_and_swap<uint32_t>(
                                  mem->TranslateVirtual(sub + w * 4));
                              if ((v & 0xFF000000u) == 0x40000000u) {
                                sp += fmt::format("{:03X}:{:08X} ", w * 4, v);
                              }
                            }
                            XELOGI("GuideDCSub +{:03X} = {:08X}: {}", off,
                                   sub, sp);
                          }
                        }
                      }
                    }
                    uint64_t da[] = {obj};
                    // Phase 634: the render now completes (46/46) yet the loop
                    // stops advancing at ~107. Bracket the call so "the render
                    // blocks" and "something after it blocks" are
                    // distinguishable.
                    const bool guide_brk = (frame >= 100 && frame <= 118);
                    if (guide_brk) {
                      XELOGI("GuideLoop {}: render in", frame);
                    }
                    ks->processor()->Execute(ts, (g_hud_render ? g_hud_render : hb + 0xAB28u), da,
                                             xe::countof(da));
                    if (guide_brk) {
                      XELOGI("GuideLoop {}: render out", frame);
                    }
                    // Phase 591: once emission actually works each frame
                    // costs real time, and the loop no longer reaches its
                    // end inside a run - a 170s run got to frame ~106. The
                    // post-loop readback is therefore unreachable in exactly
                    // the configuration that works. Emit from inside; the
                    // underlying emitter is one-shot.
                    // frame 200 was still too late: with emission working
                    // the render hangs at ~frame 108 (waiting on a
                    // submission that never comes), so the loop never gets
                    // there. The emitter is one-shot, so calling it on every
                    // frame from 100 costs nothing.
                    // Phase 625: frame 100 is BEFORE the render DC is
                    // installed (~frame 105), so the one-shot captured the
                    // pre-DC path and reported the same 17/46 as phase 587 -
                    // the fourth time a readback has answered a question about
                    // a state it fired ahead of. Wait until the DC is actually
                    // in the object.
                    const bool guide_dc_ready =
                        xe::load_and_swap<uint32_t>(
                            mem->TranslateVirtual(obj + 12)) != 0;
                    if (cvars::guide_coverage_fn && frame >= 100 &&
                        guide_dc_ready) {
                      kernel::xboxkrnl::GuideEmitCoverageNow();
                    }
                    // Phase 589: the reservation at 81A042E0 fails its fit
                    // test (cur+size <= end) on all four calls even with a
                    // 512KB window bound. Log the window either side of the
                    // frame where the DC lands and the crash follows.
                    // Phase 594: is the title device's render target null
                    // always, or only when hud's render happens to read it?
                    // Sample it across the run, not just around the crash.
                    if (frame % 20 == 0 && frame <= 200) {
                      uint32_t td = xe::load_and_swap<uint32_t>(
                          mem->TranslateVirtual(0x801E6FC4u));
                      if (td) {
                        XELOGI("GuideTitleRT {}: dev={:08X} [32A0]={:08X} "
                               "[32B0]={:08X} [3F78]={:08X}",
                               frame, td,
                               xe::load_and_swap<uint32_t>(
                                   mem->TranslateVirtual(td + 0x32A0u)),
                               xe::load_and_swap<uint32_t>(
                                   mem->TranslateVirtual(td + 0x32B0u)),
                               xe::load_and_swap<uint32_t>(
                                   mem->TranslateVirtual(td + 0x3F78u)));
                      }
                    }
                    if (frame < 3 || frame % 500 == 0 ||
                        (frame >= 100 && frame <= 108)) {
                      uint32_t wdc = kernel::xboxkrnl::GuideBootDc();
                      uint32_t wsub =
                          wdc ? xe::load_and_swap<uint32_t>(
                                    mem->TranslateVirtual(wdc + 0x1CCu))
                              : 0u;
                      uint32_t wdev =
                          wsub ? xe::load_and_swap<uint32_t>(
                                     mem->TranslateVirtual(wsub + 0x0Cu))
                               : 0u;
                      if (wdev) {
                        // Phase 648: the submit/service handshake never
                        // primes (phase 646). Watch all four counters rather
                        // than reasoning from which spans were skipped -
                        // static tracing has twice pointed at the wrong
                        // branch here.
                        XELOGI("GuideCounters {}: dev={:08X} [462C]={} "
                               "[4634]={} [46C8]={} [46CC]={} [4644]={}",
                               frame, wdev,
                               xe::load_and_swap<uint32_t>(
                                   mem->TranslateVirtual(wdev + 0x462Cu)),
                               xe::load_and_swap<uint32_t>(
                                   mem->TranslateVirtual(wdev + 0x4634u)),
                               xe::load_and_swap<uint32_t>(
                                   mem->TranslateVirtual(wdev + 0x46C8u)),
                               xe::load_and_swap<uint32_t>(
                                   mem->TranslateVirtual(wdev + 0x46CCu)),
                               xe::load_and_swap<uint32_t>(
                                   mem->TranslateVirtual(wdev + 0x4644u)));
                        // All zero on the present device means the submit
                        // function measured running in phase 646 ran against
                        // a different one. Log mode 1's alongside.
                        uint32_t m1d = kernel::xboxkrnl::GuideMode1Device();
                        if (m1d) {
                          XELOGI("GuideCountersM1 {}: dev={:08X} [462C]={} "
                                 "[4634]={} [46C8]={} [46CC]={} [4644]={}",
                                 frame, m1d,
                                 xe::load_and_swap<uint32_t>(
                                     mem->TranslateVirtual(m1d + 0x462Cu)),
                                 xe::load_and_swap<uint32_t>(
                                     mem->TranslateVirtual(m1d + 0x4634u)),
                                 xe::load_and_swap<uint32_t>(
                                     mem->TranslateVirtual(m1d + 0x46C8u)),
                                 xe::load_and_swap<uint32_t>(
                                     mem->TranslateVirtual(m1d + 0x46CCu)),
                                 xe::load_and_swap<uint32_t>(
                                     mem->TranslateVirtual(m1d + 0x4644u)));
                        }
                        XELOGI("GuideWin {}: dev={:08X} [30]={:08X} "
                               "[34]={:08X} [2B4C]={:08X} [2B54]={:08X}",
                               frame, wdev,
                               xe::load_and_swap<uint32_t>(
                                   mem->TranslateVirtual(wdev + 0x30u)),
                               xe::load_and_swap<uint32_t>(
                                   mem->TranslateVirtual(wdev + 0x34u)),
                               xe::load_and_swap<uint32_t>(
                                   mem->TranslateVirtual(wdev + 0x2B4Cu)),
                               xe::load_and_swap<uint32_t>(
                                   mem->TranslateVirtual(wdev + 0x2B54u)));
                      }
                    }
                    if (frame < 3 || frame % 500 == 0) {
                      // Phase 587: [obj+12] is null on every frame of every
                      // run, so `flag` below was the 0xFFFFFFFF "no dc"
                      // sentinel throughout - every [dc+0x134] reading taken
                      // through this loop since phase 524 was of that
                      // sentinel, not of the null-render flag. The live DC is
                      // the bootstrap's (GuideDCVtable reports it as
                      // 408CEEE0); fall back to it so the loop reports on a
                      // pointer that exists.
                      uint32_t dcp = xe::load_and_swap<uint32_t>(
                          mem->TranslateVirtual(obj + 12));
                      if (!dcp) {
                        dcp = kernel::xboxkrnl::GuideBootDc();
                      }
                      uint32_t flag =
                          dcp ? xe::load_and_swap<uint32_t>(
                                    mem->TranslateVirtual(dcp + 0x134u))
                              : 0xFFFFFFFFu;
                      uint32_t now = cpd ? cpd->guide_draw_count_ : 0u;
                      XELOGI("GuideFrame {}: dc={:08X} [134]={:08X} "
                             "gpu_draws +{} (total {})",
                             frame, dcp, flag, now - prev_draws, now);
                      prev_draws = now;
                    }
                    xe::threading::Sleep(std::chrono::milliseconds(16));
                  }
                  XELOGI("Guide: hud draw loop finished");
                  if (cvars::guide_coverage_fn) {
                    auto* cf = ks->processor()->LookupFunction(
                        cvars::guide_coverage_fn);
                    auto* cgf =
                        cf ? dynamic_cast<cpu::GuestFunction*>(cf) : nullptr;
                    if (cgf && cgf->trace_data().is_valid()) {
                      auto& td = cgf->trace_data();
                      auto* cnt = reinterpret_cast<uint64_t*>(
                          td.instruction_execute_counts());
                      uint32_t n = td.instruction_count(), ex = 0, last = 0;
                      for (uint32_t i = 0; i < n; ++i) {
                        if (cnt[i]) {
                          ++ex;
                          last = td.start_address() + i * 4;
                        }
                      }
                      XELOGI("Coverage {:08X}: {}/{} executed, furthest "
                             "{:08X} (+0x{:X})",
                             uint32_t(cvars::guide_coverage_fn), ex, n, last,
                             last - td.start_address());
                      // Phase 1096bb: the trace header already carries
                      // function_call_count, a thread-use bitmask, and
                      // function_caller_history[4] - the last four CALLER
                      // addresses - filled by the x64 emitter. They are only
                      // populated under kDebugInfoTraceFunctions, which used to
                      // be global and therefore unusable; ppc_translator.cc now
                      // restricts it to trace_coverage_only_fn, so
                      // `--trace_functions=true` alongside a coverage target
                      // instruments exactly one function and records who calls
                      // it. Print it - this is the caller attribution phase 1096
                      // could not otherwise get for an internal function with no
                      // kernel import to hook.
                      {
                        auto* hdr = td.header();
                        std::string ch;
                        for (uint32_t i = 0;
                             i < cpu::FunctionTraceData::
                                     kFunctionCallerHistoryCount;
                             ++i) {
                          ch += fmt::format("{:08X} ",
                                            hdr->function_caller_history[i]);
                        }
                        XELOGI("CoverageCallers {:08X}: calls={} threads={:016X}"
                               " | last {} callers: {}",
                               uint32_t(cvars::guide_coverage_fn),
                               hdr->function_call_count,
                               hdr->function_thread_use,
                               cpu::FunctionTraceData::
                                   kFunctionCallerHistoryCount,
                               ch);
                      }
                      // Phase 585: "17 of 203 executed" says the function
                      // bailed early but not down WHICH path. The executed
                      // set is the path; print it. Capped so a hot function
                      // cannot flood the log.
                      if (ex <= 96) {
                        std::string ep;
                        for (uint32_t i = 0; i < n; ++i) {
                          if (cnt[i]) {
                            ep += fmt::format("{:08X}x{} ",
                                              td.start_address() + i * 4,
                                              cnt[i]);
                          }
                        }
                        XELOGI("CoveragePath {:08X}: {}",
                               uint32_t(cvars::guide_coverage_fn), ep);
                      }
                    } else {
                      // Phase 775: `lookup` is NOT an execution signal.
                      // Processor::LookupFunction calls DeclareFunction, which
                      // CREATES the symbol when new, so lookup=ok holds for any
                      // address in a loaded module whether or not it ever ran.
                      // Reading it as "found, therefore called" - as covrun's
                      // -NoTrace docstring invited - is unsound. Translation
                      // happens in DefineFunction, so the status is the signal:
                      // kDefined means the function was actually translated,
                      // i.e. reached; kDeclared means only that we asked about
                      // it. trace_valid is meaningful ONLY with
                      // --trace_functions, which allocates the trace data.
                      const char* st = "?";
                      switch (cf ? cf->status() : cpu::Symbol::Status::kFailed) {
                        case cpu::Symbol::Status::kNew:       st = "new"; break;
                        case cpu::Symbol::Status::kDeclaring: st = "declaring"; break;
                        case cpu::Symbol::Status::kDeclared:  st = "DECLARED-not-run"; break;
                        case cpu::Symbol::Status::kDefining:  st = "defining"; break;
                        case cpu::Symbol::Status::kDefined:   st = "DEFINED-translated"; break;
                        case cpu::Symbol::Status::kFailed:    st = "failed"; break;
                      }
                      XELOGI("Coverage {:08X}: no counts - lookup={}, "
                             "guest_fn={}, status={}, trace_valid={}",
                             uint32_t(cvars::guide_coverage_fn),
                             cf ? "ok" : "null", cgf ? "ok" : "cast-failed", st,
                             (cgf && cgf->trace_data().is_valid())
                                 ? "yes" : "no");
                    }
                  }
                }
              }

              const uint32_t msg = static_cast<uint32_t>(cvars::guide_message);
              if (msg != 0x80000004u) {
                XELOGI("Guide: dispatch msg={:08X} -> {:08X}", msg, h);
                uint64_t gargs[] = {msg, buf, out_sz};
                uint64_t r = ks->processor()->Execute(ts, h, gargs,
                                                      xe::countof(gargs));
                XELOGI("Guide: handler returned {:08X}",
                       static_cast<uint32_t>(r));
              }
              return 0;
            }, cvars::guide_system_process ? ks->GetSystemProcess() : 0u));  // phase 1055 menus
    hud_boot->set_name("Guide Loader");
    if (XFAILED(hud_boot->Create())) {
      XELOGE("Guide: failed to create loader thread");
    }
  }
  on_launch(title_id_.value(), title_name_);

  // Plugins must be loaded after calling LaunchModule() and
  // FinishLoadingUserModule() which will apply TUs and patching to the main
  // xex.
  if (cvars::allow_plugins) {
    if (plugin_loader_->IsAnyPluginForTitleAvailable(title_id_.value(),
                                                     module->hash().value())) {
      plugin_loader_->LoadTitlePlugins(title_id_.value(),
                                       module->hash().value());
    }
  }

  // Resume the main thread now.
  // If the debugger has requested a suspend this will just decrement the
  // suspend count without resuming it until the debugger wants.
  main_thread_->Resume();

  return X_STATUS_SUCCESS;
}

}  // namespace xe
