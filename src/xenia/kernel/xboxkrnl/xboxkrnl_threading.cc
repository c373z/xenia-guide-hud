/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <set>

#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/base/atomic.h"
#include "xenia/base/clock.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xsemaphore.h"
#include "xenia/kernel/xtimer.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// r13 + 0x100: pointer to thread local state
// Thread local state:
//   0x058: kernel time
//   0x14C: thread id
//   0x150: if >0 then error states don't get set
//   0x160: last error

// GetCurrentThreadId:
// lwz       r11, 0x100(r13)
// lwz       r3, 0x14C(r11)

// RtlGetLastError:
// lwz r11, 0x150(r13)
// if (r11 == 0) {
//   lwz r11, 0x100(r13)
//   stw r3, 0x160(r11)
// }

// RtlSetLastError:
// lwz r11, 0x150(r13)
// if (r11 == 0) {
//   lwz r11, 0x100(r13)
//   stw r3, 0x160(r11)
// }

// RtlSetLastNTError:
// r3 = RtlNtStatusToDosError(r3)
// lwz r11, 0x150(r13)
// if (r11 == 0) {
//   lwz r11, 0x100(r13)
//   stw r3, 0x160(r11)
// }

template <typename T>
object_ref<T> LookupNamedObject(KernelState* kernel_state,
                                uint32_t obj_attributes_ptr) {
  // If the name exists and its type matches, we can return that (ref+1)
  // with a success of NAME_EXISTS.
  // If the name exists and its type doesn't match, we do NAME_COLLISION.
  // Otherwise, we add like normal.
  if (!obj_attributes_ptr) {
    return nullptr;
  }
  auto obj_attributes =
      kernel_state->memory()->TranslateVirtual<X_OBJECT_ATTRIBUTES*>(
          obj_attributes_ptr);
  assert_true(obj_attributes->name_ptr != 0);
  auto name = util::TranslateAnsiStringAddress(kernel_state->memory(),
                                               obj_attributes->name_ptr);
  if (!name.empty()) {
    X_HANDLE handle = X_INVALID_HANDLE_VALUE;
    X_RESULT result =
        kernel_state->object_table()->GetObjectByName(name, &handle);
    if (XSUCCEEDED(result)) {
      // Found something! It's been retained, so return.
      auto obj = kernel_state->object_table()->LookupObject<T>(handle);
      if (obj) {
        // The caller will do as it likes.
        obj->ReleaseHandle();
        return obj;
      }
    }
  }
  return nullptr;
}

enum CreateThreadFlags : uint32_t {
  ThreadInitiallySuspended = 0x00000001,
  SystemThread = 0x00000002,
  PriorityClass1 = 0x00000020,
  PriorityClass2 = 0x00000040,
  ReturnKThreadPtr = 0x00000080,
  AffinityCpu0 = 0x01000000,
  AffinityCpu1 = 0x02000000,
  AffinityCpu2 = 0x04000000,
  AffinityCpu3 = 0x08000000,
  AffinityCpu4 = 0x10000000,
  AffinityCpu5 = 0x20000000,
};

inline const std::map<uint32_t, std::string> ex_thread_flag_map = {
    {ThreadInitiallySuspended, "Thread Initially Suspended"},
    {SystemThread, "Guest Created System Thread"},
    {PriorityClass1, "Thread Priority Class 1"},
    {PriorityClass2, "Thread Priority Class 2"},
    {ReturnKThreadPtr, "Return Kthread Ptr"},
    {AffinityCpu0, "Thread Starts At Cpu 1"},
    {AffinityCpu1, "Thread Starts At Cpu 2"},
    {AffinityCpu2, "Thread Starts At Cpu 3"},
    {AffinityCpu3, "Thread Starts At Cpu 4"},
    {AffinityCpu4, "Thread Starts At Cpu 5"},
    {AffinityCpu5, "Thread Starts At Cpu 6"}};

uint32_t ExCreateThread(xe::be<uint32_t>* handle_ptr, uint32_t stack_size,
                        xe::be<uint32_t>* thread_id_ptr,
                        uint32_t xapi_thread_startup, uint32_t start_address,
                        uint32_t start_context, uint32_t creation_flags) {
  // Invalid Link
  // http://jafile.com/uploads/scoop/main.cpp.txt
  // DWORD
  // LPHANDLE Handle,
  // DWORD    StackSize,
  // LPDWORD  ThreadId,
  // LPVOID   XapiThreadStartup, ?? often 0
  // LPVOID   StartAddress,
  // LPVOID   StartContext,
  // DWORD    CreationFlags // 0x80?

  std::string summary = "ExCreateThread Active:";
  uint32_t unused_flag = creation_flags;

  for (const auto& entry : ex_thread_flag_map) {
    if (creation_flags & entry.first) {
      summary += fmt::format(" {},", entry.second);
      unused_flag &= ~entry.first;
    }
  }
  if (unused_flag) {
    summary += fmt::format(" Unk flag: {:08X}", unused_flag);
  }
  XELOGD("{}", summary);

  uint32_t thread_process = (creation_flags & SystemThread)
                                ? kernel_state()->GetSystemProcess()
                                : kernel_state()->GetTitleProcess();
  X_KPROCESS* target_process =
      kernel_state()->memory()->TranslateVirtual<X_KPROCESS*>(thread_process);
  // Inherit default stack size
  uint32_t actual_stack_size = stack_size;

  if (actual_stack_size == 0) {
    actual_stack_size = target_process->kernel_stack_size;
  }

  // Stack must be aligned to 16kb pages
  actual_stack_size =
      std::max((uint32_t)0x4000, ((actual_stack_size + 0xFFF) & 0xFFFFF000));

  auto thread = object_ref<XThread>(new XThread(
      kernel_state(), actual_stack_size, xapi_thread_startup, start_address,
      start_context, creation_flags, true, false, thread_process));

  X_STATUS result = thread->Create();
  if (XFAILED(result)) {
    // Failed!
    XELOGE("Thread creation failed: {:08X}", result);
    return result;
  }

  if (XSUCCEEDED(result)) {
    if (handle_ptr) {
      if (creation_flags & ReturnKThreadPtr) {
        *handle_ptr = thread->guest_object();
      } else {
        *handle_ptr = thread->handle();
      }
    }
    if (thread_id_ptr) {
      *thread_id_ptr = thread->thread_id();
    }
  }
  return result;
}

dword_result_t ExCreateThread_entry(lpdword_t handle_ptr, dword_t stack_size,
                                    lpdword_t thread_id_ptr,
                                    dword_t xapi_thread_startup,
                                    lpvoid_t start_address,
                                    lpvoid_t start_context,
                                    dword_t creation_flags) {
  // Phase 1096dm: log every guest thread's ENTRY POINT. Phase 1096 spent many
  // segments asking "why does xam never observe the Guide button", and one
  // answer it could never test was "because the thread that would watch for it
  // is never started". Nothing logged thread entry points, so that could not be
  // checked. One line here makes the whole set visible, and it is a probe, not
  // a behaviour change.
  {
    static std::mutex tl_mu;
    static uint32_t tl_n = 0;
    uint32_t n = 0;
    {
      std::lock_guard<std::mutex> lk(tl_mu);
      n = ++tl_n;
    }
    if (n <= 64u) {
      XELOGI("GuideThreadStart #{}: entry {:08X} context {:08X} flags {:08X}",
             n, uint32_t(start_address), uint32_t(start_context),
             uint32_t(creation_flags));
    }
  }
  return ExCreateThread(handle_ptr, stack_size, thread_id_ptr,
                        xapi_thread_startup, start_address, start_context,
                        creation_flags);
}
DECLARE_XBOXKRNL_EXPORT1(ExCreateThread, kThreading, kImplemented);

uint32_t ExTerminateThread(uint32_t exit_code) {
  XThread* thread = XThread::GetCurrentThread();

  // NOTE: this kills us right now. We won't return from it.
  return thread->Exit(exit_code);
}

dword_result_t ExTerminateThread_entry(dword_t exit_code) {
  return ExTerminateThread(exit_code);
}
DECLARE_XBOXKRNL_EXPORT1(ExTerminateThread, kThreading, kImplemented);

uint32_t NtResumeThread(uint32_t handle, uint32_t* suspend_count_ptr) {
  X_RESULT result = X_STATUS_INVALID_HANDLE;
  uint32_t suspend_count = 0;

  auto thread = kernel_state()->object_table()->LookupObject<XThread>(handle);

  if (thread) {
    if (thread->type() == XObject::Type::Thread) {
      result = thread->Resume(&suspend_count);
    } else {
      return X_STATUS_OBJECT_TYPE_MISMATCH;
    }
  } else {
    return X_STATUS_INVALID_HANDLE;
  }
  if (suspend_count_ptr) {
    *suspend_count_ptr = suspend_count;
  }

  return result;
}

dword_result_t NtResumeThread_entry(dword_t handle,
                                    lpdword_t suspend_count_ptr) {
  uint32_t suspend_count =
      suspend_count_ptr ? static_cast<uint32_t>(*suspend_count_ptr) : 0u;

  const X_RESULT result =
      NtResumeThread(handle, suspend_count_ptr ? &suspend_count : nullptr);

  if (suspend_count_ptr) {
    *suspend_count_ptr = suspend_count;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtResumeThread, kThreading, kImplemented);

dword_result_t KeResumeThread_entry(pointer_t<X_KTHREAD> thread_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;
  auto thread = XObject::GetNativeObject<XThread>(kernel_state(), thread_ptr,
                                                  ThreadObject);
  if (thread) {
    result = thread->Resume();
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(KeResumeThread, kThreading, kImplemented);

dword_result_t NtSuspendThread_entry(dword_t handle,
                                     lpdword_t suspend_count_ptr,
                                     const ppc_context_t& context) {
  X_RESULT result = X_STATUS_SUCCESS;
  uint32_t suspend_count = 0;

  auto thread = kernel_state()->object_table()->LookupObject<XThread>(handle);
  if (thread) {
    if (thread->type() == XObject::Type::Thread) {
      auto current_pcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);

#if XE_PLATFORM_WIN32
      if (current_pcr->prcb_data.current_thread == thread->guest_object() ||
          !thread->guest_object<X_KTHREAD>()->terminated) {
        result = thread->Suspend(&suspend_count);
      } else {
        return X_STATUS_THREAD_IS_TERMINATING;
      }
#else
      // Handle self-suspension specially to avoid deadlock.
      if (!thread->guest_object<X_KTHREAD>()->terminated) {
        bool is_self_suspend =
            (current_pcr->prcb_data.current_thread == thread->guest_object());

        if (is_self_suspend) {
          XELOGD("Thread {:X} self-suspending", thread->handle());
          suspend_count = thread->SelfSuspend();
          result = X_STATUS_SUCCESS;
          XELOGD("Thread {:X} resumed", thread->handle());
        } else {
          result = thread->Suspend(&suspend_count);
        }
      } else {
        return X_STATUS_THREAD_IS_TERMINATING;
      }
#endif
    } else {
      return X_STATUS_OBJECT_TYPE_MISMATCH;
    }
  } else {
    return X_STATUS_INVALID_HANDLE;
  }

  if (suspend_count_ptr) {
    *suspend_count_ptr = suspend_count;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtSuspendThread, kThreading, kImplemented);

dword_result_t KeSuspendThread_entry(pointer_t<X_KTHREAD> kthread,
                                     const ppc_context_t& context) {
  auto thread = XObject::GetNativeObject<XThread>(context->kernel_state,
                                                  kthread, ThreadObject);
  uint32_t suspend_count_out = 0;

  if (thread) {
    suspend_count_out = thread->suspend_count();

    uint32_t discarded_new_suspend_count = 0;
    thread->Suspend(&discarded_new_suspend_count);
  }

  return suspend_count_out;
}
DECLARE_XBOXKRNL_EXPORT1(KeSuspendThread, kThreading, kImplemented);

void KeSetCurrentStackPointers_entry(lpvoid_t stack_ptr,
                                     pointer_t<X_KTHREAD> thread,
                                     lpvoid_t stack_alloc_base,
                                     lpvoid_t stack_base, lpvoid_t stack_limit,
                                     const ppc_context_t& context) {
  auto current_thread = XThread::GetCurrentThread();

  auto pcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  // also supposed to load msr mask, and the current msr with that, and store
  thread->stack_alloc_base = stack_alloc_base.value();
  thread->stack_base = stack_base.value();
  thread->stack_limit = stack_limit.value();
  pcr->stack_base_ptr = stack_base.guest_address();
  pcr->stack_end_ptr = stack_limit.guest_address();
  context->r[1] = stack_ptr.guest_address();

  // If a fiber is set, and the thread matches, reenter to avoid issues with
  // host stack overflowing.
  if (thread->fiber_ptr &&
      current_thread->guest_object() == thread.guest_address()) {
    context->processor->backend()->PrepareForReentry(context.value());
    current_thread->Reenter(static_cast<uint32_t>(context->lr));
  }
}
DECLARE_XBOXKRNL_EXPORT2(KeSetCurrentStackPointers, kThreading, kImplemented,
                         kHighFrequency);

dword_result_t KeSetAffinityThread_entry(pointer_t<X_KTHREAD> thread_ptr,
                                         dword_t affinity,
                                         lpdword_t previous_affinity_ptr) {
  // The Xbox 360, according to disassembly of KeSetAffinityThread, unlike
  // Windows NT, stores the previous affinity via the pointer provided as an
  // argument, not in the return value - the return value is used for the
  // result.
  if (!affinity) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto thread = XObject::GetNativeObject<XThread>(kernel_state(), thread_ptr,
                                                  ThreadObject);
  if (!thread) {
    XELOGW(
        "KeSetAffinityThread: guest thread pointer {:08X} did not resolve to "
        "an XThread; returning STATUS_INVALID_HANDLE",
        thread_ptr.guest_address());
    return X_STATUS_INVALID_HANDLE;
  }
  if (previous_affinity_ptr) {
    *previous_affinity_ptr = uint32_t(1) << thread->active_cpu();
  }
  thread->SetAffinity(affinity);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeSetAffinityThread, kThreading, kImplemented);

dword_result_t KeQueryBasePriorityThread_entry(
    pointer_t<X_KTHREAD> thread_ptr) {
  int32_t priority = 0;

  auto thread = XObject::GetNativeObject<XThread>(kernel_state(), thread_ptr,
                                                  ThreadObject);
  if (thread) {
    priority = thread->QueryPriority();
  }

  return priority;
}
DECLARE_XBOXKRNL_EXPORT1(KeQueryBasePriorityThread, kThreading, kImplemented);

dword_result_t KeSetBasePriorityThread_entry(pointer_t<X_KTHREAD> thread_ptr,
                                             dword_t increment) {
  int32_t prev_priority = 0;
  auto thread = XObject::GetNativeObject<XThread>(kernel_state(), thread_ptr,
                                                  ThreadObject);

  if (thread) {
    prev_priority = thread->QueryPriority();
    thread->SetPriority(increment);
  }

  return prev_priority;
}
DECLARE_XBOXKRNL_EXPORT1(KeSetBasePriorityThread, kThreading, kImplemented);

dword_result_t KeSetDisableBoostThread_entry(pointer_t<X_KTHREAD> thread_ptr,
                                             dword_t disabled) {
  // supposed to acquire dispatcher lock + a prcb lock, all just to exchange
  // this char there is no other special behavior going on in this function,
  // just acquiring locks to do this exchange
  auto old_boost_disabled =
      reinterpret_cast<std::atomic_uint8_t*>(&thread_ptr->boost_disabled)
          ->exchange(static_cast<uint8_t>(disabled));

  return old_boost_disabled;
}
DECLARE_XBOXKRNL_EXPORT1(KeSetDisableBoostThread, kThreading, kImplemented);

uint32_t xeKeGetCurrentProcessType(cpu::ppc::PPCContext* context) {
  auto pcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);

  if (!pcr->prcb_data.dpc_active) {
    return context->TranslateVirtual(pcr->prcb_data.current_thread)
        ->process_type;
  }
  return pcr->processtype_value_in_dpc;
}
void xeKeSetCurrentProcessType(uint32_t type, cpu::ppc::PPCContext* context) {
  auto pcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  if (pcr->prcb_data.dpc_active) {
    pcr->processtype_value_in_dpc = type;
  }
}

dword_result_t KeGetCurrentProcessType_entry(const ppc_context_t& context) {
  return xeKeGetCurrentProcessType(context);
}
DECLARE_XBOXKRNL_EXPORT2(KeGetCurrentProcessType, kThreading, kImplemented,
                         kHighFrequency);

void KeSetCurrentProcessType_entry(dword_t type, const ppc_context_t& context) {
  xeKeSetCurrentProcessType(type, context);
}
DECLARE_XBOXKRNL_EXPORT1(KeSetCurrentProcessType, kThreading, kImplemented);

dword_result_t KeQueryPerformanceFrequency_entry() {
  uint64_t result = Clock::guest_tick_frequency();
  return static_cast<uint32_t>(result);
}
DECLARE_XBOXKRNL_EXPORT2(KeQueryPerformanceFrequency, kThreading, kImplemented,
                         kHighFrequency);

uint32_t KeDelayExecutionThread(uint32_t processor_mode, uint32_t alertable,
                                uint64_t* interval_ptr,
                                cpu::ppc::PPCContext* ctx) {
  XThread* thread = XThread::GetCurrentThread();

  if (alertable) {
    X_STATUS stat = xeProcessUserApcs(ctx);
    if (stat == X_STATUS_USER_APC) {
      return stat;
    }
  }
  X_STATUS result = thread->Delay(processor_mode, alertable, *interval_ptr);

  if (result == X_STATUS_USER_APC) {
    xeProcessUserApcs(ctx);
  }

  return result;
}

dword_result_t KeDelayExecutionThread_entry(dword_t processor_mode,
                                            dword_t alertable,
                                            lpqword_t interval_ptr,
                                            const ppc_context_t& context) {
  uint64_t interval = interval_ptr ? static_cast<uint64_t>(*interval_ptr) : 0u;
  return KeDelayExecutionThread(processor_mode, alertable,
                                interval_ptr ? &interval : nullptr, context);
}
DECLARE_XBOXKRNL_EXPORT3(KeDelayExecutionThread, kThreading, kImplemented,
                         kBlocking, kHighFrequency);

dword_result_t NtYieldExecution_entry() {
  xe::threading::MaybeYield();
  return 0;
}
DECLARE_XBOXKRNL_EXPORT2(NtYieldExecution, kThreading, kImplemented,
                         kHighFrequency);

void KeQuerySystemTime_entry(lpqword_t time_ptr, const ppc_context_t& ctx) {
  if (time_ptr) {
    // update the timestamp bundle to the time we queried.
    // this is a race, but i don't of any sw that requires it, it just seems
    // like we ought to keep it consistent with ketimestampbundle in case
    // something uses this function, but also reads it directly
    uint32_t ts_bundle = ctx->kernel_state->GetKeTimestampBundle();
    uint64_t time = Clock::QueryGuestSystemTime();
    // todo: cmpxchg?
    xe::store_and_swap<uint64_t>(
        &ctx->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(ts_bundle)->system_time,
        time);
    *time_ptr = time;
  }
}
DECLARE_XBOXKRNL_EXPORT1(KeQuerySystemTime, kThreading, kImplemented);

// https://msdn.microsoft.com/en-us/library/ms686801
// Phase 1095: xam's per-thread block. Both 81778D38 (install) and 81779618 /
// 817795EC (set/get slot) assert KeTlsGetValue([81D227F0]) != 0, and the
// getter's assert does not stop Xenia (the guest's `twui` falls through), so a
// null block reads on as a null base and faults at 81779604. Report who
// allocates the index and every set/get a task-pool worker (start 8177AD80)
// makes on it, so the null is attributed to a thread rather than guessed at.
static void GuideTlsTrace(const char* what, uint32_t slot, uint32_t value,
                          bool ok) {
  if (!cvars::guide_bkgnd_watch) return;
  auto* th = XThread::GetCurrentThread();
  if (!th) return;
  if (th->start_address() != 0x8177AD80u) return;
  static std::atomic<uint32_t> n{0};
  const uint32_t i = ++n;
  if (i > 48u) return;
  XELOGI("GuideTls #{}: {} slot {} value {:08X} ok {} | tid {:08X} tls_size {}",
         i, what, slot, value, ok ? 1 : 0, th->thread_id(),
         th->tls_total_size());
}

dword_result_t KeTlsAlloc_entry(const ppc_context_t& context) {
  uint32_t slot = kernel_state()->AllocateTLS(context);
  XThread::GetCurrentThread()->SetTLSValue(slot, 0);
  if (cvars::guide_bkgnd_watch) {
    auto* th = XThread::GetCurrentThread();
    XELOGI(
        "GuideTls: KeTlsAlloc -> slot {} on tid {:08X} start {:08X} tls_size {}",
        slot, th ? th->thread_id() : 0u, th ? th->start_address() : 0u,
        th ? th->tls_total_size() : 0u);
  }
  return slot;
}
DECLARE_XBOXKRNL_EXPORT1(KeTlsAlloc, kThreading, kImplemented);

// https://msdn.microsoft.com/en-us/library/ms686804
dword_result_t KeTlsFree_entry(dword_t tls_index,
                               const ppc_context_t& context) {
  if (tls_index == X_TLS_OUT_OF_INDEXES) {
    return 0;
  }

  kernel_state()->FreeTLS(context, tls_index);
  return 1;
}
DECLARE_XBOXKRNL_EXPORT1(KeTlsFree, kThreading, kImplemented);

// https://msdn.microsoft.com/en-us/library/ms686812
// Phase 1095bf: CORRECTED. The earlier version of this watch hardcoded
// 401EA360, and the record MOVES between runs - it was 401EA380 the very next
// run. Two polls reported nothing and I nearly read that as "no writer exists";
// they were simply watching the wrong address. Resolve the record from the pool
// every time instead: pool+0x1D4 is the parallel record array and pool+0x234 the
// count, both established in 1095ap/at.
void GuidePoisonWatch() {
  if (!cvars::guide_bkgnd_watch) return;
  auto* m = kernel_memory();
  auto rd = [&](uint32_t a) {
    return xe::load_and_swap<uint32_t>(m->TranslateVirtual(a));
  };
  auto readable = [&](uint32_t a) {
    auto* h = m->LookupHeap(a);
    return h && h->QueryRangeAccess(a, a + 0xF) !=
                    xe::memory::PageAccess::kNoAccess;
  };
  // Phase 1095bh: do NOT gate the pool read on QueryRangeAccess. The pool
  // lives in xam's IMAGE (81D423C0), and QueryRangeAccess reports
  // kNoAccess for those pages - that is the guard-0 problem this project
  // has hit since 1056, and it silently disabled this watch on every
  // call. It also produced the useless 'hdr 00000000' column in
  // GuideWait8. The image is always mapped; only the heap records need a
  // guard.
  const uint32_t pool = 0x81D423C0u;
  const uint32_t cnt = rd(pool + 0x234u);
  if (!cnt || cnt > 24u) return;
  static std::atomic<uint32_t> lines{0};
  for (uint32_t k = 0; k < cnt && k < 8u; ++k) {
    const uint32_t rec = rd(pool + 0x1D4u + k * 4u);
    if (!rec || !readable(rec)) continue;
    const uint32_t v = rd(rec + 0xCu);
    static uint32_t last[8] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                               0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                               0xFFFFFFFFu, 0xFFFFFFFFu};
    if (last[k] == v) continue;
    const uint32_t prev = last[k];
    last[k] = v;
    if (lines.load() >= 12u) continue;
    ++lines;
    auto* th = XThread::GetCurrentThread();
    auto* ctx = (th && th->thread_state()) ? th->thread_state()->context()
                                           : nullptr;
    XELOGI("GuidePoisonWatch: rec[{}]={:08X} +0xC {:08X} -> {:08X} | flags "
           "{:08X} | tid {:08X} lr {:08X}",
           k, rec, prev, v, rd(rec + 8u), th ? th->thread_id() : 0u,
           ctx ? static_cast<uint32_t>(ctx->lr) : 0u);
  }
}

dword_result_t KeTlsGetValue_entry(dword_t tls_index) {
  // xboxkrnl doesn't actually have an error branch - it always succeeds, even
  // if it overflows the TLS.
  uint32_t value = 0;
  const bool ok = XThread::GetCurrentThread()->GetTLSValue(tls_index, &value);
  GuideTlsTrace("get", tls_index, ok ? value : 0u, ok);
  if (ok) {
    return value;
  }

  return 0;
}
DECLARE_XBOXKRNL_EXPORT2(KeTlsGetValue, kThreading, kImplemented,
                         kHighFrequency);

// https://msdn.microsoft.com/en-us/library/ms686818
dword_result_t KeTlsSetValue_entry(dword_t tls_index, dword_t tls_value) {
  // xboxkrnl doesn't actually have an error branch - it always succeeds, even
  // if it overflows the TLS.
  const bool ok =
      XThread::GetCurrentThread()->SetTLSValue(tls_index, tls_value);
  GuideTlsTrace("set", tls_index, tls_value, ok);
  if (ok) {
    return 1;
  }

  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeTlsSetValue, kThreading, kImplemented);

void KeInitializeEvent_entry(pointer_t<X_KEVENT> event_ptr, dword_t event_type,
                             dword_t initial_state) {
  event_ptr.Zero();
  event_ptr->header.type = static_cast<X_OBJECT_TYPES>(event_type.value());
  event_ptr->header.signal_state = initial_state.value();
  auto ev = XObject::GetNativeObject<XEvent>(kernel_state(), event_ptr,
                                             event_ptr->header.type);
  if (!ev) {
    // Phase 1096hn: assert_always() is a NO-OP in Release, so this path
    // returns leaving the header exactly as Zero() left it - all sixteen
    // bytes, including the wait_list fields where StashHandle would have put
    // its signature. A guest event that failed here is indistinguishable
    // afterwards from one that was never initialised at all, which is what
    // nine of dash's events looked like when a wait on them was inspected.
    // Say so instead of failing silently.
    // Phase 1096hn2: a pointer_t built from a HOST pointer - as the internal
    // callers here do, e.g. KeInitializeEvent_entry(&lock_ptr->writer_event,
    // 1, 0) - leaves value_ at 0, so guest_address() is 0 for those. The first
    // pass of this probe logged only those and said nothing about the guest's
    // own events. Tag them instead of conflating them.
    static std::atomic<uint32_t> init_fail_n{0};
    const uint32_t n = ++init_fail_n;
    if (n <= 24u) {
      XELOGE(
          "KeInitializeEvent: GetNativeObject FAILED for {:08X} ({}) type {} "
          "state {} - header left all-zero, every later wait on it will not "
          "resolve. Failure #{}",
          event_ptr.guest_address(),
          event_ptr.guest_address() ? "GUEST CALL" : "internal host call",
          uint32_t(event_type), uint32_t(initial_state), n);
    }
    assert_always();
    return;
  }
  static std::atomic<uint32_t> init_ok_n{0}, init_ok_guest_n{0};
  const uint32_t ok = ++init_ok_n;
  const bool from_guest = event_ptr.guest_address() != 0;
  const uint32_t okg = from_guest ? ++init_ok_guest_n : 0u;
  if (ok <= 3u || (from_guest && okg <= 12u)) {
    XELOGI("KeInitializeEvent: OK for {:08X} ({}) type {} - Success #{}",
           event_ptr.guest_address(),
           from_guest ? "GUEST CALL" : "internal host call",
           uint32_t(event_type), ok);
  }
}
DECLARE_XBOXKRNL_EXPORT1(KeInitializeEvent, kThreading, kImplemented);

uint32_t xeKeSetEvent(X_KEVENT* event_ptr, uint32_t increment, uint32_t wait) {
  auto ev = XObject::GetNativeObject<XEvent>(kernel_state(), event_ptr,
                                             event_ptr->header.type);
  if (!ev) {
    assert_always();
    return 0;
  }

  return ev->Set(increment, !!wait);
}

// Phase 1056: is anything signalling xam's task pool?
static void GuidePoolSyncLog(const char* what, uint32_t guest_ptr, uint32_t a, uint32_t b) {
  if (!cvars::guide_log_pool_sync) return;
  if (guest_ptr < 0x81D42400u || guest_ptr > 0x81D42540u) return;
  uint32_t lr = 0;
  auto* t = XThread::GetCurrentThread();
  if (t && t->thread_state() && t->thread_state()->context()) {
    lr = uint32_t(t->thread_state()->context()->lr);
  }
  XELOGI("GuidePoolSync: {} {:08X} ({:08X}, {:08X}) from lr {:08X} on thread {:08X}",
         what, guest_ptr, a, b, lr, t ? t->handle() : 0);
}

// Phase 1096do: see guide_event_census.
static void GuideEventCensus(const char* what, uint32_t guest_ptr) {
  if (!cvars::guide_event_census) return;
  static std::mutex ec_mu;
  static std::set<uint32_t> ec_seen;
  bool fresh = false;
  size_t n = 0;
  {
    std::lock_guard<std::mutex> lk(ec_mu);
    if (ec_seen.size() < 64u) {
      fresh = ec_seen.insert(guest_ptr).second;
      n = ec_seen.size();
    }
  }
  if (fresh) {
    uint32_t lr = 0;
    auto* t = XThread::GetCurrentThread();
    if (t && t->thread_state() && t->thread_state()->context()) {
      lr = uint32_t(t->thread_state()->context()->lr);
    }
    XELOGI("GuideEventCensus #{}: {} obj {:08X} from lr {:08X}", n, what,
           guest_ptr, lr);
  }
}

dword_result_t KeSetEvent_entry(pointer_t<X_KEVENT> event_ptr,
                                dword_t increment, dword_t wait) {
  GuideEventCensus("KeSetEvent", event_ptr.guest_address());
  GuidePoolSyncLog("KeSetEvent", event_ptr.guest_address(), increment, wait);
  return xeKeSetEvent(event_ptr, increment, wait);
}
DECLARE_XBOXKRNL_EXPORT2(KeSetEvent, kThreading, kImplemented, kHighFrequency);

dword_result_t KePulseEvent_entry(pointer_t<X_KEVENT> event_ptr,
                                  dword_t increment, dword_t wait) {
  GuideEventCensus("KePulseEvent", event_ptr.guest_address());
  auto ev = XObject::GetNativeObject<XEvent>(kernel_state(), event_ptr,
                                             event_ptr->header.type);
  if (!ev) {
    assert_always();
    return 0;
  }

  return ev->Pulse(increment, !!wait);
}
DECLARE_XBOXKRNL_EXPORT2(KePulseEvent, kThreading, kImplemented,
                         kHighFrequency);

dword_result_t KeResetEvent_entry(pointer_t<X_KEVENT> event_ptr) {
  auto ev = XObject::GetNativeObject<XEvent>(kernel_state(), event_ptr,
                                             event_ptr->header.type);
  if (!ev) {
    assert_always();
    return 0;
  }

  return ev->Reset();
}
DECLARE_XBOXKRNL_EXPORT1(KeResetEvent, kThreading, kImplemented);

dword_result_t NtCreateEvent_entry(
    lpdword_t handle_ptr, pointer_t<X_OBJECT_ATTRIBUTES> obj_attributes_ptr,
    dword_t event_type, dword_t initial_state) {
  // Check for an existing timer with the same name.
  auto existing_object =
      LookupNamedObject<XEvent>(kernel_state(), obj_attributes_ptr);
  if (existing_object) {
    if (existing_object->type() == XObject::Type::Event) {
      if (handle_ptr) {
        existing_object->RetainHandle();
        *handle_ptr = existing_object->handle();
      }
      return X_STATUS_OBJECT_NAME_EXISTS;
    } else {
      return X_STATUS_INVALID_HANDLE;
    }
  }

  auto ev = object_ref<XEvent>(new XEvent(kernel_state()));
  ev->Initialize(!event_type, !!initial_state);

  // obj_attributes may have a name inside of it, if != NULL.
  if (obj_attributes_ptr) {
    ev->SetAttributes(obj_attributes_ptr);
  }

  if (handle_ptr) {
    *handle_ptr = ev->handle();
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtCreateEvent, kThreading, kImplemented);

uint32_t xeNtSetEvent(uint32_t handle, xe::be<uint32_t>* previous_state_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = kernel_state()->object_table()->LookupObject<XEvent>(handle);
  if (ev) {
    // d3 ros does this
    if (ev->type() != XObject::Type::Event) {
      return X_STATUS_OBJECT_TYPE_MISMATCH;
    }
    int32_t was_signalled = ev->Set(0, false);
    if (previous_state_ptr) {
      *previous_state_ptr = static_cast<uint32_t>(was_signalled);
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

dword_result_t NtSetEvent_entry(dword_t handle, lpdword_t previous_state_ptr) {
  return xeNtSetEvent(handle, previous_state_ptr);
}
DECLARE_XBOXKRNL_EXPORT2(NtSetEvent, kThreading, kImplemented, kHighFrequency);

dword_result_t NtPulseEvent_entry(dword_t handle,
                                  lpdword_t previous_state_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = kernel_state()->object_table()->LookupObject<XEvent>(handle);
  if (ev) {
    int32_t was_signalled = ev->Pulse(0, false);
    if (previous_state_ptr) {
      *previous_state_ptr = static_cast<uint32_t>(was_signalled);
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT2(NtPulseEvent, kThreading, kImplemented,
                         kHighFrequency);
dword_result_t NtQueryEvent_entry(dword_t handle, lpdword_t out_struc) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = kernel_state()->object_table()->LookupObject<XEvent>(handle);
  if (ev) {
    uint32_t type_tmp, state_tmp;

    ev->Query(&type_tmp, &state_tmp);

    out_struc[0] = type_tmp;
    out_struc[1] = state_tmp;
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT2(NtQueryEvent, kThreading, kImplemented,
                         kHighFrequency);
uint32_t xeNtClearEvent(uint32_t handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = kernel_state()->object_table()->LookupObject<XEvent>(handle);
  if (ev) {
    ev->Reset();
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

dword_result_t NtClearEvent_entry(dword_t handle) {
  return xeNtClearEvent(handle);
}
DECLARE_XBOXKRNL_EXPORT2(NtClearEvent, kThreading, kImplemented,
                         kHighFrequency);

// https://msdn.microsoft.com/en-us/library/windows/hardware/ff552150(v=vs.85).aspx
void KeInitializeSemaphore_entry(pointer_t<X_KSEMAPHORE> semaphore_ptr,
                                 dword_t count, dword_t limit) {
  semaphore_ptr->header.type = SemaphoreObject;
  semaphore_ptr->header.signal_state = (uint32_t)count;
  semaphore_ptr->limit = (uint32_t)limit;

  auto sem = XObject::GetNativeObject<XSemaphore>(kernel_state(), semaphore_ptr,
                                                  SemaphoreObject);
  if (!sem) {
    assert_always();
    return;
  }
}
DECLARE_XBOXKRNL_EXPORT1(KeInitializeSemaphore, kThreading, kImplemented);

uint32_t xeKeReleaseSemaphore(X_KSEMAPHORE* semaphore_ptr, uint32_t increment,
                              uint32_t adjustment, uint32_t wait) {
  auto sem = XObject::GetNativeObject<XSemaphore>(kernel_state(), semaphore_ptr,
                                                  SemaphoreObject);
  if (!sem) {
    assert_always();
    return 0;
  }

  sem->set_priority_increment(increment);

  int32_t previous_count = 0;
  [[maybe_unused]] bool success =
      sem->ReleaseSemaphore(adjustment, &previous_count);
  return static_cast<uint32_t>(previous_count);
}

dword_result_t KeReleaseSemaphore_entry(pointer_t<X_KSEMAPHORE> semaphore_ptr,
                                        dword_t increment, dword_t adjustment,
                                        dword_t wait) {
  GuidePoolSyncLog("KeReleaseSemaphore", semaphore_ptr.guest_address(), increment, adjustment);
  return xeKeReleaseSemaphore(semaphore_ptr, increment, adjustment, wait);
}
DECLARE_XBOXKRNL_EXPORT1(KeReleaseSemaphore, kThreading, kImplemented);

dword_result_t NtCreateSemaphore_entry(
    lpdword_t handle_ptr, pointer_t<X_OBJECT_ATTRIBUTES> obj_attributes_ptr,
    dword_t count, dword_t limit) {
  // Check for an existing semaphore with the same name.
  auto existing_object =
      LookupNamedObject<XSemaphore>(kernel_state(), obj_attributes_ptr);
  if (existing_object) {
    if (existing_object->type() == XObject::Type::Semaphore) {
      if (handle_ptr) {
        existing_object->RetainHandle();
        *handle_ptr = existing_object->handle();
      }
      return X_STATUS_OBJECT_NAME_EXISTS;
    } else {
      return X_STATUS_INVALID_HANDLE;
    }
  }

  auto sem = object_ref<XSemaphore>(new XSemaphore(kernel_state()));
  if (!sem->Initialize((int32_t)count, (int32_t)limit)) {
    if (handle_ptr) {
      *handle_ptr = 0;
    }
    sem->ReleaseHandle();
    return X_STATUS_INVALID_PARAMETER;
  }

  // obj_attributes may have a name inside of it, if != NULL.
  if (obj_attributes_ptr) {
    sem->SetAttributes(obj_attributes_ptr);
  }

  if (handle_ptr) {
    *handle_ptr = sem->handle();
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtCreateSemaphore, kThreading, kImplemented);

dword_result_t NtReleaseSemaphore_entry(dword_t sem_handle,
                                        dword_t release_count,
                                        lpdword_t previous_count_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;
  int32_t previous_count = 0;

  auto sem =
      kernel_state()->object_table()->LookupObject<XSemaphore>(sem_handle);
  if (sem) {
    bool success =
        sem->ReleaseSemaphore((int32_t)release_count, &previous_count);
    if (!success) {
      XELOGW(
          "NtReleaseSemaphore: release_count={} would exceed maximum (current "
          "count={})",
          uint32_t(release_count), previous_count);
      result = X_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }
  if (previous_count_ptr) {
    *previous_count_ptr = (uint32_t)previous_count;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT2(NtReleaseSemaphore, kThreading, kImplemented,
                         kHighFrequency);

dword_result_t NtCreateMutant_entry(
    lpdword_t handle_out, pointer_t<X_OBJECT_ATTRIBUTES> obj_attributes,
    dword_t initial_owner) {
  // Check for an existing timer with the same name.
  auto existing_object = LookupNamedObject<XMutant>(
      kernel_state(), obj_attributes.guest_address());
  if (existing_object) {
    if (existing_object->type() == XObject::Type::Mutant) {
      if (handle_out) {
        existing_object->RetainHandle();
        *handle_out = existing_object->handle();
      }
      return X_STATUS_OBJECT_NAME_EXISTS;
    } else {
      return X_STATUS_INVALID_HANDLE;
    }
  }

  auto mutant = object_ref<XMutant>(new XMutant(kernel_state()));
  mutant->Initialize(initial_owner ? true : false);

  // obj_attributes may have a name inside of it, if != NULL.
  if (obj_attributes) {
    mutant->SetAttributes(obj_attributes);
  }

  if (handle_out) {
    *handle_out = mutant->handle();
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtCreateMutant, kThreading, kImplemented);

dword_result_t NtReleaseMutant_entry(dword_t mutant_handle,
                                     lpdword_t previous_count) {
  // This doesn't seem to be supported.
  // int32_t previous_count_ptr = SHIM_GET_ARG_32(2);

  // Whatever arg 1 is all games seem to set it to 0, so whether it's
  // abandon or wait we just say false. Which is good, cause they are
  // both ignored.
  uint32_t priority_increment = 0;
  bool abandon = false;
  bool wait = false;

  X_STATUS result = X_STATUS_SUCCESS;

  auto mutant =
      kernel_state()->object_table()->LookupObject<XMutant>(mutant_handle);
  if (mutant) {
    mutant->ReleaseMutant(priority_increment, abandon, wait);
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtReleaseMutant, kThreading, kImplemented);

dword_result_t NtCreateTimer_entry(
    lpdword_t handle_ptr, pointer_t<X_OBJECT_ATTRIBUTES> obj_attributes_ptr,
    dword_t timer_type) {
  // timer_type = NotificationTimer (0) or SynchronizationTimer (1)

  // Check for an existing timer with the same name.
  auto existing_object =
      LookupNamedObject<XTimer>(kernel_state(), obj_attributes_ptr);
  if (existing_object) {
    if (existing_object->type() == XObject::Type::Timer) {
      if (handle_ptr) {
        existing_object->RetainHandle();
        *handle_ptr = existing_object->handle();
      }
      return X_STATUS_OBJECT_NAME_EXISTS;
    } else {
      return X_STATUS_INVALID_HANDLE;
    }
  }

  auto timer = object_ref<XTimer>(new XTimer(kernel_state()));
  timer->Initialize(timer_type);

  // obj_attributes may have a name inside of it, if != NULL.
  if (obj_attributes_ptr) {
    timer->SetAttributes(obj_attributes_ptr);
  }

  if (handle_ptr) {
    *handle_ptr = timer->handle();
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtCreateTimer, kThreading, kImplemented);

// KeSetTimer / KeSetTimerEx / KeCancelTimer were declared in the export table
// with no implementation. They operate on a guest KTIMER pointer rather than a
// handle, so they resolve it through GetNativeObject, which now adopts guest
// timers (see XTimer::InitializeNative). Arming reuses XTimer::SetTimer, the
// same path NtSetTimerEx uses. The DPC argument is not dispatched yet - the
// timer still signals, which is what waiters need.
static object_ref<XTimer> GetGuestTimer(uint32_t timer_guest_ptr) {
  if (!timer_guest_ptr) {
    return object_ref<XTimer>();
  }
  auto* native = kernel_memory()->TranslateVirtual(timer_guest_ptr);
  return XObject::GetNativeObject<XTimer>(kernel_state(), native);
}

// A KDPC carries the routine to run when the timer expires. Xenia dispatches
// DPCs inline on the calling thread (KeInsertQueueDpc), and XTimer already
// enqueues its routine as an APC on the thread that set the timer, so passing
// the DPC's routine through gives the callback a thread with a valid KPCR.
// This is looser than real DPC semantics - a DPC runs at DISPATCH_IRQL, not as
// a thread APC - but it is the same approximation Xenia already makes.
// Phase 1095bn: the pool's 8-object WaitAny is now all-valid and all-unsignalled
// (1095bm), so it blocks correctly and waits for someone to wake it. Slot 0 is a
// TimerSynchronizationObject at 81D424A8, and an ARMED timer would fire by
// itself. Report every timer arm - address, due time, period, and the guest lr -
// so it is a fact rather than an assumption whether 81D424A8 is ever armed.
static void GuideTimerArm(const char* what, uint32_t timer_guest, int64_t due,
                          uint32_t period) {
  if (!cvars::guide_bkgnd_watch) return;
  static std::atomic<uint32_t> n{0};
  const uint32_t i = ++n;
  if (i > 24u) return;
  auto* th = XThread::GetCurrentThread();
  auto* ctx = (th && th->thread_state()) ? th->thread_state()->context() : nullptr;
  XELOGI("GuideTimerArm #{}: {} timer {:08X}{} due {} period {} | tid {:08X} "
         "lr {:08X}",
         i, what, timer_guest,
         timer_guest == 0x81D424A8u ? "  <== THE POOL'S TIMER" : "", due, period,
         th ? th->thread_id() : 0u,
         ctx ? static_cast<uint32_t>(ctx->lr) : 0u);
}

static void ReadDpc(uint32_t dpc_guest_ptr, uint32_t* out_routine,
                    uint32_t* out_context) {
  *out_routine = 0;
  *out_context = 0;
  if (!dpc_guest_ptr) {
    return;
  }
  auto* dpc = kernel_memory()->TranslateVirtual<XDPC*>(dpc_guest_ptr);
  *out_routine = dpc->routine;
  *out_context = dpc->context;
}

dword_result_t KeSetTimer_entry(lpvoid_t timer_ptr, qword_t due_time,
                                lpvoid_t dpc_ptr) {
  auto timer = GetGuestTimer(timer_ptr.guest_address());
  if (!timer) {
    return 0;
  }
  uint32_t routine, context;
  ReadDpc(dpc_ptr.guest_address(), &routine, &context);
  GuideTimerArm("KeSetTimer", timer_ptr.guest_address(),
                static_cast<int64_t>(due_time), 0);
  timer->SetTimer(due_time, 0, routine, context, false,
                  routine ? dpc_ptr.guest_address() : 0);
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeSetTimer, kThreading, kImplemented);

dword_result_t KeSetTimerEx_entry(lpvoid_t timer_ptr, qword_t due_time,
                                  dword_t period_ms, lpvoid_t dpc_ptr) {
  auto timer = GetGuestTimer(timer_ptr.guest_address());
  if (!timer) {
    return 0;
  }
  uint32_t routine, context;
  ReadDpc(dpc_ptr.guest_address(), &routine, &context);
  GuideTimerArm("KeSetTimerEx", timer_ptr.guest_address(),
                static_cast<int64_t>(due_time), period_ms);
  timer->SetTimer(due_time, period_ms, routine, context, false,
                  routine ? dpc_ptr.guest_address() : 0);
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeSetTimerEx, kThreading, kImplemented);

dword_result_t KeCancelTimer_entry(lpvoid_t timer_ptr) {
  auto timer = GetGuestTimer(timer_ptr.guest_address());
  if (!timer) {
    return 0;
  }
  timer->Cancel();
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeCancelTimer, kThreading, kImplemented);

dword_result_t NtSetTimerEx_entry(dword_t timer_handle, lpqword_t due_time_ptr,
                                  lpvoid_t routine_ptr /*PTIMERAPCROUTINE*/,
                                  dword_t mode, lpvoid_t routine_arg,
                                  dword_t resume, dword_t period_ms,
                                  lpdword_t unk_zero) {
  assert_true(mode == 1);
  assert_true(!unk_zero);

  if (unk_zero) {
    XELOGI("NtSetTimerEx: unk_zero is set!");
  }

  uint64_t due_time = *due_time_ptr;

  X_STATUS result = X_STATUS_SUCCESS;

  auto timer =
      kernel_state()->object_table()->LookupObject<XTimer>(timer_handle);
  if (timer) {
    result =
        timer->SetTimer(due_time, period_ms, routine_ptr.guest_address(),
                        routine_arg.guest_address(), resume ? true : false);
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtSetTimerEx, kThreading, kImplemented);

dword_result_t NtCancelTimer_entry(dword_t timer_handle,
                                   lpdword_t current_state_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto timer =
      kernel_state()->object_table()->LookupObject<XTimer>(timer_handle);
  if (timer) {
    result = timer->Cancel();
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }
  if (current_state_ptr) {
    *current_state_ptr = 0;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtCancelTimer, kThreading, kImplemented);

uint32_t xeKeWaitForSingleObject(void* object_ptr, uint32_t wait_reason,
                                 uint32_t processor_mode, uint32_t alertable,
                                 uint64_t* timeout_ptr) {
  auto object = XObject::GetNativeObject<XObject>(kernel_state(), object_ptr);

  if (!object) {
    // Phase 1096gx: "this should never happen" happens, and in a Release build
    // assert_always() is a no-op, so a wait on an object Xenia cannot resolve
    // returns INSTANTLY and claims the wait completed. Measured consequence:
    // dash.xex's loop at 92262334..92262378 asks for a 500 ms wait on
    // [ctx+0x14] each iteration and runs 335,000 iterations a second instead
    // of two, which is the 33.5M-per-100s HdDvdRom poll. Report the object and
    // its dispatch header, once per distinct call site, so the thing Xenia is
    // failing to model names itself rather than being guessed at. Behaviour is
    // deliberately left unchanged here - returning early is wrong, but so is
    // sleeping to paper over an object that should have existed.
    static std::mutex unresolved_mu;
    static std::unordered_map<uint32_t, uint32_t> unresolved_seen;
    auto* th = XThread::GetCurrentThread();
    const uint32_t lr =
        (th && th->thread_state() && th->thread_state()->context())
            ? static_cast<uint32_t>(th->thread_state()->context()->lr)
            : 0u;
    bool first = false;
    {
      std::lock_guard<std::mutex> lk(unresolved_mu);
      if (unresolved_seen.size() < 24u &&
          unresolved_seen.find(lr) == unresolved_seen.end()) {
        unresolved_seen[lr] = 1;
        first = true;
      }
    }
    if (first) {
      const uint32_t guest =
          static_cast<uint32_t>(reinterpret_cast<uint8_t*>(object_ptr) -
                                kernel_memory()->virtual_membase());
      // Phase 1096gy: the first word alone is not enough to tell an
      // uninitialised object from an initialised one. KeInitializeEvent zeroes
      // the whole header and then writes type and signal_state, so a
      // NotificationEvent created unsignalled has an all-zero first word -
      // indistinguishable from never having been touched. What separates them
      // is the stash XObject::StashHandle leaves in the wait_list fields at +8
      // and +12, so print the whole 16-byte dispatch header.
      // Phase 1096hw: this used QueryRangeAccess alone, and that query returns
      // kNoAccess for readable IMAGE addresses (the same trap that made
      // XamTextWatch blind to dash). When it said no, hdr stayed {0,0,0,0} and
      // printed as four zero words - indistinguishable from a genuinely blank
      // header, which is exactly the reading that was drawn from it. Fall back
      // to the region's COMMIT state, and say explicitly when nothing was read.
      uint32_t hdr[4] = {0, 0, 0, 0};
      bool hdr_read = false;
      auto* hp = kernel_memory()->LookupHeap(guest);
      if (hp) {
        bool ok = hp->QueryRangeAccess(guest, guest + 15) !=
                  xe::memory::PageAccess::kNoAccess;
        if (!ok) {
          HeapAllocationInfo info = {};
          if (hp->QueryRegionInfo(guest & ~0xFFFu, &info)) {
            ok = (info.state & kMemoryAllocationCommit) != 0;
          }
        }
        // Phase 1096hw2: both queries still say no for dash's data pages, yet
        // the guest demonstrably reads and writes them - the XEX loader commits
        // only the page-descriptor total, so the bookkeeping disagrees with the
        // mapping. The comment on the heap-descriptor scan in emulator.cc
        // already settled what the right bound is: "staying inside
        // [base, base+size) is the correct and sufficient bound". Use it.
        if (!ok) {
          auto mod = kernel_state()->GetExecutableModule();
          if (mod && mod->xex_module()) {
            const uint32_t lo = mod->xex_module()->base_address();
            const uint32_t hi = lo + mod->xex_module()->image_size();
            if (guest >= lo && guest + 16 <= hi) {
              ok = true;
            }
          }
        }
        if (ok) {
          hdr_read = true;
          for (int w = 0; w < 4; ++w) {
            hdr[w] = xe::load_and_swap<uint32_t>(
                kernel_memory()->TranslateVirtual(guest + w * 4));
          }
        }
      }
      XELOGW(
          "KeWaitForSingleObject: {:08X} will not resolve (header {} {:08X} "
          "{:08X} {:08X} {:08X}, type {}), timeout {} - returning "
          "ABANDONED_WAIT_0 WITHOUT waiting | lr {:08X}",
          guest, hdr_read ? "READ" : "NOT-READABLE", hdr[0], hdr[1], hdr[2],
          hdr[3], hdr[0] & 0xFF, timeout_ptr ? "finite" : "INFINITE", lr);
    }
    return X_STATUS_ABANDONED_WAIT_0;
  }

  X_STATUS result =
      object->Wait(wait_reason, processor_mode, alertable, timeout_ptr);
  if (alertable) {
    if (result == X_STATUS_USER_APC) {
      xeProcessUserApcs(nullptr);
    }
  }
  return result;
}

dword_result_t KeWaitForSingleObject_entry(lpvoid_t object_ptr,
                                           dword_t wait_reason,
                                           dword_t processor_mode,
                                           dword_t alertable,
                                           lpqword_t timeout_ptr) {
  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  // Phase 1095m: 81750FA8's init task blocks forever at 817BC0C8, an infinite
  // KeWaitForSingleObject inside 817BBC20 (coverage: 140/467, furthest
  // 817BC0C8; the next instruction never executes). Name the object and its
  // dispatch header for that one call site - the return address of that bl is
  // 817BC0CC - so the thing nothing signals is identified rather than guessed.
  {
    auto* th = XThread::GetCurrentThread();
    const uint32_t lr =
        (th && th->thread_state() && th->thread_state()->context())
            ? static_cast<uint32_t>(th->thread_state()->context()->lr)
            : 0u;
    // Phase 1096k: 8172E1DC is the second, UPSTREAM infinite wait -
    // 81750ED8 reaches `bl 8172E190` at 81750F7C and never returns, so its
    // KeSetEvent(81D3FBAC) at 81750F90 never runs, which is why the wait at
    // 817BC0C8 can never be satisfied. Name both objects, not just the
    // downstream one.
    // Phase 1096r: the app-table walk stalls inside musicplayer's initialiser
    // (entry 3): 817D23C4 `bl 81AA9FB8` is the furthest instruction reached and
    // 817D23C8 never executes, and 81AA9FB8 tail-dispatches through vtable slot
    // +0x28 at 81AAA0BC. Rather than guess the virtual target, catch EVERY
    // infinite wait and report each DISTINCT caller once - the blocking call in
    // that chain names itself. Bounded to 16 distinct sites.
    if (!timeout_ptr) {
      static std::mutex lr_mu;
      static std::unordered_map<uint32_t, uint32_t> lr_seen;
      bool first = false;
      {
        std::lock_guard<std::mutex> lk(lr_mu);
        if (lr_seen.size() < 16u && lr_seen.find(lr) == lr_seen.end()) {
          lr_seen[lr] = 1;
          first = true;
        }
      }
      if (first) {
        const uint32_t o = object_ptr.guest_address();
        uint32_t h = 0;
        if (o) {
          h = xe::load_and_swap<uint32_t>(kernel_memory()->TranslateVirtual(o));
        }
        XELOGI("GuideInfWait: lr={:08X} obj={:08X} header={:08X} (INFINITE "
               "KeWaitForSingleObject, first time from this caller)",
               lr, o, h);
        XELOGI("GuideInfWait: chain {}", kernel_state()->GuestBackChain());
      }
    }
    if (lr == 0x817BC0CCu || lr == 0x8172E1DCu) {
      static std::atomic<uint32_t> n{0};
      const uint32_t i = ++n;
      if (i <= 8u) {
        const uint32_t obj = object_ptr.guest_address();
        uint32_t hdr = 0;
        if (obj) {
          hdr = xe::load_and_swap<uint32_t>(
              kernel_memory()->TranslateVirtual(obj));
        }
        XELOGI("GuideInitWait #{}: {} waits on {:08X} header {:08X} "
               "(type {}) timeout {} | tid {:08X}",
               i, lr == 0x8172E1DCu ? "8172E1D8 (UPSTREAM)" : "817BC0C8",
               obj, hdr, hdr & 0xFF,
               timeout_ptr ? "finite" : "INFINITE",
               th ? th->thread_id() : 0u);
      }
    }
  }
  return xeKeWaitForSingleObject(object_ptr, wait_reason, processor_mode,
                                 alertable, timeout_ptr ? &timeout : nullptr);
}
DECLARE_XBOXKRNL_EXPORT3(KeWaitForSingleObject, kThreading, kImplemented,
                         kBlocking, kHighFrequency);

uint32_t NtWaitForSingleObjectEx(uint32_t object_handle, uint32_t wait_mode,
                                 uint32_t alertable, uint64_t* timeout_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto object =
      kernel_state()->object_table()->LookupObject<XObject>(object_handle);
  if (object) {
    uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
    result =
        object->Wait(3, wait_mode, alertable, timeout_ptr ? &timeout : nullptr);
    if (alertable) {
      if (result == X_STATUS_USER_APC) {
        xeProcessUserApcs(nullptr);
      }
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

dword_result_t NtWaitForSingleObjectEx_entry(dword_t object_handle,
                                             dword_t wait_mode,
                                             dword_t alertable,
                                             lpqword_t timeout_ptr) {
  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  return NtWaitForSingleObjectEx(object_handle, wait_mode, alertable,
                                 timeout_ptr ? &timeout : nullptr);
}
DECLARE_XBOXKRNL_EXPORT3(NtWaitForSingleObjectEx, kThreading, kImplemented,
                         kBlocking, kHighFrequency);

dword_result_t KeWaitForMultipleObjects_entry(
    dword_t count, lpdword_t objects_ptr, dword_t wait_type,
    dword_t wait_reason, dword_t processor_mode, dword_t alertable,
    lpqword_t timeout_ptr, pointer_t<X_KWAIT_BLOCK> wait_block_array_ptr) {
  if (cvars::guide_log_pool_sync && objects_ptr) {
    for (uint32_t i = 0; i < count && i < 8; ++i) {
      GuidePoolSyncLog("wait-begin", objects_ptr[i], count, i);
    }
  }
  assert_true(wait_type <= X_KWAIT_REASON::WaitAny);

  // Phase 1096s: same deduplicated infinite-wait census as in
  // KeWaitForSingleObject, but for the MULTIPLE-object form. 1096s showed the
  // musicplayer stall (app-table entry 3) is not a single-object wait, so the
  // blocking call in 817D2xxx / 81AAAxxx has to be one of these instead.
  {
    auto* mth = XThread::GetCurrentThread();
    const uint32_t mlr =
        (mth && mth->thread_state() && mth->thread_state()->context())
            ? static_cast<uint32_t>(mth->thread_state()->context()->lr)
            : 0u;
    if (!timeout_ptr) {
      static std::mutex m_mu;
      static std::unordered_map<uint32_t, uint32_t> m_seen;
      bool mfirst = false;
      {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_seen.size() < 16u && m_seen.find(mlr) == m_seen.end()) {
          m_seen[mlr] = 1;
          mfirst = true;
        }
      }
      if (mfirst) {
        XELOGI("GuideInfWaitMulti: lr={:08X} count={} type={} (INFINITE "
               "KeWaitForMultipleObjects, first time from this caller)",
               mlr, uint32_t(count), uint32_t(wait_type));
      }
    }
  }

  // Phase 1096m: the pool worker loop at 8177AC50 waits here and then does
  //   8177AC78  cmplwi r3, 2 / blt 8177ACE8      <- DEQUEUE the pool+0xB4 queue
  //   8177AC84  r11 = r3 - 2
  //   8177AC88  cmplw r11, [r31+0x234] / bge 8177ACE0   <- `twui` ASSERT
  // Coverage says the dequeue span is never executed and the trap branch IS, so
  // this call returns a value that is neither a low index nor a valid extra
  // index. Xenia does not honour twui, so xam's own assert is silent and the
  // worker just loops - which is why task 817318F0 never runs and xam's init is
  // stranded. Log the arguments for this one call site (return address
  // 8177AC78) so the bad value is named rather than inferred.
  {
    auto* gth = XThread::GetCurrentThread();
    const uint32_t glr =
        (gth && gth->thread_state() && gth->thread_state()->context())
            ? static_cast<uint32_t>(gth->thread_state()->context()->lr)
            : 0u;
    if (glr == 0x8177AC78u) {
      static std::atomic<uint32_t> gn{0};
      const uint32_t gi = ++gn;
      if (gi <= 6u) {
        std::string objs;
        for (uint32_t i = 0; i < count && i < 8; ++i) {
          const uint32_t o = objects_ptr[i];
          uint32_t hdr = 0;
          if (o) {
            hdr = xe::load_and_swap<uint32_t>(
                kernel_memory()->TranslateVirtual(o));
          }
          objs += fmt::format("[{}]={:08X}(hdr {:08X}) ", i, o, hdr);
        }
        XELOGI("GuidePoolWait #{}: count={} type={} timeout={} | {}", gi,
               uint32_t(count), uint32_t(wait_type),
               timeout_ptr ? "finite" : "INFINITE", objs);
      }
    }
  }


  // Phase 1095bc: nothing found so far WRITES D2F1BEEF into [401EA360+0xC] -
  // not the record constructor (817785C8 zeroes it), not the USB teardown
  // (817472A8 zeroes it), and the value appears nowhere in xam, hud or the dash
  // as code or data. Catch the transition: poll the field on every wait and
  // report the FIRST time it is non-zero, with the thread that was running.
  if (cvars::guide_bkgnd_watch) {
    static std::atomic<uint32_t> seen{0};
    if (!seen.load(std::memory_order_relaxed)) {
      auto rdq = [&](uint32_t a) {
        return xe::load_and_swap<uint32_t>(kernel_memory()->TranslateVirtual(a));
      };
      const uint32_t v = rdq(0x401EA360u + 0xCu);
      if (v && seen.exchange(1) == 0) {
        auto* pth = XThread::GetCurrentThread();
        auto* pctx = (pth && pth->thread_state()) ? pth->thread_state()->context()
                                                  : nullptr;
        XELOGI("GuideRecPoison: [401EA360+0xC] first seen as {:08X} | flags {:08X}"
               " | tid {:08X} lr {:08X}",
               v, rdq(0x401EA360u + 8u), pth ? pth->thread_id() : 0u,
               pctx ? static_cast<uint32_t>(pctx->lr) : 0u);
      }
    }
  }

  // Phase 1095ap: the pool spins millions of times because slot 7 of an
  // EIGHT-object wait holds D2F1BEEF, which is not an object. Dump the whole
  // array once so the seven good entries name the subsystem and the bad one
  // can be attributed. Bounded and gated.
  if (cvars::guide_bkgnd_watch && count == 8 && objects_ptr) {
    static std::atomic<uint32_t> w8{0};
    const uint32_t wi = ++w8;
    if (wi <= 4u) {
      auto* wth = XThread::GetCurrentThread();
      auto* wctx = (wth && wth->thread_state()) ? wth->thread_state()->context()
                                                : nullptr;
      std::string ents;
      for (uint32_t k = 0; k < 8u; ++k) {
        const uint32_t g = static_cast<uint32_t>(objects_ptr[k]);
        // Phase 1095bm: do NOT gate this on QueryRangeAccess - most of these
        // objects live in xam's IMAGE, whose pages that check reports as
        // kNoAccess, which is why this column read 00000000 for everything up
        // to now (1095f flagged it, 1095bh proved it). Read the dispatch
        // header directly: word 0 is type/size, word 1 is SIGNAL STATE - the
        // thing that decides whether this wait can ever complete.
        uint32_t hdr0 = 0, sig = 0;
        if (g && kernel_memory()->LookupHeap(g)) {
          auto* hb = kernel_memory()->TranslateVirtual(g);
          hdr0 = xe::load_and_swap<uint32_t>(hb);
          sig = xe::load_and_swap<uint32_t>(hb + 4);
        }
        ents += fmt::format("[{}]={:08X}(t{} sig{}) ", k, g,
                            (hdr0 >> 24) & 0xFF, sig);
      }
      XELOGI("GuideWait8 #{}: wait_type {} ({}) from guest lr {:08X} "
             "tid {:08X} | {}", wi, static_cast<uint32_t>(wait_type),
             static_cast<uint32_t>(wait_type) == 0 ? "WaitAll" : "WaitAny",
             wctx ? static_cast<uint32_t>(wctx->lr) : 0u,
             wth ? wth->thread_id() : 0u, ents);
      // Phase 1095at: the append at 8177A670 writes the wait object from
      // [record+0xC], and at 8177A67C the RECORD ITSELF into the parallel array
      // pool+0x1D4 at the same index. So the record that supplied the poison is
      // recoverable. The wait array has two FIXED leading slots
      // (count = [pool+0x234] + 2), so wait slot k maps to parallel index k-2.
      {
        auto rdp = [&](uint32_t a) {
          return xe::load_and_swap<uint32_t>(kernel_memory()->TranslateVirtual(a));
        };
        std::string recs;
        const uint32_t cnt = rdp(0x81D423C0u + 0x234u);
        for (uint32_t k = 0; k < 8u && k < cnt; ++k) {
          const uint32_t rec = rdp(0x81D423C0u + 0x1D4u + k * 4u);
          uint32_t f8 = 0, fc = 0;
          auto* rh = rec ? kernel_memory()->LookupHeap(rec) : nullptr;
          if (rh && rh->QueryRangeAccess(rec, rec + 0xF) !=
                        xe::memory::PageAccess::kNoAccess) {
            f8 = rdp(rec + 8);
            fc = rdp(rec + 0xC);
          }
          // Phase 1095av: dump the whole record head - +0 is usually a
          // vtable or tag and should identify what kind of thing this
          // is, without needing to find its allocator.
          std::string w;
          if (f8 || fc) {
            for (uint32_t q = 0; q < 16u; ++q) {
              w += fmt::format("{:08X} ", rdp(rec + q * 4u));
            }
          }
          recs += fmt::format("<{}>rec={:08X} [{}] ", k, rec, w);
        }
        XELOGI("GuideWait8Rec #{}: [pool+234]={} arraybase={:08X} | {}", wi, cnt,
               rdp(0x81D423C0u + 0x1D0u), recs);
      }
    }
  }
  assert_true(count <= 64);
  object_ref<XObject> objects[64];
  {
    auto crit = global_critical_region::AcquireDirect();
    for (uint32_t n = 0; n < count; n++) {
      auto object_ptr = kernel_memory()->TranslateVirtual(objects_ptr[n]);
      auto object_ref = XObject::GetNativeObject<XObject>(
          kernel_state(), object_ptr, UndefinedObject, true);
      if (!object_ref) {
        // This returns immediately - it does not wait - so a caller that
        // loops on the result spins at full speed. xam's mode-1 device
        // bring-up does exactly that, retrying a 3-object wait tens of
        // thousands of times a second. Name the object that will not resolve
        // and its dispatcher type; a type Xenia does not implement lands here
        // and is indistinguishable from a real failure at the call site.
        static std::atomic<uint32_t> bad{0};
        uint32_t bn = ++bad;
        if (bn <= 8 || (bn % 100000) == 0) {
          // Read the dispatch type only if the page is actually there. The
          // pointer that lands here can be unmapped - dereferencing it
          // unconditionally faulted, and because this runs while the global
          // critical region is held (and /EHsc does not unwind SEH), that
          // fault wedged the whole emulator behind a modal dialog.
          uint32_t obj_guest = objects_ptr[n];
          auto* obj_heap = kernel_memory()->LookupHeap(obj_guest);
          const bool obj_readable =
              obj_heap && obj_heap->QueryRangeAccess(
                              obj_guest, obj_guest + sizeof(X_DISPATCH_HEADER) -
                                             1) !=
                              xe::memory::PageAccess::kNoAccess;
          auto* hdr =
              obj_readable ? reinterpret_cast<uint8_t*>(object_ptr) : nullptr;
          // Phase 1096fz: log the CALLER. The existing message names the
          // object but not who is waiting on it, and D2DCBEEF now blocks BOTH
          // the dashboard and Fable III - so which guest code builds that
          // array decides whether this is an LLE-xam bug (in scope) or a
          // title bug (not).
          uint32_t bad_lr = 0;
          if (auto* bt = XThread::GetCurrentThread()) {
            if (bt->thread_state() && bt->thread_state()->context()) {
              bad_lr = uint32_t(bt->thread_state()->context()->lr);
            }
          }
          // Phase 1096gf: dump the header bytes. Type 129 (0x81) is not in
          // X_OBJECT_TYPES (0x0-0xE), and the two candidate readings - a real
          // type 0x01 with an unmasked high-bit flag, versus an uninitialised
          // header - are told apart by whether the REST of the header looks
          // like a live dispatch object (plausible signal_state, wait-list
          // links pointing at themselves) or like garbage.
          std::string hx;
          if (hdr) {
            for (int hb = 0; hb < 16; ++hb) {
              hx += fmt::format("{:02X} ", hdr[hb]);
            }
          }
          XELOGW(
              "KeWaitForMultipleObjects #{}: object {} of {} at {:08X} will "
              "not resolve; dispatch type {} -> returning INVALID_PARAMETER "
              "without waiting | lr {:08X} | hdr {}",
              bn, n, static_cast<uint32_t>(count),
              static_cast<uint32_t>(objects_ptr[n]),
              hdr ? hdr[0] : 0xFF, bad_lr, hx.empty() ? "(unreadable)" : hx);
        }
        return X_STATUS_INVALID_PARAMETER;
      }

      objects[n] = std::move(object_ref);
    }
  }
  // The retry loop in xam's mode-1 device bring-up (8178EE08) waits on three
  // objects. A previous pass instrumented only the failure path and saw a
  // two-object wait, which says nothing about this one. Log three-object waits
  // whether they succeed or not.
  if (count == 3) {
    static std::atomic<uint32_t> w3{0};
    uint32_t wn = ++w3;
    if (wn <= 6 || (wn % 20000) == 0) {
      auto* m = kernel_memory();
      uint8_t t0 = *reinterpret_cast<uint8_t*>(
          m->TranslateVirtual(objects_ptr[0]));
      uint8_t t1 = *reinterpret_cast<uint8_t*>(
          m->TranslateVirtual(objects_ptr[1]));
      uint8_t t2 = *reinterpret_cast<uint8_t*>(
          m->TranslateVirtual(objects_ptr[2]));
      // Phase 1095i: 81D3CA08 is .bss (zero in xam.bin, checked statically)
      // and is self-linked by 81727500, which only 81750FA8's pool task
      // reaches. If it is still 0 by the time the pool is waiting, that task
      // has not run and the fault at 817286C0 is a MISSING DISPATCH, not a
      // race between two tasks.
      {
        auto* m = kernel_memory();
        // 1099z17559-4: these are 17489 xam addresses; on another build the
        // page can be unmapped and the read faulted (x17559n). Read only
        // committed pages - the log line itself is unchanged.
        auto rd = [&](uint32_t a) -> uint32_t {
          auto* h = m->LookupHeap(a);
          if (!h || h->QueryRangeAccess(a, a + 3) ==
                        xe::memory::PageAccess::kNoAccess) {
            return 0;
          }
          return xe::load_and_swap<uint32_t>(m->TranslateVirtual(a));
        };
        XELOGI("GuideUiGlobals@Wait3 #{}: [81D3C8E8]={:08X} [81D3CA08]={:08X} "
               "self-linked? {}",
               wn, rd(0x81D3C8E8u), rd(0x81D3CA08u),
               rd(0x81D3CA08u) == 0x81D3CA08u ? "yes" : "no");
      }
      XELOGI("Wait3 #{}: {:08X}(type {}) {:08X}(type {}) {:08X}(type {})", wn,
             static_cast<uint32_t>(objects_ptr[0]), t0,
             static_cast<uint32_t>(objects_ptr[1]), t1,
             static_cast<uint32_t>(objects_ptr[2]), t2);
    }
  }
  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  X_STATUS result = XObject::WaitMultiple(
      uint32_t(count), reinterpret_cast<XObject**>(&objects[0]), wait_type,
      wait_reason, processor_mode, alertable, timeout_ptr ? &timeout : nullptr);
  if (alertable) {
    if (result == X_STATUS_USER_APC) {
      xeProcessUserApcs(nullptr);
    }
  }

  // Phase 1096m: report the RESULT for the pool worker's wait (return address
  // 8177AC78). xam does `cmplwi r3,2 / blt` to the dequeue, then
  // `r11 = r3-2 / cmplw r11,[pool+0x234] / bge` to a `twui` assert. With
  // count==2 and [pool+0x234]==0, ONLY 0 or 1 are legal - anything else is the
  // assert path, which Xenia silently ignores because it does not honour twui.
  {
    auto* rth = XThread::GetCurrentThread();
    const uint32_t rlr =
        (rth && rth->thread_state() && rth->thread_state()->context())
            ? static_cast<uint32_t>(rth->thread_state()->context()->lr)
            : 0u;
    if (rlr == 0x8177AC78u) {
      static std::atomic<uint32_t> rn{0};
      const uint32_t ri = ++rn;
      if (ri <= 6u) {
        XELOGI("GuidePoolWait #{} RESULT = {:08X} (count={}; legal indices are "
               "0..{}; anything else takes xam's twui assert at 8177ACE0 and "
               "the pool+0xB4 queue is never drained)",
               ri, uint32_t(result), uint32_t(count),
               count ? uint32_t(count - 1) : 0u);
      }
    }
  }
  return result;
}
DECLARE_XBOXKRNL_EXPORT3(KeWaitForMultipleObjects, kThreading, kImplemented,
                         kBlocking, kHighFrequency);

uint32_t xeNtWaitForMultipleObjectsEx(uint32_t count, xe::be<uint32_t>* handles,
                                      uint32_t wait_type, uint32_t wait_mode,
                                      uint32_t alertable,
                                      uint64_t* timeout_ptr) {
  assert_true(wait_type <= X_KWAIT_REASON::WaitAny);

  assert_true(count <= 64);
  object_ref<XObject> objects[64];

  /*
        Reserving to squash the constant reallocations, in a benchmark of one
     particular game over a period of five minutes roughly 11% of CPU time was
     spent inside a helper function to Windows' heap allocation function. 7% of
     that time was traced back to here

         edit: actually switched to fixed size array, as there can never be more
     than 64 events specified
  */
  {
    auto crit = global_critical_region::AcquireDirect();
    for (uint32_t n = 0; n < count; n++) {
      uint32_t object_handle = handles[n];
      auto object = kernel_state()->object_table()->LookupObject<XObject>(
          object_handle, true);
      if (!object) {
        return X_STATUS_INVALID_PARAMETER;
      }
      objects[n] = std::move(object);
    }
  }
  auto result =
      XObject::WaitMultiple(count, reinterpret_cast<XObject**>(&objects[0]),
                            wait_type, 6, wait_mode, alertable, timeout_ptr);
  if (alertable) {
    if (result == X_STATUS_USER_APC) {
      xeProcessUserApcs(nullptr);
    }
  }
  return result;
}

dword_result_t NtWaitForMultipleObjectsEx_entry(
    dword_t count, lpdword_t handles, dword_t wait_type, dword_t wait_mode,
    dword_t alertable, lpqword_t timeout_ptr) {
  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  if (!count || count > 64 ||
      (wait_type != X_KWAIT_REASON::WaitAny && wait_type)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  return xeNtWaitForMultipleObjectsEx(count, handles, wait_type, wait_mode,
                                      alertable,
                                      timeout_ptr ? &timeout : nullptr);
}
DECLARE_XBOXKRNL_EXPORT3(NtWaitForMultipleObjectsEx, kThreading, kImplemented,
                         kBlocking, kHighFrequency);

dword_result_t NtSignalAndWaitForSingleObjectEx_entry(dword_t signal_handle,
                                                      dword_t wait_handle,
                                                      dword_t wait_mode,
                                                      dword_t alertable,
                                                      lpqword_t timeout_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;
  // pre-lock for these two handle lookups
  global_critical_region::mutex().lock();

  auto signal_object = kernel_state()->object_table()->LookupObject<XObject>(
      signal_handle, true);
  auto wait_object =
      kernel_state()->object_table()->LookupObject<XObject>(wait_handle, true);
  global_critical_region::mutex().unlock();
  if (signal_object && wait_object) {
    uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
    result = XObject::SignalAndWait(signal_object.get(), wait_object.get(), 3,
                                    wait_mode, alertable,
                                    timeout_ptr ? &timeout : nullptr);
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  if (alertable) {
    if (result == X_STATUS_USER_APC) {
      xeProcessUserApcs(nullptr);
    }
  }
  return result;
}
DECLARE_XBOXKRNL_EXPORT3(NtSignalAndWaitForSingleObjectEx, kThreading,
                         kImplemented, kBlocking, kHighFrequency);

static void PrefetchForCAS(const void* value) { swcache::PrefetchW(value); }

uint32_t xeKeKfAcquireSpinLock(PPCContext* ctx, X_KSPINLOCK* lock,
                               bool change_irql) {
  auto old_irql = change_irql ? xeKfRaiseIrql(ctx, 2) : 0;

  PrefetchForCAS(lock);
  assert_true(lock->prcb_of_owner != static_cast<uint32_t>(ctx->r[13]));

  uint32_t our_pcr = static_cast<uint32_t>(ctx->r[13]);
  uint8_t our_cpu =
      ctx->TranslateVirtualGPR<X_KPCR*>(our_pcr)->prcb_data.current_cpu;

  // Lock.
  while (
      !xe::atomic_cas(0, xe::byte_swap(our_pcr), &lock->prcb_of_owner.value)) {
    // On real hardware, threads sharing a Xenon HW thread are serialized by
    // the kernel scheduler — the spinner would be preempted within one
    // timeslice (~1ms) so the holder can make progress.  In the naive
    // host-thread model both threads run truly in parallel, so the spinner
    // can burn its entire host quantum without giving the holder a chance.
    //
    // Check whether the lock holder is assigned to the same guest CPU as us.
    // If so, yield the host thread aggressively (Sleep(0)) to force a host
    // context switch and give the holder a chance to run and release.
    // The relationship is stable — affinity doesn't change while a thread
    // holds a spinlock — so one check per contention episode is sufficient.
    uint32_t owner_pcr_be = lock->prcb_of_owner.value;
    if (owner_pcr_be) {
      uint32_t owner_pcr = xe::byte_swap(owner_pcr_be);
      auto* owner_kpcr = ctx->TranslateVirtual<X_KPCR*>(owner_pcr);
      if (owner_kpcr->prcb_data.current_cpu == our_cpu) {
        xe::threading::Sleep(std::chrono::milliseconds(0));
        continue;
      }
    }
    xe::threading::MaybeYield();
  }

  return old_irql;
}

dword_result_t KfAcquireSpinLock_entry(pointer_t<X_KSPINLOCK> lock_ptr,
                                       const ppc_context_t& context) {
  return xeKeKfAcquireSpinLock(context, lock_ptr, true);
}
DECLARE_XBOXKRNL_EXPORT3(KfAcquireSpinLock, kThreading, kImplemented, kBlocking,
                         kHighFrequency);

void xeKeKfReleaseSpinLock(PPCContext* ctx, X_KSPINLOCK* lock,
                           uint32_t old_irql, bool change_irql) {
  assert_true(lock->prcb_of_owner == static_cast<uint32_t>(ctx->r[13]));
  // Unlock with release semantics to ensure all prior writes are visible.
  xe::atomic_store_release(0u, &lock->prcb_of_owner.value);

  if (change_irql) {
    // Unlock.
    if (old_irql >= 2) {
      return;
    }

    // Restore IRQL.
    xeKfLowerIrql(ctx, old_irql);
  }
}

void KfReleaseSpinLock_entry(pointer_t<X_KSPINLOCK> lock_ptr, dword_t old_irql,
                             const ppc_context_t& ppc_ctx) {
  xeKeKfReleaseSpinLock(ppc_ctx, lock_ptr, old_irql, true);
}

DECLARE_XBOXKRNL_EXPORT2(KfReleaseSpinLock, kThreading, kImplemented,
                         kHighFrequency);
// todo: this is not accurate
void KeAcquireSpinLockAtRaisedIrql_entry(pointer_t<X_KSPINLOCK> lock_ptr,
                                         const ppc_context_t& ppc_ctx) {
  xeKeKfAcquireSpinLock(ppc_ctx, lock_ptr, false);
}
DECLARE_XBOXKRNL_EXPORT3(KeAcquireSpinLockAtRaisedIrql, kThreading,
                         kImplemented, kBlocking, kHighFrequency);

dword_result_t KeTryToAcquireSpinLockAtRaisedIrql_entry(
    pointer_t<X_KSPINLOCK> lock_ptr, const ppc_context_t& ppc_ctx) {
  // Lock.
  auto lock = reinterpret_cast<uint32_t*>(lock_ptr.host_address());
  assert_true(lock_ptr->prcb_of_owner != static_cast<uint32_t>(ppc_ctx->r[13]));
  PrefetchForCAS(lock);
  if (!ppc_ctx->processor->GuestAtomicCAS32(
          ppc_ctx, 0, static_cast<uint32_t>(ppc_ctx->r[13]),
          lock_ptr.guest_address())) {
    return 0;
  }
  return 1;
}
DECLARE_XBOXKRNL_EXPORT4(KeTryToAcquireSpinLockAtRaisedIrql, kThreading,
                         kImplemented, kBlocking, kHighFrequency, kSketchy);

void KeReleaseSpinLockFromRaisedIrql_entry(pointer_t<X_KSPINLOCK> lock_ptr,
                                           const ppc_context_t& ppc_ctx) {
  xeKeKfReleaseSpinLock(ppc_ctx, lock_ptr, 0, false);
}

DECLARE_XBOXKRNL_EXPORT2(KeReleaseSpinLockFromRaisedIrql, kThreading,
                         kImplemented, kHighFrequency);

void KeEnterCriticalRegion_entry() {
  XThread::GetCurrentThread()->EnterCriticalRegion();
}
DECLARE_XBOXKRNL_EXPORT2(KeEnterCriticalRegion, kThreading, kImplemented,
                         kHighFrequency);

void KeLeaveCriticalRegion_entry() {
  XThread::GetCurrentThread()->LeaveCriticalRegion();
}
DECLARE_XBOXKRNL_EXPORT2(KeLeaveCriticalRegion, kThreading, kImplemented,
                         kHighFrequency);

dword_result_t KeRaiseIrqlToDpcLevel_entry(const ppc_context_t& ctx) {
  auto pcr = ctx.GetPCR();
  uint32_t old_irql = pcr->current_irql;

  if (old_irql > 2) {
    XELOGE("KeRaiseIrqlToDpcLevel - old_irql > 2");
  }

  pcr->current_irql = 2;

  return old_irql;
}
DECLARE_XBOXKRNL_EXPORT2(KeRaiseIrqlToDpcLevel, kThreading, kImplemented,
                         kHighFrequency);
void xeKfLowerIrql(PPCContext* ctx, unsigned char new_irql) {
  X_KPCR* kpcr = ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13]);

  if (new_irql > kpcr->current_irql) {
    XELOGE("KfLowerIrql : new_irql > kpcr->current_irql!");
  }
  kpcr->current_irql = new_irql;
  if (new_irql < 2) {
    // the called function does a ton of other stuff including changing the
    // irql and interrupt_related
  }
}
// irql is supposed to be per thread afaik...
void KfLowerIrql_entry(dword_t new_irql, const ppc_context_t& ctx) {
  xeKfLowerIrql(ctx, static_cast<unsigned char>(new_irql));
}
DECLARE_XBOXKRNL_EXPORT2(KfLowerIrql, kThreading, kImplemented, kHighFrequency);

unsigned char xeKfRaiseIrql(PPCContext* ctx, unsigned char new_irql) {
  X_KPCR* v1 = ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13]);

  uint32_t old_irql = v1->current_irql;
  v1->current_irql = new_irql;

  if (old_irql > (unsigned int)new_irql) {
    XELOGE("KfRaiseIrql - old_irql > new_irql!");
  }
  return old_irql;
}
// used by aurora's nova plugin
// like the other irql related functions, writes to an unknown mmio range (
// 0x7FFF ). The range is indexed by the low 16 bits of the KPCR's pointer (so
// r13)
dword_result_t KfRaiseIrql_entry(dword_t new_irql, const ppc_context_t& ctx) {
  return xeKfRaiseIrql(ctx, new_irql);
}

DECLARE_XBOXKRNL_EXPORT2(KfRaiseIrql, kThreading, kImplemented, kHighFrequency);

uint32_t xeNtQueueApcThread(uint32_t thread_handle, uint32_t apc_routine,
                            uint32_t apc_routine_context, uint32_t arg1,
                            uint32_t arg2, cpu::ppc::PPCContext* context) {
  auto kernelstate = context->kernel_state;
  auto memory = kernelstate->memory();
  auto thread =
      kernelstate->object_table()->LookupObject<XThread>(thread_handle);

  if (!thread) {
    XELOGE("NtQueueApcThread: Incorrect thread handle! Might cause crash");
    return X_STATUS_INVALID_HANDLE;
  }

  uint32_t apc_ptr = memory->SystemHeapAlloc(XAPC::kSize);
  if (!apc_ptr) {
    return X_STATUS_NO_MEMORY;
  }
  XAPC* apc = context->TranslateVirtual<XAPC*>(apc_ptr);
  xeKeInitializeApc(apc, thread->guest_object(), XAPC::kDummyKernelRoutine, 0,
                    apc_routine, 1 /*user apc mode*/, apc_routine_context);

  if (!xeKeInsertQueueApc(apc, arg1, arg2, 0, context)) {
    memory->SystemHeapFree(apc_ptr);
    return X_STATUS_UNSUCCESSFUL;
  }
  // no-op, just meant to awaken a sleeping alertable thread to process real
  // apcs
  thread->thread()->QueueUserCallback([]() {});
  return X_STATUS_SUCCESS;
}
dword_result_t NtQueueApcThread_entry(dword_t thread_handle,
                                      lpvoid_t apc_routine,
                                      lpvoid_t apc_routine_context,
                                      lpvoid_t arg1, lpvoid_t arg2,
                                      const ppc_context_t& context) {
  return xeNtQueueApcThread(thread_handle, apc_routine, apc_routine_context,
                            arg1, arg2, context);
}

X_STATUS xeProcessUserApcs(PPCContext* ctx) {
  if (!ctx) {
    ctx = cpu::ThreadState::Get()->context();
  }
  X_STATUS alert_status = X_STATUS_SUCCESS;
  auto kpcr = ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13]);

  auto current_thread = ctx->TranslateVirtual(kpcr->prcb_data.current_thread);

  uint32_t unlocked_irql =
      xeKeKfAcquireSpinLock(ctx, &current_thread->apc_lock);

  auto& user_apc_queue = current_thread->apc_lists[1];

  // use guest stack for temporaries
  uint32_t old_stack_pointer = static_cast<uint32_t>(ctx->r[1]);

  uint32_t scratch_address = old_stack_pointer - 16;
  ctx->r[1] = old_stack_pointer - 32;

  while (!user_apc_queue.empty(ctx)) {
    uint32_t apc_ptr = user_apc_queue.flink_ptr;

    XAPC* apc = user_apc_queue.ListEntryObject(
        ctx->TranslateVirtual<X_LIST_ENTRY*>(apc_ptr));

    uint8_t* scratch_ptr = ctx->TranslateVirtual(scratch_address);
    xe::store_and_swap<uint32_t>(scratch_ptr + 0, apc->normal_routine);
    xe::store_and_swap<uint32_t>(scratch_ptr + 4, apc->normal_context);
    xe::store_and_swap<uint32_t>(scratch_ptr + 8, apc->arg1);
    xe::store_and_swap<uint32_t>(scratch_ptr + 12, apc->arg2);
    util::XeRemoveEntryList(&apc->list_entry, ctx);
    apc->enqueued = 0;

    xeKeKfReleaseSpinLock(ctx, &current_thread->apc_lock, unlocked_irql);
    alert_status = X_STATUS_USER_APC;
    if (apc->kernel_routine != XAPC::kDummyKernelRoutine) {
      uint64_t kernel_args[] = {
          apc_ptr,
          scratch_address + 0,
          scratch_address + 4,
          scratch_address + 8,
          scratch_address + 12,
      };
      ctx->processor->Execute(ctx->thread_state, apc->kernel_routine,
                              kernel_args, xe::countof(kernel_args));
    } else {
      ctx->kernel_state->memory()->SystemHeapFree(apc_ptr);
    }

    uint32_t normal_routine = xe::load_and_swap<uint32_t>(scratch_ptr + 0);
    uint32_t normal_context = xe::load_and_swap<uint32_t>(scratch_ptr + 4);
    uint32_t arg1 = xe::load_and_swap<uint32_t>(scratch_ptr + 8);
    uint32_t arg2 = xe::load_and_swap<uint32_t>(scratch_ptr + 12);

    if (normal_routine) {
      uint64_t normal_args[] = {normal_context, arg1, arg2};
      ctx->processor->Execute(ctx->thread_state, normal_routine, normal_args,
                              xe::countof(normal_args));
    }

    unlocked_irql = xeKeKfAcquireSpinLock(ctx, &current_thread->apc_lock);
  }

  ctx->r[1] = old_stack_pointer;

  xeKeKfReleaseSpinLock(ctx, &current_thread->apc_lock, unlocked_irql);
  return alert_status;
}

static void YankApcList(PPCContext* ctx, X_KTHREAD* current_thread,
                        unsigned apc_mode, bool rundown) {
  uint32_t unlocked_irql =
      xeKeKfAcquireSpinLock(ctx, &current_thread->apc_lock);

  XAPC* result = nullptr;
  auto& user_apc_queue = current_thread->apc_lists[apc_mode];

  if (user_apc_queue.empty(ctx)) {
    result = nullptr;
  } else {
    result = user_apc_queue.HeadObject(ctx);
    for (auto&& entry : user_apc_queue.IterateForward(ctx)) {
      entry.enqueued = 0;
    }
    util::XeRemoveEntryList(&user_apc_queue, ctx);
  }

  xeKeKfReleaseSpinLock(ctx, &current_thread->apc_lock, unlocked_irql);

  if (rundown && result) {
    XAPC* current_entry = result;
    while (true) {
      XAPC* this_entry = current_entry;
      uint32_t next_entry = this_entry->list_entry.flink_ptr;

      if (this_entry->rundown_routine) {
        uint64_t args[] = {ctx->HostToGuestVirtual(this_entry)};
        kernel_state()->processor()->Execute(ctx->thread_state,
                                             this_entry->rundown_routine, args,
                                             xe::countof(args));
      } else {
        ctx->kernel_state->memory()->SystemHeapFree(
            ctx->HostToGuestVirtual(this_entry));
      }

      if (next_entry == 0) {
        break;
      }
      current_entry = user_apc_queue.ListEntryObject(
          ctx->TranslateVirtual<X_LIST_ENTRY*>(next_entry));
      if (current_entry == result) {
        break;
      }
    }
  }
}

void xeRundownApcs(cpu::ppc::PPCContext* ctx) {
  auto kpcr = ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13]);

  auto current_thread = ctx->TranslateVirtual(kpcr->prcb_data.current_thread);
  YankApcList(ctx, current_thread, 1, true);
  YankApcList(ctx, current_thread, 0, false);
}
DECLARE_XBOXKRNL_EXPORT1(NtQueueApcThread, kThreading, kImplemented);
void xeKeInitializeApc(XAPC* apc, uint32_t thread_ptr, uint32_t kernel_routine,
                       uint32_t rundown_routine, uint32_t normal_routine,
                       uint32_t apc_mode, uint32_t normal_context) {
  apc->thread_ptr = thread_ptr;
  apc->kernel_routine = kernel_routine;
  apc->rundown_routine = rundown_routine;
  apc->normal_routine = normal_routine;
  apc->type = 18;
  if (normal_routine) {
    apc->apc_mode = apc_mode;
    apc->normal_context = normal_context;
  } else {
    apc->apc_mode = 0;
    apc->normal_context = 0;
  }
  apc->enqueued = 0;
}
void KeInitializeApc_entry(pointer_t<XAPC> apc, pointer_t<X_KTHREAD> thread_ptr,
                           lpvoid_t kernel_routine, lpvoid_t rundown_routine,
                           lpvoid_t normal_routine, dword_t processor_mode,
                           lpvoid_t normal_context) {
  xeKeInitializeApc(apc, thread_ptr, kernel_routine, rundown_routine,
                    normal_routine, processor_mode, normal_context);
}
DECLARE_XBOXKRNL_EXPORT1(KeInitializeApc, kThreading, kImplemented);

uint32_t xeKeInsertQueueApc(XAPC* apc, uint32_t arg1, uint32_t arg2,
                            uint32_t priority_increment,
                            cpu::ppc::PPCContext* context) {
  uint32_t thread_guest_pointer = apc->thread_ptr;
  if (!thread_guest_pointer) {
    return 0;
  }
  auto target_thread = context->TranslateVirtual<X_KTHREAD*>(apc->thread_ptr);
  auto old_irql = xeKeKfAcquireSpinLock(context, &target_thread->apc_lock);
  uint32_t result;
  if (!target_thread->may_queue_apcs || apc->enqueued) {
    result = 0;
  } else {
    apc->arg1 = arg1;
    apc->arg2 = arg2;

    auto& which_list = target_thread->apc_lists[apc->apc_mode];

    if (apc->normal_routine) {
      which_list.InsertTail(apc, context);
    } else {
      XAPC* insertion_pos = nullptr;
      for (auto&& sub_apc : which_list.IterateForward(context)) {
        insertion_pos = &sub_apc;
        if (sub_apc.normal_routine) {
          break;
        }
      }
      if (!insertion_pos) {
        which_list.InsertHead(apc, context);
      } else {
        util::XeInsertHeadList(insertion_pos->list_entry.blink_ptr,
                               &apc->list_entry, context);
      }
    }

    apc->enqueued = 1;

    /*
        todo: this is incomplete, a ton of other logic happens here, i believe
       for waking the target thread if its alertable
    */
    result = 1;
  }
  xeKeKfReleaseSpinLock(context, &target_thread->apc_lock, old_irql);
  return result;
}

dword_result_t KeInsertQueueApc_entry(pointer_t<XAPC> apc, lpvoid_t arg1,
                                      lpvoid_t arg2, dword_t priority_increment,
                                      const ppc_context_t& context) {
  return xeKeInsertQueueApc(apc, arg1, arg2, priority_increment, context);
}
DECLARE_XBOXKRNL_EXPORT1(KeInsertQueueApc, kThreading, kImplemented);

dword_result_t KeRemoveQueueApc_entry(pointer_t<XAPC> apc,
                                      const ppc_context_t& context) {
  bool result = false;

  uint32_t thread_guest_pointer = apc->thread_ptr;
  if (!thread_guest_pointer) {
    return 0;
  }
  auto target_thread = context->TranslateVirtual<X_KTHREAD*>(apc->thread_ptr);
  auto old_irql = xeKeKfAcquireSpinLock(context, &target_thread->apc_lock);

  if (apc->enqueued) {
    result = true;
    apc->enqueued = 0;
    util::XeRemoveEntryList(&apc->list_entry, context);
    // todo: this is incomplete, there is more logic here in actual kernel
  }
  xeKeKfReleaseSpinLock(context, &target_thread->apc_lock, old_irql);

  return result ? 1 : 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeRemoveQueueApc, kThreading, kImplemented);

dword_result_t KiApcNormalRoutineNop_entry(dword_t unk0 /* output? */,
                                           dword_t unk1 /* 0x13 */) {
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(KiApcNormalRoutineNop, kThreading, kStub);

void KeInitializeDpc_entry(pointer_t<XDPC> dpc, lpvoid_t routine,
                           lpvoid_t context) {
  dpc->Initialize(routine, context);
}
DECLARE_XBOXKRNL_EXPORT2(KeInitializeDpc, kThreading, kImplemented, kSketchy);

dword_result_t KeInsertQueueDpc_entry(pointer_t<XDPC> dpc, dword_t arg1,
                                      dword_t arg2) {
  uint32_t list_entry_ptr = dpc.guest_address() + 4;

  // Lock dispatcher.
  auto global_lock = xe::global_critical_region::AcquireDirect();
  auto dpc_list = kernel_state()->dpc_list();

  // Prep DPC.
  dpc->arg1 = (uint32_t)arg1;
  dpc->arg2 = (uint32_t)arg2;

  dpc_list->Insert(list_entry_ptr);

  // Dispatch the DPC inline on the calling thread.  On real hardware DPCs
  // are deferred to DISPATCH_IRQL on the target processor, but DPC routines
  // access per-CPU state via r13 (KPCR) so they must run on a thread whose
  // KPCR is valid for the target CPU.  The calling thread's KPCR satisfies
  // this for the common case (desired_cpu_number == 0, meaning current CPU).
  // Inline dispatch also avoids latency issues with shared work queues.
  uint32_t routine = dpc->routine;
  if (routine) {
    auto thread = XThread::GetCurrentThread();
    if (thread) {
      auto thread_state = thread->thread_state();
      auto ppc_context = thread_state->context();
      auto kpcr = ppc_context->TranslateVirtualGPR<X_KPCR*>(ppc_context->r[13]);

      // If we're already inside a DPC (reentrant KeInsertQueueDpc from a DPC
      // routine), skip the impersonation — we're already at DISPATCH_IRQL.
      bool already_in_dpc = kpcr->prcb_data.dpc_active != 0;

      DPCImpersonationScope dpc_scope{};
      if (!already_in_dpc) {
        kernel_state()->BeginDPCImpersonation(ppc_context, dpc_scope);
      }

      uint64_t args[] = {dpc.guest_address(), (uint64_t)dpc->context,
                         (uint64_t)arg1, (uint64_t)arg2};
      kernel_state()->processor()->Execute(thread_state, routine, args,
                                           xe::countof(args));

      if (!already_in_dpc) {
        kernel_state()->EndDPCImpersonation(ppc_context, dpc_scope);
      }
    }
  }

  return 1;
}
DECLARE_XBOXKRNL_EXPORT2(KeInsertQueueDpc, kThreading, kImplemented, kSketchy);

dword_result_t KeRemoveQueueDpc_entry(pointer_t<XDPC> dpc) {
  bool result = false;

  uint32_t list_entry_ptr = dpc.guest_address() + 4;

  auto global_lock = xe::global_critical_region::AcquireDirect();
  auto dpc_list = kernel_state()->dpc_list();
  if (dpc_list->IsQueued(list_entry_ptr)) {
    dpc_list->Remove(list_entry_ptr);
    result = true;
  }

  return result ? 1 : 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeRemoveQueueDpc, kThreading, kImplemented);

// https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/51e4dfcaacfdbd1a9692272931a436371492f72d/import/OpenXDK/include/xboxkrnl/xboxkrnl.h#L1372
struct X_ERWLOCK {
  be<int32_t> lock_count;              // 0x0
  be<uint32_t> writers_waiting_count;  // 0x4
  be<uint32_t> readers_waiting_count;  // 0x8
  be<uint32_t> readers_entry_count;    // 0xC
  X_KEVENT writer_event;               // 0x10
  X_KSEMAPHORE reader_semaphore;       // 0x20
  X_KSPINLOCK spin_lock;               // 0x34
};
static_assert_size(X_ERWLOCK, 0x38);

void ExInitializeReadWriteLock_entry(pointer_t<X_ERWLOCK> lock_ptr) {
  lock_ptr->lock_count = -1;
  lock_ptr->writers_waiting_count = 0;
  lock_ptr->readers_waiting_count = 0;
  lock_ptr->readers_entry_count = 0;
  KeInitializeEvent_entry(&lock_ptr->writer_event, 1, 0);
  KeInitializeSemaphore_entry(&lock_ptr->reader_semaphore, 0, 0x7FFFFFFF);
  lock_ptr->spin_lock.prcb_of_owner = 0;
}
DECLARE_XBOXKRNL_EXPORT1(ExInitializeReadWriteLock, kThreading, kImplemented);

void ExAcquireReadWriteLockExclusive_entry(pointer_t<X_ERWLOCK> lock_ptr,
                                           const ppc_context_t& ppc_context) {
  auto old_irql = xeKeKfAcquireSpinLock(ppc_context, &lock_ptr->spin_lock);

  int32_t lock_count = ++lock_ptr->lock_count;
  if (!lock_count) {
    xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
    return;
  }

  lock_ptr->writers_waiting_count++;

  xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
  xeKeWaitForSingleObject(&lock_ptr->writer_event, 7, 0, 0, nullptr);
}
DECLARE_XBOXKRNL_EXPORT2(ExAcquireReadWriteLockExclusive, kThreading,
                         kImplemented, kBlocking);

dword_result_t ExTryToAcquireReadWriteLockExclusive_entry(
    pointer_t<X_ERWLOCK> lock_ptr, const ppc_context_t& ppc_context) {
  auto old_irql = xeKeKfAcquireSpinLock(ppc_context, &lock_ptr->spin_lock);

  uint32_t result;
  if (lock_ptr->lock_count < 0) {
    lock_ptr->lock_count = 0;
    result = 1;
  } else {
    result = 0;
  }

  xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ExTryToAcquireReadWriteLockExclusive, kThreading,
                         kImplemented);

void ExAcquireReadWriteLockShared_entry(pointer_t<X_ERWLOCK> lock_ptr,
                                        const ppc_context_t& ppc_context) {
  auto old_irql = xeKeKfAcquireSpinLock(ppc_context, &lock_ptr->spin_lock);

  int32_t lock_count = ++lock_ptr->lock_count;
  if (!lock_count ||
      (lock_ptr->readers_entry_count && !lock_ptr->writers_waiting_count)) {
    lock_ptr->readers_entry_count++;
    xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
    return;
  }

  lock_ptr->readers_waiting_count++;

  xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
  xeKeWaitForSingleObject(&lock_ptr->reader_semaphore, 7, 0, 0, nullptr);
}
DECLARE_XBOXKRNL_EXPORT2(ExAcquireReadWriteLockShared, kThreading, kImplemented,
                         kBlocking);

dword_result_t ExTryToAcquireReadWriteLockShared_entry(
    pointer_t<X_ERWLOCK> lock_ptr, const ppc_context_t& ppc_context) {
  auto old_irql = xeKeKfAcquireSpinLock(ppc_context, &lock_ptr->spin_lock);

  uint32_t result;
  if (lock_ptr->lock_count < 0 ||
      (lock_ptr->readers_entry_count && !lock_ptr->writers_waiting_count)) {
    lock_ptr->lock_count++;
    lock_ptr->readers_entry_count++;
    result = 1;
  } else {
    result = 0;
  }

  xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ExTryToAcquireReadWriteLockShared, kThreading,
                         kImplemented);

void ExReleaseReadWriteLock_entry(pointer_t<X_ERWLOCK> lock_ptr,
                                  const ppc_context_t& ppc_context) {
  auto old_irql = xeKeKfAcquireSpinLock(ppc_context, &lock_ptr->spin_lock);

  int32_t lock_count = --lock_ptr->lock_count;

  if (lock_count < 0) {
    lock_ptr->readers_entry_count = 0;
    xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
    return;
  }

  // Phase 1099q: a WRITER releasing has readers_entry_count == 0. The old code
  // decremented it anyway when no reader was waiting, so it wrapped to
  // 0xFFFFFFFF, read as "readers still inside", and returned WITHOUT waking
  // the waiting writer - and left the counters corrupt for every later user.
  // Two threads taking the lock exclusively in turn deadlocked: measured,
  // signin.xex's thread parked forever in ExAcquireReadWriteLockExclusive
  // (return 90115010, lock 404E1140) after a profile was picked on the
  // dashboard's sign-in screen, the sign-in UI never tore down, xam's UI gate
  // [81D43CF8] stayed 1 with HUD state 0x10 pending, and every later Guide
  // press was refused. Only a READER release decrements the reader count
  // (same as the Xbox kernel logic in Cxbx-Reloaded's ExReleaseReadWriteLock).
  if (!lock_ptr->readers_entry_count) {
    auto readers_waiting_count = lock_ptr->readers_waiting_count;
    if (readers_waiting_count) {
      lock_ptr->readers_waiting_count = 0;
      lock_ptr->readers_entry_count = readers_waiting_count;
      xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
      xeKeReleaseSemaphore(&lock_ptr->reader_semaphore, 1,
                           readers_waiting_count, 0);
      return;
    }
  } else {
    auto readers_entry_count = --lock_ptr->readers_entry_count;
    if (readers_entry_count) {
      xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
      return;
    }
  }

  lock_ptr->writers_waiting_count--;
  xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
  xeKeSetEvent(&lock_ptr->writer_event, 1, 0);
}
DECLARE_XBOXKRNL_EXPORT1(ExReleaseReadWriteLock, kThreading, kImplemented);

// NOTE: This function is very commonly inlined, and probably won't be called!
pointer_result_t InterlockedPushEntrySList_entry(
    pointer_t<X_SLIST_HEADER> plist_ptr, pointer_t<X_SINGLE_LIST_ENTRY> entry) {
  assert_not_null(plist_ptr);
  assert_not_null(entry);

  alignas(8) X_SLIST_HEADER old_hdr = *plist_ptr;
  alignas(8) X_SLIST_HEADER new_hdr = {{0}, 0, 0};
  uint32_t old_head = 0;
  do {
    old_hdr = *plist_ptr;
    new_hdr.depth = old_hdr.depth + 1;
    new_hdr.sequence = old_hdr.sequence + 1;

    old_head = old_hdr.next.next;
    entry->next = old_hdr.next.next;
    new_hdr.next.next = entry.guest_address();
  } while (
      !xe::atomic_cas(*(uint64_t*)(&old_hdr), *(uint64_t*)(&new_hdr),
                      reinterpret_cast<uint64_t*>(plist_ptr.host_address())));

  return old_head;
}
DECLARE_XBOXKRNL_EXPORT2(InterlockedPushEntrySList, kThreading, kImplemented,
                         kHighFrequency);

pointer_result_t InterlockedPopEntrySList_entry(
    pointer_t<X_SLIST_HEADER> plist_ptr) {
  assert_not_null(plist_ptr);

  uint32_t popped = 0;
  alignas(8) X_SLIST_HEADER old_hdr = {{0}, 0, 0};
  alignas(8) X_SLIST_HEADER new_hdr = {{0}, 0, 0};
  do {
    old_hdr = *plist_ptr;
    auto next = kernel_memory()->TranslateVirtual<X_SINGLE_LIST_ENTRY*>(
        old_hdr.next.next);
    if (!old_hdr.next.next) {
      return 0;
    }
    popped = old_hdr.next.next;

    new_hdr.depth = old_hdr.depth - 1;
    new_hdr.next.next = next->next;
    new_hdr.sequence = old_hdr.sequence;
  } while (
      !xe::atomic_cas(*(uint64_t*)(&old_hdr), *(uint64_t*)(&new_hdr),
                      reinterpret_cast<uint64_t*>(plist_ptr.host_address())));

  return popped;
}
DECLARE_XBOXKRNL_EXPORT2(InterlockedPopEntrySList, kThreading, kImplemented,
                         kHighFrequency);

pointer_result_t InterlockedFlushSList_entry(
    pointer_t<X_SLIST_HEADER> plist_ptr) {
  assert_not_null(plist_ptr);

  alignas(8) X_SLIST_HEADER old_hdr = *plist_ptr;
  alignas(8) X_SLIST_HEADER new_hdr = {{0}, 0, 0};
  uint32_t first = 0;
  do {
    old_hdr = *plist_ptr;
    first = old_hdr.next.next;
    new_hdr.next.next = 0;
    new_hdr.depth = 0;
    new_hdr.sequence = 0;
  } while (
      !xe::atomic_cas(*(uint64_t*)(&old_hdr), *(uint64_t*)(&new_hdr),
                      reinterpret_cast<uint64_t*>(plist_ptr.host_address())));

  return first;
}
DECLARE_XBOXKRNL_EXPORT1(InterlockedFlushSList, kThreading, kImplemented);

dword_result_t KeSetPriorityThread_entry(pointer_t<X_KTHREAD> thread_ptr,
                                         dword_t new_priority,
                                         const ppc_context_t& context) {
  if (!thread_ptr) {
    XELOGE("{}: Invalid thread_ptr.", __func__);
    return 0;
  }

  if (thread_ptr->header.type != ThreadObject) {
    XELOGW("{}: Invalid object type: {}", __func__,
           static_cast<uint8_t>(thread_ptr->header.type));
  }

  X_KPRCB* prcb = context->TranslateVirtual(thread_ptr->a_prcb_ptr);
  const uint32_t old_irql = xeKeKfAcquireSpinLock(context, &prcb->spin_lock);
  const uint8_t old_priority = thread_ptr->priority;

  auto thread_ref = XObject::GetNativeObject<XThread>(kernel_state(),
                                                      thread_ptr, ThreadObject);

  if (!thread_ref) {
    XELOGW("{}: Missing native thread: {}", __func__,
           static_cast<uint8_t>(thread_ptr->header.type));
  } else {
    thread_ref->SetPriority(new_priority);
  }

  xeKeKfReleaseSpinLock(context, &prcb->spin_lock, old_irql);
  return old_priority;
}
DECLARE_XBOXKRNL_EXPORT1(KeSetPriorityThread, kThreading, kImplemented);

void xeKeInitializeTimerEx(X_KTIMER* timer, uint32_t type, uint32_t proctype,
                           PPCContext* context) {
  xenia_assert(proctype < 3);
  xenia_assert(type == 0 || type == 1);
  // other fields are unmodified, they must carry through multiple calls of
  // initialize
  timer->header.process_type = proctype;
  timer->header.inserted = 0;
  timer->header.type =
      type ? TimerSynchronizationObject : TimerNotificationObject;
  timer->header.signal_state = 0;
  util::XeInitializeListHead(&timer->header.wait_list, context);
  timer->due_time = 0;
  timer->period = 0;
}

void KeInitializeTimerEx_entry(pointer_t<X_KTIMER> timer, dword_t type,
                               dword_t proctype, const ppc_context_t& context) {
  xeKeInitializeTimerEx(timer, type, proctype & 0xFF, context);
}
DECLARE_XBOXKRNL_EXPORT1(KeInitializeTimerEx, kThreading, kImplemented);

// Declared at ordinal 0x1A but never implemented. xam's XamApp thread reaches
// this and exits, which is why system app 0xFE never registers. Traps are not
// involved - with ignore_trap_instructions=false no twi fires, so this is
// normal control flow, not a failed assertion. Walk the guest stack to find
// who calls it. Xenon MSVC stores the saved LR 8 bytes below the caller's SP.
void ExTerminateTitleProcess_entry(dword_t exit_code, dword_t unk,
                                   const ppc_context_t& ctx) {
  XELOGI("ExTerminateTitleProcess({:08X}, {:08X}) chain {}",
         uint32_t(exit_code), uint32_t(unk), kernel_state()->GuestBackChain());
  // Phase 1099v: this was a research stub that only logged, so xam's title
  // teardown did nothing. Do what the real kernel does (80056800): run the
  // title-terminate notification chain, tearing down only the title process,
  // and RETURN to the caller (a xam system worker). The old stack dump below is
  // kept behind guide_bkgnd_watch.
  kernel_state()->TerminateTitleProcessSelective();
  if (!cvars::guide_bkgnd_watch) {
    return;
  }
  XELOGE("ExTerminateTitleProcess(code={:08X}, unk={:08X}) - guest stack:",
         static_cast<uint32_t>(exit_code), static_cast<uint32_t>(unk));
  auto* mem = ctx->kernel_state->memory();
  // XamApp's entry pointer is installed from [[r13+0x100]+0x14C]
  // (81781FC4/FD8/FDC). Read the chain to see what it actually yields.
  uint32_t r13 = static_cast<uint32_t>(ctx->r[13]);
  uint32_t p100 = r13 ? xe::load_and_swap<uint32_t>(
                            mem->TranslateVirtual(r13 + 0x100)) : 0;
  uint32_t entry = p100 ? xe::load_and_swap<uint32_t>(
                              mem->TranslateVirtual(p100 + 0x14C)) : 0;
  XELOGE("  r13={:08X}  [r13+0x100]={:08X}  [+0x14C]={:08X} (ghidra {:08X})",
         r13, p100, entry, entry + 0x7200u);
  uint32_t sp = static_cast<uint32_t>(ctx->r[1]);
  for (int i = 0; i < 12 && sp; ++i) {
    uint32_t next = xe::load_and_swap<uint32_t>(mem->TranslateVirtual(sp));
    if (!next || next <= sp) {
      break;
    }
    uint32_t lr = xe::load_and_swap<uint32_t>(mem->TranslateVirtual(next - 8));
    if (lr < 0x80000000u) {
      break;
    }
    XELOGE("  frame[{}] lr={:08X}  (xam ghidra {:08X})", i, lr, lr + 0x7200u);
    // 81780EE0 virtual-calls [r31+48]; r31 is saved in its frame by
    // __savegprs. Dump the words just below the frame so the saved
    // registers (and the object pointer) are visible.
    if (i <= 1) {
      std::string dump;
      for (int k = 1; k <= 10; ++k) {
        dump += fmt::format("{:08X} ", xe::load_and_swap<uint32_t>(
            mem->TranslateVirtual(next - k * 4)));
      }
      XELOGE("    saved below frame[{}]: {}", i, dump);
      // 81780EE0 receives obj = [ctx+12] and calls [obj+48]. The saved
      // word 3 slots below the frame is that object; dump it so the
      // function pointer in slot 48 is visible.
      if (i == 0) {
        uint32_t obj = xe::load_and_swap<uint32_t>(
            mem->TranslateVirtual(next - 12));
        if (obj >= 0x40000000u && obj < 0x50000000u) {
          std::string od;
          for (int k = 0; k < 16; ++k) {
            od += fmt::format("{:08X} ", xe::load_and_swap<uint32_t>(
                mem->TranslateVirtual(obj + k * 4)));
          }
          uint32_t fn = xe::load_and_swap<uint32_t>(
              mem->TranslateVirtual(obj + 48));
          XELOGE("    obj @{:08X}: {}", obj, od);
          XELOGE("    [obj+48] = {:08X}  (xam ghidra {:08X})", fn,
                 fn + 0x7200u);
        }
      }
    }
    sp = next;
  }
}
DECLARE_XBOXKRNL_EXPORT1(ExTerminateTitleProcess, kThreading, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Threading);
