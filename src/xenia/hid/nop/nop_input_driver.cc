/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/nop/nop_input_driver.h"

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/hid/hid_flags.h"

namespace xe {
namespace hid {
namespace nop {

namespace {
// 1099z17559-2: TEST-ONLY scripted pad (--hid_test_pad_script). See
// hid_flags.cc. Each step holds its buttons from ms to ms+hold.
// Phase 1099z159: an optional ":lt:rt:lx:ly:rx:ry" tail (decimal) gives the
// triggers and sticks held for the step, as the kernel_xinputd recorder writes.
struct TestPadStep {
  uint32_t at_ms;
  uint16_t buttons;
  uint32_t hold_ms;
  int32_t analog[6];  // lt, rt, lx, ly, rx, ry
};
std::vector<TestPadStep> g_test_steps;
std::mutex g_test_steps_mutex;
bool g_test_pad = false;
std::chrono::steady_clock::time_point g_test_origin;

void TestPadState(X_INPUT_GAMEPAD* g) {
  const auto now_ms = uint32_t(std::chrono::duration_cast<
                                   std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() -
                                   g_test_origin)
                                   .count());
  uint16_t b = 0;
  const TestPadStep* analog = nullptr;
  std::lock_guard<std::mutex> lock(g_test_steps_mutex);
  for (const auto& s : g_test_steps) {
    if (now_ms >= s.at_ms && now_ms < s.at_ms + s.hold_ms) {
      b |= s.buttons;
      analog = &s;  // steps are in time order: the latest started wins
    }
  }
  *g = {};
  g->buttons = b;
  if (analog) {
    g->left_trigger = uint8_t(analog->analog[0]);
    g->right_trigger = uint8_t(analog->analog[1]);
    g->thumb_lx = int16_t(analog->analog[2]);
    g->thumb_ly = int16_t(analog->analog[3]);
    g->thumb_rx = int16_t(analog->analog[4]);
    g->thumb_ry = int16_t(analog->analog[5]);
  }
}
}  // namespace

NopInputDriver::NopInputDriver(xe::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {
  // Once per process: after a host Power Off a new driver is built, and a
  // restarted clock would replay every step already pressed.
  if (g_test_origin.time_since_epoch().count() == 0) {
    g_test_origin = std::chrono::steady_clock::now();
  }
}

NopInputDriver::~NopInputDriver() = default;

// The app creates the nop driver without calling Setup(), so the script is
// parsed on first use; the clock starts at driver construction.
static void ParseTestPadScript() {
  static bool parsed = false;
  if (parsed) return;
  parsed = true;
  std::string sc = cvars::hid_test_pad_script;
  // 2026-09-16 (TEST-ONLY): "live:<path>" follows a file; every complete line
  // appended to it, "buttons_hex[:hold_ms[:lt:rt:lx:ly:rx:ry]]", is pressed
  // at once. Lets a tool drive the console step by step.
  if (sc.rfind("live:", 0) == 0) {
    const std::string path = sc.substr(5);
    g_test_pad = true;
    XELOGI("HID nop: TEST-ONLY live pad following {}", path);
    std::thread([path]() {
      std::streamoff offset = 0;
      std::string pending;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < offset) offset = 0;  // Truncated: start over.
        if (size == offset) continue;
        in.seekg(offset);
        std::string chunk(size_t(size - offset), '\0');
        in.read(chunk.data(), std::streamsize(chunk.size()));
        offset = size;
        pending += chunk;
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
          std::string line = pending.substr(0, nl);
          pending.erase(0, nl + 1);
          while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
          }
          if (line.empty() || line[0] == '#') continue;
          TestPadStep st = {};
          st.at_ms = uint32_t(std::chrono::duration_cast<
                                  std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() -
                                  g_test_origin)
                                  .count()) +
                     10;
          const char* p = line.c_str();
          char* end = nullptr;
          st.buttons = uint16_t(std::strtoul(p, &end, 16));
          st.hold_ms = 150;
          if (end && *end == ':') {
            st.hold_ms = uint32_t(std::strtoul(end + 1, &end, 10));
            for (int k = 0; k < 6 && end && *end == ':'; ++k) {
              st.analog[k] = int32_t(std::strtol(end + 1, &end, 10));
            }
          }
          XELOGI("HID nop: live pad {:04X} for {} ms", st.buttons,
                 st.hold_ms);
          std::lock_guard<std::mutex> lock(g_test_steps_mutex);
          g_test_steps.push_back(st);
        }
      }
    }).detach();
    return;
  }
  // Phase 1099z156: "@<path>" reads the script from a file (a recording made
  // with --kernel_xinputd_record_path).
  if (!sc.empty() && sc[0] == '@') {
    std::ifstream in(sc.substr(1), std::ios::binary);
    sc.assign(std::istreambuf_iterator<char>(in),
              std::istreambuf_iterator<char>());
  }
  size_t i = 0;
  while (i < sc.size()) {
    size_t comma = sc.find(',', i);
    if (comma == std::string::npos) comma = sc.size();
    std::string item = sc.substr(i, comma - i);
    i = comma + 1;
    size_t c1 = item.find(':');
    if (c1 == std::string::npos) continue;
    size_t c2 = item.find(':', c1 + 1);
    TestPadStep st = {};
    st.at_ms = uint32_t(std::strtoul(item.substr(0, c1).c_str(), nullptr, 10));
    st.buttons = uint16_t(std::strtoul(
        item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos
                                                    : c2 - c1 - 1)
            .c_str(),
        nullptr, 16));
    st.hold_ms = c2 == std::string::npos
                     ? 150u
                     : uint32_t(std::strtoul(item.substr(c2 + 1).c_str(),
                                             nullptr, 10));
    if (c2 != std::string::npos) {
      size_t pos = item.find(':', c2 + 1);
      for (int k = 0; k < 6 && pos != std::string::npos; ++k) {
        st.analog[k] =
            int32_t(std::strtol(item.c_str() + pos + 1, nullptr, 10));
        pos = item.find(':', pos + 1);
      }
    }
    g_test_steps.push_back(st);
  }
  g_test_pad = !sc.empty();
  if (g_test_pad) {
    XELOGI("HID nop: TEST-ONLY scripted pad on slot 0, {} step(s)",
           g_test_steps.size());
  }
}

X_STATUS NopInputDriver::Setup() {
  ParseTestPadScript();
  return X_STATUS_SUCCESS;
}

// TODO(benvanik): spoof a device so that games don't stop waiting for
//     a controller to be plugged in.

X_RESULT NopInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  ParseTestPadScript();
  if (g_test_pad && user_index == 0) {
    // The values XInput reports for a standard wired gamepad.
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = int16_t(0xFFC0);
    out_caps->gamepad.thumb_ly = int16_t(0xFFC0);
    out_caps->gamepad.thumb_rx = int16_t(0xFFC0);
    out_caps->gamepad.thumb_ry = int16_t(0xFFC0);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
    return X_ERROR_SUCCESS;
  }
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT NopInputDriver::GetState(uint32_t user_index,
                                  X_INPUT_STATE* out_state) {
  ParseTestPadScript();
  if (g_test_pad && user_index == 0) {
    static X_INPUT_GAMEPAD last = {};
    static bool have_last = false;
    static uint32_t packet = 0;
    X_INPUT_GAMEPAD g;
    TestPadState(&g);
    if (!have_last || std::memcmp(&g, &last, sizeof(g)) != 0) {
      if (!have_last || uint16_t(g.buttons) != uint16_t(last.buttons)) {
        XELOGI("HID nop: TEST-ONLY scripted pad buttons {:04X}",
               uint16_t(g.buttons));
      }
      have_last = true;
      last = g;
      ++packet;
    }
    out_state->packet_number = packet;
    out_state->gamepad = g;
    return X_ERROR_SUCCESS;
  }
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT NopInputDriver::SetState(uint32_t user_index,
                                  X_INPUT_VIBRATION* vibration) {
  if (g_test_pad && user_index == 0) return X_ERROR_SUCCESS;
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT NopInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

InputType NopInputDriver::GetInputType() const {
  // Phase 1099z168, TEST-ONLY: with a scripted pad the nop driver IS a
  // controller, and InputSystem::FilterDrivers only hands X_INPUT_FLAG_GAMEPAD
  // queries to drivers of that type. Reporting Other hid the scripted pad from
  // every host-side gamepad poll - including the guide_power_on_with_guide_button
  // gate in xenia_main, so a replay of a recorded session could never power the
  // console on. Without a script the driver behaves exactly as before.
  // 1099z169: parse here too - the script used to be read only by GetState,
  // which FilterDrivers never reached while this still said Other.
  ParseTestPadScript();
  return g_test_pad ? InputType::Controller : InputType::Other;
}

}  // namespace nop
}  // namespace hid
}  // namespace xe
