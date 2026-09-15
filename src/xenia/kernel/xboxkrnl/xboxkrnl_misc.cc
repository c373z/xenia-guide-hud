/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <chrono>
#include <cstring>

#include "xenia/base/threading.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
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

// DrvSetDeviceConfigChangeCallback: moved to xboxkrnl_xinputd.cc.

dword_result_t DrvSetMicArrayStartCallback_entry(lpvoid_t callback) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(DrvSetMicArrayStartCallback, kNone, kStub);

// DrvSetUserBindingCallback, XInputdSetFailedConnectionOrBindCallback: moved
// to xboxkrnl_xinputd.cc (1099z17559-2).

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

// Phase 1096gj: NicGetLinkState (xboxkrnl 0x208) was declared in
// xboxkrnl_table.inc and never implemented, so every call was an "undefined
// extern call" and the caller read whatever happened to be in r3. The
// dashboard polls it ~1/s during device bring-up (91 calls in a 90 s run,
// research/FINDINGS.md 1096gi).
//
// The signature is MEASURED from the guest call site, not guessed:
//     81846664: bl   0x81d1066c        ; no argument registers set beforehand
//     81846668: mr   r31, r3           ; a single DWORD result is kept
//
// Return 0 - no link. Xenia emulates no network adapter, so "cable
// unplugged" is the honest answer; claiming a link would have the guest wait
// on a network that will never come up.
// Phase 1099z15: the real kernel (800B8210) returns 0x10000 - not 0 - when no
// adapter is attached (or the argument's bit 0 is clear), and [nic+0x1A4]
// otherwise. xam's IP-config poll (818B53B8) only advances on bit 0x10000, so
// 0 left XNetGetTitleXnAddr PENDING forever. Xenia models no adapter:
// NicAttach hands back NULL, as the real one does when 800B9670 finds none.
//
// Phase 1099z85: superseded. The real 17489 kernel never hands back NULL: a
// console has its built-in wired adapter even with no cable. Xenia now models
// exactly that - wired adapter present, cable unplugged, no wireless - see the
// Nic* block below (addresses are 17489).

// Phase 1099z76: ExExpansionCall (17489 kernel 80089880) scans the 4-slot
// expansion table at 0x90001000 for the id and jumps to the installed code;
// with nothing installed it returns STATUS_NOT_IMPLEMENTED and touches no
// buffers. Nothing on this software set installs one (no ExExpansionInstall
// importer, no HXPR/HXPC blob), so that is the real answer. Before this was
// implemented the call was an undefined extern that left r3 = the id
// ("PV03", positive), which xam took as success - e.g. 8173A7B0 then skipped
// the content device-ID check.
dword_result_t ExExpansionCall_entry(dword_t expansion_id, dword_t command,
                                     dword_t arg1, dword_t arg2,
                                     dword_t arg3) {
  static std::atomic<uint32_t> logs{0};
  if (++logs <= 20) {
    XELOGI("ExExpansionCall({:08X}, {}, {:08X}) -> not installed",
           uint32_t(expansion_id), uint32_t(command), uint32_t(arg1));
  }
  return X_STATUS_NOT_IMPLEMENTED;
}
DECLARE_XBOXKRNL_EXPORT1(ExExpansionCall, kNone, kImplemented);

// ExExpansionInstall (800898E0): size > 0x10000 or low 7 bits set ->
// INVALID_PARAMETER; otherwise the hypervisor RSA-checks a signed HXPR/HXPC
// blob and runs its HV and kernel sections. Xenia cannot run signed
// hypervisor code, so an otherwise valid install fails as the kernel reports
// any hypervisor failure. No caller exists on this software set.
dword_result_t ExExpansionInstall_entry(lpvoid_t address, dword_t size) {
  if (size > 0x10000 || (size & 0x7F)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  XELOGW("ExExpansionInstall({:08X}, {:X}): signed hypervisor expansions are "
         "not supported - failing",
         address.guest_address(), uint32_t(size));
  return X_STATUS_UNSUCCESSFUL;
}
DECLARE_XBOXKRNL_EXPORT1(ExExpansionInstall, kNone, kImplemented);

// ---------------------------------------------------------------------------
// Phase 1099z85: network adapter - wired present, cable unplugged, no wireless
// (17489 kernel). Masks: 1 = wired adapter, 2 = registered-device slot (the
// wireless path; with no device the kernel's default ops report 0x10000 and
// its attach always succeeds, so a wired console attaches as 3).
//
// Client object (NicAttach 80104DE8, 0x58 bytes, tag 'NICU'): +00/+08/+10
// list heads, +18 -> +3C, +1C 0, +20..+3B params copy (then +20 = attached
// mask), +3C..+57 second params copy. Params: +0 requested mask, +4 rx filter,
// +8 ctx, +C rx callback, +10 xmit-complete callback, +14 link-change
// callback, +18. xam never dereferences the handle.
// ---------------------------------------------------------------------------
namespace {
const auto nic_phy_reset_time = std::chrono::steady_clock::now();

// Wired adapter link state (8010A038 via the poll): no cable -> no speed code,
// no link bit; bit 0x10000 (link detection finished) once auto-negotiation
// completes or 3500 ms pass after PHY reset. PHY reset is taken as emulator
// start - declared approximation (real: adapter init during kernel boot).
uint32_t NicWiredLinkState() {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - nic_phy_reset_time)
                      .count();
  return ms >= 3500 ? 0x10000u : 0u;
}
const uint32_t kNicRegisteredDeviceLinkState = 0x10000;  // default ops 80109AF8
}  // namespace

// NicGetLinkState(adapter_mask) 801057A8 - the argument is a MASK (xam passes
// [81D27614]: 0xFFFFFFF7 before attach, the attached mask after).
dword_result_t NicGetLinkState_entry(dword_t mask) {
  uint32_t r = 0x10000;
  uint32_t all = 0x10000;
  if (mask & 1) {
    const uint32_t wired = NicWiredLinkState();
    all &= wired;
    r = (all | ~0x10000u) & wired;
  }
  if (!(r & 1) && (mask & 2)) {
    all &= kNicRegisteredDeviceLinkState;
    r = (all | ~0x10000u) & kNicRegisteredDeviceLinkState;
  }
  return r;
}
DECLARE_XBOXKRNL_EXPORT1(NicGetLinkState, kNone, kImplemented);

void NicAttach_entry(lpvoid_t params, lpdword_t out_handle,
                     const ppc_context_t& ctx) {
  auto* mem = kernel_memory();
  const uint32_t client = mem->SystemHeapAlloc(0x58);
  if (!client) {
    if (out_handle) *out_handle = 0;
    return;
  }
  auto* c = mem->TranslateVirtual<uint8_t*>(client);
  std::memset(c, 0, 0x58);
  xe::store_and_swap<uint32_t>(c + 0x00, client);
  xe::store_and_swap<uint32_t>(c + 0x04, client);
  xe::store_and_swap<uint32_t>(c + 0x08, client + 0x08);
  xe::store_and_swap<uint32_t>(c + 0x0C, client + 0x08);
  xe::store_and_swap<uint32_t>(c + 0x10, client + 0x10);
  xe::store_and_swap<uint32_t>(c + 0x14, client + 0x10);
  xe::store_and_swap<uint32_t>(c + 0x18, client + 0x3C);
  std::memcpy(c + 0x20, params.as<uint8_t*>(), 0x1C);
  std::memcpy(c + 0x3C, params.as<uint8_t*>(), 0x1C);
  const uint32_t requested = xe::load_and_swap<uint32_t>(c + 0x20);
  uint32_t attached = 0;
  if (requested & 1) attached |= 1;  // wired adapter present
  if (requested & 2) attached |= 2;  // 80105D98 always succeeds
  xe::store_and_swap<uint32_t>(c + 0x20, attached);
  if (out_handle) *out_handle = client;
  XELOGI("NicAttach: client {:08X} requested {:08X} attached {} (wired, no "
         "cable)",
         client, requested, attached);

  // The wired poll (80108670) calls each attached client's link-change
  // callback once, when it first sees 0x10000 at the 3.5 s timeout. A client
  // attached after that never gets it. Declared bypass: the real call runs at
  // DPC level under the adapter lock; here it runs on a host thread.
  const uint32_t link_cb = xe::load_and_swap<uint32_t>(c + 0x34);
  const uint32_t cb_ctx = xe::load_and_swap<uint32_t>(c + 0x28);
  if ((attached & 1) && link_cb && NicWiredLinkState() == 0) {
    auto* ks = kernel_state();
    auto t = object_ref<XHostThread>(new XHostThread(
        ks, 128 * 1024, 0,
        [ks, link_cb, cb_ctx]() -> int {
          const auto wait = std::chrono::milliseconds(3500) -
                            (std::chrono::steady_clock::now() -
                             nic_phy_reset_time);
          if (wait.count() > 0) xe::threading::Sleep(
              std::chrono::duration_cast<std::chrono::milliseconds>(wait));
          XELOGI("NicAttach: link detection finished (no cable) - link-change "
                 "callback {:08X}({:08X})",
                 link_cb, cb_ctx);
          uint64_t args[] = {cb_ctx};
          ks->processor()->Execute(XThread::GetCurrentThread()->thread_state(),
                                   link_cb, args, 1);
          return 0;
        },
        ks->GetSystemProcess()));
    t->set_name("NIC link detect");
    t->Create();
  }
}
DECLARE_XBOXKRNL_EXPORT1(NicAttach, kNone, kImplemented);

// NicDetach 80104F18: unlink from the adapters and free; no callbacks.
void NicDetach_entry(dword_t client) {
  if (client) {
    kernel_memory()->SystemHeapFree(client);
  }
}
DECLARE_XBOXKRNL_EXPORT1(NicDetach, kNone, kImplemented);

// NicXmit(client, flag, frame, len, packet) 801056B0: no active link on either
// adapter, so the frame is completed at once through the client's
// xmit-complete callback [client+0x30](ctx, packet), on the caller's thread.
void NicXmit_entry(dword_t client, dword_t flag, lpvoid_t frame, dword_t len,
                   dword_t packet, const ppc_context_t& ctx) {
  if (!client) return;
  auto* c = kernel_memory()->TranslateVirtual<uint8_t*>(client);
  const uint32_t cb = xe::load_and_swap<uint32_t>(c + 0x30);
  const uint32_t cb_ctx = xe::load_and_swap<uint32_t>(c + 0x28);
  if (cb) {
    uint64_t args[] = {cb_ctx, uint32_t(packet)};
    ctx->processor->Execute(ctx->thread_state, cb, args, 2);
  }
}
DECLARE_XBOXKRNL_EXPORT1(NicXmit, kNone, kImplemented);

// NicGetOpt(client, option, out, inout_size) 801051B8.
dword_result_t NicGetOpt_entry(dword_t client, dword_t option, lpdword_t out,
                               lpdword_t inout_size) {
  if (!client) {
    return 0xC0000008;  // STATUS_INVALID_HANDLE
  }
  auto* c = kernel_memory()->TranslateVirtual<uint8_t*>(client);
  auto answer = [&](uint32_t value) -> uint32_t {
    if (!inout_size || *inout_size < 4) {
      if (inout_size) *inout_size = 4;
      return 0xC0000023;  // STATUS_BUFFER_TOO_SMALL
    }
    *inout_size = 4;
    if (out) *out = value;
    return X_STATUS_SUCCESS;
  };
  switch (uint32_t(option)) {
    case 0x2731:  // rx filter
      return answer(xe::load_and_swap<uint32_t>(c + 0x24));
    case 0x2733:  // attached adapter mask ("NIC device flags")
      return answer(xe::load_and_swap<uint32_t>(c + 0x20));
    case 0x2730:  // wired flow control: none negotiated (inferred default)
    case 0x2732:  // wired loopback: off
    case 0x2AF9:  // default device
      return answer(0);
    default:
      // 0x2710..0x272F are wired PHY registers (80106D40). Their no-cable
      // contents are not traced, so they fall through as unanswered.
      return 0xC00000BB;  // STATUS_NOT_SUPPORTED
  }
}
DECLARE_XBOXKRNL_EXPORT1(NicGetOpt, kNone, kImplemented);

// NicSetOpt(client, option, value, size) 80105318.
dword_result_t NicSetOpt_entry(dword_t client, dword_t option, lpdword_t value,
                               dword_t size) {
  if (!client) {
    return 0xC0000008;
  }
  auto* c = kernel_memory()->TranslateVirtual<uint8_t*>(client);
  switch (uint32_t(option)) {
    case 0x2731: {
      if (size < 4 || !value) return X_STATUS_INVALID_PARAMETER;
      if (xe::load_and_swap<uint32_t>(c + 0x24) == uint32_t(*value)) {
        return X_STATUS_UNSUCCESSFUL;  // same value
      }
      xe::store_and_swap<uint32_t>(c + 0x24, *value);
      return X_STATUS_SUCCESS;
    }
    case 0x2733: {
      if (size < 4 || !value) return X_STATUS_INVALID_PARAMETER;
      const uint32_t want = uint32_t(*value) & 3;
      if (xe::load_and_swap<uint32_t>(c + 0x20) == want) {
        return X_STATUS_UNSUCCESSFUL;
      }
      xe::store_and_swap<uint32_t>(c + 0x20, want);
      return X_STATUS_SUCCESS;
    }
    case 0x2730:
    case 0x2732:
      return X_STATUS_SUCCESS;
    default:
      if (uint32_t(option) >= 0x2710 && uint32_t(option) <= 0x272F) {
        return X_STATUS_SUCCESS;  // PHY register write
      }
      return 0xC00000BB;
  }
}
DECLARE_XBOXKRNL_EXPORT1(NicSetOpt, kNone, kImplemented);

// NicSetUnicastAddress(mask, mac, bool) 80104D18: programs the adapter; there
// is nothing on the wire to affect.
void NicSetUnicastAddress_entry(dword_t mask, lpvoid_t mac, dword_t flag) {}
DECLARE_XBOXKRNL_EXPORT1(NicSetUnicastAddress, kNone, kImplemented);

// NicUpdateMcastMembership(client, mac, add) 80104FB0.
dword_result_t NicUpdateMcastMembership_entry(dword_t client, lpvoid_t mac,
                                              dword_t add) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NicUpdateMcastMembership, kNone, kImplemented);

// NicFlushXmitQueue(client) 80105078: nothing is ever queued without link.
void NicFlushXmitQueue_entry(dword_t client) {}
DECLARE_XBOXKRNL_EXPORT1(NicFlushXmitQueue, kNone, kImplemented);

// Phase 1096gj: two more exports the dashboard polls that were declared and
// never implemented (165 and 41 calls in a 90 s run). Both SIGNATURES ARE
// MEASURED from the guest call sites at 817C4690-817C46C4, not guessed:
//
//   817C4690: addi r4, r1, 0x60      ; out pointer
//   817C4694: mr   r3, r31           ; r31 counts 0..3 - the user index
//   817C4698: bl   XInputdGetLastTextInputTime
//   817C469C: cmplwi r3, 0           ; 0 = success
//   817C46A4: ld   r11, 0x60(r1)     ; the out value is 64-bit
//
//   817C46BC: addi r3, r1, 0x60      ; out pointer, no user index
//   817C46C0: bl   HidGetLastInputTime
//   817C46C4: cmpwi r3, 0
//
// The caller compares the returned time against a threshold, so this is an
// idle check. Report the current guest system time - "input happened just
// now" - which is the honest answer for a session that is being driven, and
// keeps the guest from taking an idle/screensaver path on the strength of a
// value the emulator never supplied.
dword_result_t HidGetLastInputTime_entry(lpqword_t time_ptr) {
  if (!time_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  *time_ptr = Clock::QueryGuestSystemTime();
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(HidGetLastInputTime, kNone, kStub);

dword_result_t XInputdGetLastTextInputTime_entry(dword_t user_index,
                                                 lpqword_t time_ptr) {
  if (!time_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  *time_ptr = Clock::QueryGuestSystemTime();
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdGetLastTextInputTime, kNone, kStub);

// Phase 1096gj: MtpdGetCurrentDevices, the last high-volume undefined extern
// in the dashboard's device bring-up (288 calls in a 100 s run). Signature
// MEASURED from the call site at 81A4D038-81A4D054:
//
//   81A4D038: stw  r11, 0x50(r1)   ; an in/out count is pre-set
//   81A4D03C: addi r5, r1, 0x50    ; ...and passed as the third argument
//   81A4D040: addi r4, r1, 0x58    ; second argument: the output buffer
//   81A4D044: li   r3, 3           ; first argument: a type/class selector
//   81A4D048: bl   MtpdGetCurrentDevices
//   81A4D050: blt  0x81a4d0d4      ; negative result = error
//   81A4D054: lwz  r31, 0x50(r1)   ; the count is read back
//
// Report zero devices and success. Xenia emulates no MTP (Media Transfer
// Protocol) devices, so an empty list is the truthful answer; returning an
// error instead would send the caller down a failure path for a query that
// did not actually fail.
dword_result_t MtpdGetCurrentDevices_entry(dword_t device_class,
                                           lpvoid_t devices_ptr,
                                           lpdword_t count_ptr) {
  if (count_ptr) {
    *count_ptr = 0;
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(MtpdGetCurrentDevices, kNone, kStub);

// Phase 1096gt: the rest of the dashboard's undefined externs. Every signature
// below was read off its call site in dashroot's xam, not guessed, and every
// return value was chosen by reading the branch the caller takes next so a
// stub cannot invent a retry loop. None of these call sites loops on the
// result.

// NicGetStats(mask, stats) 801050F0: memset 0x5C, then add the wired
// counters (6 qwords, 11 dwords) - all zero with no cable. Phase 1099z85: the
// real kernel writes the full 0x5C bytes (xam 8184E7D0 reads +0x30), so this
// no longer writes only the first qword. Returns nothing on hardware.
void NicGetStats_entry(dword_t adapter_mask, lpvoid_t stats_ptr) {
  if (stats_ptr) {
    std::memset(stats_ptr.as<uint8_t*>(), 0, 0x5C);
  }
}
DECLARE_XBOXKRNL_EXPORT1(NicGetStats, kNone, kImplemented);

// 817CC9A4: addi r3,r1,0x50 (a request block filled at +50..+60) / bl
// 817CC9BC: cmpwi r3,0 / bge (skip) / oris r31,r3,0x1000 - a negative status
// is tagged and returned to its caller, and nothing retries. There is no
// camera, so say so rather than reporting a success that produced no data.
dword_result_t PsCamDeviceRequest_entry(lpvoid_t request_ptr) {
  return 0xC000000E;  // STATUS_NO_SUCH_DEVICE
}
DECLARE_XBOXKRNL_EXPORT1(PsCamDeviceRequest, kNone, kStub);

// 817CCF64: bl UsbdGetNatalHardwareVersion / cmplwi r3,1 ... the caller maps
// the version to a category and yields 0 for anything that is not 1 or 2.
// There is no Kinect attached, so 0 is both truthful and the value that lands
// on the caller's "none" path.
dword_result_t UsbdGetNatalHardwareVersion_entry() { return 0; }
DECLARE_XBOXKRNL_EXPORT1(UsbdGetNatalHardwareVersion, kNone, kStub);

// 81A7234C: li r5,0x28 / li r3,0x100 / add r4,r11,r30 / bl XeKeysExSetKey
// Xenia models no key vault, so nothing is stored. Reported as accepted
// because the only readers of what would have been stored are themselves
// stubs; this is a stub, not a working key store.
dword_result_t XeKeysExSetKey_entry(dword_t key_id, lpvoid_t key_ptr,
                                    dword_t key_size) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XeKeysExSetKey, kNone, kStub);

// 817D7844: li r3,1 / lfs f1,0x1ef0(r11) / bl XAudioSetVoiceCategoryVolume.
// A category id and a float gain. Nothing to route it to; accept it.
dword_result_t XAudioSetVoiceCategoryVolume_entry(dword_t category) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioSetVoiceCategoryVolume, kNone, kStub);

// 81756450: li r5,0x4c / lwa r4,0x6c(r1) / li r3,3 / bl
// DumpRegisterDedicatedDataBlock. Registers a block to be included in a crash
// dump. Xenia writes no console crash dumps, so accepting the registration
// and keeping nothing is the whole of the behaviour.
dword_result_t DumpRegisterDedicatedDataBlock_entry(dword_t kind, dword_t id,
                                                    dword_t size) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(DumpRegisterDedicatedDataBlock, kNone, kStub);


// --- Phase 1097: the Xenon (Guide) button, as the real kernel delivers it ---
//
// Both halves of this were DECLARED in xboxkrnl_table.inc and never
// implemented, so every call was an "undefined extern call" that threw the
// argument away. That is the whole reason a Guide button press has never
// reached xam: xam registers a callback with the kernel and waits to be
// called, and nothing was ever holding the pointer.
//
// MEASURED, from the user's own kernel image (xboxkrnlce.bin, a FLAT image -
// file offset == RVA; the PE section table's raw offsets do NOT apply, and
// reading it that way lands mid-function, which is how research/kdis.py had
// been reading it):
//
//   ordinal 0x20C DrvSetSysReqCallback   VA 8007FE58
//     lis r11,0x800E / addi r11,r11,-0x7AEC        ; r11 = 800D8514
//     lwarx r10,0,r11 / stwcx. r3,0,r11 / bne -    ; store the callback
//     li r3,0 / blr                                ; always succeeds
//
//   ordinal 0x278 DrvXenonButtonPressed  VA 8007FF88
//     r10 = [800D8514]
//     if (!r10) return 0xC000007A                  ; STATUS_PROCEDURE_NOT_FOUND
//     mtctr r10 / bctrl                            ; r3,r4,r5 passed straight
//     li r3,0                                      ; through, untouched
//
// And the kernel's OWN caller of it, in its HID dispatch at 800BBE30:
//     r5 = [r1+0x58]   ; the kernel checks it is 0 or 1
//     r4 = 0
//     r3 = [r1+0x54]   ; the HID device object for the port
//     bl 8007FF88
// so the callback's contract is (device, class, kind) with class 0 and kind
// in {0,1}.
//
// On the xam side the callback xam registers is 817C26C8, which forwards to
// 817C23A8(ctx, device, class, kind); that posts the code into [ctx+0x10] and
// calls KeSetEvent on ctx, which is what wakes xam's Guide worker. xam's own
// null-device path (817C243C: device == 0) supplies user 0 when the device
// cannot be resolved, so passing 0 leaves the choice to xam rather than
// inventing an index here.
static std::atomic<uint32_t> xe_sysreq_callback_{0};

uint32_t GuideSysReqCallback() { return xe_sysreq_callback_.load(); }

dword_result_t DrvSetSysReqCallback_entry(dword_t callback) {
  if (!cvars::guide_sysreq_kernel) {
    // Same answer the real kernel gives (it stores and returns 0); the store
    // is what guide_sysreq_kernel gates while its effect is being measured.
    return X_STATUS_SUCCESS;
  }
  const uint32_t prev = xe_sysreq_callback_.exchange(callback);
  XELOGI("DrvSetSysReqCallback: {:08X} -> {:08X}", prev,
         static_cast<uint32_t>(callback));
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(DrvSetSysReqCallback, kNone, kImplemented);

dword_result_t DrvXenonButtonPressed_entry(dword_t device, dword_t cls,
                                           dword_t kind) {
  const uint32_t cb = xe_sysreq_callback_.load();
  if (!cb) {
    return X_STATUS_PROCEDURE_NOT_FOUND;
  }
  auto* thread = XThread::GetCurrentThread();
  if (!thread) {
    return X_STATUS_PROCEDURE_NOT_FOUND;
  }
  uint64_t args[] = {device, cls, kind};
  kernel_state()->processor()->Execute(thread->thread_state(), cb, args,
                                       xe::countof(args));
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(DrvXenonButtonPressed, kNone, kImplemented);

// Host side of the same door. The emulator sees the physical Guide button;
// the kernel is the thing that is supposed to hand it to xam, and Xenia IS
// the kernel here, so this runs the guest's own registered callback on a
// guest thread and nothing else. Returns the callback that was called, or 0.
uint32_t GuideDeliverXenonButton(uint32_t device, uint32_t cls,
                                 uint32_t kind) {
  const uint32_t cb = xe_sysreq_callback_.load();
  if (!cb) {
    XELOGW("GuideSysReq: no callback registered - xam never called "
           "DrvSetSysReqCallback");
    return 0;
  }
  auto* ks = kernel_state();
  auto t = object_ref<XHostThread>(new XHostThread(
      ks, 256 * 1024, 0, [ks, cb, device, cls, kind]() -> int {
        auto* ts = XThread::GetCurrentThread()->thread_state();
        uint64_t args[] = {device, cls, kind};
        uint64_t r = ks->processor()->Execute(ts, cb, args, xe::countof(args));
        XELOGI("GuideSysReq: {:08X}({:08X},{:08X},{:08X}) -> {:08X}", cb,
               device, cls, kind, static_cast<uint32_t>(r));
        return 0;
      }));
  t->set_name("GuideSysReq");
  if (XFAILED(t->Create())) {
    XELOGE("GuideSysReq: could not create the delivery thread");
    return 0;
  }
  t->Wait(0, 0, 0, nullptr);
  return cb;
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Misc);
