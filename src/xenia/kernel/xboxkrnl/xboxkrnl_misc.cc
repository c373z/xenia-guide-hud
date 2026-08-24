/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

DEFINE_bool(allow_mic_initialization, false,
            "Enable Mic Initialization\n"
            " Only set true when testing mic related functions",
            "Kernel");

namespace xe {
namespace kernel {
namespace xboxkrnl {

enum class XMicRequestType : uint16_t {
  MicGain = 0x0001,
  MicIoPending = 0x0007,
  MicGetCapabilities = 0x0009,
  MicUnk = 0x000B,
};

enum class XMicState : uint32_t {
  MicNotConnected = 0x00000000,
  MicInitilizied = 0x00000005,
};

struct X_MIC_INFO {
  xe::be<XMicRequestType> request_type;
  xe::be<uint16_t> user_index;
  xe::be<XMicState> state;  // 8

  xe::be<uint64_t> unk1;  // 16
  xe::be<uint64_t> unk2;  // 24
};

struct X_MIC_CAPABILITIES {
  xe::be<uint32_t> features;
  xe::be<uint16_t> format_tag;
  xe::be<uint16_t> channels;
  xe::be<uint32_t> sample_rates;
  xe::be<uint16_t> bits_per_sample;
  xe::be<uint16_t> frame_length;  // 0xE
  xe::be<uint8_t> mic_color;      // 0x10
  xe::be<uint16_t> vendor_id;
  xe::be<uint16_t> product_id;
  xe::be<uint16_t> revision;
  xe::be<uint32_t> device_id;
};
static_assert_size(X_MIC_CAPABILITIES, 0x1C);

struct X_MIC_DEVICE {
  X_MIC_INFO info;
  X_MIC_CAPABILITIES capabilities;
};

void KeEnableFpuExceptions_entry(
    const ppc_context_t& ctx) {  // dword_t enabled) {
  // TODO(benvanik): can we do anything about exceptions?
  // theres a lot more thats supposed to happen here, the floating point state
  // has to be saved to kthread, the irql changes, the machine state register is
  // changed to enable exceptions

  X_KTHREAD* kthread = ctx->TranslateVirtual(
      ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13])->prcb_data.current_thread);
  kthread->fpu_exceptions_on = static_cast<uint32_t>(ctx->r[3]) != 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeEnableFpuExceptions, kNone, kStub);

void KeSaveFloatingPointState_entry(const ppc_context_t& ctx) {
  // Probably we should use: thread_fpu_related to store/restore state
  X_KTHREAD* kthread = ctx->TranslateVirtual(
      ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13])->prcb_data.current_thread);

  for (size_t i = 0; i < xe::countof(ctx->f); ++i) {
    kthread->fpu_context[i] = ctx->f[i];
  }
}
DECLARE_XBOXKRNL_EXPORT1(KeSaveFloatingPointState, kNone, kSketchy);

void KeRestoreFloatingPointState_entry(const ppc_context_t& ctx) {
  const X_KTHREAD* kthread = ctx->TranslateVirtual(
      ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13])->prcb_data.current_thread);

  for (size_t i = 0; i < xe::countof(ctx->f); ++i) {
    ctx->f[i] = kthread->fpu_context[i];
  }
}
DECLARE_XBOXKRNL_EXPORT1(KeRestoreFloatingPointState, kNone, kSketchy);

static qword_result_t KeQueryInterruptTime_entry(const ppc_context_t& ctx) {
  auto kstate = ctx->kernel_state;
  uint32_t ts_bundle = kstate->GetKeTimestampBundle();
  X_TIME_STAMP_BUNDLE* bundle =
      ctx->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(ts_bundle);

  return xe::load_and_swap<uint64_t>(&bundle->interrupt_time);
}
DECLARE_XBOXKRNL_EXPORT1(KeQueryInterruptTime, kNone, kImplemented);

dword_result_t MicDeviceRequest_entry(pointer_t<X_MIC_DEVICE> device_ptr) {
  if (!device_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  XELOGE("MicDeviceRequest State: {:08X} Action: {:04X} USER: {:08X}",
         static_cast<uint32_t>(device_ptr->info.state.get()),
         static_cast<uint16_t>(device_ptr->info.request_type.get()),
         device_ptr->info.user_index.get());
  if (device_ptr->info.user_index > XUserMaxUserCount) {
    return X_STATUS_INVALID_PARAMETER;
  }

  if (device_ptr->info.request_type > XMicRequestType::MicUnk) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }

  device_ptr->info.state = cvars::allow_mic_initialization
                               ? XMicState::MicInitilizied
                               : XMicState::MicNotConnected;

  switch (device_ptr->info.request_type) {
    case XMicRequestType::MicGain:
      // GAIN
      break;
    case XMicRequestType::MicIoPending:
      return X_STATUS_PENDING;
    case XMicRequestType::MicGetCapabilities:
      device_ptr->capabilities.features = 0x100;
      device_ptr->capabilities.format_tag = 1;
      device_ptr->capabilities.mic_color = 0;
      break;
    default:
      // 0 Seems like initialization!
      break;
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(MicDeviceRequest, kNone, kStub);

// Debug monitor RPC used by retail xam/dash builds. There is no debug monitor
// attached; report failure so callers take the "not present" path.
dword_result_t ExDebugMonitorService_entry(dword_t r3, dword_t r4, dword_t r5,
                                           dword_t r6) {
  return X_STATUS_UNSUCCESSFUL;
}
DECLARE_XBOXKRNL_EXPORT1(ExDebugMonitorService, kNone, kStub);

// Runs a routine on every logical processor via IPI. Xenia has no real IPI
// mechanism; invoke the routine once on the calling thread, which is
// sufficient for the initialization uses in xam.
dword_result_t KeIpiGenericCall_entry(lpvoid_t routine, dword_t context,
                                      const ppc_context_t& ppc_context) {
  if (routine) {
    uint64_t args[] = {context};
    ppc_context->processor->Execute(ppc_context->thread_state,
                                    routine.guest_address(), args,
                                    xe::countof(args));
  }
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeIpiGenericCall, kNone, kSketchy);

// Reads a Digital Video Encoder register. No DVE is emulated; report zero.
dword_result_t VdReadDVERegisterUlong_entry(dword_t offset) {
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(VdReadDVERegisterUlong, kNone, kStub);

// --- Event tracing (Etx) -----------------------------------------------
// xam registers several trace producers during init. No tracing backend is
// emulated; report success so registration does not fail the caller.
dword_result_t EtxProducerRegister_entry(dword_t a, dword_t b, dword_t c,
                                         dword_t d) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(EtxProducerRegister, kNone, kStub);

dword_result_t EtxProducerUnregister_entry(dword_t handle) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(EtxProducerUnregister, kNone, kStub);

dword_result_t EtxProducerLog_entry(dword_t a, dword_t b, dword_t c,
                                    dword_t d) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(EtxProducerLog, kNone, kStub, kHighFrequency);

// --- Driver callback registration --------------------------------------
// xam installs these callbacks so drivers can notify it of state changes.
// Nothing drives them in Xenia; accept the registration and drop it.
dword_result_t DrvSetAudioLatencyCallback_entry(lpvoid_t callback) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(DrvSetAudioLatencyCallback, kNone, kStub);

dword_result_t DrvSetContentStorageCallback_entry(lpvoid_t callback) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(DrvSetContentStorageCallback, kNone, kStub);

dword_result_t DrvSetDeviceConfigChangeCallback_entry(lpvoid_t callback) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(DrvSetDeviceConfigChangeCallback, kNone, kStub);

dword_result_t DrvSetMicArrayStartCallback_entry(lpvoid_t callback) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(DrvSetMicArrayStartCallback, kNone, kStub);

dword_result_t DrvSetUserBindingCallback_entry(lpvoid_t callback) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(DrvSetUserBindingCallback, kNone, kStub);

dword_result_t XInputdSetFailedConnectionOrBindCallback_entry(
    lpvoid_t callback) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdSetFailedConnectionOrBindCallback, kNone,
                         kStub);

// --- Misc hardware queries ---------------------------------------------
// Reason the console powered on. 0x01 = power button.
dword_result_t HalGetPowerUpCause_entry() { return 0x01; }
DECLARE_XBOXKRNL_EXPORT1(HalGetPowerUpCause, kNone, kStub);

dword_result_t XeKeysGetStatus_entry(lpdword_t status_out) {
  if (status_out) {
    *status_out = 0;
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XeKeysGetStatus, kNone, kStub);

// Serializes shimmed module loads. No shim database is emulated.
dword_result_t XexShimLock_entry() { return X_STATUS_SUCCESS; }
DECLARE_XBOXKRNL_EXPORT1(XexShimLock, kNone, kStub);

dword_result_t KeSetPriorityClassThread_entry(lpvoid_t thread_ptr,
                                              dword_t priority_class) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeSetPriorityClassThread, kNone, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Misc);
