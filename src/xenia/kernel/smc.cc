/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/smc.h"
#include <algorithm>
#include <cstring>
#include "xenia/base/threading.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xthread.h"

DECLARE_int32(avpack);

namespace xe {
namespace kernel {

SystemManagementController::SystemManagementController()
    : dvd_tray_state_(X_DVD_TRAY_STATE::CLOSED) {
  auto registerQuery =
      [&](X_SMC_CMD command,
          void (SystemManagementController::*fn)(X_SMC_DATA*, X_SMC_DATA*)) {
        smc_commands_[command] = [this, fn](X_SMC_DATA* message,
                                            X_SMC_DATA* response) {
          (this->*fn)(message, response);
        };
      };

  registerQuery(X_SMC_CMD::QUERY_TEMP_SENSOR,
                &SystemManagementController::QueryTemperatureSensor);
  registerQuery(X_SMC_CMD::QUERY_TRAY,
                &SystemManagementController::QueryDriveTraySensor);
  registerQuery(X_SMC_CMD::QUERY_AV_PACK,
                &SystemManagementController::QueryAvPack);
  registerQuery(X_SMC_CMD::QUERY_SMC_VERSION,
                &SystemManagementController::QuerySmcVersion);
  registerQuery(X_SMC_CMD::QUERY_IR_ADDRESS,
                &SystemManagementController::QueryIRAddress);
  registerQuery(X_SMC_CMD::QUERY_TILT_SENSOR,
                &SystemManagementController::QueryTiltState);
  registerQuery(X_SMC_CMD::SET_FAN_SPEED_CPU,
                &SystemManagementController::SetFanSpeed);
  registerQuery(X_SMC_CMD::SET_FAN_SPEED_GPU,
                &SystemManagementController::SetFanSpeed);
  registerQuery(X_SMC_CMD::SET_DVD_TRAY,
                &SystemManagementController::SetDriveTray);
  registerQuery(X_SMC_CMD::SET_IR_ADDRESS,
                &SystemManagementController::SetIRAddress);
  registerQuery(X_SMC_CMD::SET_POWER_LED,
                &SystemManagementController::SetPowerLed);
  registerQuery(X_SMC_CMD::SET_LEDS, &SystemManagementController::SetLedState);
};
SystemManagementController::~SystemManagementController() {};

void SystemManagementController::SetTrayState(X_DVD_TRAY_STATE state) {
  dvd_tray_state_ = state;
  kernel_state()->BroadcastNotification(kXNotificationSystemTrayStateChanged,
                                        static_cast<uint8_t>(state));
}

void SystemManagementController::CallCommand(X_SMC_DATA* smc_message,
                                             X_SMC_DATA* smc_response) {
  const auto itr = smc_commands_.find(smc_message->command);
  if (itr == smc_commands_.cend()) {
    XELOGW("Unimplemented SMC Command: {:02X}",
           static_cast<uint8_t>(smc_message->command));
    return;
  }

  itr->second(smc_message, smc_response);
}

void SystemManagementController::QueryTemperatureSensor(
    X_SMC_DATA* smc_message, X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->temps.cpu.SetTemp(69.6f);
  smc_response->temps.gpu.SetTemp(69.9f);
  smc_response->temps.edram.SetTemp(69.6f);
  smc_response->temps.mb.SetTemp(69.9f);
}

void SystemManagementController::QueryDriveTraySensor(
    X_SMC_DATA* smc_message, X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  // Phase 1099z12: the SMC reports 0x60 | state (xam 8176D7F4-8176D80C
  // compares 0x60 open, 0x62 closed, 0x63 opening, 0x64 closing). The raw
  // enum never matched, so xam never learned the tray state.
  smc_response->dvd_tray.state =
      static_cast<X_DVD_TRAY_STATE>(0x60 | static_cast<uint8_t>(dvd_tray_state_));
  static uint32_t query_count = 0;
  if (++query_count <= 40) {
    XELOGI("SMC QUERY_TRAY #{} -> {:02X}", query_count,
           0x60 | static_cast<uint8_t>(dvd_tray_state_));
  }
};

void SystemManagementController::RegisterNotification(uint32_t record,
                                                      bool add) {
  std::lock_guard<std::mutex> lock(notification_lock_);
  auto it = std::find(notification_records_.begin(),
                      notification_records_.end(), record);
  if (add && it == notification_records_.end()) {
    notification_records_.push_back(record);
  } else if (!add && it != notification_records_.end()) {
    notification_records_.erase(it);
  }
  XELOGI("HalRegisterSMCNotification({:08X}, {}) -> {} record(s)", record, add,
         notification_records_.size());
}

void SystemManagementController::DispatchNotification(uint8_t event_code) {
  auto* ks = kernel_state();
  auto* thread = XThread::GetCurrentThread();
  if (!ks || !thread) {
    return;
  }
  std::vector<uint32_t> records;
  {
    std::lock_guard<std::mutex> lock(notification_lock_);
    records = notification_records_;
  }
  auto* mem = ks->memory();
  uint32_t msg = mem->SystemHeapAlloc(sizeof(X_SMC_DATA));
  std::memset(mem->TranslateVirtual(msg), 0, sizeof(X_SMC_DATA));
  auto* bytes = mem->TranslateVirtual<uint8_t*>(msg);
  bytes[0] = 0x83;  // SMC interrupt: tray event
  bytes[1] = event_code;
  for (uint32_t record : records) {
    uint32_t routine =
        xe::load_and_swap<uint32_t>(mem->TranslateVirtual(record));
    XELOGI("SMC tray event {:02X} -> {:08X}({:08X})", event_code, routine,
           record);
    uint64_t args[] = {record, msg};
    ks->processor()->Execute(thread->thread_state(), routine, args, 2);
  }
  mem->SystemHeapFree(msg);
}

void SystemManagementController::MoveTray(bool open) {
  if (tray_moving_.exchange(true)) {
    XELOGW("SMC: tray already moving, ignoring {} request",
           open ? "open" : "close");
    return;
  }
  auto* ks = kernel_state();
  auto mover = object_ref<XHostThread>(new XHostThread(
      ks, 256 * 1024, 0,
      [this, open]() -> int {
        const auto moving =
            open ? X_DVD_TRAY_STATE::OPENING : X_DVD_TRAY_STATE::CLOSING;
        const auto done =
            open ? X_DVD_TRAY_STATE::OPEN : X_DVD_TRAY_STATE::CLOSED;
        SetTrayState(moving);
        DispatchNotification(0x60 | static_cast<uint8_t>(moving));
        // Tray motion. Not measured on hardware; about a second.
        xe::threading::Sleep(std::chrono::milliseconds(1200));
        if (tray_hook_) {
          tray_hook_(open);
        }
        SetTrayState(done);
        DispatchNotification(0x60 | static_cast<uint8_t>(done));
        tray_moving_ = false;
        return 0;
      },
      ks->GetSystemProcess()));
  mover->set_name("SMC Tray");
  mover->Create();
}

void SystemManagementController::QueryAvPack(X_SMC_DATA* smc_message,
                                             X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->av_pack.av_pack = 0;

  const auto entry = av_pack_to_smc_value.find(cvars::avpack);
  if (entry == av_pack_to_smc_value.cend()) {
    return;
  }

  smc_response->av_pack.av_pack = entry->second;
}

void SystemManagementController::QuerySmcVersion(X_SMC_DATA* smc_message,
                                                 X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->smc_version.unk = smc_version[0];
  smc_response->smc_version.major = smc_version[1];
  smc_response->smc_version.minor = smc_version[2];
}

void SystemManagementController::QueryIRAddress(X_SMC_DATA* smc_message,
                                                X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->ir_address.ir_address = ir_address_;
}

void SystemManagementController::QueryTiltState(X_SMC_DATA* smc_message,
                                                X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->tilt_state.tilt_state = tilt_state_;
}

void SystemManagementController::SetIRAddress(X_SMC_DATA* smc_message,
                                              X_SMC_DATA* smc_response) {
  if (!smc_response) {
    return;
  }

  smc_response->command = smc_message->command;
  smc_response->ir_address.ir_address = ir_address_;
}

void SystemManagementController::SetDriveTray(X_SMC_DATA* smc_message,
                                              X_SMC_DATA* smc_response) {
  SetTrayState(
      static_cast<X_DVD_TRAY_STATE>((smc_message->smc_data[0] & 0xF) % 5));
}

void SystemManagementController::SetFanSpeed(X_SMC_DATA* smc_message,
                                             X_SMC_DATA* smc_response) {
  if (smc_message->command == X_SMC_CMD::SET_FAN_SPEED_CPU) {
    cpu_fan_speed_ = (smc_message->smc_data[0] - 0x80);
  }
  if (smc_message->command == X_SMC_CMD::SET_FAN_SPEED_GPU) {
    gpu_fan_speed_ = (smc_message->smc_data[0] - 0x80);
  }
}

void SystemManagementController::SetPowerLed(X_SMC_DATA* smc_message,
                                             X_SMC_DATA* smc_response) {
  power_led_state_ = {smc_message->power_led_state.state,
                      smc_message->power_led_state.animate};
}

void SystemManagementController::SetLedState(X_SMC_DATA* smc_message,
                                             X_SMC_DATA* smc_response) {
  led_state_ = {smc_message->led_state.state, smc_message->led_state.region};
}

}  // namespace kernel
}  // namespace xe
