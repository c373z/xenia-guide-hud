/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_FIRMWARE_DIALOG_H_
#define XENIA_APP_FIRMWARE_DIALOG_H_

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "xenia/ui/imgui_dialog.h"

namespace xe {
namespace app {

class EmulatorWindow;

// 2026-09-16: Console > Firmware. Shows the installed firmware and installs
// system updates the way a console does from a USB stick ("Update Console
// to..."). Pre-2010 updates are delta patches on the console's shipped
// firmware (2.0.1888.0), which "Select Firmware Zip..." provides; the update
// is then installed on top of it.
class FirmwareDialog final : public ui::ImGuiDialog {
 public:
  FirmwareDialog(ui::ImGuiDrawer* imgui_drawer,
                 EmulatorWindow& emulator_window);
  ~FirmwareDialog() override;

  // Closes the window and gives up the EmulatorWindow's ownership.
  void Dismiss();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void RefreshState();
  void PickFirmware();
  void PickUpdate();
  void RunUpdate(std::filesystem::path zip);
  void RunFirmware(std::filesystem::path source);
  void StartJob(std::function<void()> job);
  void SetStatus(std::string status, bool error = false);

  EmulatorWindow& emulator_window_;

  // Where things live, fixed when the dialog opens.
  std::filesystem::path exe_dir_;
  std::filesystem::path systems_dir_;
  std::filesystem::path firmware_root_;
  std::filesystem::path fonts_dir_;
  bool updates_folder_mode_ = false;

  std::mutex mutex_;  // guards everything below
  std::string current_build_;   // "17559"
  std::string base_build_;      // "1888", or "" when none is selected
  std::string update_build_;    // the update being installed, "8955"
  std::string needs_base_;      // "2.0.1888.0" while waiting for it
  std::filesystem::path pending_update_;
  std::string status_;
  bool status_error_ = false;

  std::thread worker_;
  std::atomic<bool> busy_{false};
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_FIRMWARE_DIALOG_H_
