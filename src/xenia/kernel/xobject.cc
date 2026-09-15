/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <unordered_map>

#include "xenia/kernel/xobject.h"

#include <mutex>

#include "xenia/base/memory.h"
#include "xenia/kernel/kernel_flags.h"  // phase 1056: guide_dispatch_guard
#include "xenia/kernel/xboxkrnl/xboxkrnl_video.h"  // phase 1073: XamAddrInImage

#include "xenia/base/byte_stream.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xmutant.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xsemaphore.h"
#include "xenia/kernel/xsymboliclink.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/xtimer.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

XObject::XObject(Type type)
    : kernel_state_(nullptr), pointer_ref_count_(1), type_(type) {
  handles_.reserve(10);
}

XObject::XObject(KernelState* kernel_state, Type type, bool host_object)
    : kernel_state_(kernel_state),
      type_(type),
      pointer_ref_count_(1),
      guest_object_ptr_(0),
      allocated_guest_object_(false),
      host_object_(host_object) {
  handles_.reserve(10);

  // TODO: Assert kernel_state != nullptr in this constructor.
  if (kernel_state) {
    kernel_state->object_table()->AddHandle(this, nullptr);
  }
}

XObject::~XObject() {
  assert_true(handles_.empty());
  assert_zero(pointer_ref_count_);

  if (allocated_guest_object_) {
    uint32_t ptr = guest_object_ptr_ - sizeof(X_OBJECT_HEADER);
    auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(ptr);

    // Free the object creation info
    if (header->object_type_ptr) {
      memory()->SystemHeapFree(header->object_type_ptr);
    }

    memory()->SystemHeapFree(ptr);
  }
}

Emulator* XObject::emulator() const { return kernel_state_->emulator_; }
KernelState* XObject::kernel_state() const { return kernel_state_; }
Memory* XObject::memory() const { return kernel_state_->memory(); }

XObject::Type XObject::type() const { return type_; }

void XObject::RetainHandle() {
  kernel_state_->object_table()->RetainHandle(handles_[0]);
}

bool XObject::ReleaseHandle() {
  // FIXME: Return true when handle is actually released.
  return kernel_state_->object_table()->ReleaseHandle(handles_[0]) ==
         X_STATUS_SUCCESS;
}

void XObject::Retain() { ++pointer_ref_count_; }

void XObject::Release() {
  if (--pointer_ref_count_ == 0) {
    delete this;
  }
}

X_STATUS XObject::Delete() {
  if (kernel_state_ == nullptr) {
    // Fake return value for api-scanner
    return X_STATUS_SUCCESS;
  } else {
    if (!name_.empty()) {
      kernel_state_->object_table()->RemoveNameMapping(name_);
    }
    return kernel_state_->object_table()->RemoveHandle(handles_[0]);
  }
}

bool XObject::SaveObject(ByteStream* stream) {
  stream->Write<uint32_t>(allocated_guest_object_);
  stream->Write<uint32_t>(guest_object_ptr_);

  stream->Write(uint32_t(handles_.size()));
  stream->Write(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  return true;
}

bool XObject::RestoreObject(ByteStream* stream) {
  allocated_guest_object_ = stream->Read<uint32_t>() > 0;
  guest_object_ptr_ = stream->Read<uint32_t>();

  handles_.resize(stream->Read<uint32_t>());
  stream->Read(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  // Restore our pointer to our handles in the object table.
  for (size_t i = 0; i < handles_.size(); i++) {
    kernel_state_->object_table()->RestoreHandle(handles_[i], this);
  }

  return true;
}

object_ref<XObject> XObject::Restore(KernelState* kernel_state, Type type,
                                     ByteStream* stream) {
  switch (type) {
    case Type::Enumerator:
      break;
    case Type::Event:
      return XEvent::Restore(kernel_state, stream);
    case Type::File:
      return XFile::Restore(kernel_state, stream);
    case Type::IOCompletion:
      break;
    case Type::Module:
      return XModule::Restore(kernel_state, stream);
    case Type::Mutant:
      return XMutant::Restore(kernel_state, stream);
    case Type::NotifyListener:
      return XNotifyListener::Restore(kernel_state, stream);
    case Type::Semaphore:
      return XSemaphore::Restore(kernel_state, stream);
    case Type::Session:
      break;
    case Type::Socket:
      break;
    case Type::SymbolicLink:
      return XSymbolicLink::Restore(kernel_state, stream);
    case Type::Thread:
      return XThread::Restore(kernel_state, stream);
    case Type::Timer:
      break;
    case Type::Undefined:
      break;
  }

  assert_always("No restore handler exists for this object!");
  return nullptr;
}

void XObject::SetAttributes(uint32_t obj_attributes_ptr) {
  if (!obj_attributes_ptr) {
    return;
  }

  auto name = util::TranslateAnsiStringAddress(
      memory(), xe::load_and_swap<uint32_t>(
                    memory()->TranslateVirtual(obj_attributes_ptr + 4)));
  if (!name.empty()) {
    name_ = std::string(name);
    kernel_state_->object_table()->AddNameMapping(name_, handles_[0]);
  }
}

uint32_t XObject::TimeoutTicksToMs(int64_t timeout_ticks) {
  if (timeout_ticks > 0) {
    // NetDll_WSAWaitForMultipleEvents provides timeout in form of MS.
    return (uint32_t)timeout_ticks;
  } else if (timeout_ticks < 0) {
    // Relative time.
    return (uint32_t)(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    return 0;
  }
}

X_STATUS XObject::Wait(uint32_t wait_reason, uint32_t processor_mode,
                       uint32_t alertable, uint64_t* opt_timeout) {
  auto wait_handle = GetWaitHandle();
  if (!wait_handle) {
    // Object doesn't support waiting.
    return X_STATUS_SUCCESS;
  }

  auto timeout_ms =
      opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                        TimeoutTicksToMs(*opt_timeout)))
                  : std::chrono::milliseconds::max();

  auto result =
      xe::threading::Wait(wait_handle, alertable ? true : false, timeout_ms);

  switch (result) {
    case xe::threading::WaitResult::kSuccess:
    case xe::threading::WaitResult::kUserCallback: {
      auto current_thread = XThread::GetCurrentThread();
      if (current_thread) {
        current_thread->BoostOnWake(priority_increment());
      }
      if (result == xe::threading::WaitResult::kSuccess) {
        WaitCallback();
        return X_STATUS_SUCCESS;
      }
      return X_STATUS_USER_APC;
    }
    case xe::threading::WaitResult::kTimeout:
      xe::threading::MaybeYield();
      return X_STATUS_TIMEOUT;
    default:
    case xe::threading::WaitResult::kAbandoned:
    case xe::threading::WaitResult::kFailed:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::SignalAndWait(XObject* signal_object, XObject* wait_object,
                                uint32_t wait_reason, uint32_t processor_mode,
                                uint32_t alertable, uint64_t* opt_timeout) {
  auto timeout_ms =
      opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                        TimeoutTicksToMs(*opt_timeout)))
                  : std::chrono::milliseconds::max();

  auto result = xe::threading::SignalAndWait(
      signal_object->GetWaitHandle(), wait_object->GetWaitHandle(),
      alertable ? true : false, timeout_ms);

  switch (result) {
    case xe::threading::WaitResult::kSuccess:
    case xe::threading::WaitResult::kUserCallback: {
      auto current_thread = XThread::GetCurrentThread();
      if (current_thread) {
        current_thread->BoostOnWake(wait_object->priority_increment());
      }
      if (result == xe::threading::WaitResult::kSuccess) {
        wait_object->WaitCallback();
        return X_STATUS_SUCCESS;
      }
      return X_STATUS_USER_APC;
    }
    case xe::threading::WaitResult::kTimeout:
      xe::threading::MaybeYield();
      return X_STATUS_TIMEOUT;
    default:
    case xe::threading::WaitResult::kAbandoned:
    case xe::threading::WaitResult::kFailed:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::WaitMultiple(uint32_t count, XObject** objects,
                               uint32_t wait_type, uint32_t wait_reason,
                               uint32_t processor_mode, uint32_t alertable,
                               uint64_t* opt_timeout) {
  xe::threading::WaitHandle* wait_handles[64];

  for (size_t i = 0; i < count; ++i) {
    wait_handles[i] = objects[i]->GetWaitHandle();
    if (!wait_handles[i]) {
      // Phase 1099z24: Sonic's first frames faulted in WaitAny on a null
      // host wait handle. DECLARED DEFENSIVE FALLBACK until the object is
      // identified: log it and fail the wait instead of crashing the host.
      static std::atomic<uint32_t> reported{0};
      if (++reported <= 10) {
        XELOGE("WaitMultiple: object {} of {} (type {}, handle {:08X}) has no "
               "wait handle; chain {}",
               i, count, static_cast<int>(objects[i]->type()),
               objects[i]->handle(),
               objects[i]->kernel_state()->GuestBackChain());
      }
      return X_STATUS_INVALID_HANDLE;
    }
  }

  auto timeout_ms =
      opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                        TimeoutTicksToMs(*opt_timeout)))
                  : std::chrono::milliseconds::max();

  X_STATUS status;
  uint32_t boost_increment = 0;
  if (wait_type) {
    auto result = xe::threading::WaitAny(wait_handles, count,
                                         alertable ? true : false, timeout_ms);
    switch (result.first) {
      case xe::threading::WaitResult::kSuccess:
        objects[result.second]->WaitCallback();
        boost_increment = objects[result.second]->priority_increment();
        status = X_STATUS(result.second);
        break;
      case xe::threading::WaitResult::kUserCallback:
        status = X_STATUS_USER_APC;
        break;
      case xe::threading::WaitResult::kTimeout:
        xe::threading::MaybeYield();
        status = X_STATUS_TIMEOUT;
        break;
      case xe::threading::WaitResult::kAbandoned:
        status = X_STATUS(X_STATUS_ABANDONED_WAIT_0 + result.second);
        break;
      default:
      case xe::threading::WaitResult::kFailed:
        status = X_STATUS_UNSUCCESSFUL;
        break;
    }
  } else {
    auto result = xe::threading::WaitAll(wait_handles, count,
                                         alertable ? true : false, timeout_ms);
    switch (result) {
      case xe::threading::WaitResult::kSuccess:
        for (uint32_t i = 0; i < count; i++) {
          objects[i]->WaitCallback();
          // Use the largest increment among the signaled objects.
          if (objects[i]->priority_increment() > boost_increment) {
            boost_increment = objects[i]->priority_increment();
          }
        }
        status = X_STATUS_SUCCESS;
        break;
      case xe::threading::WaitResult::kUserCallback:
        status = X_STATUS_USER_APC;
        break;
      case xe::threading::WaitResult::kTimeout:
        xe::threading::MaybeYield();
        status = X_STATUS_TIMEOUT;
        break;
      default:
      case xe::threading::WaitResult::kAbandoned:
      case xe::threading::WaitResult::kFailed:
        status = X_STATUS_ABANDONED_WAIT_0;
        break;
    }
  }

  // Apply priority boost if the thread actually blocked (not on
  // timeout/failure).
  if (status != X_STATUS_TIMEOUT && status != X_STATUS_UNSUCCESSFUL &&
      status != X_STATUS_ABANDONED_WAIT_0) {
    auto current_thread = XThread::GetCurrentThread();
    if (current_thread) {
      current_thread->BoostOnWake(boost_increment);
    }
  }
  return status;
}

uint8_t* XObject::CreateNative(uint32_t size) {
  auto global_lock = xe::global_critical_region::AcquireDirect();

  uint32_t total_size = size + sizeof(X_OBJECT_HEADER);

  auto mem = memory()->SystemHeapAlloc(total_size);
  if (!mem) {
    // Out of memory!
    return nullptr;
  }

  allocated_guest_object_ = true;
  memory()->Zero(mem, total_size);
  SetNativePointer(mem + sizeof(X_OBJECT_HEADER), true);

  auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(mem);

  auto object_type = memory()->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));
  if (object_type) {
    // Set it up in the header.
    // Some kernel method is accessing this struct and dereferencing a member
    // @ offset 0x14
    header->object_type_ptr = object_type;
  }

  return memory()->TranslateVirtual(guest_object_ptr_);
}

void XObject::SetNativePointer(uint32_t native_ptr, bool uninitialized) {
  auto global_lock = xe::global_critical_region::AcquireDirect();

  // If hit: We've already setup the native ptr with CreateNative!
  assert_zero(guest_object_ptr_);

  auto header =
      kernel_state_->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(native_ptr);

  // Memory uninitialized, so don't bother with the check.
  if (!uninitialized) {
    assert_true(!(header->wait_list.blink_ptr & 0x1));
  }

  // Stash pointer in struct.
  // FIXME: This assumes the object has a dispatch header (some don't!)
  StashHandle(header, handle());

  guest_object_ptr_ = native_ptr;
}

// Guest-address -> handle for adopted guest timers. See the timer case below
// for why these cannot be stashed in the object itself.
static std::unordered_map<uint32_t, uint32_t>& GuestTimerTable() {
  static std::unordered_map<uint32_t, uint32_t> table;
  return table;
}

object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state,
                                             void* native_ptr,
                                             X_OBJECT_TYPES as_type,
                                             bool already_locked) {
  assert_not_null(native_ptr);

  // A null or unmapped dispatch header used to fault a few lines below, while
  // the global critical region was held. Because that lock is taken with a
  // bare lock()/unlock() pair, the fault left it held for the life of the
  // process: Xenia's handler puts up a *modal* exception dialog on the
  // faulting thread, so it never reaches the unlock, and every later kernel
  // operation blocks behind it. The emulator looks like it froze twenty
  // seconds later, wherever the next lock acquisition happens to be.
  //
  // A scoped lock would not fix this. The build is /EHsc, under which an
  // access violation is an SEH exception that does not run C++ destructors.
  // The only fix is to not fault, so validate the pointer first.
  auto* mem = kernel_state->memory();
  uint32_t guest_ptr = native_ptr ? mem->HostToGuestVirtual(native_ptr) : 0;
  auto* ptr_heap = guest_ptr ? mem->LookupHeap(guest_ptr) : nullptr;
  // STOPGAP, and knowingly over-broad. QueryRangeAccess reports kNoAccess for
  // pages the wait shim then reads successfully (81D424A8, dispatch type 9),
  // so this rejects some objects that are readable. Narrowing it to
  // "guest_ptr != 0 && LookupHeap() != nullptr" was tried and brings the
  // access violation straight back, so something this rejects really does
  // fault on the header read. Until the exact bad pointer is characterised,
  // the over-broad test is the one that keeps the emulator alive: the objects
  // it wrongly rejects are dispatch types Xenia does not implement, which
  // resolve to nullptr a few lines below anyway, so the observable outcome
  // for them is unchanged.
  // Phase 1056: the stopgap above refuses xam's own task-pool semaphores and
  // timer (81D42450 / 81D424A8 / 81D424E4, dispatch types 5 and 9, live in
  // xam's image), because the image's pages report kNoAccess in the heap's
  // page table while being perfectly readable. Every wait on them then
  // returned INVALID_PARAMETER and xam's task workers spun instead of
  // running the tasks the Guide schedules. guide_dispatch_guard picks the
  // test; the refusal path logs the page's own state so the choice is made
  // from data, not from the protection bits alone.
  uint32_t pg_state = 0, pg_alloc = 0, pg_cur = 0, pg_base = 0, pg_pages = 0;
  const bool pg_ok = ptr_heap && ptr_heap->QueryPageEntry(guest_ptr, &pg_state, &pg_alloc,
                                                          &pg_cur, &pg_base, &pg_pages);
  bool mapped;
  switch (cvars::guide_dispatch_guard) {
    case 0:
      mapped = guest_ptr != 0 && ptr_heap &&
               ptr_heap->QueryRangeAccess(
                   guest_ptr, guest_ptr + sizeof(X_DISPATCH_HEADER) - 1) !=
                   xe::memory::PageAccess::kNoAccess;
      break;
    case 1:
      // the heap has this page allocated (state != free), whatever the
      // protection bits say. Useless for an LLE module, whose image the heap
      // never recorded.
      mapped = guest_ptr != 0 && pg_ok && pg_state != 0;
      break;
    default: {
      // Is the host page mapped? Cached per 64 KB page: only positive answers
      // are cached, so the cache can never turn a mapped page into a refusal.
      static std::mutex mp_mu;
      static std::unordered_map<uint32_t, bool> mp_ok;
      mapped = false;
      if (guest_ptr) {
        const uint32_t key = guest_ptr >> 16;
        {
          std::lock_guard<std::mutex> lk(mp_mu);
          auto it = mp_ok.find(key);
          if (it != mp_ok.end()) mapped = it->second;
        }
        if (!mapped) {
          size_t len = 0;
          auto access = xe::memory::PageAccess::kNoAccess;
          if (xe::memory::QueryProtect(native_ptr, len, access) &&
              access != xe::memory::PageAccess::kNoAccess && len != 0) {
            mapped = true;
            std::lock_guard<std::mutex> lk(mp_mu);
            mp_ok[key] = true;
          }
        }
      }
      // Phase 1073: xam's own dispatch objects live in the LLE image, whose
      // pages the heap never recorded (the refusal logs `state 0 alloc 0`) and
      // for which QueryProtect answers no. That refuses 81D42450 - the
      // semaphore xam's task workers wait on - on every wait, from guest
      // lr=8177A9F4, the instruction after their KeWaitForMultipleObjects. The
      // wait then returns INVALID_PARAMETER and the workers spin: 81.8 MILLION
      // refusals in a nine-second run (`resume6`). Accept an address that lies
      // inside the loaded xam image; the range comes from the module itself,
      // not from a hardcoded constant.
      if (!mapped && guest_ptr &&
          xboxkrnl::XamAddrInImage(guest_ptr, sizeof(X_DISPATCH_HEADER))) {
        mapped = true;
      }
      // Phase 1096hx: the same argument as Phase 1073, for the TITLE image.
      // dash builds its events INLINE, in C++ constructors - 92262200 does
      // "stb 1,0x14(r3)" and self-links the wait list - which is a legitimate
      // way to create a dispatcher header on the console, and it never calls
      // KeInitializeEvent, so nothing ever stashes Xenia's signature in it.
      // The nine objects dash waits on measure as
      //     01000000 00000000 <self+8> <self+8>
      // i.e. type 1 EventSynchronizationObject, unsignalled, empty wait list.
      // They are perfectly valid. But they live in the title image, whose
      // pages the heap never recorded and for which QueryProtect answers no,
      // so `mapped` stayed false and EVERY wait on them returned
      // ABANDONED_WAIT_0 without waiting - which is why dash's 500 ms poll
      // ran 335,000 times a second. Accept an address inside the loaded title
      // image, exactly as xam's image is accepted above, with the range taken
      // from the module rather than hardcoded.
      // Phase 1099z129: any loaded module's image, not only the executable's.
      // bootanim.xex is a DLL loaded before any title, and its event objects
      // (980590CC init-done, 98079068 audio) were refused the same way, so its
      // waits returned at once without waiting.
      if (!mapped && guest_ptr &&
          kernel_state->AddressInUserModuleImage(
              guest_ptr, sizeof(X_DISPATCH_HEADER))) {
        mapped = true;
      }
      // Phase 1095: the heap's own page table is the authority on whether a
      // guest page is readable; QueryProtect is not, and disagrees with it.
      // Every worker KTHREAD xam's task pool creates (3002A010, 3002E010,
      // 30032010, ... - as_type 6, and the header really does read 06000000,
      // a valid Thread dispatch header) reports `state 3 alloc 3 cur 3`, i.e.
      // Reserve|Commit with Read|Write, while QueryProtect answers no. The
      // factory at 8177AE78 creates every pool thread suspended (81778940
      // passes ExCreateThread creation_flags 0x82|1 - SystemThread |
      // ReturnKThreadPtr | Suspended, which is why [worker+0x1C] holds a
      // guest KTHREAD pointer and not a handle) and then starts it with
      // KeResumeThread([worker+0x1C]). Refusing that pointer made
      // KeResumeThread and KeSetBasePriorityThread return INVALID_HANDLE from
      // inside the factory (guest lr 8177AF74 / 8177AF80 / 8177B04C /
      // 81778924), so every task-pool worker stayed suspended for the whole
      // run. Trust the page table: committed and readable, both ends of the
      // header.
      if (!mapped && guest_ptr && cvars::guide_dispatch_guard >= 3 && pg_ok &&
          (pg_state & xe::kMemoryAllocationCommit) &&
          (pg_cur & xe::kMemoryProtectRead)) {
        uint32_t e_state = 0, e_cur = 0;
        const uint32_t last = guest_ptr + sizeof(X_DISPATCH_HEADER) - 1;
        if (ptr_heap->QueryPageEntry(last, &e_state, nullptr, &e_cur, nullptr,
                                     nullptr) &&
            (e_state & xe::kMemoryAllocationCommit) &&
            (e_cur & xe::kMemoryProtectRead)) {
          mapped = true;
        }
      }
      break;
    }
  }
  if (!mapped) {
    uint32_t caller_lr = 0;
    auto* cur_thread = XThread::GetCurrentThread();
    if (cur_thread && cur_thread->thread_state() &&
        cur_thread->thread_state()->context()) {
      caller_lr =
          static_cast<uint32_t>(cur_thread->thread_state()->context()->lr);
    }
    static std::atomic<uint32_t> refused{0};
    uint32_t rn = ++refused;
    if (rn <= 8 || (rn % 100000) == 0) {
      XELOGE(
          "GetNativeObject #{}: refusing {} dispatch header guest={:08X} "
          "host={} membase={} (as_type={}) from guest lr={:08X} | guard {} "
          "heap {:08X}+{:08X} page {} | entry {} state {} alloc {:X} cur {:X} "
          "base {:08X} pages {} | header {:08X}",
          rn, guest_ptr ? "unmapped" : "null", guest_ptr,
          fmt::ptr(native_ptr), fmt::ptr(mem->virtual_membase()),
          static_cast<uint32_t>(as_type), caller_lr,
          int(cvars::guide_dispatch_guard),
          ptr_heap ? ptr_heap->heap_base() : 0,
          ptr_heap ? ptr_heap->heap_size() : 0,
          ptr_heap ? ptr_heap->page_size() : 0,
          pg_ok ? "yes" : "no", pg_state, pg_alloc, pg_cur, pg_base, pg_pages,
          guest_ptr ? xe::load_and_swap<uint32_t>(mem->TranslateVirtual(guest_ptr)) : 0u);
    }
    return object_ref<XObject>(nullptr);
  }

  // Unfortunately the XDK seems to inline some KeInitialize calls, meaning
  // we never see it and just randomly start getting passed events/timers/etc.
  // Luckily it seems like all other calls (Set/Reset/Wait/etc) are used and
  // we don't have to worry about PPC code poking the struct. Because of that,
  // we init on first use, store our handle in the struct, and dereference it
  // each time.
  // We identify this by setting wait_list.flink_ptr to a magic value. When set,
  // wait_list.blink_ptr will hold a handle to our object.
  if (!already_locked) {
    global_critical_region::mutex().lock();
  }

  XObject* result = nullptr;

  auto header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);
  X_OBJECT_TYPES type = as_type;

  if (as_type == X_OBJECT_TYPES::UndefinedObject) {
    type = header->type;
  }

  if (header->wait_list.flink_ptr == kXObjSignature) {
    // Already initialized.
    uint32_t handle = header->wait_list.blink_ptr;
    result = kernel_state->object_table()
                 ->LookupObject<XObject>(handle, true)
                 .release();
    // Phase 1095ao: the TODO that used to sit here - "assert if the type
    // of the object != as_type" - is a real hazard, not a nicety. The
    // signature and handle live in header->wait_list, which the GUEST
    // also owns and writes (the timer case a few hundred lines below
    // avoids this mechanism for exactly that reason). A stale or
    // guest-overwritten blink_ptr that still resolves in the object
    // table hands back a VALID object of the WRONG type, and the
    // templated caller then downcasts it - GetNativeObject<XEvent> ->
    // ev->Set() - and calls an XEvent method on something that is not
    // one. That is the "non-null but invalid" event behind all four
    // host faults in the timers path (symbolicated: xeKeSetEvent
    // xboxkrnl_threading.cc:619, fault_addr FFFFFFFFFFFFFFFF).
    // Map only the guest dispatch types this function actually
    // constructs; anything else is left alone.
    XObject::Type expect = XObject::Type::Undefined;
    switch (type) {
      case X_OBJECT_TYPES::EventNotificationObject:
      case X_OBJECT_TYPES::EventSynchronizationObject:
        expect = XObject::Type::Event;
        break;
      case X_OBJECT_TYPES::MutantObject:
        expect = XObject::Type::Mutant;
        break;
      case X_OBJECT_TYPES::SemaphoreObject:
        expect = XObject::Type::Semaphore;
        break;
      case X_OBJECT_TYPES::TimerNotificationObject:
      case X_OBJECT_TYPES::TimerSynchronizationObject:
        expect = XObject::Type::Timer;
        break;
      default:
        break;
    }
    if (result && expect != XObject::Type::Undefined &&
        result->type() != expect) {
      static std::atomic<uint32_t> mism{0};
      const uint32_t mn = ++mism;
      if (mn <= 8u) {
        XELOGE(
            "GetNativeObject: stashed handle {:08X} in header {:08X} "
            "resolves to host type {} but the caller asked for guest "
            "type {} - refusing the stale association",
            handle, kernel_state->memory()->HostToGuestVirtual(native_ptr),
            static_cast<uint32_t>(result->type()),
            static_cast<uint32_t>(type));
      }
      result->Release();
      result = nullptr;
    }
  } else {
    // First use, create new.
    // https://www.nirsoft.net/kernel_struct/vista/KOBJECTS.html
    switch (type) {
      case X_OBJECT_TYPES::EventNotificationObject:
      case X_OBJECT_TYPES::EventSynchronizationObject: {
        auto ev = new XEvent(kernel_state);
        ev->InitializeNative(native_ptr, header);
        result = ev;
      } break;
      case X_OBJECT_TYPES::MutantObject: {
        auto mutant = new XMutant(kernel_state);
        mutant->InitializeNative(native_ptr, header);
        result = mutant;
      } break;
      case X_OBJECT_TYPES::SemaphoreObject: {
        auto sem = new XSemaphore(kernel_state);
        auto success = sem->InitializeNative(native_ptr, header);
        // Can't report failure to the guest at late initialization:
        assert_true(success);
        result = sem;
      } break;
      case X_OBJECT_TYPES::TimerNotificationObject:
      case X_OBJECT_TYPES::TimerSynchronizationObject: {
        if (!cvars::guest_native_timers && !cvars::kernel_guest_timers) {
          result = nullptr;
          break;
        }
        // Timers cannot use the StashHandle association below: the guest
        // initialises header.wait_list itself and then walks and unlinks
        // entries from it, so overwriting that field with a signature and a
        // handle corrupts a list the guest is using. Keep the association in a
        // side table keyed by guest address and leave the structure alone.
        uint32_t guest_addr =
            kernel_state->memory()->HostToGuestVirtual(native_ptr);
        auto& table = GuestTimerTable();
        auto it = table.find(guest_addr);
        if (it != table.end()) {
          auto existing = kernel_state->object_table()
                              ->LookupObject<XObject>(it->second, true);
          if (existing) {
            result = existing.release();
            break;
          }
          table.erase(it);
        }
        auto timer = new XTimer(kernel_state);
        timer->InitializeNative(native_ptr, header);
        table[guest_addr] = timer->handle();
        // Retain: the table holds a reference for the lifetime of the guest
        // object, and the caller gets its own via object_ref below.
        timer->Retain();
        result = timer;
      } break;
      case X_OBJECT_TYPES::ProcessObject:
      case X_OBJECT_TYPES::QueueObject:
      case X_OBJECT_TYPES::ThreadObject:
      case X_OBJECT_TYPES::Spare1Object:
      case X_OBJECT_TYPES::ApcObject:
      case X_OBJECT_TYPES::DpcObject:
      case X_OBJECT_TYPES::DeviceQueueObject:
      case X_OBJECT_TYPES::EventPairObject:
      case X_OBJECT_TYPES::InterruptObject:
      case X_OBJECT_TYPES::ProfileObject:
      default:
        // Only Event, Mutant and Semaphore are wrapped above; every other
        // dispatcher type lands here and the caller sees a null, which for
        // KeWaitForMultipleObjects becomes STATUS_INVALID_PARAMETER. Guest
        // modules that create their own timers or queues - real xam does -
        // then fail every wait, with assert_always() compiled out in release
        // so nothing is reported. Log it once per type.
        {
          static std::atomic<uint32_t> seen_mask{0};
          uint32_t bit = 1u << (static_cast<uint32_t>(type) & 31);
          if (!(seen_mask.fetch_or(bit) & bit)) {
            XELOGE("GetNativeObject: unsupported dispatcher type {} at {:08X}",
                   static_cast<uint32_t>(type),
                   kernel_state->memory()->HostToGuestVirtual(native_ptr));
          }
        }
        assert_always();
        result = nullptr;
    }
    // Stash pointer in struct.
    // FIXME: This assumes the object contains a dispatch header (some don't!)
    if (result && type != X_OBJECT_TYPES::TimerNotificationObject &&
        type != X_OBJECT_TYPES::TimerSynchronizationObject) {
      StashHandle(header, result->handle());
    }
    // Phase 1099h: and record WHICH guest object the handle refers to.
    //
    // Wrapping a dispatcher the GUEST built (InitializeNative) set up the host
    // side and stashed the handle, but never set guest_object_ptr_ - so
    // guest_object() stayed 0 for every such object. ObReferenceObjectByHandle
    // returns that as the referenced pointer, so the guest was handed a NULL
    // and stored through it: the Guide's SECOND open crashed at
    //     817A6174  stw r31, 0x18(r11)      r11 = [sp+0x54], fault 100000018
    // whose caller (817C2D68 -> ObReferenceObjectByHandle) does not check the
    // status, because on hardware the call cannot come back with a null.
    // Measured: handle F80005C0, type 2 (Event), guest_object 0, while every
    // other reference at that site had a real pointer.
    //
    // set_guest_object_no_stash, not SetNativePointer: the handle is already
    // stashed above where a dispatch header exists, and objects the guest
    // builds through ObCreateObject need not have one.
    if (result && !result->guest_object()) {
      result->set_guest_object_no_stash(
          kernel_state->memory()->HostToGuestVirtual(native_ptr));
    }
  }

  if (!already_locked) {
    global_critical_region::mutex().unlock();
  }
  return object_ref<XObject>(result);
}

}  // namespace kernel
}  // namespace xe
