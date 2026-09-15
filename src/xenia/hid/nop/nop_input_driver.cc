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
struct TestPadStep {
  uint32_t at_ms;
  uint16_t buttons;
  uint32_t hold_ms;
};
std::vector<TestPadStep> g_test_steps;
bool g_test_pad = false;
std::chrono::steady_clock::time_point g_test_origin;

uint16_t TestPadButtons() {
  const auto now_ms = uint32_t(std::chrono::duration_cast<
                                   std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() -
                                   g_test_origin)
                                   .count());
  uint16_t b = 0;
  for (const auto& s : g_test_steps) {
    if (now_ms >= s.at_ms && now_ms < s.at_ms + s.hold_ms) b |= s.buttons;
  }
  return b;
}
}  // namespace

NopInputDriver::NopInputDriver(xe::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {
  g_test_origin = std::chrono::steady_clock::now();
}

NopInputDriver::~NopInputDriver() = default;

// The app creates the nop driver without calling Setup(), so the script is
// parsed on first use; the clock starts at driver construction.
static void ParseTestPadScript() {
  static bool parsed = false;
  if (parsed) return;
  parsed = true;
  const std::string& sc = cvars::hid_test_pad_script;
  size_t i = 0;
  while (i < sc.size()) {
    size_t comma = sc.find(',', i);
    if (comma == std::string::npos) comma = sc.size();
    std::string item = sc.substr(i, comma - i);
    i = comma + 1;
    size_t c1 = item.find(':');
    if (c1 == std::string::npos) continue;
    size_t c2 = item.find(':', c1 + 1);
    TestPadStep st;
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
    static uint16_t last = 0xFFFF;
    static uint32_t packet = 0;
    const uint16_t b = TestPadButtons();
    if (b != last) {
      last = b;
      ++packet;
      XELOGI("HID nop: TEST-ONLY scripted pad buttons {:04X}", b);
    }
    out_state->packet_number = packet;
    out_state->gamepad = {};
    out_state->gamepad.buttons = b;
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

InputType NopInputDriver::GetInputType() const { return InputType::Other; }

}  // namespace nop
}  // namespace hid
}  // namespace xe
