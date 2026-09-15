/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/emulator.h"
#include "xenia/hid/input.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// Phase 1096hl: the call site that actually checks this result is 817C50E4 in
// xam, and it pins the contract down:
//   817C50C8: addi r3,r1,0x54 / bl memset      - the caller clears its buffer
//   817C50D4: addi r5,r1,0x50 / stb 0,0x50(r1) - r5 is a pre-zeroed byte
//   817C50DC: addi r4,r1,0x54                  - r4 is that cleared buffer
//   817C50E0: mr   r3,r30                      - r3 is a device index
//   817C50E4: bl   HidReadKeys
//   817C50E8: cmpwi r3,0 / bne 817C52A0        - ZERO means a key was read;
//                                                anything else and it bails
// so r4 = &r5[1] in 32-bit terms, matching the older note that unk2 points into
// unk3's buffer.
//
// The other call site, 8177FCC4, is inside xam's input pump 8177FC88 - the same
// function that forms the message port 81D426D0 at 8177FCE8 and is called from
// 81780A90 right after 81780A84 registers the dispatcher 81780460 against it.
// That pump is measured to RUN in the dashboard (77/114 instructions, calls=1).
//
// What changes here is only WHICH non-zero status is returned. 0xC000009D is
// ERROR_DEVICE_NOT_CONNECTED - "there is no input device at all" - and that is
// not true: Xenia models connected controllers, which is how XamInputGetState
// answers. The status for "the device is there and has no key queued" is the
// 0x103 this file's own TODO already documented as translating to ERROR_EMPTY.
// Reporting a present device as absent is a claim about hardware Xenia does
// have, and xam's input code is entitled to branch on the difference.
//
// This does NOT synthesize key events. No key is ever reported, because none is
// being routed here yet; the buffers are left exactly as the caller cleared
// them. Both known call sites treat any non-zero identically (bail), so this is
// expected to be behaviour-neutral at those two sites - it is a correctness fix
// to the status, not a fix for the input path.
dword_result_t HidReadKeys_entry(dword_t device_index, unknown_t key_buffer,
                                 unknown_t state_buffer) {
  auto* emulator = kernel_state() ? kernel_state()->emulator() : nullptr;
  auto* input_system = emulator ? emulator->input_system() : nullptr;
  if (!input_system) {
    return 0xC000009D;  // ERROR_DEVICE_NOT_CONNECTED
  }
  xe::hid::X_INPUT_CAPABILITIES caps = {};
  X_RESULT connected;
  {
    auto lock = input_system->lock();
    connected = input_system->GetCapabilities(
        device_index, uint32_t(xe::hid::X_INPUT_FLAG::X_INPUT_FLAG_GAMEPAD),
        &caps);
  }
  if (connected != X_ERROR_SUCCESS) {
    return 0xC000009D;  // no device in that slot - the old blanket answer
  }
  return 0x103;  // device present, no key queued (ERROR_EMPTY to the guest)
}
DECLARE_XBOXKRNL_EXPORT1(HidReadKeys, kInput, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Hid);
