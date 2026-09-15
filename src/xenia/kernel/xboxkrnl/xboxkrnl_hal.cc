/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/smc.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"

DECLARE_int32(avpack);

namespace xe {
namespace kernel {
namespace xboxkrnl {

constexpr std::array<std::string_view, 9> FirmwareReentryMessage = {
    "hard poweroff",
    "hard reset (video error)",
    "hard reset (used for dumpwritedump/frozen processor)",
    "hard reset",
    "power off (hard)",
    "power off (nice)",
    "Shut off (Lost Settings)",
    "Shut off (Frozen Console)",
    "Shut off",
};

void HalReturnToFirmware_entry(dword_t routine) {
  // TODO(benvank): diediedie much more gracefully
  // Not sure how to blast back up the stack in LLVM without exceptions, though.
  const std::string exitMessage = fmt::format(
      "Game requested a {} via HalReturnToFirmware",
      static_cast<size_t>(routine) < FirmwareReentryMessage.size()
          ? FirmwareReentryMessage[routine]
          : fmt::format("Reboot (Routine Code: {})", routine.value()));
  XELOGE(exitMessage);
  exit(0);
}
DECLARE_XBOXKRNL_EXPORT2(HalReturnToFirmware, kNone, kStub, kImportant);

dword_result_t HalGetCurrentAVPack_entry() { return cvars::avpack; }
DECLARE_XBOXKRNL_EXPORT1(HalGetCurrentAVPack, kNone, kImplemented);

void HalSendSMCMessage_entry(pointer_t<X_SMC_DATA> smc_message,
                             pointer_t<X_SMC_DATA> smc_response) {
  if (!smc_message) {
    return;
  }

  kernel_state()->smc()->CallCommand(smc_message, smc_response);
}
DECLARE_XBOXKRNL_EXPORT3(HalSendSMCMessage, kNone, kStub, kImportant,
                         kHighFrequency);

// Phase 1099z12: real kernel 80059800 sends SMC 0x8B with 0x60 for a nonzero
// argument (OPEN) and 0x62 for zero (CLOSE) - the old mapping was inverted -
// and the SMC then reports the motion through registered notifications.
void HalOpenCloseODDTray_entry(dword_t open_close) {
  XELOGI("HalOpenCloseODDTray({})", uint32_t(open_close));
  kernel_state()->smc()->MoveTray(open_close != 0);
}
DECLARE_XBOXKRNL_EXPORT1(HalOpenCloseODDTray, kNone, kImplemented);

// Phase 1099z12: HalRegisterSMCNotification(record, register) - xam 8177D878
// registers 81D42628 (routine 8177D418) with register = 1. It was an
// undefined extern, so xam never heard about the tray.
void HalRegisterSMCNotification_entry(dword_t record, dword_t reg) {
  kernel_state()->smc()->RegisterNotification(record, reg != 0);
}
DECLARE_XBOXKRNL_EXPORT1(HalRegisterSMCNotification, kNone, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Hal);
