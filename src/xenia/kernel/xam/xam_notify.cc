/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

#include <atomic>

namespace xe {
namespace kernel {
namespace xam {

uint32_t xeXamNotifyCreateListener(uint64_t mask, uint32_t is_system,
                                   uint32_t max_version) {
  assert_true(max_version < 11);

  if (max_version > 10) {
    max_version = 10;
  }

  auto listener =
      object_ref<XNotifyListener>(new XNotifyListener(kernel_state()));
  listener->Initialize(mask, is_system, max_version);

  // Handle ref is incremented, so return that.
  uint32_t handle = listener->handle();

  // Phase 1095ai: with lle_xam_scope blank the dash binds to real xam,
  // yet type-7 (NotifyListener) objects with Xenia handles still reach
  // real xam's ObReferenceObjectByHandle and get the 0xDEADF00D
  // sentinel (crash 8176738C). This is the ONLY constructor, reached by
  // both XamNotifyCreateListener (0x28A) and
  // XamNotifyCreateListenerInternal (0x291) - the earlier probe only
  // covered the first. Log every creation with its guest caller.
  {
    auto* th = XThread::GetCurrentThread();
    auto* ctx = (th && th->thread_state()) ? th->thread_state()->context()
                                           : nullptr;
    XELOGI("GuideNotifyNew: handle {:08X} mask {:016X} is_system {} "
           "max_version {} | guest lr {:08X} tid {:08X}",
           handle, mask, is_system, max_version,
           ctx ? static_cast<uint32_t>(ctx->lr) : 0u,
           th ? th->thread_id() : 0u);
  }

  return handle;
}

dword_result_t XamNotifyCreateListener_entry(qword_t mask,
                                             dword_t max_version) {
  auto thread = kernel::XThread::GetCurrentThread();
  auto ctx = thread->thread_state()->context();
  auto type = xboxkrnl::xeKeGetCurrentProcessType(ctx);
  uint32_t handle = xeXamNotifyCreateListener(mask, type == 2, max_version);
  // hud returns E_FAIL from XuiSceneCreate when this comes back zero, so
  // record whether the HLE shim is even the thing being called - with LLE xam
  // the import may be redirected to the guest implementation instead.
  static std::atomic<uint32_t> calls{0};
  uint32_t n = ++calls;
  if (n <= 12) {
    XELOGI(
        "XamNotifyCreateListener[HLE] #{}: mask={:016X} max_version={} "
        "type={} -> handle {:08X} (caller lr={:08X})",
        n, uint64_t(mask), uint32_t(max_version), type, handle,
        uint32_t(ctx->lr));
  }
  return handle;
}
DECLARE_XAM_EXPORT1(XamNotifyCreateListener, kNone, kImplemented);

dword_result_t XamNotifyCreateListenerInternal_entry(qword_t mask,
                                                     dword_t is_system,
                                                     dword_t max_version) {
  return xeXamNotifyCreateListener(mask, is_system, max_version);
}
DECLARE_XAM_EXPORT1(XamNotifyCreateListenerInternal, kNone, kImplemented);

// https://github.com/CodeAsm/ffplay360/blob/master/Common/AtgSignIn.cpp
dword_result_t XNotifyGetNext_entry(dword_t handle, dword_t match_id,
                                    lpdword_t id_ptr, lpdword_t param_ptr) {
  if (param_ptr) {
    *param_ptr = 0;
  }

  if (!id_ptr) {
    return 0;
  }
  *id_ptr = 0;

  // Grab listener.
  auto listener =
      kernel_state()->object_table()->LookupObject<XNotifyListener>(handle);
  if (!listener) {
    return 0;
  }

  bool dequeued = false;
  uint32_t id = 0;
  uint32_t param = 0;
  if (match_id) {
    // Asking for a specific notification
    id = match_id;
    dequeued = listener->DequeueNotification(match_id, &param);
  } else {
    // Just get next.
    dequeued = listener->DequeueNotification(&id, &param);
  }

  *id_ptr = dequeued ? id : 0;
  // param_ptr may be null - 555307F0 Demo explicitly passes nullptr in the
  // code.
  // https://github.com/xenia-project/xenia/pull/1577
  if (param_ptr) {
    *param_ptr = dequeued ? param : 0;
  }
  return dequeued ? 1 : 0;
}
DECLARE_XAM_EXPORT2(XNotifyGetNext, kNone, kImplemented, kHighFrequency);

dword_result_t XNotifyDelayUI_entry(dword_t delay_ms) {
  // Ignored.
  return 0;
}
DECLARE_XAM_EXPORT1(XNotifyDelayUI, kNone, kStub);

void XNotifyPositionUI_entry(dword_t position) {
  kernel_state()->notification_position_ = position;
  // Ignored.
}
DECLARE_XAM_EXPORT1(XNotifyPositionUI, kNone, kStub);

dword_result_t XNotifyBroadcast_entry(dword_t notification, dword_t data) {
  kernel_state()->BroadcastNotification(notification, data);

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XNotifyBroadcast, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Notify);
