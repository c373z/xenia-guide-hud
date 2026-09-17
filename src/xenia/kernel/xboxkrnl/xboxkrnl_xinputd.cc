/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// 1099z17559-2: the kernel side of the controller - XInputd* / Drv* exports.
//
// xam never talks to USB. The gamepad driver (RGC) is inside the kernel and xam
// reaches it only through these exports (research/XINPUTD_SPEC.md). Xenia IS
// the kernel, so a controller for LLE xam means implementing this contract,
// after the 17489 kernel (research/kernel17489, VAs below are that image):
//
//   DrvSetUserBindingCallback 8016C3D8   atomic store, return 0
//   DrvBindToUser             8016C538   [cb](UserId, Context, Category,
//                                         bUnbind, u8* pUserIndex) -> r3;
//                                         no cb -> 0xC000007A
//   DrvSetDeviceConfigChange  8016C400   atomic store, return 0
//   RgcBindToUser             8012BB10   DrvBindToUser([port+0x80] UserId,
//                                         0x10000000|port, 0, 0, &idx=0xFF)
//   Guide button              8012AB30   bit 0x0400 down arms a 2000 ms timer;
//                                         released first -> DrvXenonButtonPressed
//                                         (ctx, 0, 0); timer fires -> (ctx, 0, 1)
//   XInputd dispatch          801304C8.. top nibble of the handle selects the
//                                         class (1 = RGC gamepad), low nibble
//                                         is the port; other class 0xC0000001;
//                                         the "suppressed" out param is zeroed
//   RGC GetCapabilities       80124F28   port >= 4 or absent -> 0xC000009D;
//                                         32 bytes from port+0x2C8
//   RGC ReadState             80125058   *pPacket = packet number; gamepad =
//                                         buttons & 0xF7FF, LT, RT, LX, LY, RX, RY
//   RGC GetDeviceStats        80125370   0x1C bytes; +0xC 0x10000000, +0x10 port
//   GetFailedConnectionOrBind 80125510   *out = xchg(global, 0); return 0
//
// HOST-SIDE model: the ports are the host InputSystem's four slots. Everything below the
// port (USB, authentication, packets) is not modelled: a host slot that reports
// capabilities is a present, authenticated gamepad.
//
// Gated by kernel_xinputd (default off) so the 17489 setup, whose input still
// comes through the automation workaround, is unchanged: with the flag off
// DrvSetUserBindingCallback discards the pointer exactly as the old stub did,
// nothing is ever bound, and xam never reaches the other exports.

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/power_reset.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

uint32_t GuideSysReqCallback();  // xboxkrnl_misc.cc

namespace {

constexpr uint32_t kClassRgc = 0x10000000;
constexpr uint32_t kStatusNotConnected = 0xC000009D;
constexpr uint32_t kStatusBadClass = 0xC0000001;
constexpr uint32_t kStatusNoCallback = 0xC000007A;

std::atomic<uint32_t> g_bind_cb{0};
std::atomic<uint32_t> g_config_cb{0};
std::atomic<uint32_t> g_failed_bind_cb{0};
std::atomic<uint32_t> g_failed_bind{0};
std::atomic<uint32_t> g_autobind{0};
std::atomic<bool> g_rgc_started{false};

struct Port {
  bool bound = false;
  uint8_t user = 0xFF;
  bool guide_armed = false;
  bool guide_held_fired = false;
  std::chrono::steady_clock::time_point guide_down;
};
std::array<Port, 4> g_ports;
std::mutex g_ports_lock;
uint32_t g_bind_idx_ptr = 0;  // guest scratch for DrvBindToUser's user index

hid::InputSystem* Input() {
  auto* emu = kernel_state()->emulator();
  return emu ? emu->input_system() : nullptr;
}

const uint32_t kAllTypes = hid::InputType::Controller |
                           hid::InputType::Keyboard | hid::InputType::Other;

bool HostPresent(uint32_t port, hid::X_INPUT_CAPABILITIES* caps) {
  auto* is = Input();
  if (!is || port >= 4) return false;
  hid::X_INPUT_CAPABILITIES c = {};
  const bool ok = is->GetCapabilities(port, kAllTypes, &c) == X_ERROR_SUCCESS;
  if (ok && caps) *caps = c;
  return ok;
}

// HOST-SIDE: stable nonzero per-port identity for [port+0x80]. The real value is the
// kernel's own device identity; xam only compares it (same UserId first when
// choosing a slot), so any stable nonzero value per port behaves the same.
uint32_t PortUserId(uint32_t port) { return 0x00524743u + port; }  // 'RGC'+n

void LogOnce(const char* name, uint32_t a, uint32_t b, uint32_t c) {
  static std::mutex m;
  static std::array<std::pair<const char*, uint32_t>, 64> seen{};
  std::lock_guard<std::mutex> lock(m);
  for (auto& s : seen) {
    if (s.first == name) {
      if (++s.second > 4) return;
      XELOGI("XInputd: {}({:08X}, {:08X}, {:08X})", name, a, b, c);
      return;
    }
    if (!s.first) {
      s = {name, 1};
      XELOGI("XInputd: {}({:08X}, {:08X}, {:08X})", name, a, b, c);
      return;
    }
  }
}

// Decodes a device handle. Returns 0 and sets *port for a present RGC port,
// else the status the real dispatch/driver returns.
uint32_t DecodeRgc(uint32_t context, uint32_t* port) {
  const uint32_t cls = context & 0xF0000000u;
  *port = context & 0x0Fu;
  if (cls == kClassRgc) {
    return *port < 4 ? 0u : kStatusNotConnected;
  }
  // Classes 2, 3 and 5 go to other kernel drivers (none emulated -> no device).
  if (cls == 0x20000000u || cls == 0x30000000u || cls == 0x50000000u) {
    return kStatusNotConnected;
  }
  return kStatusBadClass;
}

uint32_t RunBind(cpu::ThreadState* ts, uint32_t port, bool unbind,
                 uint8_t* out_user) {
  const uint32_t cb = g_bind_cb.load();
  if (!cb) return kStatusNoCallback;
  auto* mem = kernel_state()->memory();
  uint32_t& idx_ptr = g_bind_idx_ptr;
  if (!idx_ptr) idx_ptr = mem->SystemHeapAlloc(4);
  xe::store_and_swap<uint32_t>(mem->TranslateVirtual(idx_ptr), 0xFF000000u);
  uint64_t args[] = {PortUserId(port), kClassRgc | port, 0, unbind ? 1u : 0u,
                     idx_ptr};
  const uint32_t r = static_cast<uint32_t>(
      kernel_state()->processor()->Execute(ts, cb, args, xe::countof(args)));
  const uint8_t idx = *mem->TranslateVirtual<uint8_t*>(idx_ptr);
  if (out_user) *out_user = idx;
  XELOGI("XInputd: DrvBindToUser(UserId {:08X}, ctx {:08X}, cat 0, unbind {}) "
         "-> {:08X}, user index {:02X}",
         PortUserId(port), kClassRgc | port, unbind ? 1 : 0, r, idx);
  return r;
}

void DeliverGuide(cpu::ThreadState* ts, uint32_t port, uint32_t kind) {
  const uint32_t cb = GuideSysReqCallback();
  if (!cb) {
    XELOGW("XInputd: Guide on port {} but no DrvSetSysReqCallback registered",
           port);
    return;
  }
  uint64_t args[] = {kClassRgc | port, 0, kind};
  kernel_state()->processor()->Execute(ts, cb, args, xe::countof(args));
  XELOGI("XInputd: Guide button port {} -> DrvXenonButtonPressed({:08X}, 0, "
         "{}) via {:08X}",
         port, kClassRgc | port, kind, cb);
}

// Phase 1099z156: TEST TOOLING, not console behaviour. Record port 0's button
// state changes (all 16 bits, Guide included) to kernel_xinputd_record_path in
// hid_test_pad_script format "ms:hexbuttons:hold_ms,...", so a play session can
// be replayed in an automated run (--hid=nop --hid_test_pad_script=@<file>).
// ms is measured from process start; the nop driver's clock starts when it is
// constructed, a fraction of a second later, so a replay runs slightly early.
// Phase 1099z159: sticks and triggers are recorded too (a step is
// "ms:buttons:hold:lt:rt:lx:ly:rx:ry" when any of them is off rest); the first
// recorder kept buttons only, so stick navigation was lost on replay.
const auto g_process_start = std::chrono::steady_clock::now();
std::mutex g_rec_lock;
FILE* g_rec_file = nullptr;
hid::X_INPUT_GAMEPAD g_rec_pad = {};
uint32_t g_rec_since_ms = 0;
bool g_rec_first = true;

bool PadAtRest(const hid::X_INPUT_GAMEPAD& g) {
  return !uint16_t(g.buttons) && !g.left_trigger && !g.right_trigger &&
         !int16_t(g.thumb_lx) && !int16_t(g.thumb_ly) && !int16_t(g.thumb_rx) &&
         !int16_t(g.thumb_ry);
}

// A new step starts when a button changes, a trigger moves by 16 or more, or a
// stick axis moves by 2048 or more (sticks jitter; the recorded values are the
// exact ones sampled at the start of the step).
bool PadChanged(const hid::X_INPUT_GAMEPAD& a, const hid::X_INPUT_GAMEPAD& b) {
  auto moved = [](int x, int y, int t) { return (x > y ? x - y : y - x) >= t; };
  return uint16_t(a.buttons) != uint16_t(b.buttons) ||
         moved(a.left_trigger, b.left_trigger, 16) ||
         moved(a.right_trigger, b.right_trigger, 16) ||
         moved(int16_t(a.thumb_lx), int16_t(b.thumb_lx), 2048) ||
         moved(int16_t(a.thumb_ly), int16_t(b.thumb_ly), 2048) ||
         moved(int16_t(a.thumb_rx), int16_t(b.thumb_rx), 2048) ||
         moved(int16_t(a.thumb_ry), int16_t(b.thumb_ry), 2048) ||
         PadAtRest(a) != PadAtRest(b);
}

void RecordPad(const hid::X_INPUT_GAMEPAD& pad) {
  if (cvars::kernel_xinputd_record_path.empty()) return;
  std::lock_guard<std::mutex> lock(g_rec_lock);
  if (!g_rec_file) {
    // "{time}" in the path becomes the launch time, so a window that records
    // every session does not overwrite the previous recording.
    std::string path = xe::path_to_utf8(cvars::kernel_xinputd_record_path);
    const size_t at = path.find("{time}");
    if (at != std::string::npos) {
      std::time_t t = std::time(nullptr);
      std::tm tm_local;
      localtime_s(&tm_local, &t);
      char stamp[32];
      std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_local);
      path.replace(at, 6, stamp);
    }
    g_rec_file = xe::filesystem::OpenFile(xe::to_path(path), "wb");
    if (!g_rec_file) return;
    XELOGI("XInputd: recording port 0 buttons, sticks and triggers to {}",
           path);
  }
  if (!PadChanged(pad, g_rec_pad)) return;
  const uint32_t now_ms = uint32_t(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - g_process_start)
          .count());
  const auto& g = g_rec_pad;
  if (!PadAtRest(g)) {
    // Close the held step: it lasted from g_rec_since_ms until now.
    fprintf(g_rec_file, "%s%u:%04X:%u", g_rec_first ? "" : ",", g_rec_since_ms,
            uint16_t(g.buttons), now_ms - g_rec_since_ms);
    if (g.left_trigger || g.right_trigger || int16_t(g.thumb_lx) ||
        int16_t(g.thumb_ly) || int16_t(g.thumb_rx) || int16_t(g.thumb_ry)) {
      fprintf(g_rec_file, ":%u:%u:%d:%d:%d:%d", uint8_t(g.left_trigger),
              uint8_t(g.right_trigger), int16_t(g.thumb_lx), int16_t(g.thumb_ly),
              int16_t(g.thumb_rx), int16_t(g.thumb_ry));
    }
    g_rec_first = false;
    fflush(g_rec_file);
  }
  g_rec_pad = pad;
  g_rec_since_ms = now_ms;
}

// The RGC driver's device thread: connection -> bind, and the Guide button.
int RgcThread() {
  auto* ts = XThread::GetCurrentThread()->thread_state();
  XELOGI("XInputd: RGC device thread running");
  while (true) {
    xe::threading::Sleep(std::chrono::milliseconds(8));
    if (!g_bind_cb.load()) continue;
    for (uint32_t port = 0; port < 4; ++port) {
      const bool present = HostPresent(port, nullptr);
      Port& p = g_ports[port];
      if (present && !p.bound) {
        uint8_t user = 0xFF;
        const uint32_t r = RunBind(ts, port, false, &user);
        if (int32_t(r) >= 0) {
          std::lock_guard<std::mutex> lock(g_ports_lock);
          p.bound = true;
          p.user = user;
        } else {
          // Real RGC records the failure for XInputdGetFailedConnectionOrBind.
          g_failed_bind.store(kClassRgc | port);
          xe::threading::Sleep(std::chrono::milliseconds(1000));
        }
      } else if (!present && p.bound) {
        RunBind(ts, port, true, nullptr);
        std::lock_guard<std::mutex> lock(g_ports_lock);
        p.bound = false;
        p.user = 0xFF;
        p.guide_armed = false;
      }
      if (!p.bound) continue;
      hid::X_INPUT_STATE st = {};
      auto* is = Input();
      if (!is || is->GetState(port, kAllTypes, &st) != X_ERROR_SUCCESS) {
        continue;
      }
      if (port == 0) RecordPad(st.gamepad);
      const bool guide = (uint16_t(st.gamepad.buttons) & 0x0400) != 0;
      const auto now = std::chrono::steady_clock::now();
      if (guide) {
        if (!p.guide_armed) {
          p.guide_armed = true;
          p.guide_held_fired = false;
          p.guide_down = now;
        } else if (!p.guide_held_fired &&
                   now - p.guide_down >= std::chrono::milliseconds(2000)) {
          p.guide_held_fired = true;
          DeliverGuide(ts, port, 1);
        }
      } else if (p.guide_armed) {
        p.guide_armed = false;
        if (!p.guide_held_fired) DeliverGuide(ts, port, 0);
      }
    }
  }
  return 0;
}

void StartRgc() {
  if (g_rgc_started.exchange(true)) return;
  auto* ks = kernel_state();
  auto t = object_ref<XHostThread>(new XHostThread(
      ks, 256 * 1024, 0, []() -> int { return RgcThread(); },
      ks->GetSystemProcess()));
  t->set_name("RGC device");
  if (XFAILED(t->Create())) {
    XELOGE("XInputd: could not create the RGC device thread");
    g_rgc_started.store(false);
  }
}

}  // namespace

// --- callback registration -------------------------------------------------

dword_result_t DrvSetUserBindingCallback_entry(dword_t callback) {
  if (!cvars::kernel_xinputd) {
    // Previous behaviour (stub): accept and discard.
    return X_STATUS_SUCCESS;
  }
  const uint32_t prev = g_bind_cb.exchange(callback);
  XELOGI("XInputd: DrvSetUserBindingCallback {:08X} -> {:08X}", prev,
         uint32_t(callback));
  if (callback) StartRgc();
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(DrvSetUserBindingCallback, kInput, kImplemented);

dword_result_t DrvSetDeviceConfigChangeCallback_entry(dword_t callback) {
  g_config_cb.store(callback);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(DrvSetDeviceConfigChangeCallback, kInput,
                         kImplemented);

dword_result_t XInputdSetFailedConnectionOrBindCallback_entry(
    dword_t callback) {
  g_failed_bind_cb.store(callback);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdSetFailedConnectionOrBindCallback, kInput,
                         kImplemented);

dword_result_t DrvBindToUser_entry(dword_t user_id, dword_t context,
                                   dword_t category, dword_t unbind,
                                   lpvoid_t user_index_ptr) {
  const uint32_t cb = g_bind_cb.load();
  if (!cb) return kStatusNoCallback;
  auto* th = XThread::GetCurrentThread();
  uint64_t args[] = {user_id, context, category, unbind,
                     user_index_ptr.guest_address()};
  return static_cast<uint32_t>(kernel_state()->processor()->Execute(
      th->thread_state(), cb, args, xe::countof(args)));
}
DECLARE_XBOXKRNL_EXPORT1(DrvBindToUser, kInput, kImplemented);

// 8016C4A0: compare-and-store; r3 is returned untouched.
dword_result_t DrvSetAutobind_entry(dword_t value) {
  g_autobind.store(value);
  LogOnce("DrvSetAutobind", value, 0, 0);
  return static_cast<uint32_t>(value);
}
DECLARE_XBOXKRNL_EXPORT1(DrvSetAutobind, kInput, kImplemented);

// --- device queries ----------------------------------------------------------

// The trailing "suppressed" out param of XInputdGetCapabilities (3rd) and
// XInputdReadState (4th) is newer than those exports. Measured at every xam
// call site (2 ReadState, 1 GetCapabilities per build): 2.0.6770 7357 8955
// 12625 13604 14699 14717 14719 leave r5/r6 unset - a leftover; 6770's r6 was
// 4 and the zeroing write host-faulted at guest 4, hanging the Blades
// dashboard on its boot logo. 15574 16197 16547 16747 17559 pass a stack
// address. Same boundary as XInputdGetDeviceStats below.
constexpr uint16_t kSuppressedOutArgBuild = 15574;

static bool HasSuppressedOutArg() {
  return kernel_state()->GetKernelVersion()->build >= kSuppressedOutArgBuild;
}

dword_result_t XInputdGetCapabilities_entry(dword_t context, lpvoid_t caps_ptr,
                                            lpdword_t suppressed_ptr) {
  if (suppressed_ptr && HasSuppressedOutArg()) *suppressed_ptr = 0;
  uint32_t port;
  uint32_t status = DecodeRgc(context, &port);
  if (status) return status;
  hid::X_INPUT_CAPABILITIES caps = {};
  if (!HostPresent(port, &caps)) return kStatusNotConnected;
  if (caps_ptr) {
    // HOST-SIDE: first 20 bytes are X_INPUT_CAPABILITIES; bytes 20..31 of the driver's
    // 32-byte record are not known and are left zero.
    std::memset(caps_ptr.as<uint8_t*>(), 0, 32);
    std::memcpy(caps_ptr.as<uint8_t*>(), &caps, sizeof(caps));
  }
  LogOnce("XInputdGetCapabilities", context, caps_ptr.guest_address(), 0);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(XInputdGetCapabilities, kInput, kImplemented,
                         kHighFrequency);

dword_result_t XInputdReadState_entry(dword_t context, lpdword_t packet_ptr,
                                      lpvoid_t gamepad_ptr,
                                      lpdword_t suppressed_ptr) {
  if (suppressed_ptr && HasSuppressedOutArg()) *suppressed_ptr = 0;
  uint32_t port;
  uint32_t status = DecodeRgc(context, &port);
  if (status) return status;
  auto* is = Input();
  hid::X_INPUT_STATE st = {};
  if (!is || is->GetState(port, kAllTypes, &st) != X_ERROR_SUCCESS) {
    return kStatusNotConnected;
  }
  if (packet_ptr) *packet_ptr = st.packet_number;
  if (gamepad_ptr) {
    hid::X_INPUT_GAMEPAD g = st.gamepad;
    g.buttons = uint16_t(g.buttons) & 0xF7FF;
    std::memcpy(gamepad_ptr.as<uint8_t*>(), &g, sizeof(g));
  }
  LogOnce("XInputdReadState", context, packet_ptr.guest_address(),
          gamepad_ptr.guest_address());
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(XInputdReadState, kInput, kImplemented,
                         kHighFrequency);

dword_result_t XInputdWriteState_entry(dword_t context, dword_t user,
                                       lpvoid_t vibration_ptr) {
  uint32_t port;
  uint32_t status = DecodeRgc(context, &port);
  if (status) return status;
  if (!HostPresent(port, nullptr)) return kStatusNotConnected;
  if (auto* is = Input(); is && vibration_ptr) {
    hid::X_INPUT_VIBRATION v = *vibration_ptr.as<hid::X_INPUT_VIBRATION*>();
    is->SetState(port, &v);
  }
  LogOnce("XInputdWriteState", context, user, vibration_ptr.guest_address());
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdWriteState, kInput, kImplemented);

// Older kernels take two arguments. Measured at xam's call sites: 2.0.12625,
// 2.0.13604 and 2.0.14719 call XInputdGetDeviceStats(handle, &stats) (13604
// xam 818C4600: r3 = handle, r4 = caller's buffer, r5/r6 left as leftovers
// pointing into xam's own frame); 2.0.16197 and 17559 call
// (handle, 1, &stats, &flag) like the 17489 kernel at 80130660. Reading the
// 4-argument form on an old xam zeroed 0x1C bytes over xam's saved LR and it
// crashed (blr to 0).
constexpr uint16_t kGetDeviceStats4ArgBuild = 15574;

dword_result_t XInputdGetDeviceStats_entry(dword_t context, dword_t unk,
                                           lpvoid_t stats_ptr,
                                           lpdword_t flag_ptr) {
  auto* mem = kernel_memory();
  uint32_t stats_addr = stats_ptr.guest_address();
  uint32_t flag_addr = flag_ptr.guest_address();
  if (kernel_state()->GetKernelVersion()->build < kGetDeviceStats4ArgBuild) {
    stats_addr = uint32_t(unk);
    flag_addr = 0;
  }
  if (flag_addr) xe::store_and_swap<uint32_t>(mem->TranslateVirtual(flag_addr), 0);
  uint32_t port;
  uint32_t status = DecodeRgc(context, &port);
  if (status) return status;
  uint8_t* stats = stats_addr ? mem->TranslateVirtual(stats_addr) : nullptr;
  if (stats) std::memset(stats, 0, 0x1C);
  if (!HostPresent(port, nullptr)) return kStatusNotConnected;
  if (stats) {
    // HOST-SIDE: +0x0/+0x4/+0x8/+0x14/+0x18 come from driver state that is not modelled
    // (left zero). +0xC and +0x10 are what xam checks.
    xe::store_and_swap<uint32_t>(stats + 0xC, kClassRgc);
    xe::store_and_swap<uint32_t>(stats + 0x10, port);
  }
  LogOnce("XInputdGetDeviceStats", context, unk, stats_addr);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdGetDeviceStats, kInput, kImplemented);

dword_result_t XInputdGetFailedConnectionOrBind_entry(lpdword_t out_ptr) {
  if (out_ptr) *out_ptr = g_failed_bind.exchange(0);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdGetFailedConnectionOrBind, kInput,
                         kImplemented);

// --- outputs and rarely used controls ----------------------------------------
// Ring of light, rumble effects, power and keep-alive have no host effect here;
// they succeed for a present RGC port. Each logs its first calls so a run
// shows whether xam reaches it.

static uint32_t PresentOr(uint32_t context) {
  uint32_t port;
  uint32_t status = DecodeRgc(context, &port);
  if (status) return status;
  return HostPresent(port, nullptr) ? 0u : kStatusNotConnected;
}

dword_result_t XInputdNotify_entry(dword_t context, dword_t a, dword_t b) {
  LogOnce("XInputdNotify", context, a, b);
  return PresentOr(context);
}
DECLARE_XBOXKRNL_EXPORT1(XInputdNotify, kInput, kStub);

dword_result_t XInputdSetRingOfLight_entry(dword_t context, dword_t a,
                                           dword_t b) {
  LogOnce("XInputdSetRingOfLight", context, a, b);
  return PresentOr(context);
}
DECLARE_XBOXKRNL_EXPORT1(XInputdSetRingOfLight, kInput, kStub);

dword_result_t XInputdSendStayAliveRequest_entry(dword_t context) {
  LogOnce("XInputdSendStayAliveRequest", context, 0, 0);
  return PresentOr(context);
}
DECLARE_XBOXKRNL_EXPORT1(XInputdSendStayAliveRequest, kInput, kStub);

dword_result_t XInputdPowerDownDevice_entry(dword_t context) {
  LogOnce("XInputdPowerDownDevice", context, 0, 0);
  return PresentOr(context);
}
DECLARE_XBOXKRNL_EXPORT1(XInputdPowerDownDevice, kInput, kStub);

dword_result_t XInputdSetMinMaxAuthDelay_entry(dword_t a, dword_t b) {
  LogOnce("XInputdSetMinMaxAuthDelay", a, b, 0);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdSetMinMaxAuthDelay, kInput, kStub);

dword_result_t XInputdFFSetRumble_entry(dword_t context, dword_t a,
                                        dword_t b) {
  LogOnce("XInputdFFSetRumble", context, a, b);
  return PresentOr(context);
}
DECLARE_XBOXKRNL_EXPORT1(XInputdFFSetRumble, kInput, kStub);

dword_result_t XInputdFFSetDeviceGain_entry(dword_t context, dword_t a) {
  LogOnce("XInputdFFSetDeviceGain", context, a, 0);
  return PresentOr(context);
}
DECLARE_XBOXKRNL_EXPORT1(XInputdFFSetDeviceGain, kInput, kStub);

dword_result_t XInputdFFDeviceControl_entry(dword_t context, dword_t a,
                                            dword_t b) {
  LogOnce("XInputdFFDeviceControl", context, a, b);
  return PresentOr(context);
}
DECLARE_XBOXKRNL_EXPORT1(XInputdFFDeviceControl, kInput, kStub);

dword_result_t XInputdControl_entry(dword_t context, dword_t a, dword_t b) {
  LogOnce("XInputdControl", context, a, b);
  return kStatusBadClass;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdControl, kInput, kStub);

dword_result_t XInputdGetDevicePid_entry(dword_t context, dword_t a) {
  LogOnce("XInputdGetDevicePid", context, a, 0);
  return kStatusNotConnected;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdGetDevicePid, kInput, kStub);

dword_result_t XInputdRawState_entry(dword_t context, dword_t a, dword_t b) {
  LogOnce("XInputdRawState", context, a, b);
  return kStatusNotConnected;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdRawState, kInput, kStub);

dword_result_t XInputdReadTextKeystroke_entry(dword_t context, dword_t a,
                                              dword_t b) {
  LogOnce("XInputdReadTextKeystroke", context, a, b);
  return kStatusNotConnected;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdReadTextKeystroke, kInput, kStub);

dword_result_t XInputdGetTextDeviceKeyLocks_entry(dword_t context, dword_t a) {
  LogOnce("XInputdGetTextDeviceKeyLocks", context, a, 0);
  return kStatusNotConnected;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdGetTextDeviceKeyLocks, kInput, kStub);

dword_result_t XInputdSetTextDeviceKeyLocks_entry(dword_t context, dword_t a) {
  LogOnce("XInputdSetTextDeviceKeyLocks", context, a, 0);
  return kStatusNotConnected;
}
DECLARE_XBOXKRNL_EXPORT1(XInputdSetTextDeviceKeyLocks, kInput, kStub);

dword_result_t HidGetCapabilities_entry(dword_t a, dword_t b) {
  LogOnce("HidGetCapabilities", a, b, 0);
  return kStatusNotConnected;
}
DECLARE_XBOXKRNL_EXPORT1(HidGetCapabilities, kInput, kStub);

void ResetXInputdStateForPowerOff() {
  g_bind_cb = 0;
  g_config_cb = 0;
  g_failed_bind_cb = 0;
  g_failed_bind = 0;
  g_autobind = 0;
  g_rgc_started = false;
  g_bind_idx_ptr = 0;
  std::lock_guard<std::mutex> lock(g_ports_lock);
  g_ports = {};
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(XInputd);
