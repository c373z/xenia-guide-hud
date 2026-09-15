/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xevent.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"

namespace xe {
namespace kernel {

XEvent::XEvent(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XEvent::~XEvent() = default;

void XEvent::Initialize(bool manual_reset, bool initial_state) {
  assert_false(event_);

  CreateNative<X_KEVENT>();

  if (manual_reset) {
    event_ = xe::threading::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = xe::threading::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
}

void XEvent::InitializeNative(void* native_ptr,
                              const X_DISPATCH_HEADER* header) {
  assert_false(event_);

  switch (header->type) {
    case X_OBJECT_TYPES::EventNotificationObject:
      manual_reset_ = true;
      break;
    case X_OBJECT_TYPES::EventSynchronizationObject:
      manual_reset_ = false;
      break;
    default:
      assert_always();
      return;
  }

  bool initial_state = header->signal_state ? true : false;
  if (manual_reset_) {
    event_ = xe::threading::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = xe::threading::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
}

// Phase 1095am: InitializeNative's `default: assert_always(); return;`
// leaves event_ NULL in release, where the assert is compiled out. Set,
// Pulse and Reset then dereferenced it and took the whole emulator down
// with a host access violation - symbolicated from xenia_canary.pdb as
// XEvent::Set <- xeKeSetEvent <- the KeSetEvent trampoline, fault_addr
// FFFFFFFFFFFFFFFF, on xam's task-pool worker threads.
//
// An uninitialised event cannot be signalled, so say so and let the
// caller carry on. This does not paper over the cause (something is
// handing us a dispatch header whose type is neither
// EventNotificationObject nor EventSynchronizationObject) - it stops
// that cause from being fatal, and logs it once per event so it stays
// visible.
bool XEvent::WarnUninitialised(const char* op) {
  if (event_) return false;
  if (!warned_uninitialised_) {
    warned_uninitialised_ = true;
    XELOGE("XEvent::{}: event is UNINITIALISED (guest object {:08X}) - "
           "InitializeNative bailed on an unsupported dispatch type. "
           "Ignoring instead of faulting.",
           op, guest_object());
  }
  return true;
}

int32_t XEvent::Set(uint32_t priority_increment, bool wait) {
  set_priority_increment(priority_increment);
  if (WarnUninitialised("Set")) return 0;
  event_->Set();
  return 1;
}

int32_t XEvent::Pulse(uint32_t priority_increment, bool wait) {
  set_priority_increment(priority_increment);
  if (WarnUninitialised("Pulse")) return 0;
  event_->Pulse();
  return 1;
}

int32_t XEvent::Reset() {
  if (WarnUninitialised("Reset")) return 0;
  event_->Reset();
  return 1;
}
void XEvent::Query(uint32_t* out_type, uint32_t* out_state) {
  auto [type, state] = event_->Query();

  *out_type = type;
  *out_state = state;
}
void XEvent::Clear() { event_->Reset(); }

bool XEvent::Save(ByteStream* stream) {
  XELOGD("XEvent {:08X} ({})", handle(), manual_reset_ ? "manual" : "auto");
  SaveObject(stream);

  bool signaled = true;
  auto result =
      xe::threading::Wait(event_.get(), false, std::chrono::milliseconds(0));
  if (result == xe::threading::WaitResult::kSuccess) {
    signaled = true;
  } else if (result == xe::threading::WaitResult::kTimeout) {
    signaled = false;
  } else {
    assert_always();
  }

  if (signaled) {
    // Reset the event in-case it's an auto-reset.
    event_->Set();
  }

  stream->Write<bool>(signaled);
  stream->Write<bool>(manual_reset_);

  return true;
}

object_ref<XEvent> XEvent::Restore(KernelState* kernel_state,
                                   ByteStream* stream) {
  auto evt = new XEvent(nullptr);
  evt->kernel_state_ = kernel_state;

  evt->RestoreObject(stream);
  bool signaled = stream->Read<bool>();
  evt->manual_reset_ = stream->Read<bool>();

  if (evt->manual_reset_) {
    evt->event_ = xe::threading::Event::CreateManualResetEvent(false);
  } else {
    evt->event_ = xe::threading::Event::CreateAutoResetEvent(false);
  }
  assert_not_null(evt->event_);

  if (signaled) {
    evt->event_->Set();
  }

  return object_ref<XEvent>(evt);
}

}  // namespace kernel
}  // namespace xe
