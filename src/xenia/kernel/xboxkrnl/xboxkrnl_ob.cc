/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <unordered_map>

#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

#include <atomic>

namespace xe {
namespace kernel {
namespace xboxkrnl {

void xeObSplitName(X_ANSI_STRING input_string,
                   X_ANSI_STRING* leading_path_component,
                   X_ANSI_STRING* remaining_path_components,
                   PPCContext* context) {
  xe::FatalError("xeObSplitName unimplemented!");
}

uint32_t xeObHashObjectName(X_ANSI_STRING* ElementName, PPCContext* context) {
  uint8_t* current_character_ptr =
      context->TranslateVirtual(ElementName->pointer);
  uint32_t result = 0;
  uint8_t* name_span_end = &current_character_ptr[ElementName->length];
  while (current_character_ptr < name_span_end) {
    uint32_t current_character = *current_character_ptr++;
    if (current_character < 0x80) {
      result = (current_character | 0x20) + (result >> 1) + 3 * result;
    }
  }
  return result % 0xD;
}

uint32_t xeObCreateObject(X_OBJECT_TYPE* object_factory,
                          X_OBJECT_ATTRIBUTES* optional_attributes,
                          uint32_t object_size_without_headers,
                          uint32_t* out_object, cpu::ppc::PPCContext* context) {
  unsigned int resulting_header_flags = 0;
  *out_object = 0;
  unsigned int poolarg = 0;

  auto get_flags_and_poolarg_for_process_type = [&resulting_header_flags,
                                                 &poolarg, context]() {
    uint32_t process_type = xboxkrnl::xeKeGetCurrentProcessType(context);
    if (process_type == X_PROCTYPE_TITLE) {
      poolarg = 1;
      resulting_header_flags = OBJECT_HEADER_IS_TITLE_OBJECT;
    } else {
      poolarg = 2;
    }
  };
  if (!optional_attributes) {
    get_flags_and_poolarg_for_process_type();
  }

  else if ((optional_attributes->attributes & 0x1000) == 0) {
    if ((optional_attributes->attributes & 0x2000) != 0) {
      poolarg = 2;
    } else {
      get_flags_and_poolarg_for_process_type();
    }
  } else {
    poolarg = 1;
    resulting_header_flags = OBJECT_HEADER_IS_TITLE_OBJECT;
  }
  uint32_t desired_object_path_ptr;

  if (!optional_attributes ||
      (desired_object_path_ptr = optional_attributes->name_ptr) == 0) {
    /*
        object has no name provided, just allocate an object with a basic header
    */
    uint64_t allocate_args[] = {
        object_size_without_headers + sizeof(X_OBJECT_HEADER),
        object_factory->pool_tag, poolarg};
    context->processor->Execute(
        context->thread_state, object_factory->allocate_proc, allocate_args, 3);

    uint32_t allocation = static_cast<uint32_t>(context->r[3]);
    if (allocation) {
      X_OBJECT_HEADER* new_object_header =
          context->TranslateVirtual<X_OBJECT_HEADER*>(allocation);
      new_object_header->pointer_count = 1;
      new_object_header->handle_count = 0;
      new_object_header->object_type_ptr =
          context->HostToGuestVirtual(object_factory);
      new_object_header->flags = resulting_header_flags;

      *out_object = allocation + sizeof(X_OBJECT_HEADER);
      return X_STATUS_SUCCESS;
    }
    return X_STATUS_INSUFFICIENT_RESOURCES;
  }
  /*
    iterate through all path components until we obtain the final one, which is
    the objects actual name
  */
  X_ANSI_STRING trailing_path_component;
  X_ANSI_STRING remaining_path;
  X_ANSI_STRING loaded_object_name;
  loaded_object_name =
      *context->TranslateVirtual<X_ANSI_STRING*>(desired_object_path_ptr);
  trailing_path_component.pointer = 0;
  trailing_path_component.length = 0;
  remaining_path = loaded_object_name;
  while (remaining_path.length) {
    xeObSplitName(remaining_path, &trailing_path_component, &remaining_path,
                  context);
    if (remaining_path.length) {
      if (*context->TranslateVirtual<char*>(remaining_path.pointer) == '\\') {
        return X_STATUS_OBJECT_NAME_INVALID;
      }
    }
  }
  if (!trailing_path_component.length) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }
  // the object and its name are all created in a single allocation

  unsigned int aligned_object_size =
      xe::align<uint32_t>(object_size_without_headers, 4);
  {
    uint64_t allocate_args[] = {
        trailing_path_component.length + aligned_object_size +
            sizeof(X_OBJECT_HEADER_NAME_INFO) + sizeof(X_OBJECT_HEADER),
        object_factory->pool_tag, poolarg};

    context->processor->Execute(
        context->thread_state, object_factory->allocate_proc, allocate_args, 3);
  }
  uint32_t named_object_allocation = static_cast<uint32_t>(context->r[3]);
  if (!named_object_allocation) {
    return X_STATUS_INSUFFICIENT_RESOURCES;
  }

  X_OBJECT_HEADER_NAME_INFO* nameinfo =
      context->TranslateVirtual<X_OBJECT_HEADER_NAME_INFO*>(
          named_object_allocation);
  nameinfo->next_in_directory = 0;
  nameinfo->object_directory = 0;

  X_OBJECT_HEADER* header_for_named_object =
      reinterpret_cast<X_OBJECT_HEADER*>(nameinfo + 1);

  char* name_string_memory_for_named_object = &reinterpret_cast<char*>(
      header_for_named_object + 1)[aligned_object_size];
  nameinfo->name.pointer =
      context->HostToGuestVirtual(name_string_memory_for_named_object);
  nameinfo->name.length = trailing_path_component.length;
  nameinfo->name.maximum_length = trailing_path_component.length;

  memcpy(name_string_memory_for_named_object,
         context->TranslateVirtual<char*>(trailing_path_component.pointer),
         trailing_path_component.length);

  header_for_named_object->pointer_count = 1;
  header_for_named_object->handle_count = 0;
  header_for_named_object->object_type_ptr =
      context->HostToGuestVirtual(object_factory);
  header_for_named_object->flags =
      resulting_header_flags & 0xFFFE | OBJECT_HEADER_FLAG_NAMED_OBJECT;
  *out_object = context->HostToGuestVirtual(&header_for_named_object[1]);
  return X_STATUS_SUCCESS;
}
dword_result_t ObOpenObjectByName_entry(lpunknown_t obj_attributes_ptr,
                                        lpunknown_t object_type_ptr,
                                        dword_t unk, lpdword_t handle_ptr) {
  // r3 = ptr to info?
  //   +0 = -4
  //   +4 = name ptr
  //   +8 = 0
  // r4 = ExEventObjectType | ExSemaphoreObjectType | ExTimerObjectType
  // r5 = 0
  // r6 = out_ptr (handle?)

  if (!obj_attributes_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  auto obj_attributes = kernel_memory()->TranslateVirtual<X_OBJECT_ATTRIBUTES*>(
      obj_attributes_ptr);
  assert_true(obj_attributes->name_ptr != 0);
  auto name = util::TranslateAnsiStringAddress(kernel_memory(),
                                               obj_attributes->name_ptr);

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  X_STATUS result =
      kernel_state()->object_table()->GetObjectByName(name, &handle);
  if (XSUCCEEDED(result)) {
    *handle_ptr = handle;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ObOpenObjectByName, kNone, kImplemented);
// chrispy: investigate this, pretty certain it does not properly emulate the
// original
dword_result_t ObOpenObjectByPointer_entry(lpvoid_t object_ptr,
                                           lpdword_t out_handle_ptr) {
  *out_handle_ptr = 0;
  auto object = XObject::GetNativeObject<XObject>(kernel_state(), object_ptr);
  if (!object) {
    return X_STATUS_UNSUCCESSFUL;
  }

  // Retain the handle. Will be released in NtClose.
  object->RetainHandle();
  *out_handle_ptr = object->handle();
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObOpenObjectByPointer, kNone, kImplemented);

dword_result_t ObLookupThreadByThreadId_entry(dword_t thread_id,
                                              lpdword_t out_object_ptr) {
  auto thread = kernel_state()->GetThreadByID(thread_id);
  if (!thread) {
    *out_object_ptr = 0;
    return X_STATUS_INVALID_PARAMETER;
  }

  // Retain the object. Will be released in ObDereferenceObject.
  thread->RetainHandle();
  *out_object_ptr = thread->guest_object();
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObLookupThreadByThreadId, kNone, kImplemented);

dword_result_t ObLookupAnyThreadByThreadId_entry(dword_t thread_id,
                                                 lpdword_t out_object_ptr) {
  return ObLookupThreadByThreadId_entry(thread_id, out_object_ptr);
}
DECLARE_XBOXKRNL_EXPORT1(ObLookupAnyThreadByThreadId, kNone, kImplemented);

dword_result_t ObReferenceObjectByHandle_entry(dword_t handle,
                                               dword_t object_type_ptr,
                                               lpdword_t out_object_ptr) {
  // chrispy: gotta preinit this to 0, kernel is expected to do that
  *out_object_ptr = 0;

  auto object = kernel_state()->object_table()->LookupObject<XObject>(handle);
  // Phase 1095ar: xam's pool append at 8177A61C (return address
  // 8177A620) takes the handle from [r30+0xC] and only writes the wait
  // slot on SUCCESS - its failure guard is a `twui` Xenia does not
  // honour, so a failure here leaves slot 7 of the pool's 8-object wait
  // unwritten while the count still counts it, and the workers spin
  // millions of times on a malformed array (1095ap/aq). Report what this
  // one call site is asked for and whether the handle exists at all.
  {
    auto* th = XThread::GetCurrentThread();
    auto* ctx = (th && th->thread_state()) ? th->thread_state()->context()
                                           : nullptr;
    if (ctx && static_cast<uint32_t>(ctx->lr) == 0x8177A620u) {
      static std::atomic<uint32_t> ar{0};
      const uint32_t i = ++ar;
      if (i <= 12u) {
        XELOGI("GuidePoolAppendRef #{}: handle {:08X} -> {} | r30 {:08X} "
               "tid {:08X}",
               i, uint32_t(handle),
               object ? "FOUND" : "NOT IN OBJECT TABLE",
               static_cast<uint32_t>(ctx->r[30]),
               th ? th->thread_id() : 0u);
      }
    }
  }
  // Phase 1099h: the Guide's SECOND open crashes at 817A6174 storing through a
  // null out-param that 817C2D68 was supposed to fill. 817C2D68 is a wrapper
  // around exactly this function (bl at 817C2D80, so lr 817C2D84), and its
  // caller dereferences the result WITHOUT checking - which means on hardware
  // it always succeeds. Report the handle and whether it resolves.
  {
    auto* th2 = XThread::GetCurrentThread();
    auto* ctx2 = (th2 && th2->thread_state()) ? th2->thread_state()->context()
                                              : nullptr;
    if (ctx2 && static_cast<uint32_t>(ctx2->lr) == 0x817C2D84u) {
      static std::atomic<uint32_t> gr{0}, gok{0};
      const uint32_t i2 = ++gr;
      if (!object) {
        // Uncapped: a FAILURE here is the thing that produces the null the
        // caller stores through. Successes are only counted.
        XELOGE("GuideObRef FAIL #{}: handle {:08X} type {:08X} | tid {:08X} "
               "({} ok before this)",
               i2, uint32_t(handle), uint32_t(object_type_ptr),
               th2 ? th2->thread_id() : 0u, gok.load());
      } else {
        // What matters is not whether the handle resolves but what the guest
        // is handed: guest_object() == 0 is the null the caller stores
        // through at 817A6174.
        const uint32_t go = object->guest_object();
        if (!go) {
          XELOGE("GuideObRef NULL guest_object: handle {:08X} type {} | the "
                 "caller will store through null at 817A6174",
                 uint32_t(handle), uint32_t(object->type()));
        } else if (++gok <= 4u) {
          XELOGI("GuideObRef ok #{}: handle {:08X} type {} guest_object {:08X}",
                 i2, uint32_t(handle), uint32_t(object->type()), go);
        }
      }
    }
  }
  if (!object) {
    return X_STATUS_INVALID_HANDLE;
  }

  uint32_t native_ptr = object->guest_object();

  if (object_type_ptr) {
    auto& object_types =
        kernel_state()->host_object_type_enum_to_guest_object_type_ptr_;

    if (object_types.contains(object->type())) {
      if (object_type_ptr != object_types[object->type()]) {
        // Phase 1099h: only a KERNEL object-type pointer can be checked
        // against a kernel object-type pointer.
        //
        // The Guide's second open crashed at 817A6174 storing through a null.
        // The null came from HERE: the early return below leaves
        // *out_object_ptr at the 0 set at the top of the function, and
        // 817C2D68's caller does not check the status - on hardware this call
        // cannot come back with a null. Measured:
        //     handle F80005C0, object type 2 (Event)
        //     guest passed 81D25CE0   <- inside XAM'S OWN IMAGE
        //     kernel expects 3000001C <- Xenia's allocated type object
        // Those can never be equal. xam is passing its own object-type
        // descriptor, which is exactly what the unmapped-type branch below
        // already says must not fail the call ("a kernel-side type check has
        // nothing valid to compare against").
        //
        // So enforce the check only when the guest actually handed us one of
        // the kernel's own type pointers - a real, checkable mismatch - and
        // let a module's private descriptor through.
        bool passed_a_kernel_type = false;
        for (const auto& kv : object_types) {
          if (kv.second == uint32_t(object_type_ptr)) {
            passed_a_kernel_type = true;
            break;
          }
        }
        if (passed_a_kernel_type) {
          return X_STATUS_OBJECT_TYPE_MISMATCH;
        }
        static std::atomic<uint32_t> mm{0};
        if (++mm <= 8u) {
          XELOGW("ObReferenceObjectByHandle: handle {:08X} type {} was asked "
                 "for with {:08X}, which is not a kernel object type - "
                 "treating it as a module's own descriptor and allowing it",
                 uint32_t(handle), uint32_t(object->type()),
                 uint32_t(object_type_ptr));
        }
      }
    } else {
      // Phase 1095ae: Xenia hands the guest 0xDEADF00D here and still
      // returns SUCCESS. That poison pointer is real - it is where
      // r30=DEADF00D and the fault at 8176738C came from - but
      // returning X_STATUS_OBJECT_TYPE_MISMATCH instead REGRESSED THE
      // DEFAULT PATH HARD (p1095reg14: 0 paints, crash at 817A6174,
      // against 431 paints and no crash before). The unmapped type is
      // 7 = NotifyListener, which xam references legitimately, and the
      // type pointer it passes (81D22460) is xam's OWN object-type
      // descriptor, not a kernel one - so a kernel-side type check has
      // nothing valid to compare against and must not fail the call.
      // Left as-is until the right answer is measured; see FINDINGS
      // 1095ae for what NOT to do.
      assert_unhandled_case(object->type());
      // Phase 1095af: DO NOT CHANGE THIS VALUE. 0xDEADF00D is a
      // deliberate sentinel (xeObDereferenceObject tests for it and
      // returns early) and it is LOAD-BEARING. Two alternatives were
      // measured and BOTH regressed the Guide from 433 paints to 0 with
      // a crash at 817A6174:
      //   return X_STATUS_OBJECT_TYPE_MISMATCH   (1095ae, p1095reg14)
      //   native_ptr = 0, still SUCCESS          (1095af, p1095reg16)
      // So a caller round-trips this token opaquely back to
      // ObDereferenceObject and only the sentinel survives that trip.
      // The real problem is narrower than it looks: ONE caller path
      // (xam 817679B0 -> [r30+0x24] list walk) dereferences it. Fix that
      // by giving NotifyListener a real guest object, not by changing
      // the sentinel.
      // Log only - the VALUE must not change (see above).
      {
        static std::atomic<uint32_t> n{0};
        const uint32_t i = ++n;
        if (i <= 12u) {
          XELOGW(
              "ObReferenceObjectByHandle: unmapped object type {} ({}) for "
              "handle {:08X} guest_object {:08X} - returning the guest "
              "object when it has one, else the 0xDEADF00D sentinel",
              static_cast<uint32_t>(object->type()),
              [&]() -> const char* {
                switch (object->type()) {
                  case XObject::Type::Undefined: return "Undefined";
                  case XObject::Type::Enumerator: return "Enumerator";
                  case XObject::Type::Event: return "Event";
                  case XObject::Type::File: return "File";
                  case XObject::Type::IOCompletion:
                    return "IOCompletion";
                  case XObject::Type::Module: return "Module";
                  case XObject::Type::Mutant: return "Mutant";
                  case XObject::Type::NotifyListener:
                    return "NotifyListener";
                  case XObject::Type::Semaphore: return "Semaphore";
                  case XObject::Type::Session: return "Session";
                  case XObject::Type::Socket: return "Socket";
                  case XObject::Type::SymbolicLink:
                    return "SymbolicLink";
                  case XObject::Type::Thread: return "Thread";
                  case XObject::Type::Timer: return "Timer";
                  case XObject::Type::Device: return "Device";
                  default: return "?";
                }
              }(),
              uint32_t(handle), object->guest_object());
        }
      }
      // Phase 1095aj: only fall back to the sentinel when there is NO
      // guest object to hand over. These objects usually HAVE one -
      // measured: type 7 (NotifyListener) handles F80001A8 and F80001E4
      // carry guest_object 30140018 and 401B76B0 - and native_ptr was
      // already set to it a few lines above, only to be thrown away
      // here. Discarding a valid pointer is what fed real xam the
      // sentinel it then dereferenced at 817679C8 (crash 8176738C).
      // Keep the sentinel ONLY for objects with no guest side, so the
      // round-trip callers that depend on it (1095af: changing it
      // unconditionally took the Guide from 433 paints to 0) are
      // untouched.
      if (!native_ptr) {
        native_ptr = 0xDEADF00D;
      }
    }
  }

  // Caller takes the reference.
  // It's released in ObDereferenceObject.
  object->RetainHandle();

  assert_not_zero(native_ptr);

  if (out_object_ptr.guest_address()) {
    *out_object_ptr = native_ptr;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObReferenceObjectByHandle, kNone, kImplemented);

uint32_t GuideCdRomDeviceObject();  // xboxkrnl_io.cc

dword_result_t ObReferenceObjectByName_entry(pointer_t<X_ANSI_STRING> name,
                                             dword_t attributes,
                                             dword_t object_type_ptr,
                                             lpvoid_t parse_context,
                                             lpdword_t out_object_ptr,
                                             const ppc_context_t& ctx) {
  X_HANDLE handle = X_INVALID_HANDLE_VALUE;

  char* name_str = ctx.TranslateVirtual<char*>(name->pointer);
  // Phase 1099z16: the optical drive's device object (see xboxkrnl_io.cc).
  if (name_str && utf8::equal_case(std::string_view(name_str, name->length),
                                   "\\Device\\CdRom0")) {
    if (out_object_ptr) {
      *out_object_ptr = GuideCdRomDeviceObject();
    }
    return X_STATUS_SUCCESS;
  }
  X_STATUS result =
      kernel_state()->object_table()->GetObjectByName(name_str, &handle);
  // Phase 1096gt: xam polls this in a 90-second busy loop after an
  // IoDismountVolumeByName (8176CD60, called from 8176D63C ~4.9M times in a
  // 100 s run) and leaves the loop only on 0xC000000E / STATUS_NO_SUCH_DEVICE.
  // Xenia returns X_STATUS_OBJECT_NAME_NOT_FOUND (0xC0000034) for any name it
  // does not hold, so the loop never ends. Log the name before deciding what
  // this should return - the answer depends on whether it is a device path.
  if (!XSUCCEEDED(result)) {
    static std::unordered_map<std::string, uint64_t> missing_seen;
    auto miss_lock = xe::global_critical_region::AcquireDirect();
    const std::string key = name_str ? name_str : "<null>";
    const uint64_t n = ++missing_seen[key];
    // MEASURED: the name is '\Device\HdDvdRom'. The HD-DVD drive was an
    // optional accessory and is not attached, and both dash (lr 9226784C, ~26M
    // calls in 100 s) and xam's dismount wait (lr 8176CDB8) poll for it. The
    // object manager resolves the \Device directory and then fails in the
    // device parse, which yields STATUS_NO_SUCH_DEVICE - not the generic
    // OBJECT_NAME_NOT_FOUND that a name missing from an ordinary directory
    // gets. xam's loop at 8176CD60/8176D63C leaves only on 0xC000000E, so
    // returning the generic status spins it for its full 90-second timeout.
    // Report a missing device path as a missing device.
    if (key.rfind(R"(\Device\)", 0) == 0) {
      result = 0xC000000E;  // STATUS_NO_SUCH_DEVICE
    }
    if (n <= 3 || (n % 500000) == 0) {
      // Phase 1096gw: lr alone named 9226784C, which turned out to be a leaf
      // helper in dash with no loop in it - the caller repeating the call is a
      // frame further up and there is no dumped dash image to disassemble.
      // Walk the PPC back chain the same way the guest crash handler does:
      // [r1] is the caller's sp and these prologues save LR at [caller_sp - 8].
      std::string chain;
      auto* mm = kernel_state() ? kernel_state()->memory() : nullptr;
      uint32_t cur = static_cast<uint32_t>(ctx->r[1]);
      for (int f = 0; mm && f < 6 && cur; ++f) {
        auto* hp = mm->LookupHeap(cur);
        if (!hp || hp->QueryRangeAccess(cur, cur + 3) ==
                       xe::memory::PageAccess::kNoAccess) {
          break;
        }
        uint32_t caller_sp =
            xe::load_and_swap<uint32_t>(mm->TranslateVirtual(cur));
        if (caller_sp <= cur || caller_sp - cur > 0x10000 || caller_sp < 8) {
          break;
        }
        auto* hp2 = mm->LookupHeap(caller_sp - 8);
        if (!hp2 || hp2->QueryRangeAccess(caller_sp - 8, caller_sp - 5) ==
                        xe::memory::PageAccess::kNoAccess) {
          break;
        }
        uint32_t ra =
            xe::load_and_swap<uint32_t>(mm->TranslateVirtual(caller_sp - 8));
        if (ra < 0x81000000u || ra >= 0x94000000u) break;
        chain += fmt::format("{:08X} ", ra);
        cur = caller_sp;
      }
      XELOGW(
          "ObReferenceObjectByName: '{}' not found (call {}, returning "
          "{:08X}), lr {:08X}, back chain: {}",
          key, n, static_cast<uint32_t>(result),
          static_cast<uint32_t>(ctx->lr), chain);
    }
  }
  if (XSUCCEEDED(result)) {
    return ObReferenceObjectByHandle_entry(handle, object_type_ptr,
                                           out_object_ptr);
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ObReferenceObjectByName, kNone, kImplemented);

// Phase 1095ak: defined below, used by xeObDereferenceObject.
static std::unordered_map<uint32_t, uint32_t>& InsertedObjectTable();

void xeObDereferenceObject(PPCContext* context, uint32_t native_ptr) {
  // Check if a dummy value from ObReferenceObjectByHandle.
  if (native_ptr == 0xDEADF00D) {
    return;
  }
  if (!native_ptr) {
    XELOGE("Null native ptr in ObDereferenceObject!");
    return;
  }

  auto object = XObject::GetNativeObject<XObject>(
      kernel_state(), kernel_memory()->TranslateVirtual(native_ptr));
  // Phase 1095ak: GetNativeObject resolves through the guest DISPATCH
  // HEADER, which the placeholder objects ObInsertObject registers do
  // not have - it wraps an arbitrary guest pointer in an XObject tagged
  // Type::NotifyListener (which is why type 7 shows up with no notify
  // listener ever being constructed) and records guest_ptr -> handle in
  // InsertedObjectTable. Now that ObReferenceObjectByHandle hands the
  // real guest pointer back instead of the 0xDEADF00D sentinel, the
  // return leg has to resolve it the same way it was registered.
  if (!object) {
    auto global_lock = xe::global_critical_region::AcquireDirect();
    auto& inserted = InsertedObjectTable();
    auto it = inserted.find(native_ptr);
    if (it != inserted.end()) {
      object = kernel_state()->object_table()->LookupObject<XObject>(
          it->second);
    }
  }
  if (object) {
    object->ReleaseHandle();

  } else {
    if (native_ptr) {
      // Phase 1096gt: this fired 299,677 times for the single pointer
      // 401E0F20 in one 100 s dashboard run, which buried the rest of the log
      // and said nothing about who was spinning. Report the caller's LR, and
      // after the first few per pointer keep only a periodic count so the loop
      // stays visible without drowning everything else.
      static std::unordered_map<uint32_t, uint64_t> unregistered_seen;
      auto global_lock = xe::global_critical_region::AcquireDirect();
      const uint64_t n = ++unregistered_seen[native_ptr];
      if (n <= 4 || (n % 100000) == 0) {
        XELOGW(
            "Unregistered guest object provided to ObDereferenceObject {:08X} "
            "(call {} from lr {:08X})",
            native_ptr, n, context ? static_cast<uint32_t>(context->lr) : 0u);
      }
    }
  }
  return;
}

void ObDereferenceObject_entry(dword_t native_ptr, const ppc_context_t& ctx) {
  xeObDereferenceObject(ctx, native_ptr);
}
DECLARE_XBOXKRNL_EXPORT1(ObDereferenceObject, kNone, kImplemented);

void ObReferenceObject_entry(dword_t native_ptr) {
  // Check if a dummy value from ObReferenceObjectByHandle.
  auto object = XObject::GetNativeObject<XObject>(
      kernel_state(), kernel_memory()->TranslateVirtual(native_ptr));
  if (object) {
    object->RetainHandle();
  } else {
    if (native_ptr) {
      XELOGW("Unregistered guest object provided to ObReferenceObject {:08X}",
             native_ptr.value());
    }
  }
  return;
}
DECLARE_XBOXKRNL_EXPORT1(ObReferenceObject, kNone, kImplemented);

dword_result_t ObCreateSymbolicLink_entry(pointer_t<X_ANSI_STRING> path_ptr,
                                          pointer_t<X_ANSI_STRING> target_ptr) {
  auto path = xe::utf8::canonicalize_guest_path(
      util::TranslateAnsiPath(kernel_memory(), path_ptr));
  auto target = xe::utf8::canonicalize_guest_path(
      util::TranslateAnsiPath(kernel_memory(), target_ptr));

  if (xe::utf8::starts_with(path, "\\??\\")) {
    path = path.substr(4);  // Strip the full qualifier
  }

  if (xe::utf8::starts_with(path, "\\System??\\")) {
    path = path.substr(10);  // Strip the full qualifier
  }

  // 4D5307DC expects success.
  // Phase 1099z36: the old test passed `target` to FindSymbolicLink, which
  // OVERWROTE the requested target with the existing one and returned
  // success - so xam's per-title "\??\GAME:" -> "\Device\CdRom0\" (title
  // start routine 8175DD2C) was silently dropped and GAME: kept pointing at
  // the previous title (Sonic, launched from the dash, found none of its
  // files). On a console "\??\GAME:" lives in the title's own object
  // directory, emptied by ExTerminateTitleProcess's Ob slot; Xenia has one
  // global link table and no Ob slot yet. DECLARED STAND-IN: an existing link
  // with a different target is replaced.
  {
    std::string existing;
    if (kernel_state()->file_system()->FindSymbolicLink(path, existing)) {
      if (xe::utf8::equal_case(existing, target)) {
        return X_STATUS_SUCCESS;
      }
      XELOGI("ObCreateSymbolicLink: replacing {} => {} (was {})", path, target,
             existing);
      kernel_state()->file_system()->UnregisterSymbolicLink(path);
    }
  }

  if (!kernel_state()->file_system()->RegisterSymbolicLink(path, target)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObCreateSymbolicLink, kNone, kImplemented);

dword_result_t ObDeleteSymbolicLink_entry(pointer_t<X_ANSI_STRING> path_ptr) {
  // Phase 1099q: normalise the same way ObCreateSymbolicLink registered it, or
  // "\System??\name:" never unregisters.
  auto path = xe::utf8::canonicalize_guest_path(
      util::TranslateAnsiPath(kernel_memory(), path_ptr));
  if (xe::utf8::starts_with(path, "\\??\\")) {
    path = path.substr(4);
  } else if (xe::utf8::starts_with(path, "\\System??\\")) {
    path = path.substr(10);
  }
  if (!kernel_state()->file_system()->UnregisterSymbolicLink(path)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObDeleteSymbolicLink, kNone, kImplemented);

dword_result_t NtDuplicateObject_entry(dword_t handle, lpdword_t new_handle_ptr,
                                       dword_t options) {
  // NOTE: new_handle_ptr can be zero to just close a handle.
  // NOTE: this function seems to be used to get the current thread handle
  //       (passed handle=-2).
  // This function actually just creates a new handle to the same object.
  // Most games use it to get real handles to the current thread or whatever.

  X_HANDLE new_handle = X_INVALID_HANDLE_VALUE;
  X_STATUS result =
      kernel_state()->object_table()->DuplicateHandle(handle, &new_handle);

  if (new_handle_ptr) {
    *new_handle_ptr = new_handle;
  }

  if (options == 1 /* DUPLICATE_CLOSE_SOURCE */) {
    // Always close the source object.
    kernel_state()->object_table()->RemoveHandle(handle);
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtDuplicateObject, kNone, kImplemented);

uint32_t NtClose(uint32_t handle) {
  return kernel_state()->object_table()->ReleaseHandle(handle);
}

dword_result_t NtClose_entry(dword_t handle) { return NtClose(handle); }
DECLARE_XBOXKRNL_EXPORT1(NtClose, kNone, kImplemented);

// Guest object pointer -> handle, for objects the guest built itself with
// ObCreateObject. Their handle cannot live in the object the way
// SetNativePointer does it, because such an object need not carry a dispatch
// header - xam's notification listener does not - and writing one there would
// corrupt it. xobject.cc already takes this approach for adopted guest timers.
static std::unordered_map<uint32_t, uint32_t>& InsertedObjectTable() {
  static std::unordered_map<uint32_t, uint32_t> table;
  return table;
}

// ObInsertObject(PVOID Object, POBJECT_ATTRIBUTES, ACCESS_MASK, PHANDLE)
//
// Turns an object created by ObCreateObject into a handle. Without this, xam's
// XamNotifyCreateListener creates its listener successfully and then has no
// way to return a handle for it, so it hands back 0 - which makes hud fail
// every XuiSceneCreate that needs a notification listener, and the Guide's
// GuideMain, GuideMainServer and MiniMediaPlayer scenes never load.
//
// Deliberately does not route through XObject::GetNativeObject the way
// ObOpenObjectByPointer does. That reads a dispatch header out of the object,
// and for an object without one it would interpret arbitrary bytes as a
// dispatcher type - quietly producing, say, an XEvent wrapper around something
// that is not an event.
dword_result_t ObInsertObject_entry(lpvoid_t object_ptr,
                                    lpvoid_t obj_attributes_ptr,
                                    dword_t desired_access,
                                    lpdword_t out_handle_ptr) {
  if (out_handle_ptr.guest_address()) {
    *out_handle_ptr = 0;
  }
  uint32_t guest_ptr = object_ptr.guest_address();
  if (!guest_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  auto global_lock = xe::global_critical_region::AcquireDirect();
  auto& table = InsertedObjectTable();

  // Inserting the same object twice hands back the same handle rather than
  // accumulating wrappers.
  auto it = table.find(guest_ptr);
  if (it != table.end()) {
    auto existing = kernel_state()->object_table()->LookupObject<XObject>(
        it->second);
    // Phase 1099z49: the entry is stale once its handle is closed and the
    // slot reused - measured: F80008B0 had become an XEvent, so xam's new
    // enumerator at the same address got that event's body back from
    // ObReferenceObjectByHandle and initialised a critical section on top of
    // the live Kinect task at 401AFFA0 (the Guide-over-Sonic crash). Only
    // reuse the handle while it still names the wrapper for this object.
    if (existing && (existing->type() != XObject::Type::NotifyListener ||
                     existing->guest_object() != guest_ptr)) {
      static std::atomic<uint32_t> stale{0};
      if (++stale <= 10u) {
        XELOGW("ObInsertObject: {:08X} stale handle {:08X} now names a "
               "type {} object at {:08X} - inserting afresh",
               guest_ptr, it->second, uint32_t(existing->type()),
               existing->guest_object());
      }
      existing.reset();
    }
    if (existing) {
      existing->RetainHandle();
      if (out_handle_ptr.guest_address()) {
        *out_handle_ptr = it->second;
      }
      return X_STATUS_SUCCESS;
    }
    table.erase(it);
  }

  // Phase 1099z25: waiting on a handle waits on the object's DEFAULT OBJECT
  // (real NtWaitForSingleObjectEx 800B56F8: type = [body-8], v = type+0x14;
  // v < 0x10000 is an offset into the body, otherwise a pointer). The bare
  // placeholder had no wait handle, so waits on it faulted the host (Sonic
  // waits on its XNotify listener, which real xam creates this way). Keep
  // the placeholder - Ob calls resolve it to the guest body - but forward
  // waits to Xenia's wrapper for that dispatcher object.
  class InsertedObject : public XObject {
   public:
    InsertedObject(KernelState* ks, object_ref<XObject> dispatcher)
        : XObject(ks, XObject::Type::NotifyListener),
          dispatcher_(std::move(dispatcher)) {}

   protected:
    xe::threading::WaitHandle* GetWaitHandle() override {
      return dispatcher_ ? dispatcher_->ForwardedWaitHandle() : nullptr;
    }
    void WaitCallback() override {
      if (dispatcher_) dispatcher_->ForwardedWaitCallback();
    }

   private:
    object_ref<XObject> dispatcher_;
  };
  object_ref<XObject> dispatcher;
  {
    auto* mem = kernel_memory();
    auto readable = [mem](uint32_t addr) {
      auto* heap = addr >= 0x10000 ? mem->LookupHeap(addr) : nullptr;
      // LLE module images (xam's object types live at 81D2xxxx) report
      // kNoAccess in the heap's page table while being readable (see
      // XObject::GetNativeObject); trust the image range when a heap owns it.
      return heap && ((addr >= 0x80000000u && addr < 0xA0000000u) ||
                      heap->QueryRangeAccess(addr, addr + 3) !=
                          xe::memory::PageAccess::kNoAccess);
    };
    const uint32_t type_ptr =
        readable(guest_ptr - 8)
            ? xe::load_and_swap<uint32_t>(mem->TranslateVirtual(guest_ptr - 8))
            : 0;
    if (!type_ptr || !readable(type_ptr + 0x14)) {
      auto* hp = mem->LookupHeap(guest_ptr - 8);
      XELOGW("ObInsertObject: {:08X} no object type (header readable {}, "
             "heap {}, type {:08X})",
             guest_ptr, readable(guest_ptr - 8), hp ? "yes" : "no", type_ptr);
    }
    if (cvars::guide_ob_insert_forward_waits && type_ptr &&
        readable(type_ptr + 0x14)) {
      const uint32_t default_object = xe::load_and_swap<uint32_t>(
          mem->TranslateVirtual(type_ptr + 0x14));
      const uint32_t disp = (default_object >> 16) == 0
                                ? guest_ptr + default_object
                                : default_object;
      dispatcher = XObject::GetNativeObject(
          kernel_state(), mem->TranslateVirtual(disp), UndefinedObject, true);
      XELOGI("ObInsertObject: {:08X} type {:08X} default object {:08X} -> "
             "dispatcher {:08X} ({})",
             guest_ptr, type_ptr, default_object, disp,
             dispatcher ? "waitable" : "not a dispatcher object");
    }
  }
  auto object = object_ref<XObject>(
      new InsertedObject(kernel_state(), std::move(dispatcher)));
  object->set_guest_object_no_stash(guest_ptr);

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  X_STATUS result =
      kernel_state()->object_table()->AddHandle(object.get(), &handle);
  if (XFAILED(result)) {
    XELOGE("ObInsertObject: no handle for guest object {:08X} ({:08X})",
           guest_ptr, result);
    return result;
  }

  table.emplace(guest_ptr, handle);
  if (out_handle_ptr.guest_address()) {
    *out_handle_ptr = handle;
  }
  XELOGI("ObInsertObject: guest object {:08X} -> handle {:08X}", guest_ptr,
         handle);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObInsertObject, kNone, kImplemented);

dword_result_t ObCreateObject_entry(
    pointer_t<X_OBJECT_TYPE> object_factory,
    pointer_t<X_OBJECT_ATTRIBUTES> optional_attributes,
    dword_t object_size_sans_headers, lpdword_t out_object,
    const ppc_context_t& context) {
  uint32_t out_object_tmp = 0;

  uint32_t result =
      xeObCreateObject(object_factory, optional_attributes,
                       object_size_sans_headers, &out_object_tmp, context);
  *out_object = out_object_tmp;
  // The Guide's scene creation ends here: xam's notification listener is made
  // with ObCreateObject, and hud returns E_FAIL from XuiSceneCreate when it
  // gets nothing back. Log the calls so the failing one can be seen directly
  // rather than inferred from the disassembly.
  static std::atomic<uint32_t> obn{0};
  uint32_t on = ++obn;
  if (on <= 40 || result != 0 || !out_object_tmp) {
    XELOGI(
        "ObCreateObject #{}: factory={:08X} tag={:08X} size={} -> status "
        "{:08X} object {:08X} (lr={:08X})",
        on, object_factory.guest_address(),
        uint32_t(object_factory->pool_tag), uint32_t(object_size_sans_headers),
        result, out_object_tmp, uint32_t(context->lr));
  }
  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ObCreateObject, kNone, kImplemented);

// Phase 1096gh: ObGetWaitableObject (ordinal 0x107) was DECLARED in
// xboxkrnl_table.inc and never implemented, so every call was an "undefined
// extern call" and xam kept whatever was already in r3 - its own input. Its
// job is to hand back the WAITABLE dispatch object for a given object; xam
// stores the result in [task+0xC] and registers it with its task pool.
//
// Measured consequence (research/FINDINGS.md 1096gf-1096gg): the dashboard
// registered a smartglass SERVICE structure (300F2018, whose +0 is a
// {handler,"smartglass"} descriptor at 81601658) as though it were a
// dispatch object. KeWaitForMultipleObjects then read 0x81 as the dispatch
// type - the top byte of that descriptor pointer - could not resolve it, and
// returned INVALID_PARAMETER without waiting, so the pool retried forever.
//
// Return the dispatch header when the pointer really is one Xenia knows, and
// NULL otherwise. NULL is the real function's failure result and it is the
// honest answer here: xam's own guard rejects NULL and skips the
// registration, which is far better than enqueueing a non-object.
dword_result_t ObGetWaitableObject_entry(lpvoid_t object_ptr) {
  if (!object_ptr) {
    return 0;
  }
  const uint32_t guest_ptr = object_ptr.guest_address();
  auto object = XObject::GetNativeObject<XObject>(
      kernel_state(), kernel_state()->memory()->TranslateVirtual(guest_ptr),
      X_OBJECT_TYPES::UndefinedObject, false);
  if (!object) {
    static std::atomic<uint32_t> miss{0};
    const uint32_t mn = ++miss;
    if (mn <= 8u) {
      XELOGW(
          "ObGetWaitableObject: {:08X} is not a dispatch object Xenia knows - "
          "returning NULL rather than letting the caller wait on it",
          guest_ptr);
    }
    return 0;
  }
  return guest_ptr;
}
DECLARE_XBOXKRNL_EXPORT1(ObGetWaitableObject, kNone, kImplemented);

// Creates a namespace directory object (e.g. \SystemNamedObjects). Xenia has
// no guest object namespace, so hand back a handle to a plain object and let
// name-based lookups resolve through the existing object table.
dword_result_t NtCreateDirectoryObject_entry(
    lpdword_t handle_out, pointer_t<X_OBJECT_ATTRIBUTES> obj_attributes) {
  if (!handle_out) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto directory = object_ref<XObject>(
      new XObject(kernel_state(), XObject::Type::Undefined));
  if (obj_attributes) {
    directory->SetAttributes(obj_attributes);
  }
  *handle_out = directory->handle();
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtCreateDirectoryObject, kNone, kSketchy);

// Resolves an object-namespace symbolic link (e.g. \Device\Flash) to its
// target path via the VFS symbolic link table.
// Phase 1099o: the signature was wrong. The first argument is an
// OBJECT_ATTRIBUTES, not an ANSI_STRING - xam's path expander 81732BD8 builds
// {root_directory = -3 (ObDosDevices) if the name starts with '\', else 0;
// name_ptr -> ANSI_STRING; attributes 0x40} at r1+0x58 and passes a caller-
// owned output ANSI_STRING {length, maximum_length, buffer} in r4, then reads
// its length back and NUL-terminates (81732C70). Reading the attributes block
// as a string matched no link, returned OBJECT_NAME_NOT_FOUND and never wrote
// the output, so xam turned it into hr 0x80070002 and every package create
// failed: 'Couldn't create and mount package "\Device\Harddisk0\Partition1\
// Content\...", hr = 0x80070002'. Expand the links, copy the fully translated
// name into the caller's buffer, and succeed when it names a real device.
dword_result_t ObTranslateSymbolicLink_entry(
    pointer_t<X_OBJECT_ATTRIBUTES> object_attributes,
    pointer_t<X_ANSI_STRING> out_target) {
  if (!object_attributes || !out_target || !object_attributes->name_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto* name_str = kernel_memory()->TranslateVirtual<X_ANSI_STRING*>(
      object_attributes->name_ptr);
  const std::string name = util::TranslateAnsiPath(kernel_memory(), name_str);
  std::string target;
  if (!kernel_state()->file_system()->TranslateSymbolicLinks(name, target)) {
    XELOGD("ObTranslateSymbolicLink: {} names no device", name);
    return X_STATUS_OBJECT_NAME_NOT_FOUND;
  }
  const uint16_t max_len = out_target->maximum_length;
  if (!out_target->pointer || target.size() > max_len) {
    return X_STATUS_BUFFER_TOO_SMALL;
  }
  std::memcpy(kernel_memory()->TranslateVirtual<char*>(out_target->pointer),
              target.data(), target.size());
  out_target->length = uint16_t(target.size());
  XELOGD("ObTranslateSymbolicLink: {} -> {}", name, target);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObTranslateSymbolicLink, kNone, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Ob);
