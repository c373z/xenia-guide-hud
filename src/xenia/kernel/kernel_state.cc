/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <ranges>

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/kernel_flags.h"  // phase 1085: guide_system_root

#include "xenia/base/byte_stream.h"
#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
// For the guest sampler's host symbols (phase 1099z159).
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif
#include <map>
#include <thread>
#include "xenia/apu/audio_system.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/function.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_memory.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/imgui_host_notification.h"
#include "xenia/vfs/devices/host_path_entry.h"  // 2026-09-16 SystemFlashPatch

#include "third_party/crypto/TinySHA1.hpp"

DEFINE_bool(apply_title_update, true, "Apply title updates.", "Kernel");
DEFINE_bool(allow_incompatible_title_update, true,
            "Allow title updates with mismatched signatures to be applied.",
            "Kernel");

DEFINE_uint32(kernel_build_version, 1888, "Define current kernel version",
              "Kernel");

DECLARE_string(cl);

namespace xe {
namespace kernel {
namespace xam {
uint32_t xeXGetGameRegion();
}  // namespace xam
namespace xboxkrnl {
// xboxkrnl_video.cc: ring buffer + persisted front buffer (phase 1099z97).
void GuideTitleSwitchKeepAddresses(std::vector<uint32_t>* out);
// xboxkrnl_video.cc: the kernel's own title terminate notifications 800F8778
// (6E800000) and 800F8358 (6D000000), phase 1099z139.
void GuideKernelEngineTerminateSlot(bool send_id5);
}  // namespace xboxkrnl

constexpr std::chrono::milliseconds kDeferredOverlappedDelayMillis(25);

// This is a global object initialized with the XboxkrnlModule.
// It references the current kernel state object that all kernel methods should
// be using to stash their variables.
KernelState* shared_kernel_state_ = nullptr;

KernelState* kernel_state() { return shared_kernel_state_; }

KernelState::KernelState(Emulator* emulator)
    : emulator_(emulator),
      memory_(emulator->memory()),
      dispatch_thread_running_(false),
      dpc_list_(emulator->memory()),
      kernel_trampoline_group_(emulator->processor()->backend()) {
  assert_null(shared_kernel_state_);
  shared_kernel_state_ = this;
  processor_ = emulator->processor();
  file_system_ = emulator->file_system();
  xam_state_ = std::make_unique<xam::XamState>(emulator, this);
  smc_ = std::make_unique<SystemManagementController>();
  xconfig_ =
      std::make_unique<XConfig>(emulator->storage_root() / "xconfig.settings");

  InitializeKernelGuestGlobals();
  kernel_version_ = KernelVersion(cvars::kernel_build_version);

  auto hc_loc_heap = memory_->LookupHeap(strange_hardcoded_page_);
  bool fixed_alloc_worked = hc_loc_heap->AllocFixed(
      strange_hardcoded_page_, 65536, 0,
      kMemoryAllocationCommit | kMemoryAllocationReserve,
      kMemoryProtectRead | kMemoryProtectWrite);

  xenia_assert(fixed_alloc_worked);
  // Phase 1099z164: +0x602 of this page is the console's game region (u16).
  // The hypervisor fills it from the key vault; the 17489 kernel reads it at
  // 80088768 and for the disc region check (8017D0F8), retail xam's
  // XGetGameRegion (816FB270) returns it. Zero made the 17559 dash treat every
  // disc as out of region (no "Install to Hard Drive").
  if (cvars::kernel_game_region) {
    const uint32_t region = cvars::kernel_game_region == 0xFFFFFFFFu
                                ? xam::xeXGetGameRegion()
                                : cvars::kernel_game_region;
    xe::store_and_swap<uint16_t>(memory_->TranslateVirtual(0x8E038602),
                                 uint16_t(region));
    XELOGI("Kernel: game region {:04X} at 8E038602", uint16_t(region));
  }
  // 2026-09-16: +0x614 of the same page. The 17489 hypervisor (se_17489 image
  // 2DFA8..2DFD8) writes 0x00070000 there on every console whose game region
  // is not 0102 (China); a China console gets 0x107 or 0x4 depending on a
  // hypervisor-internal flag (0x30 bit 4) that Xenia does not model - 0x107 is
  // used. Every dashboard (6770 921C0160, 7357 921FED68, 17559 922832E8)
  // enables System Settings > Initial Setup only when bit 0 of the upper
  // halfword is set, and retail xam tests bits 0x10000/0x2/0x1 of it. Zero
  // left Initial Setup greyed out on every version.
  if (cvars::kernel_hv_console_flags) {
    const uint16_t region = xe::load_and_swap<uint16_t>(
        memory_->TranslateVirtual(0x8E038602));
    const uint32_t flags = region == 0x0102 ? 0x107u : 0x00070000u;
    xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(0x8E038614), flags);
    XELOGI("Kernel: hypervisor console flags {:08X} at 8E038614", flags);
  }
  StartGuestSampler();
}

// Phase 1099z159 (DIAGNOSTIC): the Guide takes ~1 s to open on 17559 with
// xam's HUD thread ~95% CPU-busy the whole time (per-thread CPU sampling) and
// no long waits, so the question is which code. Samples every guest thread
// that consumed CPU cycles since the last sample: translated code is named by
// its guest function, host code by symbol plus the nearest translated caller
// found on the stack.
void KernelState::StartGuestSampler() {
#if XE_PLATFORM_WIN32
  if (cvars::kernel_sample_to_ms <= cvars::kernel_sample_from_ms) return;
  std::thread([this]() {
    auto process_ms = []() {
      FILETIME c, e, k, u;
      GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
      FILETIME now;
      GetSystemTimeAsFileTime(&now);
      const uint64_t c64 = (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
      const uint64_t n64 =
          (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
      return int64_t((n64 - c64) / 10000);
    };
    while (process_ms() < cvars::kernel_sample_from_ms) {
      xe::threading::Sleep(std::chrono::milliseconds(5));
    }
    XELOGI("GuestSampler: started at process time {} ms", process_ms());
    auto* cache = processor_->backend()->code_cache();
    const uint64_t code_lo = cache->execute_base_address();
    const uint64_t code_hi = code_lo + cache->total_size();
    struct Tracked {
      HANDLE h;
      std::string name;
      uint64_t cycles;
      uint64_t samples;
      std::map<std::string, uint64_t> hits;
    };
    std::map<uint32_t, Tracked> threads;  // by guest handle
    std::map<uint64_t, std::string> sym_cache;
    int64_t last_refresh = -1000;
    uint64_t total = 0;
    while (process_ms() < cvars::kernel_sample_to_ms) {
      const int64_t now = process_ms();
      if (now - last_refresh >= 100) {
        last_refresh = now;
        auto lock = global_critical_region_.Acquire();
        for (auto& kv : threads_by_id_) {
          XThread* t = kv.second;
          if (!t->is_guest_thread() || !t->thread()) continue;
          if (threads.count(t->handle())) continue;
          HANDLE dup = nullptr;
          if (!DuplicateHandle(GetCurrentProcess(),
                               HANDLE(t->thread()->native_handle()),
                               GetCurrentProcess(), &dup, 0, FALSE,
                               DUPLICATE_SAME_ACCESS)) {
            continue;
          }
          Tracked tr{dup, t->name(), 0, 0, {}};
          QueryThreadCycleTime(dup, &tr.cycles);
          threads.emplace(t->handle(), std::move(tr));
        }
      }
      for (auto& kv : threads) {
        Tracked& tr = kv.second;
        uint64_t cyc = 0;
        if (!QueryThreadCycleTime(tr.h, &cyc) || cyc == tr.cycles) continue;
        tr.cycles = cyc;
        if (SuspendThread(tr.h) == DWORD(-1)) continue;
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_CONTROL;
        uint64_t stack[512];
        size_t stack_words = 0;
        if (GetThreadContext(tr.h, &ctx)) {
          // The stack belongs to a suspended thread of this process.
          MEMORY_BASIC_INFORMATION mbi;
          if (VirtualQuery(PVOID(ctx.Rsp), &mbi, sizeof(mbi))) {
            const uint64_t avail = uint64_t(mbi.BaseAddress) +
                                   mbi.RegionSize - ctx.Rsp;
            stack_words = size_t(std::min<uint64_t>(avail / 8, 512));
            std::memcpy(stack, PVOID(ctx.Rsp), stack_words * 8);
          }
        }
        ResumeThread(tr.h);
        if (!ctx.Rip) continue;
        auto guest_fn_at = [&](uint64_t pc) -> uint32_t {
          auto lock = global_critical_region_.Acquire();
          auto* fn = cache->LookupFunction(pc);
          return fn ? fn->address() : 0;
        };
        std::string key;
        if (ctx.Rip >= code_lo && ctx.Rip < code_hi) {
          key = fmt::format("guest {:08X}", guest_fn_at(ctx.Rip));
        } else {
          auto it = sym_cache.find(ctx.Rip);
          if (it == sym_cache.end()) {
            static bool sym_ready = false;
            if (!sym_ready) {
              SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
              sym_ready = SymInitialize(GetCurrentProcess(), nullptr, TRUE);
            }
            char buf[sizeof(SYMBOL_INFO) + 256] = {};
            auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            DWORD64 disp = 0;
            std::string s = SymFromAddr(GetCurrentProcess(), ctx.Rip, &disp, si)
                                ? std::string(si->Name)
                                : fmt::format("rip {:X}", ctx.Rip);
            it = sym_cache.emplace(ctx.Rip, std::move(s)).first;
          }
          uint32_t caller = 0;
          for (size_t w = 0; w < stack_words; ++w) {
            if (stack[w] >= code_lo && stack[w] < code_hi) {
              caller = guest_fn_at(stack[w]);
              if (caller) break;
            }
          }
          key = fmt::format("host {} <- guest {:08X}", it->second, caller);
        }
        ++tr.hits[key];
        ++tr.samples;
        ++total;
      }
      xe::threading::Sleep(std::chrono::milliseconds(1));
    }
    XELOGI("GuestSampler: stopped at process time {} ms, {} samples",
           process_ms(), total);
    for (auto& kv : threads) {
      Tracked& tr = kv.second;
      if (tr.samples) {
        XELOGI("GuestSampler: thread {:08X} '{}' {} samples", kv.first, tr.name,
               tr.samples);
        std::vector<std::pair<uint64_t, std::string>> v;
        for (auto& h : tr.hits) v.emplace_back(h.second, h.first);
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 30; ++i) {
          XELOGI("GuestSampler:   {:5} {:5.1f}% {}", v[i].first,
                 100.0 * double(v[i].first) / double(tr.samples), v[i].second);
        }
      }
      CloseHandle(tr.h);
    }
  }).detach();
#endif
}

KernelState::~KernelState() {
  SetExecutableModule(nullptr);

  if (dispatch_thread_running_) {
    dispatch_thread_running_ = false;
    dispatch_cond_.notify_all();
    dispatch_thread_->Wait(0, 0, 0, nullptr);
  }

  executable_module_.reset();
  user_modules_.clear();
  kernel_modules_.clear();

  // Delete all objects.
  object_table_.Reset();

  xam_state_.reset();

  assert_true(shared_kernel_state_ == this);
  shared_kernel_state_ = nullptr;
}

KernelState* KernelState::shared() { return shared_kernel_state_; }

uint32_t KernelState::title_id() const {
  if (!executable_module_) {
    return 0;
  }

  assert_not_null(executable_module_);

  xex2_opt_execution_info* exec_info = 0;
  executable_module_->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &exec_info);

  if (exec_info) {
    return exec_info->title_id;
  }

  return 0;
}

bool KernelState::is_title_open() const { return emulator_->is_title_open(); }

const std::unique_ptr<xam::SpaInfo> KernelState::title_xdbf() const {
  return module_xdbf(executable_module_);
}

const std::unique_ptr<xam::SpaInfo> KernelState::module_xdbf(
    object_ref<UserModule> exec_module) const {
  assert_not_null(exec_module);

  uint32_t resource_data = 0;
  uint32_t resource_size = 0;
  if (XSUCCEEDED(exec_module->GetSection(
          fmt::format("{:08X}", exec_module->title_id()).c_str(),
          &resource_data, &resource_size))) {
    return std::make_unique<xam::SpaInfo>(std::span<uint8_t>(
        memory()->TranslateVirtual(resource_data), resource_size));
  }

  return nullptr;
}

uint32_t KernelState::AllocateTLS(cpu::ppc::PPCContext* context) {
  auto globals =
      memory()->TranslateVirtual<KernelGuestGlobals*>(GetKernelGuestGlobals());
  auto tls_lock = &globals->tls_lock;
  auto old_irql = xboxkrnl::xeKeKfAcquireSpinLock(context, tls_lock);

  int result = -1;

  auto current_thread = XThread::GetCurrentThread();
  if (!current_thread) {
    XELOGE("AllocateTLS: No current thread");
    xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
    return X_TLS_OUT_OF_INDEXES;
  }

  auto process_ptr = memory()->TranslateVirtual(
      current_thread->guest_object<X_KTHREAD>()->process);
  if (!process_ptr) {
    XELOGE("AllocateTLS: Failed to translate process pointer");
    xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
    return X_TLS_OUT_OF_INDEXES;
  }

  // Search for a free TLS slot in the process bitmap
  // Bitmap format: 1 = free, 0 = allocated
  // 8 x 32-bit words = 256 total TLS slots
  for (xe::be<uint32_t>* i = &process_ptr->tls_slot_bitmap[0];
       i < &process_ptr->tls_slot_bitmap[8]; ++i) {
    // Read bitmap value (handles big-endian conversion)
    uint32_t bitmap_value = static_cast<uint32_t>(*i);

    // Find highest free slot using lzcnt (leading zero count)
    // Returns 0-31 if a bit is set, 32 if no bits are set
    uint32_t leading_zeros = xe::lzcnt(bitmap_value);

    if (leading_zeros != 32) {
      // Calculate absolute slot index from bitmap position and bit offset
      // Each bitmap word represents 32 slots
      size_t bitmap_index = i - &process_ptr->tls_slot_bitmap[0];
      uint32_t base_slot = static_cast<uint32_t>(bitmap_index) * 32;
      int calculated_slot = base_slot + leading_zeros;

      // Validate slot is within Xbox 360 TLS range
      if (calculated_slot >= 0 && calculated_slot < 256) {
        result = calculated_slot;

        // Clear the bit to mark as allocated
        // lzcnt returns 0 for bit 31, 31 for bit 0
        uint32_t bit_index = 31 - leading_zeros;
        *i = bitmap_value & ~(1U << bit_index);
        break;
      } else {
        XELOGE("AllocateTLS: Invalid slot calculation: {}", calculated_slot);
      }
    }
  }

  if (result == -1) {
    XELOGW("AllocateTLS: All TLS slots exhausted for current process");
  }

  xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
  return static_cast<uint32_t>(result);
}

void KernelState::FreeTLS(cpu::ppc::PPCContext* context, uint32_t slot) {
  if (slot >= 256) {
    XELOGE("FreeTLS: Invalid slot index {}", slot);
    return;
  }

  auto current_thread = XThread::GetCurrentThread();
  if (!current_thread) {
    XELOGE("FreeTLS: No current thread");
    return;
  }

  auto current_kthread = current_thread->guest_object<X_KTHREAD>();
  if (!current_kthread) {
    XELOGE("FreeTLS: Failed to get guest thread object");
    return;
  }

  auto process_ptr = memory()->TranslateVirtual(current_kthread->process);
  if (!process_ptr) {
    XELOGE("FreeTLS: Failed to translate process pointer");
    return;
  }

  auto globals =
      memory()->TranslateVirtual<KernelGuestGlobals*>(GetKernelGuestGlobals());
  auto tls_lock = &globals->tls_lock;
  auto old_irql = xboxkrnl::xeKeKfAcquireSpinLock(context, tls_lock);

  uint32_t bitmap_index = slot / 32;
  uint32_t bit_mask = 1U << (31 - (slot % 32));
  uint32_t bitmap_value =
      static_cast<uint32_t>(process_ptr->tls_slot_bitmap[bitmap_index]);

  if (bitmap_value & bit_mask) {
    XELOGW("FreeTLS: Slot {} is already free", slot);
    xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
    return;
  }

  // Clear TLS values in all threads of this process
  const std::vector<object_ref<XThread>> threads =
      object_table()->GetObjectsByType<XThread>();

  uint32_t current_process_ptr = current_kthread->process.m_ptr;
  for (const object_ref<XThread>& thread : threads) {
    if (!thread || !thread->is_guest_thread()) {
      continue;
    }

    auto thread_kthread = thread->guest_object<X_KTHREAD>();
    if (thread_kthread &&
        thread_kthread->process.m_ptr == current_process_ptr) {
      thread->SetTLSValue(slot, 0);
    }
  }

  // Mark slot as free in bitmap
  process_ptr->tls_slot_bitmap[bitmap_index] = bitmap_value | bit_mask;

  xboxkrnl::xeKeKfReleaseSpinLock(context, tls_lock, old_irql);
}

void KernelState::RegisterTitleTerminateNotification(uint32_t routine,
                                                     uint32_t priority,
                                                     uint32_t record) {
  TerminateNotification notify;
  notify.guest_routine = routine;
  notify.priority = priority;
  notify.record = record;

  terminate_notifications_.push_back(notify);
}

void KernelState::RemoveTitleTerminateNotification(uint32_t routine) {
  for (auto it = terminate_notifications_.begin();
       it != terminate_notifications_.end(); it++) {
    if (it->guest_routine == routine) {
      terminate_notifications_.erase(it);
      break;
    }
  }
}

void KernelState::RegisterModule(XModule* module) {}

void KernelState::UnregisterModule(XModule* module) {}

bool KernelState::RegisterUserModule(object_ref<UserModule> module) {
  auto lock = global_critical_region_.Acquire();

  for (auto user_module : user_modules_) {
    if (user_module->path() == module->path()) {
      // Already loaded.
      return false;
    }
  }

  user_modules_.push_back(module);
  return true;
}

void KernelState::UnregisterUserModule(UserModule* module) {
  auto lock = global_critical_region_.Acquire();

  for (auto it = user_modules_.begin(); it != user_modules_.end(); it++) {
    if ((*it)->path() == module->path()) {
      user_modules_.erase(it);
      return;
    }
  }
}

bool KernelState::IsKernelModule(const std::string_view name) {
  if (name.empty()) {
    // Executing module isn't a kernel module.
    return false;
  }
  // NOTE: no global lock required as the kernel module list is static.
  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return true;
    }
  }
  return false;
}

bool KernelState::IsModuleLoaded(const std::string_view name) {
  if (name.empty()) {
    return true;
  }

  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return true;
    }
  }

  auto global_lock = global_critical_region_.Acquire();

  for (auto user_module : user_modules_) {
    if (user_module->Matches(name)) {
      return true;
    }
  }

  return false;
}

object_ref<KernelModule> KernelState::GetKernelModule(
    const std::string_view name) {
  assert_true(IsKernelModule(name));

  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return retain_object(kernel_module.get());
    }
  }

  return nullptr;
}

bool KernelState::AddressInUserModuleImage(uint32_t address, uint32_t length) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& user_module : user_modules_) {
    auto* xex = user_module ? user_module->xex_module() : nullptr;
    if (!xex) continue;
    const uint32_t lo = xex->base_address();
    const uint32_t hi = lo + xex->image_size();
    if (address >= lo && address + length <= hi) return true;
  }
  return false;
}

object_ref<UserModule> KernelState::GetUserModuleByAddress(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& user_module : user_modules_) {
    auto* xex = user_module ? user_module->xex_module() : nullptr;
    if (!xex) continue;
    const uint32_t lo = xex->base_address();
    if (address >= lo && address - lo < xex->image_size()) {
      return user_module;
    }
  }
  return nullptr;
}

object_ref<XModule> KernelState::GetModule(const std::string_view name,
                                           bool user_only) {
  if (name.empty()) {
    // NULL name = self.
    // TODO(benvanik): lookup module from caller address.
    return GetExecutableModule();
  } else if (xe::utf8::equal_case(name, "kernel32.dll")) {
    // Some games request this, for some reason. wtf.
    return nullptr;
  }

  auto global_lock = global_critical_region_.Acquire();

  if (!user_only) {
    for (auto kernel_module : kernel_modules_) {
      if (kernel_module->Matches(name)) {
        return retain_object(kernel_module.get());
      }
    }
  }

  auto path(name);

  // Resolve the path to an absolute path.
  auto entry = file_system_->ResolvePath(name);
  if (entry) {
    path = entry->absolute_path();
  }

  for (auto user_module : user_modules_) {
    if (user_module->Matches(path)) {
      return retain_object(user_module.get());
    }
  }
  return nullptr;
}

object_ref<XThread> KernelState::LaunchModule(object_ref<UserModule> module) {
  if (!module->is_executable()) {
    return nullptr;
  }

  SetExecutableModule(module);
  // Phase 1096hs: dash's entry point writes -1 to [92A7C224] and [92A7C228]
  // four instructions in, and a watch that demonstrably reads dash image
  // memory never sees either change - yet dash's code runs. Print the entry
  // this thread is actually created with, so "Xenia launches 92196660" stops
  // being an inference from the XEX header and becomes a measurement.
  XELOGI("KernelState: Launching module... entry_point={:08X} stack={:X} "
         "@{}ms",
         module->entry_point(), module->stack_size(),
         xe::Clock::QueryHostUptimeMillis());

  // Create a thread to run in.
  // We start suspended so we can run the debugger prep.
  auto thread = object_ref<XThread>(
      new XThread(kernel_state(), module->stack_size(), 0,
                  module->entry_point(), 0, X_CREATE_SUSPENDED, true, true));

  // We know this is the 'main thread'.
  thread->set_name("Main XThread");

  X_STATUS result = thread->Create();
  if (XFAILED(result)) {
    XELOGE("Could not create launch thread: {:08X}", result);
    return nullptr;
  }

  // Waits for a debugger client, if desired.
  emulator()->processor()->PreLaunch();

  return thread;
}

object_ref<UserModule> KernelState::GetExecutableModule() {
  if (!executable_module_) {
    return nullptr;
  }
  return executable_module_;
}

void KernelState::SetExecutableModule(object_ref<UserModule> module) {
  if (module.get() == executable_module_.get()) {
    return;
  }
  executable_module_ = std::move(module);
  if (!executable_module_) {
    return;
  }

  auto title_process =
      memory_->TranslateVirtual<X_KPROCESS*>(GetTitleProcess());

  // Phase 1099z125: the title process exists from kernel init (see
  // InitializeKernelGuestGlobals) and can already own threads here - the boot
  // animation runs in it before any title. Re-initializing its thread list
  // under a live thread corrupts the list when that thread exits, so keep the
  // list and count when they are in use.
  const X_LIST_ENTRY live_threads = title_process->thread_list;
  const auto live_count = title_process->thread_count;
  InitializeProcess(title_process, X_PROCTYPE_TITLE, 10, 13, 17);
  if (live_count != 0) {
    title_process->thread_list = live_threads;
    title_process->thread_count = live_count;
  }

  xex2_opt_tls_info* tls_header = nullptr;
  executable_module_->GetOptHeader(XEX_HEADER_TLS_INFO, &tls_header);
  if (tls_header) {
    title_process->tls_static_data_address = tls_header->raw_data_address;
    title_process->tls_data_size = tls_header->data_size;
    title_process->tls_raw_data_size = tls_header->raw_data_size;
    title_process->tls_slot_size = tls_header->slot_count * 4;
    SetProcessTLSVars(title_process, tls_header->slot_count,
                      tls_header->data_size, tls_header->raw_data_address);
  }

  uint32_t kernel_stacksize = 0;

  executable_module_->GetOptHeader(XEX_HEADER_DEFAULT_STACK_SIZE,
                                   &kernel_stacksize);
  if (kernel_stacksize) {
    kernel_stacksize = (kernel_stacksize + 4095) & 0xFFFFF000;
    if (kernel_stacksize < 0x4000) {
      kernel_stacksize = 0x4000;
    }
    title_process->kernel_stack_size = kernel_stacksize;
  }

  // Setup the kernel's XexExecutableModuleHandle field.
  auto export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::XexExecutableModuleHandle);
  if (export_entry) {
    assert_not_zero(export_entry->variable_ptr);
    auto variable_ptr = memory()->TranslateVirtual<xe::be<uint32_t>*>(
        export_entry->variable_ptr);
    *variable_ptr = executable_module_->hmodule_ptr();
  }

  // Setup the kernel's ExLoadedImageName field
  export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::ExLoadedImageName);

  if (export_entry) {
    char* variable_ptr =
        memory()->TranslateVirtual<char*>(export_entry->variable_ptr);
    xe::string_util::copy_truncating(
        variable_ptr, executable_module_->path(),
        xboxkrnl::XboxkrnlModule::kExLoadedImageNameSize);
  }

  // Setup the kernel's ExLoadedCommandLine field
  export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::ExLoadedCommandLine);
  if (export_entry) {
    char* variable_ptr =
        memory()->TranslateVirtual<char*>(export_entry->variable_ptr);

    std::string module_name =
        fmt::format("\"{}.xex\"", executable_module_->name());
    if (!cvars::cl.empty()) {
      module_name += " " + cvars::cl;
    }

    xe::string_util::copy_truncating(
        variable_ptr, module_name,
        xboxkrnl::XboxkrnlModule::kExLoadedCommandLineSize);
  }

  // Initialize file I/O hooks for XMP volume title-specific patches.
  InitXmpVolumePatch();

  // Spin up deferred dispatch worker.
  // TODO(benvanik): move someplace more appropriate (out of ctor, but around
  // here).
  if (!dispatch_thread_running_) {
    dispatch_thread_running_ = true;
    dispatch_thread_ = object_ref<XHostThread>(new XHostThread(
        this, 128 * 1024, 0,
        [this]() {
          // As we run guest callbacks the debugger must be able to suspend us.
          dispatch_thread_->set_can_debugger_suspend(true);

          auto global_lock = global_critical_region_.AcquireDeferred();
          while (dispatch_thread_running_) {
            global_lock.lock();
            if (dispatch_queue_.empty()) {
              dispatch_cond_.wait(global_lock);
              if (!dispatch_thread_running_) {
                global_lock.unlock();
                break;
              }
            }
            auto fn = std::move(dispatch_queue_.front());
            dispatch_queue_.pop_front();
            global_lock.unlock();

            fn();
          }
          return 0;
        },
        GetSystemProcess()));  // don't think an equivalent exists on real hw
    dispatch_thread_->set_name("Kernel Dispatch");
    dispatch_thread_->Create();
  }
}

void KernelState::LoadKernelModule(object_ref<KernelModule> kernel_module) {
  auto global_lock = global_critical_region_.Acquire();
  kernel_modules_.push_back(std::move(kernel_module));
}

object_ref<UserModule> KernelState::LoadUserModule(
    const std::string_view raw_name, bool call_entry) {
  // Some games try to load relative to launch module, others specify full path.
  auto name = xe::utf8::find_name_from_guest_path(raw_name);
  std::string path(raw_name);
  if (name == raw_name) {
    if (!executable_module_) {
      path = xe::utf8::join_guest_paths(
          xe::utf8::find_base_guest_path((*user_modules_.cbegin())->path()),
          name);
    } else {
      path = xe::utf8::join_guest_paths(
          xe::utf8::find_base_guest_path(executable_module_->path()), name);
    }
    // A bare name that is not next to the running title is looked up next to
    // the loaded xam.xex: xam loads its system apps by bare name
    // ("signin.xex", "hud.xex"), which live beside it in the system
    // partition. Joining them to the title's directory worked while the
    // dashboard (itself in the system partition) was running, but with a
    // disc title it produced \Device\CdRom0\signin.xex, the in-game sign-in
    // UI failed to load, and xam showed "Xbox Live ... Status Code d000000f".
    if (!file_system_->ResolvePath(path)) {
      for (const auto& user_module : user_modules_) {
        if (xe::utf8::equal_case(
                xe::utf8::find_name_from_guest_path(user_module->path()),
                "xam.xex")) {
          std::string beside_xam = xe::utf8::join_guest_paths(
              xe::utf8::find_base_guest_path(user_module->path()), name);
          if (file_system_->ResolvePath(beside_xam)) {
            XELOGI("LoadUserModule: {} not beside the title; using {}", name,
                   beside_xam);
            path = beside_xam;
          }
          break;
        }
      }
    }
  }

  object_ref<UserModule> module;
  {
    auto global_lock = global_critical_region_.Acquire();

    // See if we've already loaded it.
    //
    // Matching on the full path alone is not enough. The same module reached
    // by a different path - SYS:\xam.xex from the LLE bootstrap and
    // \Device\Flash\xam.xex from an import, say - fails every comparison in
    // Matches(path) and gets loaded a second time. Both copies use the image
    // base out of the xex header, so the second load's AllocFixed lands on
    // top of the first: it zeroes the range the first copy is already
    // executing from, writes the same bytes back, and re-applies the section
    // protections. Anything translated during that window sees zeros, which
    // is why xam .text was observed "going zero and coming back identical"
    // and why functions intermittently failed to translate at all.
    //
    // Matches() already compares the basename and the module's own name, so
    // passing the name as well makes this dedupe by module name - which is
    // what the console does, and what the fixed image bases require.
    for (auto& existing_module : user_modules_) {
      if (existing_module->Matches(path)) {
        return existing_module;
      }
      if (existing_module->Matches(name)) {
        XELOGW(
            "LoadUserModule: {} is already loaded as {}; returning the "
            "existing module rather than loading a second copy over it",
            path, existing_module->path());
        return existing_module;
      }
    }

    global_lock.unlock();

    // Module wasn't loaded, so load it.
    module = object_ref<UserModule>(new UserModule(this));
    X_STATUS status = module->LoadFromFile(path);
    if (XFAILED(status) && cvars::guide_system_app_fallback &&
        name == raw_name && !cvars::guide_system_root.empty()) {
      // Phase 1085: a bare module name is joined to the EXECUTABLE's directory
      // above, which for a real game disc is \Device\Cdrom0. xam asks for its
      // system apps that way - `createprofile.xex` - and they live on the
      // console's system partition, which this build mounts as SYS: via
      // guide_system_root (the same reason that flag exists for xam.xex and
      // hud.xex: "without it those files have to live on the title's own GAME:
      // device, which only works when the title is the dashboard folder").
      // OPT-IN (guide_system_app_fallback, default false): without the gate
      // this also satisfies the title's own probe for xbdm.xex, which
      // dashroot happens to contain - measured on reg1085 - and silently
      // changes what the title sees. The default path must not move.
      // Strictly a fallback: the title's own
      // device is still tried first, so nothing that works today changes.
      const std::string sys_path = "SYS:\\" + std::string(name);
      X_STATUS sys_status = module->LoadFromFile(sys_path);
      XELOGI("LoadUserModule: {} not on the title's device ({:08X}); SYS: retry {} -> {:08X}",
             path, uint32_t(status), sys_path, uint32_t(sys_status));
      status = sys_status;
    }
    if (XFAILED(status)) {
      object_table()->ReleaseHandle(module->handle());
      return nullptr;
    }

    global_lock.lock();

    // Phase 1099z161: remember DLLs a title-process thread loads, so the
    // title terminate unloads them as the console does.
    if (auto* th = XThread::GetCurrentThread()) {
      auto* kt = th->guest_object<X_KTHREAD>();
      if (kt && kt->process_type == X_PROCTYPE_TITLE) {
        module->set_loaded_by_title(true);
      }
    }

    // Putting into the listing automatically retains.
    user_modules_.push_back(module);
  }
  return module;
}

object_ref<UserModule> KernelState::LoadUserModuleFromMemory(
    const std::string_view raw_name, const void* addr, const size_t length) {
  auto name = xe::utf8::find_base_name_from_guest_path(raw_name);

  object_ref<UserModule> module;
  {
    auto global_lock = global_critical_region_.Acquire();

    // See if we've already loaded it
    for (auto& existing_module : user_modules_) {
      if (existing_module->Matches(name)) {
        return existing_module;
      }
    }

    global_lock.unlock();

    // Module wasn't loaded, so load it.
    module = object_ref<UserModule>(new UserModule(this));
    X_STATUS status = module->LoadFromMemoryNamed(name, addr, length);
    if (XFAILED(status)) {
      object_table()->ReleaseHandle(module->handle());
      return nullptr;
    }

    global_lock.lock();

    // Putting into the listing automatically retains.
    user_modules_.push_back(module);
  }
  return module;
}

X_RESULT KernelState::FinishLoadingUserModule(
    const object_ref<UserModule> module, bool call_entry) {
  // TODO(Gliniak): Apply custom patches here
  X_RESULT result = module->LoadContinue();
  if (XFAILED(result)) {
    return result;
  }
  module->Dump();
  emulator_->patcher()->ApplyPatchesForTitle(memory_, module->title_id(),
                                             module->hash());
  emulator_->on_patch_apply();
  if (user_module_loaded_hook) {
    user_module_loaded_hook(module.get());
  }
  if (module->xex_module()) {
    module->xex_module()->Precompile();
  }

  if (module->is_dll_module() && module->entry_point() && call_entry) {
    // Call DllMain(DLL_PROCESS_ATTACH):
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
    uint64_t args[] = {
        module->handle(),
        1,  // DLL_PROCESS_ATTACH
        0,  // 0 because always dynamic
    };

    module->is_attached_ = true;

    auto cur_thread = XThread::GetCurrentThread();
    if (!cur_thread) {
      // No guest thread state on this thread (e.g. called from the UI thread
      // during launch). Executing guest code here corrupts/deadlocks; the
      // caller is responsible for attaching from a guest thread instead.
      XELOGE("DllMain: no guest thread context, skipping entry for {}",
             module->name());
      module->is_attached_ = false;
      return result;
    }
    XELOGI("DllMain: entering {} entry={:08X}", module->name(),
           module->entry_point());
    auto thread_state = cur_thread->thread_state();
    processor()->Execute(thread_state, module->entry_point(), args,
                         xe::countof(args));
    XELOGI("DllMain: returned from {}", module->name());
  }
  return result;
}

X_RESULT KernelState::ApplyTitleUpdate(
    const object_ref<UserModule> title_module) {
  // 2026-09-16: pre-2010 system updates ship most flash modules as delta
  // patches against the base 2.0.1888 flash ($flash_xam.xexp ...); the
  // console loads the base image and applies the patch. The importer puts
  // the patch beside the base file as <name>.xexp.
  if (cvars::kernel_system_flash_patches) {
    const std::string& path = title_module->path();
    bool system_module =
        xe::utf8::starts_with_case(path, "SYS:") ||
        xe::utf8::starts_with_case(path, "\\SYS\\") ||
        xe::utf8::starts_with_case(path, "\\Device\\Flash");
    // 2026-09-16 (NXE 7357): xam loads its system apps by BARE name
    // ("hud.xex"), which LoadUserModule joins to the running title's
    // directory. While the dashboard runs, that directory IS the system
    // folder, so hud.xex came in as \Device\TitleXex\hud.xex and its
    // hud.xexp was never applied: the 1888 hud ran against 7357 xam and its
    // imports of ordinals 452/470 (exported by 1888 xam, not by 7357 xam)
    // jumped to xam's image base when the Guide opened (GUEST CRASH at
    // 913F0C98, ctr=81870000). On the console these files are all flash
    // files, so the module is a system module when it IS the file under SYS:.
    if (!system_module && !cvars::guide_system_root.empty()) {
      const std::string sys_path =
          "SYS:\\" + std::string(xe::utf8::find_name_from_guest_path(path));
      auto* a = dynamic_cast<xe::vfs::HostPathEntry*>(
          file_system()->ResolvePath(path));
      auto* b = dynamic_cast<xe::vfs::HostPathEntry*>(
          file_system()->ResolvePath(sys_path));
      std::error_code ec;
      if (a && b &&
          std::filesystem::equivalent(a->host_path(), b->host_path(), ec)) {
        XELOGI("SystemFlashPatch: {} is the system file {}", path, sys_path);
        system_module = true;
      }
    }
    xe::vfs::Entry* patch_entry =
        system_module ? file_system()->ResolvePath(path + 'p') : nullptr;
    if (patch_entry) {
      auto patch_module = object_ref<UserModule>(new UserModule(this));
      X_RESULT load = patch_module->LoadFromFile(patch_entry->absolute_path());
      if (load != X_STATUS_SUCCESS || !patch_module->xex_module() ||
          !patch_module->xex_module()->is_patch()) {
        XELOGE("SystemFlashPatch: {}p is not a loadable delta patch ({:08X})",
               path, load);
        return X_STATUS_UNSUCCESSFUL;
      }
      if (!IsPatchSignatureProper(title_module, patch_module)) {
        XELOGE("SystemFlashPatch: {}p was made for another base image; not "
               "applied",
               path);
        return X_STATUS_UNSUCCESSFUL;
      }
      X_RESULT result = ApplyTitleUpdate(title_module, patch_module);
      XELOGI("SystemFlashPatch: applied {}p -> {:08X}", path, result);
      return result;
    }
  }

  const auto title_updates = FindTitleUpdate(title_module->title_id());
  if (title_updates.empty()) {
    return X_STATUS_SUCCESS;
  }

  auto patch_module = LoadTitleUpdate(&title_updates.front(), title_module);
  if (!patch_module) {
    return X_STATUS_SUCCESS;
  }

  if (!patch_module->xex_module()->is_patch()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (!IsPatchSignatureProper(title_module, patch_module)) {
    if (!cvars::allow_incompatible_title_update) {
      XELOGW(
          "Skipping incompatible title update for {} due to signature mismatch",
          title_module->name());
      return X_STATUS_SUCCESS;
    }

    // First module that is loaded is always main executable. That way we can
    // prevent random message spam in case of loading/unloading.
    if (!GetExecutableModule()) {
      emulator_->display_window()->app_context().CallInUIThread([&]() {
        new xe::ui::HostNotificationWindow(
            emulator_->imgui_drawer(), "Warning!",
            "Title Update signature doesn't match. This can cause unexpected "
            "issues or crashes!",
            0);
      });
    }
  }

  return ApplyTitleUpdate(title_module, patch_module);
}

std::vector<xam::XCONTENT_AGGREGATE_DATA> KernelState::FindTitleUpdate(
    const uint32_t title_id) const {
  if (!cvars::apply_title_update) {
    return {};
  }

  return xam_state_->content_manager()->ListContent(
      1, 0, title_id, xe::XContentType::kInstaller);
}

const object_ref<UserModule> KernelState::LoadTitleUpdate(
    const xam::XCONTENT_AGGREGATE_DATA* title_update,
    const object_ref<UserModule> module) {
  uint32_t disc_number = -1;
  if (module->is_multi_disc_title()) {
    disc_number = module->disc_number();
  }

  uint32_t content_license = 0;
  X_RESULT open_status = content_manager()->OpenContent(
      "UPDATE", 0, *title_update, content_license, disc_number);

  std::string mount_path = "";
  if (!file_system()->FindSymbolicLink(kDefaultGameSymbolicLink, mount_path)) {
    return nullptr;
  }

  if (!module->path().starts_with(mount_path)) {
    return nullptr;
  }

  std::string resolved_path = "";
  if (!file_system()->FindSymbolicLink(kDefaultUpdateSymbolicLink,
                                       resolved_path)) {
    return nullptr;
  }

  const std::string relative_path =
      module->path().substr(mount_path.size() + 1) + 'p';

  xe::vfs::Entry* patch_entry =
      kernel_state()->file_system()->ResolvePath(resolved_path + relative_path);

  if (!patch_entry) {
    return nullptr;
  }

  const std::string patch_path = patch_entry->absolute_path();
  XELOGI("Loading XEX patch from {}", patch_path);
  auto patch_module = object_ref<UserModule>(new UserModule(this));

  X_RESULT result = patch_module->LoadFromFile(patch_path);
  if (result != X_STATUS_SUCCESS) {
    XELOGE("Failed to load XEX patch, code: {}", result);
    return nullptr;
  }

  return patch_module;
}

bool KernelState::IsPatchSignatureProper(
    const object_ref<UserModule> title_module,
    const object_ref<UserModule> patch_module) const {
  xex2_opt_delta_patch_descriptor* patch_header = nullptr;
  patch_module->GetOptHeader(XEX_HEADER_DELTA_PATCH_DESCRIPTOR,
                             reinterpret_cast<void**>(&patch_header));

  assert_not_null(patch_header);

  // Compare hash inside delta descriptor to base XEX signature
  uint8_t digest[0x14];
  sha1::SHA1 s;
  s.processBytes(title_module->xex_module()->xex_security_info()->rsa_signature,
                 0x100);
  s.finalize(digest);

  if (memcmp(digest, patch_header->digest_source, 0x14) != 0) {
    XELOGW(
        "XEX patch signature hash doesn't match base XEX signature hash, patch "
        "will likely fail!");

    return false;
  }
  return true;
}

X_RESULT KernelState::ApplyTitleUpdate(
    const object_ref<UserModule> title_module,
    const object_ref<UserModule> patch_module) {
  if (!title_module) {
    XELOGE("{}: No title_module provided!", __FUNCTION__);
    return X_STATUS_UNSUCCESSFUL;
  }

  if (!patch_module) {
    XELOGE("{}: No patch_module provided!", __FUNCTION__);
    return X_STATUS_UNSUCCESSFUL;
  }

  X_STATUS result =
      patch_module->xex_module()->ApplyPatch(title_module->xex_module());
  if (result != X_STATUS_SUCCESS) {
    XELOGE("Failed to apply XEX patch, code: {}", result);
  }
  return result;
}

void KernelState::UnloadUserModule(const object_ref<UserModule>& module,
                                   bool call_entry) {
  auto global_lock = global_critical_region_.Acquire();

  if (module->is_dll_module() && module->entry_point() && call_entry) {
    // Call DllMain(DLL_PROCESS_DETACH):
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
    uint64_t args[] = {
        module->handle(),
        0,  // DLL_PROCESS_DETACH
        0,  // 0 for now, assume XexUnloadImage is like FreeLibrary
    };
    auto thread_state = XThread::GetCurrentThread()->thread_state();
    processor()->Execute(thread_state, module->entry_point(), args,
                         xe::countof(args));
  }

  auto iter = std::ranges::find_if(user_modules_, [&module](const auto& e) {
    return e->path() == module->path();
  });
  assert_true(iter != user_modules_.end());  // Unloading an unregistered module
                                             // is probably really bad
  user_modules_.erase(iter);

  // Ensure this module was not somehow registered twice
  assert_true(std::find_if(user_modules_.begin(), user_modules_.end(),
                           [&module](const auto& e) {
                             return e->path() == module->path();
                           }) == user_modules_.end());

  object_table()->ReleaseHandleInLock(module->handle());
}

void KernelState::InitXmpVolumePatch() {
  xmp_volume_patch_ = XmpVolumePatch::CreateForTitle(title_id(), this);
}

// Phase 1099v: the real ExTerminateTitleProcess (xboxkrnlce 80056800) does not
// kill threads itself. It walks the title-terminate notification list
// (800D03E0) from the HIGHEST signed priority down, calling routine(&record),
// and never clears the list. The kernel's own subsystems are entries in that
// list; their host equivalents run here at the same priority slots:
//   0x6F000000 Ps  (800621C0) terminate every TITLE-process thread, wait for 0
//   0x6E000000 Ob  (80075528) close title handles        - NOT YET (no owner)
//   0x92000000 Xex (800678A0) unload title modules, clear XexExecutableModule-
//                             Handle, free 82000000-8BFFFFFF / 92000000-9FFFFFFF
//   0x91000000 Mm  (8006F090) free title physical memory - NOT YET (no owner)
// xam registers its own at 7C800000, 6EF00000, 0 and 81000000, and they run in
// between as guest code on this (system) thread.
// Phase 1099x: XexSendDeferredNotifications (real 80067518). Under the loader
// lock, ONCE per title (flag 800DEA38), 80067050 drains the deferred list
// 800D071C: for each loader entry flagged deferred (+0x4C & 0x08, not 0x80)
// with an entry point and not yet attached (+0x34 & 0x200), set attached and
// call DllMain(handle, DLL_PROCESS_ATTACH, 0); a FALSE return stops with
// C0000142. Xenia attaches DLLs at load, so the list is normally empty; this
// attaches whatever a load skipped (e.g. no guest thread at the time).
void KernelState::RestoreHeap0Alias(const char* why) {
  if (heap0_alias_address && !heap0_original.empty()) {
    std::memcpy(memory()->TranslateVirtual(heap0_alias_address),
                heap0_original.data(), heap0_original.size());
    XELOGI("{}: restored heap[0] descriptor at {:08X} ({} bytes) - the "
           "boot-time heap0 alias is undone",
           why, heap0_alias_address, heap0_original.size());
    heap0_alias_address = 0;
    heap0_original.clear();
  }
}

std::string KernelState::GuestBackChain(int max_frames) {
  auto* th = XThread::GetCurrentThread();
  if (!th) {
    return "(no guest thread)";
  }
  auto* ctx = th->thread_state()->context();
  std::string bt = fmt::format("lr {:08X}:", static_cast<uint32_t>(ctx->lr));
  uint32_t sp = static_cast<uint32_t>(ctx->r[1]);
  for (int f = 0; f < max_frames && sp; ++f) {
    uint32_t caller_sp = xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(sp));
    if (caller_sp <= sp || caller_sp - sp > 0x100000) break;
    uint32_t ra =
        xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(caller_sp - 8));
    bt += fmt::format(" {:08X}", ra);
    sp = caller_sp;
  }
  return bt;
}

X_STATUS KernelState::SendDeferredNotifications() {
  std::vector<object_ref<UserModule>> pending;
  {
    auto global_lock = global_critical_region_.Acquire();
    if (deferred_notifications_sent_) {
      return X_STATUS_SUCCESS;
    }
    deferred_notifications_sent_ = true;
    for (auto& module : user_modules_) {
      if (module->is_dll_module() && module->entry_point() &&
          !module->is_attached()) {
        pending.push_back(module);
      }
    }
  }
  auto cur_thread = XThread::GetCurrentThread();
  XELOGI("XexSendDeferredNotifications: {} module(s) not yet attached",
         pending.size());
  if (!cur_thread) {
    return X_STATUS_SUCCESS;
  }
  for (auto& module : pending) {
    module->is_attached_ = true;
    uint64_t args[] = {module->handle(), 1, 0};
    processor()->Execute(cur_thread->thread_state(), module->entry_point(),
                         args, xe::countof(args));
    const uint32_t ok = uint32_t(cur_thread->thread_state()->context()->r[3]);
    XELOGI("XexSendDeferredNotifications: DllMain({}) -> {}", module->name(),
           ok);
    if (!(ok & 0xFF)) {
      return X_STATUS(0xC0000142);  // STATUS_DLL_INIT_FAILED
    }
  }
  return X_STATUS_SUCCESS;
}

void KernelState::RecordTitleAllocation(uint32_t base) {
  std::lock_guard<std::mutex> lock(title_allocations_mutex_);
  title_allocations_.insert(base);
}

void KernelState::ForgetTitleAllocation(uint32_t base) {
  std::lock_guard<std::mutex> lock(title_allocations_mutex_);
  title_allocations_.erase(base);
}

uint32_t KernelState::ReleaseTitleAllocations(uint32_t* out_bytes) {
  std::set<uint32_t> bases;
  {
    std::lock_guard<std::mutex> lock(title_allocations_mutex_);
    bases.swap(title_allocations_);
  }
  uint32_t released = 0;
  uint32_t bytes = 0;
  for (uint32_t base : bases) {
    auto heap = memory()->LookupHeap(base);
    if (!heap || heap->heap_type() != HeapType::kGuestVirtual) {
      continue;
    }
    uint32_t size = 0;
    if (heap->Release(base, &size)) {
      ++released;
      bytes += size;
      // 1099z160: name each region (8 or so per switch).
      XELOGI("TitleSwitch:   Mm:   released virtual {:08X} size {:08X}", base,
             size);
    }
  }
  if (out_bytes) {
    *out_bytes = bytes;
  }
  return released;
}

void KernelState::RecordPhysicalAllocation(uint32_t base, uint32_t size,
                                           uint32_t type, uint32_t lr) {
  uint32_t proc = 0;
  if (auto* th = XThread::GetCurrentThread()) {
    if (auto* kt = th->guest_object<X_KTHREAD>()) {
      proc = kt->process_type;
    }
  }
  std::lock_guard<std::mutex> lock(title_allocations_mutex_);
  physical_allocations_[base] = {size, type, proc, lr};
}

void KernelState::ForgetPhysicalAllocation(uint32_t base) {
  std::lock_guard<std::mutex> lock(title_allocations_mutex_);
  physical_allocations_.erase(base);
}

uint32_t KernelState::ReleaseTitlePhysicalAllocations(
    const std::vector<uint32_t>& keep, uint64_t* out_bytes) {
  std::vector<std::pair<uint32_t, PhysicalAllocation>> victims;
  std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint64_t>> held;
  {
    std::lock_guard<std::mutex> lock(title_allocations_mutex_);
    for (auto it = physical_allocations_.begin();
         it != physical_allocations_.end();) {
      const auto& a = it->second;
      auto& h = held[{a.type, a.proc_type}];
      h.first++;
      h.second += a.size;
      // Type 1 = title, 2 = system, 0 = the calling thread's process.
      const bool title_owned =
          a.type == 1 || (a.type == 0 && a.proc_type == X_PROCTYPE_TITLE);
      bool kept = !title_owned;
      if (title_owned) {
        const uint32_t lo = it->first & 0x1FFFFFFF;
        const uint32_t hi = lo + a.size;
        for (uint32_t k : keep) {
          const uint32_t pk = k & 0x1FFFFFFF;
          if (k && pk >= lo && pk < hi) {
            kept = true;
            XELOGI("TitleSwitch:   Mm: keeping physical {:08X} size {:X} (in "
                   "use: {:08X})",
                   it->first, a.size, k);
            break;
          }
        }
      }
      if (kept) {
        ++it;
      } else {
        victims.emplace_back(it->first, a);
        it = physical_allocations_.erase(it);
      }
    }
  }
  for (const auto& [key, v] : held) {
    XELOGI("TitleSwitch:   Mm: physical held at terminate: type {} proc {} -> "
           "{} allocation(s), {:X} bytes",
           key.first, key.second, v.first, v.second);
  }
  uint32_t released = 0;
  uint64_t bytes = 0;
  for (const auto& [base, a] : victims) {
    auto heap = memory()->LookupHeap(base);
    if (heap && heap->Release(base)) {
      ++released;
      bytes += a.size;
    }
  }
  if (out_bytes) *out_bytes = bytes;
  return released;
}

// Suspends and terminates the threads pick() selects (guest threads only
// unless guest_only is false), never while one is in the emulator's own code
// or holds the global critical region. Returns how many were still running
// afterwards.
uint32_t KernelState::TerminateGuestThreadsSafely(
    const std::function<bool(XThread*)>& pick, const char* tag,
    bool guest_only) {
  // Snapshot the picked threads WITHOUT holding the global lock while killing.
  std::vector<object_ref<XThread>> victims;
  uint32_t kept = 0;
  {
    auto global_lock = global_critical_region_.Acquire();
    for (auto& kv : threads_by_id_) {
      auto* thread = kv.second;
      if ((thread->is_guest_thread() || !guest_only) &&
          !XThread::IsInThread(thread) && pick(thread)) {
        victims.push_back(retain_object(thread));
      } else {
        ++kept;
      }
    }
  }
#if XE_PLATFORM_WIN32
  // Host code of the emulator itself: a thread stopped in here may hold a
  // host lock (the global critical region, a heap lock, ...), and killing it
  // there abandons that lock - measured: the switch deadlocked on the next
  // lock acquire. Translated guest code and OS wait routines are safe.
  const HMODULE exe_module = GetModuleHandleW(nullptr);
  const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe_module);
  const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
      reinterpret_cast<uint8_t*>(exe_module) + dos->e_lfanew);
  const uint64_t exe_lo = reinterpret_cast<uint64_t>(exe_module);
  const uint64_t exe_hi = exe_lo + nt->OptionalHeader.SizeOfImage;
#endif
  uint32_t killed = 0, retries = 0, lock_retries = 0, not_killed = 0;
  for (auto& thread : victims) {
    if (!thread->is_running()) {
      ++killed;
      continue;
    }
    // NO StepToGuestSafePoint, unlike TerminateTitle: it only handles
    // exports tagged kBlocking and otherwise steps the thread until its
    // kernel call RETURNS - a title thread parked in a wait never returns
    // (measured: every dash thread was in a wait and the switch hung). These
    // threads are never resumed, so no synchronized guest context is needed;
    // the real kernel ends them with a terminate APC (800620D8).
    for (int attempt = 0; attempt < 5000; ++attempt) {
      auto* native = thread->thread();
#if XE_PLATFORM_WIN32
      HANDLE h = reinterpret_cast<HANDLE>(native->native_handle());
      if (SuspendThread(h) == static_cast<DWORD>(-1)) {
        break;
      }
      CONTEXT c = {};
      c.ContextFlags = CONTEXT_CONTROL;
      const bool got = GetThreadContext(h, &c) != 0;
      const bool in_emulator_host_code =
          got && c.Rip >= exe_lo && c.Rip < exe_hi;
      // Phase 1099z159: outside the exe is not enough. A thread parked in an
      // OS wait can still OWN the global critical region (a kernel export
      // that waits while holding it); killing it abandons the lock and every
      // later acquire hangs - measured: launching Fable III ~2 s after
      // inserting the disc hung this step in 3 of 8 runs, and a sampler
      // thread started afterwards blocked on the global lock too. The target
      // is suspended, so if the lock can be taken now, the target does not
      // hold it and cannot take it before it is killed.
      bool holds_global_lock = false;
      if (got && !in_emulator_host_code &&
          cvars::kernel_terminate_lock_check) {
        auto probe = xe::global_critical_region::TryAcquire();
        holds_global_lock = !probe.owns_lock();
      }
      if (in_emulator_host_code || holds_global_lock) {
        ResumeThread(h);
        ++retries;
        if (holds_global_lock) ++lock_retries;
        xe::threading::Sleep(std::chrono::milliseconds(1));
        continue;
      }
#else
      native->Suspend();
#endif
      thread->Terminate(0);
      break;
    }
    if (thread->is_running()) ++not_killed;
    ++killed;
    UnregisterThread(thread.get());
  }
  XELOGI("{}: terminated {} guest thread(s) ({} retries to leave host "
         "code, {} of them for the global lock; {} never reached a safe "
         "point), kept {} other(s)",
         tag, killed, retries, lock_retries, not_killed, kept);
  return not_killed;
}

// 2026-09-16 (disc-agent): the boot image Xenia pre-loads for
// kernel_boot_via_xam is not something a console has loaded. When xam's first
// launch names a different executable (6770 xam with a disc in the tray at
// power-on launches \Device\CdRom0\default.xex straight away), drop the
// never-started image so XexLoadExecutable sees no executable, as on a console.
// Measured before: -> C0000022 "an executable is still loaded (dash.xex)", xam
// fell back to relaunching the dashboard.
void KernelState::DiscardUnstartedBootImage(object_ref<UserModule> module) {
  if (!module || executable_module_.get() != module.get()) {
    return;
  }
  XELOGI("XexLoadExecutable: discarding the unstarted boot image {}",
         module->path());
  executable_module_ = nullptr;
  auto export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::XexExecutableModuleHandle);
  if (export_entry && export_entry->variable_ptr) {
    *memory()->TranslateVirtual<xe::be<uint32_t>*>(export_entry->variable_ptr) =
        0;
  }
  UnloadUserModule(module, false);
  module->Unload();
}

void KernelState::TerminateTitleProcessSelective() {
  title_switch_log_budget.store(300);
  if (title_terminate_hook) {
    title_terminate_hook();
  }
  deferred_notifications_sent_ = false;
  // Phase 1099w: undo lle_xam_heap0_alias before xam's terminate callbacks run.
  // The alias copies the SYSTEM heap[1] descriptor into heap[0], xam's per-title
  // workspace, because the boot title (launched by Xenia, not xam's launcher)
  // never got a workspace. xam tears heap[0] down as title memory here, which
  // with the alias destroyed and released its own system heap at 40000000
  // (measured: NtFreeVirtualMemory 40000000+1F0000 from 817B2A04, then crashes
  // on 401Axxxx-401Exxxx). Restored, heap[0] is the placeholder again and the
  // new title's start routine (8175DF50 -> 8175DC58 -> 817B4750) creates a
  // real workspace, as on a console.
  RestoreHeap0Alias("TitleSwitch");
  XELOGI("TitleSwitch: ExTerminateTitleProcess - {} registered notification(s)",
         terminate_notifications_.size());
  std::vector<TerminateNotification> ordered = terminate_notifications_;
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const TerminateNotification& a,
                      const TerminateNotification& b) {
                     return int32_t(a.priority) > int32_t(b.priority);
                   });

  auto run_guest = [&](const TerminateNotification& n) {
    auto* cur = XThread::GetCurrentThread();
    if (!cur || !n.guest_routine) return;
    XELOGI("TitleSwitch:   notify {:08X}(&{:08X}) priority {:08X}",
           n.guest_routine, n.record, n.priority);
    uint64_t args[] = {n.record};
    processor()->Execute(cur->thread_state(), n.guest_routine, args,
                         xe::countof(args));
  };

  auto kill_title_threads = [&]() {
    TerminateGuestThreadsSafely(
        [](XThread* thread) {
          return thread->guest_object<X_KTHREAD>() &&
                 thread->guest_object<X_KTHREAD>()->process_type ==
                     X_PROCTYPE_TITLE;
        },
        "TitleSwitch:   Ps");
  };

  auto unload_title_executable = [&]() {
    auto exe = executable_module_;
    if (!exe) {
      XELOGI("TitleSwitch:   Xex: no executable module loaded");
      return;
    }
    XELOGI("TitleSwitch:   Xex: unloading title executable {}", exe->path());
    executable_module_ = nullptr;
    auto export_entry = processor()->export_resolver()->GetExportByOrdinal(
        "xboxkrnl.exe", ordinals::XexExecutableModuleHandle);
    if (export_entry && export_entry->variable_ptr) {
      *memory()->TranslateVirtual<xe::be<uint32_t>*>(
          export_entry->variable_ptr) = 0;
    }
    // DECLARED HOST CLEANUP, not traced from the real kernel: callbacks the
    // title registered with the video and audio drivers point into its image.
    // Left in place they fire into released memory until the next title
    // re-registers (measured 1099x: 921AFCB0 x3081, 924160C0 x125).
    if (exe->xex_module()) {
      const uint32_t low = exe->xex_module()->low_address();
      const uint32_t high = exe->xex_module()->high_address();
      auto* gs = emulator()->graphics_system();
      if (gs && gs->interrupt_callback() >= low &&
          gs->interrupt_callback() < high) {
        XELOGI("TitleSwitch:   Xex: clearing graphics interrupt callback {:08X}",
               gs->interrupt_callback());
        gs->SetInterruptCallback(0, 0);
      }
      if (auto* as = emulator()->audio_system()) {
        XELOGI("TitleSwitch:   Xex: unregistered {} audio client(s) in "
               "{:08X}-{:08X}",
               as->UnregisterClientsInRange(low, high), low, high);
      }
    }
    UnloadUserModule(exe, false);
    // Unload now. Left to the destructor, it runs whenever the last ref
    // drops - after the new dash is loaded at the same base - and releases
    // that image's memory and its processor registration.
    auto status = exe->Unload();
    XELOGI("TitleSwitch:   Xex: image released status={:08X} refs held "
           "elsewhere may still exist",
           status);
    // Phase 1099z161: DLLs the title process loaded go with it (the console
    // unloads the whole title process image list). Measured: dash 17559 loads
    // SEP\20449700\dashnui.xex; left loaded across Dash -> Avatar Editor ->
    // Dash, the second dash's load returned the stale module, its Kinect
    // function table (9218B5A0..) was never bound, and it called address 0
    // (9218B3C0 via [92992CDC]). No DllMain detach: the title's threads are
    // already gone.
    if (cvars::kernel_title_unload_dlls) {
      std::vector<object_ref<UserModule>> dlls;
      for (auto& m : user_modules_) {
        if (m->loaded_by_title() && m.get() != exe.get()) dlls.push_back(m);
      }
      for (auto& dll : dlls) {
        XELOGI("TitleSwitch:   Xex: unloading title DLL {}", dll->path());
        if (dll->xex_module()) {
          const uint32_t low = dll->xex_module()->low_address();
          const uint32_t high = dll->xex_module()->high_address();
          auto* gs = emulator()->graphics_system();
          if (gs && gs->interrupt_callback() >= low &&
              gs->interrupt_callback() < high) {
            gs->SetInterruptCallback(0, 0);
          }
          if (auto* as = emulator()->audio_system()) {
            as->UnregisterClientsInRange(low, high);
          }
        }
        UnloadUserModule(dll, false);
        dll->Unload();
      }
    }
  };

  bool did_ps = false, did_ob = false, did_xex = false, did_mm = false;
  bool did_vd9 = false, did_vd5 = false;
  auto run_kernel_slots_above = [&](int32_t priority) {
    if (!did_ps && int32_t(0x6F000000) > priority) {
      did_ps = true;
      kill_title_threads();
    }
    // Phase 1099z139: the kernel's video notifications. At an equal priority
    // the guest registrations run first here; the real list order for ties
    // was not read.
    if (!did_vd9 && int32_t(0x6E800000) > priority) {
      did_vd9 = true;
      xboxkrnl::GuideKernelEngineTerminateSlot(false);
    }
    if (!did_ob && int32_t(0x6E000000) > priority) {
      did_ob = true;
      // Phase 1099z47: close the title's handles - those created by threads
      // of the title process (real Ob slot 80075528 empties the title handle
      // table; objects still referenced elsewhere stay alive).
      if (cvars::guide_title_switch_close_handles) {
        // Phase 1099z160: Xenia's wrapper for a dispatcher the guest built in
        // its own memory is not a handle on the console; the Ob slot cannot
        // close it. With --kernel_ob_keep_guest_dispatchers such an entry is
        // left alone unless its dispatcher lives in memory this terminate is
        // about to release (the title image or a title virtual allocation),
        // where the console object ends with its memory.
        std::function<bool(uint32_t)> keep;
        if (cvars::kernel_ob_keep_guest_dispatchers) {
          uint32_t img_lo = 0, img_hi = 0;
          if (executable_module_ && executable_module_->xex_module()) {
            img_lo = executable_module_->xex_module()->base_address();
            img_hi = img_lo + executable_module_->xex_module()->image_size();
          }
          std::vector<std::pair<uint32_t, uint32_t>> regions;
          {
            std::lock_guard<std::mutex> lock(title_allocations_mutex_);
            for (uint32_t base : title_allocations_) {
              auto* heap = memory()->LookupHeap(base);
              HeapAllocationInfo info = {};
              if (heap && heap->QueryRegionInfo(base, &info) &&
                  info.allocation_base == base) {
                // Walk the allocation's regions to its end.
                uint32_t end = base;
                while (heap->QueryRegionInfo(end, &info) &&
                       info.allocation_base == base && info.region_size) {
                  end = info.base_address + info.region_size;
                }
                regions.emplace_back(base, end);
              }
            }
          }
          keep = [img_lo, img_hi, regions](uint32_t guest) {
            if (guest >= img_lo && guest < img_hi) return false;
            for (const auto& r : regions) {
              if (guest >= r.first && guest < r.second) return false;
            }
            return true;
          };
        }
        uint32_t kept = 0, wrappers = 0;
        const uint32_t closed = object_table()->CloseHandlesOwnedBy(
            static_cast<uint8_t>(X_PROCTYPE_TITLE), keep, &kept, &wrappers);
        XELOGI("TitleSwitch:   Ob: closed {} title handle(s); guest dispatcher "
               "wrappers: {} seen, {} kept",
               closed, wrappers, kept);
      } else {
        XELOGI("TitleSwitch:   Ob: title handle close disabled");
      }
    }
    if (!did_vd5 && int32_t(0x6D000000) > priority) {
      did_vd5 = true;
      xboxkrnl::GuideKernelEngineTerminateSlot(true);
    }
    if (!did_xex && int32_t(0x92000000) > priority) {
      did_xex = true;
      unload_title_executable();
    }
    if (!did_mm && int32_t(0x91000000) > priority) {
      did_mm = true;
      // Phase 1099z47: release the title's NtAllocateVirtualMemory regions.
      // Phase 1099z97: and its physical allocations, except the live ring
      // buffer and the persisted front buffer (the GPU still references
      // them until the next title takes over) - without this a second launch
      // of the same game found the physical heap empty.
      if (cvars::guide_title_switch_release_memory) {
        uint32_t bytes = 0;
        const uint32_t released = ReleaseTitleAllocations(&bytes);
        XELOGI("TitleSwitch:   Mm: released {} title virtual region(s), {:X} "
               "bytes",
               released, bytes);
        std::vector<uint32_t> keep;
        xboxkrnl::GuideTitleSwitchKeepAddresses(&keep);
        uint64_t pbytes = 0;
        const uint32_t preleased =
            ReleaseTitlePhysicalAllocations(keep, &pbytes);
        XELOGI("TitleSwitch:   Mm: released {} title physical allocation(s), "
               "{:X} bytes",
               preleased, pbytes);
        // Phase 1099z147: what the next title can get in one piece.
        if (auto* ph = memory()->GetPhysicalHeap()) {
          uint32_t run_base = 0;
          const uint32_t run = ph->LargestFreeRun(&run_base);
          XELOGI("TitleSwitch:   Mm: physical free {}/{} pages, largest free "
                 "run {:X} bytes at {:08X}",
                 ph->unreserved_page_count(), ph->total_page_count(), run,
                 run_base);
          // Who bounds that run: the tracked allocations just above and below.
          const uint32_t run_end = run_base + run;
          HeapAllocationInfo above = {};
          if (run_end < ph->heap_size() &&
              ph->QueryRegionInfo(run_end, &above)) {
            XELOGI("TitleSwitch:   Mm:   region above the run: base {:08X} "
                   "size {:X} alloc_base {:08X} alloc_protect {:X} state {:X}",
                   above.base_address, above.region_size,
                   above.allocation_base, above.allocation_protect,
                   above.state);
          }
          std::lock_guard<std::mutex> lock(title_allocations_mutex_);
          for (const auto& kv : physical_allocations_) {
            const uint32_t phys =
                (kv.first & 0x1FFFFFFF) +
                ((kv.first >= 0xE0000000u) ? 0x1000u : 0u);
            if ((phys >= run_end && phys < run_end + 0x100000) ||
                (phys + kv.second.size <= run_base &&
                 phys + kv.second.size + 0x100000 > run_base)) {
              XELOGI("TitleSwitch:   Mm:   bounding allocation {:08X} (phys "
                     "{:08X}) size {:X} type {} proc {} lr {:08X}",
                     kv.first, phys, kv.second.size, kv.second.type,
                     kv.second.proc_type, kv.second.lr);
            }
          }
        }
      } else {
        XELOGI("TitleSwitch:   Mm: title memory release disabled");
      }
    }
  };

  for (const auto& n : ordered) {
    run_kernel_slots_above(int32_t(n.priority));
    run_guest(n);
  }
  run_kernel_slots_above(INT32_MIN);
  XELOGI("TitleSwitch: ExTerminateTitleProcess done");
}

void KernelState::TerminateTitle() {
  XELOGD("KernelState::TerminateTitle");
  xmp_volume_patch_.reset();
  auto global_lock = global_critical_region_.Acquire();

  // Call terminate routines.
  // TODO(benvanik): these might take arguments.
  // FIXME: Calling these will send some threads into kernel code and they'll
  // hold the lock when terminated! Do we need to wait for all threads to exit?
  /*
  if (from_guest_thread) {
    for (auto routine : terminate_notifications_) {
      auto thread_state = XThread::GetCurrentThread()->thread_state();
      processor()->Execute(thread_state, routine.guest_routine);
    }
  }
  terminate_notifications_.clear();
  */

  // Kill all guest threads.
  for (auto it = threads_by_id_.begin(); it != threads_by_id_.end();) {
    if (!XThread::IsInThread(it->second) && it->second->is_guest_thread()) {
      auto thread = it->second;

      if (thread->is_running()) {
        // Need to step the thread to a safe point (returns it to guest code
        // so it's guaranteed to not be holding any locks / in host kernel
        // code / etc). Can't do that properly if we have the lock.
        if (!emulator_->is_paused()) {
          thread->thread()->Suspend();
        }

        global_lock.unlock();
        processor_->StepToGuestSafePoint(thread->thread_id());
        thread->Terminate(0);
        global_lock.lock();
      }

      // Erase it from the thread list.
      it = threads_by_id_.erase(it);
    } else {
      ++it;
    }
  }

  // Third: Unload all user modules (including the executable).
  for (size_t i = 0; i < user_modules_.size(); i++) {
    user_modules_[i]->ReleaseHandle();
  }
  user_modules_.clear();

  // Release all objects in the object table.
  object_table_.PurgeAllObjects();

  // Unregister all notify listeners.
  notify_listeners_.clear();

  // Unset the executable module.
  executable_module_ = nullptr;

  if (XThread::IsInThread()) {
    threads_by_id_.erase(XThread::GetCurrentThread()->thread_id());

    // Now commit suicide (using Terminate, because we can't call into guest
    // code anymore).
    global_lock.unlock();
    XThread::GetCurrentThread()->Terminate(0);
  }
}

void KernelState::RegisterThread(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  threads_by_id_[thread->thread_id()] = thread;
}

void KernelState::UnregisterThread(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = threads_by_id_.find(thread->thread_id());
  if (it != threads_by_id_.end()) {
    threads_by_id_.erase(it);
  }
}

void KernelState::OnThreadExecute(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();

  // Must be called on executing thread.
  assert_true(XThread::GetCurrentThread() == thread);

  // Call DllMain(DLL_THREAD_ATTACH) for each user module:
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
  auto thread_state = thread->thread_state();
  for (auto user_module : user_modules_) {
    if (user_module->is_dll_module() && user_module->entry_point()) {
      uint64_t args[] = {
          user_module->handle(),
          user_module->is_attached()
              ? static_cast<uint64_t>(2)   // DLL_THREAD_ATTACH - Used to call
                                           // DLL for each thread created.
              : static_cast<uint64_t>(1),  // DLL_PROCESS_ATTACH - Used only
                                           // once for initialization.
          0,                               // 0 because always dynamic
      };

      user_module->is_attached_ = true;

      processor()->Execute(thread_state, user_module->entry_point(), args,
                           xe::countof(args));
    }
  }
}

void KernelState::OnThreadExit(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();

  // Must be called on executing thread.
  assert_true(XThread::GetCurrentThread() == thread);

  // Call DllMain(DLL_THREAD_DETACH) for each user module:
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
  auto thread_state = thread->thread_state();
  for (auto user_module : user_modules_) {
    if (user_module->is_dll_module() && user_module->entry_point()) {
      uint64_t args[] = {
          user_module->handle(),
          3,  // DLL_THREAD_DETACH
          0,  // 0 because always dynamic
      };
      processor()->Execute(thread_state, user_module->entry_point(), args,
                           xe::countof(args));
    }
  }

  emulator()->processor()->OnThreadExit(thread->thread_id());
}

object_ref<XThread> KernelState::GetThreadByID(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  XThread* thread = nullptr;
  auto it = threads_by_id_.find(thread_id);
  if (it != threads_by_id_.end()) {
    thread = it->second;
  }
  return retain_object(thread);
}

std::vector<uint32_t> KernelState::GetAllThreadIDs() {
  auto global_lock = global_critical_region_.Acquire();

  auto thread_ids_view =
      threads_by_id_ |
      std::views::transform([](const auto& pair) { return pair.first; });

  std::vector<std::uint32_t> thread_ids(thread_ids_view.begin(),
                                        thread_ids_view.end());

  return thread_ids;
}

void KernelState::RegisterNotifyListener(XNotifyListener* listener) {
  auto global_lock = global_critical_region_.Acquire();
  notify_listeners_.push_back(retain_object(listener));

  // Games seem to expect a few notifications on startup, only for the first
  // listener.
  // https://cs.rin.ru/forum/viewtopic.php?f=38&t=60668&hilit=resident+evil+5&start=375
  if (!has_notified_startup_ && listener->mask() & kXNotifySystem) {
    has_notified_startup_ = true;
    listener->EnqueueNotification(kXNotificationSystemUI,
                                  xam_state()->IsUIActive());
    listener->EnqueueNotification(kXNotificationSystemSignInChanged, 1);
  }
  if (!has_notified_live_startup_ && listener->mask() & kXNotifyLive) {
    has_notified_live_startup_ = true;
    // X_ONLINE_S_LOGON_DISCONNECTED
    listener->EnqueueNotification(kXNotificationLiveConnectionChanged,
                                  0x001510F1L);
    listener->EnqueueNotification(kXNotificationLiveLinkStateChanged, 0);
  }
}

void KernelState::UnregisterNotifyListener(XNotifyListener* listener) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto it = notify_listeners_.begin(); it != notify_listeners_.end();
       ++it) {
    if ((*it).get() == listener) {
      notify_listeners_.erase(it);
      break;
    }
  }
}

void KernelState::BroadcastNotification(XNotificationID id, uint32_t data) {
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& notify_listener : notify_listeners_) {
    notify_listener->EnqueueNotification(id, data);
  }
}

void KernelState::CompleteOverlapped(uint32_t overlapped_ptr, X_RESULT result) {
  CompleteOverlappedEx(overlapped_ptr, result, result, 0);
}

void KernelState::CompleteOverlappedEx(uint32_t overlapped_ptr, X_RESULT result,
                                       uint32_t extended_error,
                                       uint32_t length) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, result);
  XOverlappedSetExtendedError(ptr, extended_error);
  XOverlappedSetLength(ptr, length);
  X_HANDLE event_handle = XOverlappedGetEvent(ptr);
  if (event_handle) {
    auto ev = object_table()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    if (ev) {
      ev->Set(0, false);
    }
  }
  if (XOverlappedGetCompletionRoutine(ptr)) {
    X_HANDLE thread_handle = XOverlappedGetContext(ptr);
    auto thread = object_table()->LookupObject<XThread>(thread_handle);
    if (thread) {
      // Queue APC on the thread that requested the overlapped operation.
      uint32_t routine = XOverlappedGetCompletionRoutine(ptr);
      thread->EnqueueApc(routine, result, length, overlapped_ptr);
    }
  }
}

void KernelState::CompleteOverlappedImmediate(uint32_t overlapped_ptr,
                                              X_RESULT result) {
  // TODO(gibbed): there are games that check 'length' of overlapped as
  // an indication of success. WTF?
  // Setting length to -1 when not success seems to be helping.
  uint32_t length = !result ? 0 : 0xFFFFFFFF;
  CompleteOverlappedImmediateEx(overlapped_ptr, result, result, length);
}

void KernelState::CompleteOverlappedImmediateEx(uint32_t overlapped_ptr,
                                                X_RESULT result,
                                                uint32_t extended_error,
                                                uint32_t length) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetContext(ptr, XThread::GetCurrentThreadHandle());
  CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
}

void KernelState::CompleteOverlappedDeferred(
    std::function<void()> completion_callback, uint32_t overlapped_ptr,
    X_RESULT result, std::function<void()> pre_callback,
    std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(std::move(completion_callback), overlapped_ptr,
                               result, result, 0, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferredEx(
    std::function<void()> completion_callback, uint32_t overlapped_ptr,
    X_RESULT result, uint32_t extended_error, uint32_t length,
    std::function<void()> pre_callback, std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback, result, extended_error, length](
          uint32_t& cb_extended_error, uint32_t& cb_length) -> X_RESULT {
        completion_callback();
        cb_extended_error = extended_error;
        cb_length = length;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferred(
    std::function<X_RESULT()> completion_callback, uint32_t overlapped_ptr,
    std::function<void()> pre_callback, std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback](uint32_t& extended_error,
                            uint32_t& length) -> X_RESULT {
        auto result = completion_callback();
        extended_error = static_cast<uint32_t>(result);
        length = 0;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferredEx(
    std::function<X_RESULT(uint32_t&, uint32_t&)> completion_callback,
    uint32_t overlapped_ptr, std::function<void()> pre_callback,
    std::function<void()> post_callback) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, X_ERROR_IO_PENDING);
  XOverlappedSetContext(ptr, XThread::GetCurrentThreadHandle());
  X_HANDLE event_handle = XOverlappedGetEvent(ptr);
  if (event_handle) {
    auto ev = object_table()->LookupObject<XObject>(event_handle);

    assert_not_null(ev);
    if (ev && ev->type() == XObject::Type::Event) {
      ev.get<XEvent>()->Reset();
    }
  }
  auto global_lock = global_critical_region_.Acquire();
  dispatch_queue_.push_back([this, completion_callback, overlapped_ptr,
                             pre_callback, post_callback]() {
    if (pre_callback) {
      pre_callback();
    }
    // 5454082B infinitely loads free roam in netplay without sleep.
    xe::threading::Sleep(kDeferredOverlappedDelayMillis);
    uint32_t extended_error, length;
    auto result = completion_callback(extended_error, length);
    CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
    if (post_callback) {
      post_callback();
    }
  });
  dispatch_cond_.notify_all();
}

bool KernelState::Save(ByteStream* stream) {
  XELOGD("Serializing the kernel...");
  stream->Write(kKernelSaveSignature);

  // Save the object table
  object_table_.Save(stream);

  // Write the TLS allocation bitmap
  // We save XThreads absolutely first, as they will execute code upon save
  // (which could modify the kernel state)
  auto threads = object_table_.GetObjectsByType<XThread>();
  uint32_t* num_threads_ptr =
      reinterpret_cast<uint32_t*>(stream->data() + stream->offset());
  stream->Write(static_cast<uint32_t>(threads.size()));

  size_t num_threads = threads.size();
  XELOGD("Serializing {} threads...", threads.size());
  for (auto thread : threads) {
    if (!thread->is_guest_thread()) {
      // Don't save host threads. They can be reconstructed on startup.
      num_threads--;
      continue;
    }

    if (!thread->Save(stream)) {
      XELOGD("Failed to save thread \"{}\"", thread->name());
      num_threads--;
    }
  }

  *num_threads_ptr = static_cast<uint32_t>(num_threads);

  // Save all other objects
  auto objects = object_table_.GetAllObjects();
  uint32_t* num_objects_ptr =
      reinterpret_cast<uint32_t*>(stream->data() + stream->offset());
  stream->Write(static_cast<uint32_t>(objects.size()));

  size_t num_objects = objects.size();
  XELOGD("Serializing {} objects...", num_objects);
  for (auto object : objects) {
    auto prev_offset = stream->offset();

    if (object->is_host_object() || object->type() == XObject::Type::Thread) {
      // Don't save host objects or save XThreads again
      num_objects--;
      continue;
    }

    stream->Write<uint32_t>(static_cast<uint32_t>(object->type()));
    if (!object->Save(stream)) {
      XELOGD("Did not save object of type {}",
             static_cast<uint32_t>(object->type()));
      assert_always();

      // Revert backwards and overwrite if a save failed.
      stream->set_offset(prev_offset);
      num_objects--;
    }
  }

  *num_objects_ptr = static_cast<uint32_t>(num_objects);
  return true;
}

// this only gets triggered once per ms at most, so fields other than tick count
// will probably not be updated in a timely manner for guest code that uses them
void KernelState::UpdateKeTimestampBundle() {
  X_TIME_STAMP_BUNDLE* lpKeTimeStampBundle =
      memory_->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(ke_timestamp_bundle_ptr_);
  uint32_t uptime_ms = Clock::QueryGuestUptimeMillis();
  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->interrupt_time,
                               Clock::QueryGuestInterruptTime());
  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->system_time,
                               Clock::QueryGuestSystemTime());
  xe::store_and_swap<uint32_t>(&lpKeTimeStampBundle->tick_count, uptime_ms);

  // Every 20 ticks (~20ms), decay priority on running guest threads.
  // This simulates the Xenon decrementer-driven quantum expiration.
  if (++quantum_timer_counter_ >= 20) {
    quantum_timer_counter_ = 0;
    auto global_lock = global_critical_region_.Acquire();
    for (auto& [id, thread] : threads_by_id_) {
      if (thread->is_running()) {
        thread->CheckQuantumAndDecay();
      }
    }
  }
}

uint32_t KernelState::GetKeTimestampBundle() {
  XE_LIKELY_IF(ke_timestamp_bundle_ptr_) { return ke_timestamp_bundle_ptr_; }
  else {
    global_critical_region::PrepareToAcquire();
    return CreateKeTimestampBundle();
  }
}

XE_NOINLINE
XE_COLD
uint32_t KernelState::CreateKeTimestampBundle() {
  auto crit = global_critical_region::Acquire();

  const uint32_t pKeTimeStampBundle = 0x80240EE0;
  X_TIME_STAMP_BUNDLE* lpKeTimeStampBundle =
      memory_->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(pKeTimeStampBundle);

  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->interrupt_time,
                               Clock::QueryGuestInterruptTime());

  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->system_time,
                               Clock::QueryGuestSystemTime());

  xe::store_and_swap<uint32_t>(&lpKeTimeStampBundle->tick_count,
                               Clock::QueryGuestUptimeMillis());

  xe::store_and_swap<uint32_t>(&lpKeTimeStampBundle->padding, 0);

  ke_timestamp_bundle_ptr_ = pKeTimeStampBundle;
  timestamp_timer_ = xe::threading::HighResolutionTimer::CreateRepeating(
      std::chrono::milliseconds(1),
      [this]() { this->UpdateKeTimestampBundle(); });
  return pKeTimeStampBundle;
}

bool KernelState::Restore(ByteStream* stream) {
  // Check the magic value.
  if (stream->Read<uint32_t>() != kKernelSaveSignature) {
    return false;
  }

  // Restore the object table
  object_table_.Restore(stream);

  // TLS bitmap is now stored per-process in X_KPROCESS structures (in guest
  // memory) Skip reading old global TLS bitmap if present in old save files
  auto num_bitmap_entries = stream->Read<uint32_t>();
  for (uint32_t i = 0; i < num_bitmap_entries; i++) {
    stream->Read<uint64_t>();  // Discard old data
  }

  uint32_t num_threads = stream->Read<uint32_t>();
  XELOGD("Loading {} threads...", num_threads);
  for (uint32_t i = 0; i < num_threads; i++) {
    auto thread = XObject::Restore(this, XObject::Type::Thread, stream);
    if (!thread) {
      // Can't continue the restore or we risk misalignment.
      assert_always();
      return false;
    }
  }

  uint32_t num_objects = stream->Read<uint32_t>();
  XELOGD("Loading {} objects...", num_objects);
  for (uint32_t i = 0; i < num_objects; i++) {
    uint32_t type = stream->Read<uint32_t>();

    auto obj = XObject::Restore(this, XObject::Type(type), stream);
    if (!obj) {
      // Can't continue the restore or we risk misalignment.
      assert_always();
      return false;
    }
  }

  return true;
}

std::bitset<4> KernelState::GetConnectedUsers() const {
  auto input_sys = emulator_->input_system();

  auto lock = input_sys->lock();

  return input_sys->GetConnectedSlots();
}
// todo: definitely need to do more to pretend to be in a dpc
void KernelState::BeginDPCImpersonation(cpu::ppc::PPCContext* context,
                                        DPCImpersonationScope& scope) {
  auto kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  xenia_assert(kpcr->prcb_data.dpc_active == 0);
  scope.previous_irql_ = kpcr->current_irql;

  kpcr->current_irql = 2;
  kpcr->prcb_data.dpc_active = 1;
}
void KernelState::EndDPCImpersonation(cpu::ppc::PPCContext* context,
                                      DPCImpersonationScope& end_scope) {
  auto kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  xenia_assert(kpcr->prcb_data.dpc_active == 1);
  kpcr->current_irql = end_scope.previous_irql_;
  kpcr->prcb_data.dpc_active = 0;
}
void KernelState::EmulateCPInterruptDPC(uint32_t interrupt_callback,
                                        uint32_t interrupt_callback_data,
                                        uint32_t source, uint32_t cpu) {
  if (!interrupt_callback) {
    return;
  }

  auto thread = kernel::XThread::GetCurrentThread();
  assert_not_null(thread);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCpu(cpu);

  /*
    in reality, our interrupt is a callback that is called in a dpc which is
    scheduled by the actual interrupt

    we need to impersonate a dpc
  */
  auto current_context = thread->thread_state()->context();
  auto kthread = memory()->TranslateVirtual<X_KTHREAD*>(thread->guest_object());

  auto pcr = memory()->TranslateVirtual<X_KPCR*>(thread->pcr_ptr());

  DPCImpersonationScope dpc_scope{};
  BeginDPCImpersonation(current_context, dpc_scope);

  // 17489 kernel 80102E00..14 (and 80102F84..98): the ISR runs as process
  // type 1 + (VdGlobalXamDevice [801E6FC8] != 0). xam's D3D picks its device
  // by process type, so running it as TITLE while only xam's device exists
  // (the launch fade, notification 5) made it use VdGlobalDevice = 0.
  uint32_t isr_type = X_PROCTYPE_TITLE;
  if (cvars::kernel_isr_process_type &&
      xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(0x801E6FC8u))) {
    isr_type = X_PROCTYPE_SYSTEM;
  }
  xboxkrnl::xeKeSetCurrentProcessType(isr_type, current_context);

  uint64_t args[] = {source, interrupt_callback_data};
  processor_->Execute(thread->thread_state(), interrupt_callback, args,
                      xe::countof(args));
  xboxkrnl::xeKeSetCurrentProcessType(X_PROCTYPE_IDLE, current_context);

  EndDPCImpersonation(current_context, dpc_scope);
}

void KernelState::InitializeProcess(X_KPROCESS* process, uint32_t type,
                                    char priority_class, char default_priority,
                                    char max_dynamic_priority) {
  uint32_t guest_kprocess = memory()->HostToGuestVirtual(process);

  uint32_t thread_list_guest_ptr =
      guest_kprocess + offsetof(X_KPROCESS, thread_list);

  process->process_priority_class = priority_class;
  process->default_thread_priority = default_priority;
  process->max_dynamic_priority = max_dynamic_priority;
  util::XeInitializeListHead(&process->thread_list, thread_list_guest_ptr);
  process->quantum = 60;
  // doubt any guest code uses this ptr, which i think probably has something to
  // do with the page table
  process->clrdataa_masked_ptr = 0;
  // clrdataa_ & ~(1U << 31);
  process->thread_count = 0;
  process->disable_quantum_decay = 0x06;
  process->kernel_stack_size = 16 * 1024;
  process->tls_slot_size = 0x80;

  process->process_type = type;
  uint32_t unk_list_guest_ptr = guest_kprocess + offsetof(X_KPROCESS, unk_54);
  // TODO(benvanik): figure out what this list is.
  util::XeInitializeListHead(&process->unk_54, unk_list_guest_ptr);
}

void KernelState::SetProcessTLSVars(X_KPROCESS* process, int num_slots,
                                    int tls_data_size,
                                    int tls_static_data_address) {
  uint32_t slots_padded = (num_slots + 3) & 0xFFFFFFFC;
  process->tls_data_size = tls_data_size;
  process->tls_raw_data_size = tls_data_size;
  process->tls_static_data_address = tls_static_data_address;
  process->tls_slot_size = 4 * slots_padded;
  uint32_t count_div32 = slots_padded / 32;
  for (unsigned word_index = 0; word_index < count_div32; ++word_index) {
    process->tls_slot_bitmap[word_index] = -1;
  }

  // set remainder of bitset
  if (((num_slots + 3) & 0x1C) != 0) {
    process->tls_slot_bitmap[count_div32] = -1
                                            << (32 - ((num_slots + 3) & 0x1C));
  }
}
void AllocateThread(PPCContext* context) {
  uint32_t thread_mem_size = static_cast<uint32_t>(context->r[3]);
  uint32_t a2 = static_cast<uint32_t>(context->r[4]);
  uint32_t a3 = static_cast<uint32_t>(context->r[5]);
  if (thread_mem_size <= 0xFD8) {
    thread_mem_size += 8;
  }
  uint32_t result =
      xboxkrnl::xeAllocatePoolTypeWithTag(context, thread_mem_size, a2, a3);
  if (((unsigned short)result & 0xFFF) != 0) {
    result += 2;
  }

  context->r[3] = static_cast<uint64_t>(result);
}
void FreeThread(PPCContext* context) {
  uint32_t thread_memory = static_cast<uint32_t>(context->r[3]);
  if ((thread_memory & 0xFFF) != 0) {
    thread_memory -= 8;
  }
  xboxkrnl::xeFreePool(context, thread_memory);
}

void SimpleForwardAllocatePoolTypeWithTag(PPCContext* context) {
  uint32_t a1 = static_cast<uint32_t>(context->r[3]);
  uint32_t a2 = static_cast<uint32_t>(context->r[4]);
  uint32_t a3 = static_cast<uint32_t>(context->r[5]);
  context->r[3] = static_cast<uint64_t>(
      xboxkrnl::xeAllocatePoolTypeWithTag(context, a1, a2, a3));
}
void SimpleForwardFreePool(PPCContext* context) {
  xboxkrnl::xeFreePool(context, static_cast<uint32_t>(context->r[3]));
}

void DeleteMutant(PPCContext* context) {
  // todo: this should call kereleasemutant with some specific args

  xe::FatalError("DeleteMutant - need KeReleaseMutant(mutant, 1, 1, 0) ");
}
void DeleteTimer(PPCContext* context) {
  // todo: this should call KeCancelTimer
  xe::FatalError("DeleteTimer - need KeCancelTimer(mutant, 1, 1, 0) ");
}

void DeleteIoCompletion(PPCContext* context) {}

void UnknownProcIoDevice(PPCContext* context) {}

void CloseFileProc(PPCContext* context) {}

void DeleteFileProc(PPCContext* context) {}

void UnknownFileProc(PPCContext* context) {}

void DeleteSymlink(PPCContext* context) {
  X_KSYMLINK* lnk = context->TranslateVirtualGPR<X_KSYMLINK*>(context->r[3]);

  context->r[3] = lnk->refed_object_maybe;
  xboxkrnl::xeObDereferenceObject(context, lnk->refed_object_maybe);
}
void KernelState::InitializeKernelGuestGlobals() {
  kernel_guest_globals_ = memory_->SystemHeapAlloc(sizeof(KernelGuestGlobals));

  KernelGuestGlobals* block =
      memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);
  memset(block, 0, sizeof(KernelGuestGlobals));

  auto idle_process = memory()->TranslateVirtual<X_KPROCESS*>(GetIdleProcess());
  InitializeProcess(idle_process, X_PROCTYPE_IDLE, 0, 0, 0);
  idle_process->quantum = 0x7F;
  auto system_process =
      memory()->TranslateVirtual<X_KPROCESS*>(GetSystemProcess());
  InitializeProcess(system_process, X_PROCTYPE_SYSTEM, 2, 5, 9);
  SetProcessTLSVars(system_process, 32, 0, 0);
  // Phase 1099z125: the title process too, so a thread can be created in it
  // before the first title (the kernel's boot animation thread). Its TLS vars
  // are set per title in SetExecutableModule.
  auto title_process = memory()->TranslateVirtual<X_KPROCESS*>(GetTitleProcess());
  InitializeProcess(title_process, X_PROCTYPE_TITLE, 10, 13, 17);

  uint32_t oddobject_offset =
      kernel_guest_globals_ +
      offsetof(KernelGuestGlobals, XboxKernelDefaultObject);

  // init unknown object

  block->XboxKernelDefaultObject.type = EventSynchronizationObject;
  block->XboxKernelDefaultObject.signal_state = 1;
  block->XboxKernelDefaultObject.wait_list.flink_ptr =
      oddobject_offset + offsetof(X_DISPATCH_HEADER, wait_list.flink_ptr);
  block->XboxKernelDefaultObject.wait_list.blink_ptr =
      block->XboxKernelDefaultObject.wait_list.flink_ptr;

  // init thread object
  block->ExThreadObjectType.pool_tag = 0x65726854;
  block->ExThreadObjectType.allocate_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(AllocateThread);

  block->ExThreadObjectType.free_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(FreeThread);

  // several object types just call freepool/allocatepool
  uint32_t trampoline_allocatepool =
      kernel_trampoline_group_.NewLongtermTrampoline(
          SimpleForwardAllocatePoolTypeWithTag);
  uint32_t trampoline_freepool =
      kernel_trampoline_group_.NewLongtermTrampoline(SimpleForwardFreePool);

  // init event object
  block->ExEventObjectType.pool_tag = 0x76657645;
  block->ExEventObjectType.allocate_proc = trampoline_allocatepool;
  block->ExEventObjectType.free_proc = trampoline_freepool;

  // init mutant object
  block->ExMutantObjectType.pool_tag = 0x6174754D;
  block->ExMutantObjectType.allocate_proc = trampoline_allocatepool;
  block->ExMutantObjectType.free_proc = trampoline_freepool;

  block->ExMutantObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteMutant);
  // init semaphore obj
  block->ExSemaphoreObjectType.pool_tag = 0x616D6553;
  block->ExSemaphoreObjectType.allocate_proc = trampoline_allocatepool;
  block->ExSemaphoreObjectType.free_proc = trampoline_freepool;
  // init timer obj
  block->ExTimerObjectType.pool_tag = 0x656D6954;
  block->ExTimerObjectType.allocate_proc = trampoline_allocatepool;
  block->ExTimerObjectType.free_proc = trampoline_freepool;
  block->ExTimerObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteTimer);
  // iocompletion object
  block->IoCompletionObjectType.pool_tag = 0x706D6F43;
  block->IoCompletionObjectType.allocate_proc = trampoline_allocatepool;
  block->IoCompletionObjectType.free_proc = trampoline_freepool;
  block->IoCompletionObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteIoCompletion);
  block->IoCompletionObjectType.unknown_size_or_object_ = oddobject_offset;

  // iodevice object
  block->IoDeviceObjectType.pool_tag = 0x69766544;
  block->IoDeviceObjectType.allocate_proc = trampoline_allocatepool;
  block->IoDeviceObjectType.free_proc = trampoline_freepool;
  block->IoDeviceObjectType.unknown_size_or_object_ = oddobject_offset;
  block->IoDeviceObjectType.unknown_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(UnknownProcIoDevice);

  // file object
  block->IoFileObjectType.pool_tag = 0x656C6946;
  block->IoFileObjectType.allocate_proc = trampoline_allocatepool;
  block->IoFileObjectType.free_proc = trampoline_freepool;
  block->IoFileObjectType.unknown_size_or_object_ =
      0x38;  // sizeof fileobject, i believe
  block->IoFileObjectType.close_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(CloseFileProc);
  block->IoFileObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteFileProc);
  block->IoFileObjectType.unknown_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(UnknownFileProc);

  // directory object
  block->ObDirectoryObjectType.pool_tag = 0x65726944;
  block->ObDirectoryObjectType.allocate_proc = trampoline_allocatepool;
  block->ObDirectoryObjectType.free_proc = trampoline_freepool;
  block->ObDirectoryObjectType.unknown_size_or_object_ = oddobject_offset;

  // symlink object
  block->ObSymbolicLinkObjectType.pool_tag = 0x626D7953;
  block->ObSymbolicLinkObjectType.allocate_proc = trampoline_allocatepool;
  block->ObSymbolicLinkObjectType.free_proc = trampoline_freepool;
  block->ObSymbolicLinkObjectType.unknown_size_or_object_ = oddobject_offset;
  block->ObSymbolicLinkObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteSymlink);

#define offsetof32(s, m) static_cast<uint32_t>(offsetof(s, m))

  host_object_type_enum_to_guest_object_type_ptr_ = {
      {XObject::Type::Event,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExEventObjectType)},
      {XObject::Type::Semaphore,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExSemaphoreObjectType)},
      {XObject::Type::Thread,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExThreadObjectType)},
      {XObject::Type::File,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, IoFileObjectType)},
      {XObject::Type::Mutant,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExMutantObjectType)},
      {XObject::Type::Device,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, IoDeviceObjectType)}};
  xboxkrnl::xeKeSetEvent(&block->UsbdBootEnumerationDoneEvent, 1, 0);
  // Phase 1095bl: do NOT signal UsbdDriverLoadRequiredEvent here. Tried it
  // (the sibling above IS signalled) and measured no benefit: the list head
  // at 81D3CA08 stayed 0 and refusals were unchanged (64 vs 54). Signalling
  // it would also assert that a driver load IS required, which is not true
  // of this machine. Leaving it unsignalled is what lets the pool BLOCK on
  // it correctly instead of spinning.
}

void KernelState::InitializeXbdmCpuCounters() {
  constexpr uint32_t counters_base_address = 0x91F00000;

  // These are not confirmed and there seems to be multiple types of counters,
  // but no idea how they're switched. For now this seems to be good enough.
  constexpr std::array<const char*, 0x11> xbdm_counters = {
      "load-hit-stores (S)",
      "instructions committed",
      "i-cache miss cycles",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "Bad counter number - must be 0-15."};

  auto xbdm_range = memory_->LookupHeap(counters_base_address);
  if (!xbdm_range->AllocFixed(
          counters_base_address, 0x1000, 0,
          kMemoryAllocationCommit | kMemoryAllocationReserve,
          kMemoryProtectRead | kMemoryProtectWrite)) {
    return;
  }

  uint32_t address = counters_base_address;

  for (size_t i = 0; i < xbdm_counters.size(); i++) {
    xbdm_counters_address[i] = address;
    const std::string entry = xbdm_counters[i];
    std::memcpy(memory_->TranslateVirtual<char*>(address), entry.c_str(),
                entry.size());
    address += static_cast<uint32_t>(entry.size()) + 1;
  }
}

}  // namespace kernel
}  // namespace xe
