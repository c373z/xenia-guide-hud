/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/firmware_dialog.h"

#include <algorithm>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/imgui/imgui.h"

#include "xenia/app/emulator_window.h"
#include "xenia/app/system_update_import.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/config.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/ui/file_picker.h"

DECLARE_bool(guide_system_updates_folder);
DECLARE_uint32(guide_system_build);
DECLARE_string(guide_system_root);
DECLARE_uint32(kernel_build_version);

namespace xe {
namespace app {

namespace fs = std::filesystem;

namespace {

// The console's shipped firmware: every retail console keeps it in flash,
// and pre-2010 updates patch it.
constexpr char kShippedFirmware[] = "2.0.1888.0";

// "2.0.8955.0" -> "8955": the numbers that matter.
std::string BuildOf(const std::string& version) {
  const size_t first = version.find('.');
  const size_t second =
      first == std::string::npos ? first : version.find('.', first + 1);
  if (second == std::string::npos) return version;
  const size_t third = version.find('.', second + 1);
  return version.substr(second + 1, third == std::string::npos
                                        ? std::string::npos
                                        : third - second - 1);
}

template <typename T>
void SetCvar(const char* name, const T& value) {
  auto it = cvar::ConfigVars->find(name);
  auto* var = it == cvar::ConfigVars->end()
                  ? nullptr
                  : dynamic_cast<cvar::ConfigVar<T>*>(it->second);
  if (var) {
    var->OverrideConfigValue(value);
  } else {
    XELOGE("Firmware: no {} setting to set", name);
  }
}

void KeepOnScreen(const ImGuiIO& io) {
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  const ImVec2 clamped(
      std::max(0.0f, std::min(pos.x, io.DisplaySize.x - size.x)),
      std::max(0.0f, std::min(pos.y, io.DisplaySize.y - size.y)));
  if (clamped.x != pos.x || clamped.y != pos.y) {
    ImGui::SetWindowPos(clamped);
  }
}

}  // namespace

FirmwareDialog::FirmwareDialog(ui::ImGuiDrawer* imgui_drawer,
                               EmulatorWindow& emulator_window)
    : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {
  exe_dir_ = xe::filesystem::GetExecutableFolder();
  updates_folder_mode_ = cvars::guide_system_updates_folder;
  if (updates_folder_mode_ || cvars::guide_system_root.empty()) {
    systems_dir_ = exe_dir_ / "systems";
  } else {
    systems_dir_ = xe::to_path(cvars::guide_system_root).parent_path();
  }
  firmware_root_ = exe_dir_ / "firmware";
  // Base .xtt fonts: the portable layout has support\fonts; the research
  // tree's flash set is dashroot beside systems.
  std::error_code ec;
  fonts_dir_ = exe_dir_ / "support" / "fonts";
  if (!fs::is_directory(fonts_dir_, ec)) {
    fonts_dir_ = systems_dir_.parent_path() / "dashroot";
  }
  RefreshState();
}

FirmwareDialog::~FirmwareDialog() {
  if (worker_.joinable()) {
    worker_.join();
  }
}

void FirmwareDialog::Dismiss() {
  Close();
  emulator_window_.ReleaseFirmwareDialog(this);
}

void FirmwareDialog::RefreshState() {
  std::string current;
  auto systems = ListImportedSystems(systems_dir_);
  std::error_code ec;
  for (auto& s : systems) {
    if (updates_folder_mode_) {
      if (!s.complete) continue;
      if (cvars::guide_system_build && s.build != cvars::guide_system_build) {
        continue;
      }
    } else if (cvars::guide_system_root.empty() ||
               !fs::equivalent(s.dir, xe::to_path(cvars::guide_system_root),
                               ec)) {
      continue;
    }
    current = BuildOf(s.xam_version);
    break;
  }
  if (current.empty()) {
    current = fmt::format("{}", cvars::kernel_build_version);
  }
  const bool have_base = !FindBaseFirmware(firmware_root_, kShippedFirmware)
                              .empty();
  std::lock_guard<std::mutex> lock(mutex_);
  current_build_ = current;
  base_build_ = have_base ? BuildOf(kShippedFirmware) : "";
}

void FirmwareDialog::SetStatus(std::string status, bool error) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_ = std::move(status);
  status_error_ = error;
}

void FirmwareDialog::StartJob(std::function<void()> job) {
  if (busy_.exchange(true)) return;
  if (worker_.joinable()) worker_.join();
  worker_ = std::thread([this, job = std::move(job)]() {
    job();
    RefreshState();
    busy_ = false;
  });
}

void FirmwareDialog::PickFirmware() {
  auto picker = ui::FilePicker::Create();
  picker->set_mode(ui::FilePicker::Mode::kOpen);
  picker->set_type(ui::FilePicker::Type::kFile);
  picker->set_multi_selection(false);
  picker->set_title("Select the console's shipped firmware (2.0.1888.0)");
  picker->set_extensions({
      {"Firmware archive (*.zip;*.rar;*.7z)", "*.zip;*.rar;*.7z"},
      {"All Files (*.*)", "*.*"},
  });
  if (!picker->Show(emulator_window_.window()) ||
      picker->selected_files().empty()) {
    return;
  }
  fs::path source = picker->selected_files()[0];
  StartJob([this, source]() { RunFirmware(source); });
}

void FirmwareDialog::PickUpdate() {
  auto picker = ui::FilePicker::Create();
  picker->set_mode(ui::FilePicker::Mode::kOpen);
  picker->set_type(ui::FilePicker::Type::kFile);
  picker->set_multi_selection(false);
  picker->set_title("Update Console to... (a USB system update zip)");
  picker->set_extensions({
      {"System update (*.zip;*.rar;*.7z)", "*.zip;*.rar;*.7z"},
      {"All Files (*.*)", "*.*"},
  });
  if (!picker->Show(emulator_window_.window()) ||
      picker->selected_files().empty()) {
    return;
  }
  fs::path zip = picker->selected_files()[0];
  StartJob([this, zip]() { RunUpdate(zip); });
}

void FirmwareDialog::RunFirmware(fs::path source) {
  SetStatus("Reading the firmware...");
  BaseFirmware base = ImportBaseFirmware(source, firmware_root_);
  if (!base.error.empty()) {
    SetStatus("Firmware not added: " + base.error, true);
    return;
  }
  if (base.version != kShippedFirmware) {
    SetStatus(fmt::format("Added firmware {}, but updates need {}.",
                          BuildOf(base.version), BuildOf(kShippedFirmware)),
              true);
    return;
  }
  fs::path pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending = pending_update_;
    needs_base_.clear();
  }
  if (pending.empty()) {
    SetStatus(fmt::format("Firmware {} is ready.", BuildOf(base.version)));
    return;
  }
  RunUpdate(pending);
}

void FirmwareDialog::RunUpdate(fs::path zip) {
  SetStatus("Reading the update...");
  const SystemUpdateInfo info = InspectSystemUpdate(zip);
  if (!info.error.empty()) {
    SetStatus("Not a system update: " + info.error, true);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    update_build_ = BuildOf(info.version);
  }
  fs::path base;
  if (!info.needs_base.empty()) {
    base = FindBaseFirmware(firmware_root_, info.needs_base);
    if (base.empty()) {
      // Ask for it; the update continues once it has been selected.
      {
        std::lock_guard<std::mutex> lock(mutex_);
        needs_base_ = info.needs_base;
        pending_update_ = zip;
      }
      SetStatus("");
      emulator_window_.app_context().CallInUIThreadDeferred(
          [this]() { PickFirmware(); });
      return;
    }
  }

  // The virtual USB stick: in the updates-folder layout the zip is kept
  // there, like every other imported update.
  fs::path source = zip;
  std::error_code ec;
  if (updates_folder_mode_) {
    fs::path usb = exe_dir_ / "updates" / zip.filename();
    fs::create_directories(usb.parent_path(), ec);
    if (!fs::equivalent(zip, usb, ec)) {
      fs::copy_file(zip, usb, fs::copy_options::overwrite_existing, ec);
      if (!ec) source = usb;
    }
  }

  const fs::path existing = systems_dir_ / fmt::format("{}", info.build);
  if (!fs::exists(existing / "system.json", ec)) {
    SetStatus(fmt::format("Updating console to {}...", BuildOf(info.version)));
    SystemImportResult r =
        ImportSystemUpdate(source, systems_dir_, fonts_dir_, base);
    if (!r.error.empty()) {
      SetStatus("Update failed: " + r.error, true);
      return;
    }
    if (!r.failures.empty()) {
      std::string text = fmt::format(
          "Update {} is missing parts and will not boot:", r.build);
      for (auto& f : r.failures) text += "\n  " + f;
      SetStatus(text, true);
      return;
    }
  }

  // Boot it from now on.
  if (updates_folder_mode_) {
    SetCvar<uint32_t>("guide_system_build", info.build);
  } else {
    SetCvar<std::string>("guide_system_root", xe::path_to_utf8(existing));
    SetCvar<fs::path>("guide_boot_target", existing / "dash.xex");
    SetCvar<uint32_t>("kernel_build_version", info.build);
    SetCvar<fs::path>("kernel_system_ext_path",
                      fs::is_directory(existing / "_sysext", ec)
                          ? existing / "_sysext"
                          : fs::path());
    SetCvar<std::string>("lle_xam", std::string("SYS:\\xam.xex"));
    SetCvar<std::string>("guide_cold_boot_path",
                         std::string("SYS:\\bootanim.xex"));
  }
  config::SaveConfig();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_update_.clear();
  }
  SetStatus(fmt::format("Console updated to {}. Restart the emulator to boot "
                        "it.",
                        BuildOf(info.version)));
}

void FirmwareDialog::OnDraw(ImGuiIO& io) {
  std::string current, base, update, needs, status;
  bool status_error;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    current = current_build_;
    base = base_build_;
    update = update_build_;
    needs = needs_base_;
    status = status_;
    status_error = status_error_;
  }
  const bool busy = busy_;
  const ImVec4 red(0.95f, 0.30f, 0.30f, 1.0f);

  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
      ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (ImGui::Begin("Firmware", &open,
                   ImGuiWindowFlags_NoCollapse |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    KeepOnScreen(io);
    const ImVec2 button(220, 0);
    const ImVec2 field(160, 0);

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Select Firmware Zip...", button)) {
      emulator_window_.app_context().CallInUIThreadDeferred(
          [this]() { PickFirmware(); });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    // Greyed out: what the console runs now. Red while an update is waiting
    // for the shipped firmware.
    if (!needs.empty()) ImGui::PushStyleColor(ImGuiCol_Text, red);
    ImGui::BeginDisabled(true);
    ImGui::Button(fmt::format("{}##current", current).c_str(), field);
    ImGui::EndDisabled();
    if (!needs.empty()) ImGui::PopStyleColor();

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Update Console to...", button)) {
      emulator_window_.app_context().CallInUIThreadDeferred(
          [this]() { PickUpdate(); });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(true);
    ImGui::Button(update.empty()
                      ? "USB Update##update"
                      : fmt::format("USB Update {}##update", update).c_str(),
                  field);
    ImGui::EndDisabled();

    ImGui::TextDisabled("Shipped firmware: %s",
                        base.empty() ? "not selected" : base.c_str());

    if (!needs.empty()) {
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, red);
      ImGui::TextUnformatted(
          fmt::format("WARNING! This update needs system firmware {} to run.",
                      needs)
              .c_str());
      ImGui::PopStyleColor();
      ImGui::TextUnformatted(
          "Please select a zip of the console's shipped build.");
    }
    if (busy) {
      ImGui::Spacing();
      ImGui::TextUnformatted(status.empty() ? "Working..." : status.c_str());
    } else if (!status.empty()) {
      ImGui::Spacing();
      if (status_error) ImGui::PushStyleColor(ImGuiCol_Text, red);
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380);
      ImGui::TextUnformatted(status.c_str());
      ImGui::PopTextWrapPos();
      if (status_error) ImGui::PopStyleColor();
    }
    ImGui::Separator();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Close")) open = false;
    ImGui::EndDisabled();
  }
  ImGui::End();
  if (!open && !busy) {
    Dismiss();
  }
}

}  // namespace app
}  // namespace xe
