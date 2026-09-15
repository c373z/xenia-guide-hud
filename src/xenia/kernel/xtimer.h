/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XTIMER_H_
#define XENIA_KERNEL_XTIMER_H_

#include <mutex>

#include "xenia/base/threading.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

class XThread;

// Queues a DPC for the kernel DPC thread (kernel_guest_timers).
void QueueKernelDpc(uint32_t routine, uint32_t dpc, uint32_t context,
                    uint32_t arg1, uint32_t arg2);

class XTimer : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Timer;

  explicit XTimer(KernelState* kernel_state);
  ~XTimer() override;

  void Initialize(uint32_t timer_type);

  // Wraps a guest-created KTIMER. KeInitializeTimerEx writes the guest
  // structure and creates no host object, so a timer the guest made cannot be
  // waited on until it is adopted here - mirrors XEvent::InitializeNative.
  void InitializeNative(void* native_ptr, const X_DISPATCH_HEADER* header);

  // dpc_ptr != 0: `routine` is a KDPC DeferredRoutine and is called as
  // routine(Dpc, DeferredContext = routine_arg, SystemArgument1 = time low).
  // dpc_ptr == 0: `routine` is a timer APC, called as (arg, low, high).
  X_STATUS SetTimer(int64_t due_time, uint32_t period_ms, uint32_t routine,
                    uint32_t routine_arg, bool resume, uint32_t dpc_ptr = 0);
  X_STATUS Cancel();

 protected:
  xe::threading::WaitHandle* GetWaitHandle() override { return timer_.get(); }

 private:
  std::unique_ptr<xe::threading::Timer> timer_;
  std::mutex timer_lock_;

  XThread* callback_thread_ = nullptr;
  uint32_t callback_routine_ = 0;
  uint32_t callback_routine_arg_ = 0;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XTIMER_H_
