/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Boot animation exports, mirroring the 17489 kernel (80081BA8..8008208C).
//
// bootanim.xex is a DLL with no entry point. AniStartBootAnimation loads it
// and runs its ordinal 1 Run(hModule, HwFlags) on a title-process thread;
// AniBlockOnAnimation / AniTerminateAnimation call ordinal 2 Stop(terminate),
// wait for that thread to exit and unload the module; AniSetLogo forwards to
// ordinal 3 SetLogo(bits, arg) on a title-process thread. xam calls Block from
// its UI thread (81794BC8) and its title start (8175DDC8), so neither draws
// until the animation has released the GPU.

#include "xenia/kernel/power_reset.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ani.h"

#include <atomic>
#include <mutex>

#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

namespace {

std::mutex ani_lock;                // critical section 801D0090
uint32_t ani_handle = 0;            // [801DD844]
object_ref<XThread> ani_thread;     // [801DD840]
std::atomic<bool> ani_started{false};  // [801DD848]
std::atomic<int32_t> ani_refs{0};   // [801DD84C]
object_ref<XEvent> ani_done;        // event 801D0080

object_ref<XModule> AniModule(uint32_t hmodule) {
  return XModule::GetFromHModule(kernel_state(),
                                 kernel_memory()->TranslateVirtual(hmodule));
}

XEvent* DoneEvent() {
  if (!ani_done) {
    ani_done = object_ref<XEvent>(new XEvent(kernel_state()));
    ani_done->Initialize(true, false);
  }
  return ani_done.get();
}

// XexUnloadImage's own steps (xboxkrnl_modules.cc), for a handle this file
// loaded.
void UnloadBootAnimation(uint32_t hmodule) {
  auto module = AniModule(hmodule);
  if (!module) return;
  auto ldr = kernel_memory()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(hmodule);
  if (--ldr->load_count == 0) {
    module->Release();
    kernel_state()->UnloadUserModule(
        object_ref<UserModule>(reinterpret_cast<UserModule*>(module.release())));
  }
}

// 80081BA8. The XConfig devkit-flags test (setting 3/0xD bit 0x4) is not
// modelled: Xenia has no devkit flags, which is that test's pass case.
bool AniGate(bool from_background) {
  const uint32_t hw = cvars::xbox_hardware_info_flags;
  if (hw & 0x8) return false;
  if (!(hw & 0x200)) return false;
  // BldrFlags (u16 at XboxHardwareInfo+0xE) is zero in Xenia: bits 0x1 and
  // 0x200 are clear, so neither boot-loader test fails.
  (void)from_background;
  return !ani_started.load();
}

}  // namespace

bool AniIsStarted() { return ani_started.load(); }

uint32_t AniStartBootAnimationHost(bool from_background) {
  if (!AniGate(from_background)) {
    XELOGI("AniStartBootAnimation({}): gate refused (hw flags {:08X}, started "
           "{})",
           from_background, cvars::xbox_hardware_info_flags,
           ani_started.load());
    return 0xC0000001;
  }
  auto* ks = kernel_state();
  const std::string path = cvars::guide_cold_boot_path;

  // XexLoadImage(path, 0x40000009, 0x20445100, &h): xeXexLoadImage's steps.
  uint32_t hmodule = 0;
  if (auto existing = ks->GetModule(path)) {
    hmodule = existing->hmodule_ptr();
  } else {
    auto module = ks->LoadUserModule(path, false);
    if (!module) {
      XELOGE("AniStartBootAnimation: failed to load {}", path);
      return X_STATUS_NO_SUCH_FILE;
    }
    ks->FinishLoadingUserModule(module, false);
    hmodule = module.release()->hmodule_ptr();
  }
  kernel_memory()
      ->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(hmodule)
      ->load_count++;

  auto module = AniModule(hmodule);
  const uint32_t run = module ? module->GetProcAddressByOrdinal(1) : 0;
  if (!run) {
    XELOGE("AniStartBootAnimation: {} has no ordinal 1", path);
    UnloadBootAnimation(hmodule);
    return X_STATUS_UNSUCCESSFUL;
  }

  // ExCreateThread(&thread, 0x8000, NULL, NULL, 80081C50, HwFlags,
  // 0x020000A0): no 0x2, so the thread belongs to the title process, which
  // makes bootanim's D3D a title device (VdGlobalDevice, title terminate
  // notifications). 80081C50 fetches ordinal 1 and calls it with
  // (hModule, HwFlags).
  const uint32_t hw = cvars::xbox_hardware_info_flags;
  {
    std::lock_guard<std::mutex> lock(ani_lock);
    ani_handle = hmodule;
    ani_thread = object_ref<XThread>(new XHostThread(
        ks, 0x8000, 0,
        [ks, run, hmodule, hw]() -> int {
          auto* self = XThread::GetCurrentThread();
          self->set_can_debugger_suspend(true);
          XELOGI("bootanim: Run({:08X}, {:08X}) at {:08X}", hmodule, hw, run);
          uint64_t args[] = {hmodule, hw};
          ks->processor()->Execute(self->thread_state(), run, args, 2);
          XELOGI("bootanim: Run returned");
          return 0;
        },
        ks->GetTitleProcess()));
    ani_thread->set_name("Boot animation");
    if (XFAILED(ani_thread->Create())) {
      ani_thread.reset();
      ani_handle = 0;
      UnloadBootAnimation(hmodule);
      return X_STATUS_UNSUCCESSFUL;
    }
  }
  DoneEvent()->Reset();
  ani_started = true;
  XELOGI("AniStartBootAnimation({}): started {} (handle {:08X})",
         from_background, path, hmodule);
  return X_STATUS_SUCCESS;
}

dword_result_t AniStartBootAnimation_entry(dword_t from_background) {
  return AniStartBootAnimationHost(from_background != 0);
}
DECLARE_XBOXKRNL_EXPORT1(AniStartBootAnimation, kNone, kImplemented);

// 80081CB0: the teardown shared by Block (terminate=0, bootanim's minimum
// play time applies) and Terminate (terminate=1, stop at the next frame).
// A second caller that finds the handle cleared with [started] still set waits
// on the done event (seen in the kernel). Clearing the handle inside the
// critical section, before the wait, is inferred - it is what keeps xam's two
// concurrent callers from unloading twice.
static void AniWorker(bool terminate) {
  if (!ani_started.load() && !ani_handle) return;
  ani_refs++;
  uint32_t hmodule = 0;
  object_ref<XThread> thread;
  {
    std::lock_guard<std::mutex> lock(ani_lock);
    hmodule = ani_handle;
    if (hmodule) {
      auto module = AniModule(hmodule);
      const uint32_t stop = module ? module->GetProcAddressByOrdinal(2) : 0;
      if (stop) {
        uint64_t args[] = {terminate ? 1u : 0u};
        kernel_state()->processor()->Execute(
            XThread::GetCurrentThread()->thread_state(), stop, args, 1);
      }
      thread = ani_thread;
      ani_handle = 0;
      ani_thread.reset();
    }
  }
  if (hmodule) {
    XELOGI("AniWorker({}): waiting for the boot animation thread", terminate);
    if (thread) thread->Wait(3, 0, 0, nullptr);
    UnloadBootAnimation(hmodule);
    // 80102A70 persists a black 1280x720 surface when nothing is persisted.
    // Not modelled: the dash's own first frame replaces it.
    XELOGI("AniWorker({}): boot animation finished and unloaded", terminate);
    DoneEvent()->Set(0, false);
  } else if (ani_started.load()) {
    DoneEvent()->Wait(3, 0, 0, nullptr);
  }
  if (--ani_refs == 0) ani_started = false;
}

void AniBlockOnAnimation_entry() { AniWorker(false); }
DECLARE_XBOXKRNL_EXPORT1(AniBlockOnAnimation, kNone, kImplemented);

void AniTerminateAnimation_entry() { AniWorker(true); }
DECLARE_XBOXKRNL_EXPORT1(AniTerminateAnimation, kNone, kImplemented);

// 80081FA8: under the critical section, if bootanim is loaded, call ordinal 3
// directly from a title-process caller, otherwise on a title-process thread
// (trampoline 80081F80) and wait for it.
void AniSetLogo_entry(lpvoid_t bits, dword_t arg, const ppc_context_t& ctx) {
  std::lock_guard<std::mutex> lock(ani_lock);
  if (!ani_handle) return;
  auto* ks = kernel_state();
  auto module = AniModule(ani_handle);
  const uint32_t set_logo = module ? module->GetProcAddressByOrdinal(3) : 0;
  if (!set_logo) return;
  const uint32_t b = bits.guest_address();
  const uint32_t a = arg;
  if (xeKeGetCurrentProcessType(ctx) == X_PROCTYPE_TITLE) {
    uint64_t args[] = {b, a};
    ks->processor()->Execute(XThread::GetCurrentThread()->thread_state(),
                             set_logo, args, 2);
    return;
  }
  auto t = object_ref<XThread>(new XHostThread(
      ks, 0x8000, 0,
      [ks, set_logo, b, a]() -> int {
        auto* self = XThread::GetCurrentThread();
        self->set_can_debugger_suspend(true);
        uint64_t args[] = {b, a};
        ks->processor()->Execute(self->thread_state(), set_logo, args, 2);
        return 0;
      },
      ks->GetTitleProcess()));
  t->set_name("Boot animation logo");
  if (XSUCCEEDED(t->Create())) t->Wait(3, 0, 0, nullptr);
}
DECLARE_XBOXKRNL_EXPORT1(AniSetLogo, kNone, kImplemented);

void ResetAniStateForPowerOff() {
  std::lock_guard<std::mutex> lock(ani_lock);
  ani_handle = 0;
  ani_thread.reset();
  ani_started = false;
  ani_refs = 0;
  ani_done.reset();
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Ani);
