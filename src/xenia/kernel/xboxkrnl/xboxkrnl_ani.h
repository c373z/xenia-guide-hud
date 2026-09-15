/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_ANI_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_ANI_H_

#include <cstdint>

namespace xe {
namespace kernel {
namespace xboxkrnl {

// AniStartBootAnimation as the kernel's phase-1 init calls it (argument 0),
// for the emulator's boot sequence. Returns the NTSTATUS.
uint32_t AniStartBootAnimationHost(bool from_background);
// True from a successful start until the last Block/Terminate caller returns
// ([801DD848] in the 17489 kernel).
bool AniIsStarted();

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_ANI_H_
