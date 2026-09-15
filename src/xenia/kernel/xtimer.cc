/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xtimer.h"

#include <condition_variable>
#include <deque>

#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

// 1099z17559-4: the kernel's DPC queue for timer DPCs (kernel_guest_timers).
// On the console an expired timer's KDPC is queued and its DeferredRoutine runs
// at DISPATCH_LEVEL with (Dpc, DeferredContext, SystemArgument1,
// SystemArgument2), outside any particular guest thread. HOST-SIDE: Xenia does not model
// IRQL; a dedicated system-process thread that drains the queue in order gives
// the routine all four arguments and no borrowed thread context.
namespace {
struct KernelDpc {
  uint32_t routine, dpc, context, arg1, arg2;
};
std::mutex g_dpc_lock;
std::condition_variable g_dpc_cv;
std::deque<KernelDpc> g_dpc_queue;
bool g_dpc_thread_started = false;
}  // namespace

void QueueKernelDpc(uint32_t routine, uint32_t dpc, uint32_t context,
                    uint32_t arg1, uint32_t arg2) {
  bool start = false;
  {
    std::lock_guard<std::mutex> lock(g_dpc_lock);
    g_dpc_queue.push_back({routine, dpc, context, arg1, arg2});
    if (!g_dpc_thread_started) {
      g_dpc_thread_started = true;
      start = true;
    }
  }
  g_dpc_cv.notify_one();
  if (!start) return;
  auto* ks = kernel_state();
  auto t = object_ref<XHostThread>(new XHostThread(
      ks, 256 * 1024, 0,
      [ks]() -> int {
        auto* ts = XThread::GetCurrentThread()->thread_state();
        XELOGI("KernelDpc: DPC thread running");
        while (true) {
          KernelDpc d;
          {
            std::unique_lock<std::mutex> lock(g_dpc_lock);
            g_dpc_cv.wait(lock, [] { return !g_dpc_queue.empty(); });
            d = g_dpc_queue.front();
            g_dpc_queue.pop_front();
          }
          uint64_t args[] = {d.dpc, d.context, d.arg1, d.arg2};
          ks->processor()->Execute(ts, d.routine, args, xe::countof(args));
        }
        return 0;
      },
      ks->GetSystemProcess()));
  t->set_name("Kernel DPC");
  t->Create();
}

XTimer::XTimer(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() = default;

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(timer_);
  switch (timer_type) {
    case 0:  // NotificationTimer
      timer_ = xe::threading::Timer::CreateManualResetTimer();
      break;
    case 1:  // SynchronizationTimer
      timer_ = xe::threading::Timer::CreateSynchronizationTimer();
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer_);
}

void XTimer::InitializeNative(void* native_ptr,
                             const X_DISPATCH_HEADER* header) {
  assert_false(timer_);
  switch (header->type) {
    case X_OBJECT_TYPES::TimerNotificationObject:
      timer_ = xe::threading::Timer::CreateManualResetTimer();
      break;
    case X_OBJECT_TYPES::TimerSynchronizationObject:
      timer_ = xe::threading::Timer::CreateSynchronizationTimer();
      break;
    default:
      assert_always();
      return;
  }
  assert_not_null(timer_);
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms,
                          uint32_t routine, uint32_t routine_arg, bool resume,
                          uint32_t dpc_ptr) {
  using xe::chrono::WinSystemClock;
  using xe::chrono::XSystemClock;

  std::lock_guard<std::mutex> lock(timer_lock_);

  period_ms = Clock::ScaleGuestDurationMillis(period_ms);
  WinSystemClock::time_point due_tp;
  if (due_time < 0) {
    // Any timer implementation uses absolute times eventually, convert as early
    // as possible for increased accuracy
    auto after = xe::chrono::hundrednanoseconds(-due_time);
    due_tp = date::clock_cast<WinSystemClock>(XSystemClock::now() + after);
  } else {
    due_tp = date::clock_cast<WinSystemClock>(
        XSystemClock::from_file_time(due_time));
  }

  // Stash routine for callback.
  callback_thread_ = XThread::GetCurrentThread();
  callback_routine_ = routine;
  callback_routine_arg_ = routine_arg;

  // This callback will only be issued when the timer is fired.
  // Capture values by value to avoid racing with a future SetTimer() call.
  std::function<void()> callback = nullptr;
  if (callback_routine_) {
    auto cb_thread = callback_thread_;
    auto cb_routine = callback_routine_;
    auto cb_routine_arg = callback_routine_arg_;
    callback = [cb_thread, cb_routine, cb_routine_arg, dpc_ptr]() {
      // Queue APC to call back routine with (arg, low, high).
      // It'll be executed on the thread that requested the timer.
      uint64_t time = xe::Clock::QueryGuestSystemTime();
      uint32_t time_low = static_cast<uint32_t>(time);
      uint32_t time_high = static_cast<uint32_t>(time >> 32);
      if (dpc_ptr && cvars::kernel_guest_timers) {
        // 1099z17559-4: a DPC, not an APC - see QueueKernelDpc.
        QueueKernelDpc(cb_routine, dpc_ptr, cb_routine_arg, time_low,
                       time_high);
        return;
      }
      if (dpc_ptr) {
        // Phase 1099q: a KeSetTimer(Ex) DPC. DeferredRoutine takes
        // (Dpc, DeferredContext, SystemArgument1, SystemArgument2). Calling it
        // APC-style (context, low, high) handed xam's routine the low half of
        // the time as its object: 8186E588 does `mr r3, r4` and 8186F898
        // stores through it - GUEST CRASH, fault_addr = r31+4 = 3BD528C5, and
        // the dashboard froze on "sign in or out" after picking a profile.
        // APC delivery passes three arguments, so SystemArgument2 (time high)
        // is not delivered.
        XELOGI("XTimer enqueuing DPC {:08X}(dpc {:08X}, ctx {:08X}, {:08X})",
               cb_routine, dpc_ptr, cb_routine_arg, time_low);
        cb_thread->EnqueueApc(cb_routine, dpc_ptr, cb_routine_arg, time_low);
        return;
      }
      XELOGI(
          "XTimer enqueuing timer callback to {:08X}({:08X}, {:08X}, {:08X})",
          cb_routine, cb_routine_arg, time_low, time_high);
      cb_thread->EnqueueApc(cb_routine, cb_routine_arg, time_low, time_high);
    };
  }

  bool result;
  if (!period_ms) {
    result = timer_->SetOnceAt(due_tp, std::move(callback));
  } else {
    result = timer_->SetRepeatingAt(
        due_tp, std::chrono::milliseconds(period_ms), std::move(callback));
  }

  if (resume) {
    XThread::SetLastError(X_ERROR_NOT_SUPPORTED);
    return X_STATUS_TIMER_RESUME_IGNORED;
  }

  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS XTimer::Cancel() {
  std::lock_guard<std::mutex> lock(timer_lock_);
  return timer_->Cancel() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

}  // namespace kernel
}  // namespace xe
