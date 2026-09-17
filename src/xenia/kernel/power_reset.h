/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_POWER_RESET_H_
#define XENIA_KERNEL_POWER_RESET_H_

namespace xe {
namespace kernel {

// Host Power Off (File > Power Off Console) destroys the Emulator and builds a
// new one in the same process. Module-level state - guest addresses, guest
// handles, XObject refs, "already started" flags - would otherwise carry into
// the new console. Called by ~Emulator after every guest thread is stopped and
// before the kernel state is destroyed (object refs must drop first).
void ResetModuleStateForPowerOff();
void ResetXObjectStateForPowerOff();
void ResetXTimerStateForPowerOff();
void ResetXThreadStateForPowerOff();

namespace xboxkrnl {
void ResetObStateForPowerOff();
void ResetXInputdStateForPowerOff();
void ResetAniStateForPowerOff();
void ResetVideoStateForPowerOff();
void ResetMiscStateForPowerOff();
void ResetIoStateForPowerOff();
void ResetCryptStateForPowerOff();
void ResetAudioStateForPowerOff();
void ResetLzxStateForPowerOff();
void ResetMemoryStateForPowerOff();
void ResetModulesStateForPowerOff();
}  // namespace xboxkrnl

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_POWER_RESET_H_
