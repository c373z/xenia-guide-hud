/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// 1099z17559-9: kernel exports xam's UI start-up (retail 17559, 81696558)
// reads to pick the boot reason. They were declared in xboxkrnl_table.inc with
// no body, so each call took the undefined-extern path, which returns without
// setting r3 - the caller read back its own first argument. For
// DumpGetRawDumpInfo(buffer, ...) that is a nonzero pointer, i.e. "a raw crash
// dump is present", so xam launched \SystemRoot\ProcessDump.Xex (not in any
// flash), failed with C0000034 and showed "Game Error - The game couldn't
// start" over the boot.
//
// After the 17489 kernel:
//   DumpGetRawDumpInfo (0x005, 801B4F68): opens \Device\Harddisk0\DumpPartition
//     (two attempts); if it cannot, returns 0 without touching the buffer.
//   HalFinalizePowerLossRecovery (0x2BA, 8008D628): returns 1 only when the
//     power-loss-recovery bit (0x8000 of a HAL state word) is set, else 0.
//   HalGetNotedArgonErrors (0x317, 8008D810): returns a noted-errors byte that
//     is 0 unless the Argon (wireless) radio reported errors.
// Xenia emulates no dump partition, no power-loss recovery and no Argon radio,
// so the values below are those the kernel returns for that hardware state.
//
// Gated by kernel_boot_state_exports (default false): 17489 xam calls the
// first two as well, and with the flag off the undefined-extern result (r3
// unchanged) is reproduced so that setup is unchanged.

#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

namespace {
// The undefined-extern result: r3 as the caller left it.
uint32_t UnchangedR3() {
  auto* th = XThread::GetCurrentThread();
  return th ? static_cast<uint32_t>(th->thread_state()->context()->r[3]) : 0;
}
}  // namespace

dword_result_t DumpGetRawDumpInfo_entry(lpvoid_t buffer, dword_t flags) {
  if (!cvars::kernel_boot_state_exports) {
    return buffer.guest_address();
  }
  auto* fs = kernel_state()->file_system();
  const bool present =
      fs && fs->ResolvePath("\\Device\\Harddisk0\\DumpPartition") != nullptr;
  XELOGI("DumpGetRawDumpInfo({:08X}, {:08X}): dump partition {}",
         buffer.guest_address(), uint32_t(flags),
         present ? "present - raw dump parsing is not implemented, reporting "
                   "none"
                 : "absent -> no dump");
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(DumpGetRawDumpInfo, kDebug, kImplemented);

dword_result_t HalFinalizePowerLossRecovery_entry() {
  if (!cvars::kernel_boot_state_exports) {
    return UnchangedR3();
  }
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(HalFinalizePowerLossRecovery, kNone, kImplemented);

dword_result_t HalGetNotedArgonErrors_entry() {
  if (!cvars::kernel_boot_state_exports) {
    return UnchangedR3();
  }
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(HalGetNotedArgonErrors, kNone, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(BootState);
