/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xconfig.h"

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_flags.h"

#include "xenia/base/filesystem.h"

#include "xenia/base/byte_order.h"
#include "xenia/base/string_util.h"
#include "xenia/xbox.h"

#include <ranges>

namespace xe {
namespace kernel {

// Phase 1099z76: where the 0x1F0 XNET_PARAMETERS record is persisted.
static std::filesystem::path SideRecordPath(const std::filesystem::path& p) {
  return p.parent_path() / "xconfig_xnet_parameters.bin";
}

XConfig::XConfig(const std::filesystem::path& xconfig_path)
    : file_path_(xconfig_path) {
  // Phase 1099z165: "no xconfig.settings" is this emulated console's
  // out-of-the-box state - nothing has ever been set up in this folder. That
  // is the only point --kernel_oobe_on_fresh_console acts at, so a console
  // that already has state can never be pushed back into OOBE by it.
  const bool fresh_console = !std::filesystem::exists(xconfig_path);
  if (fresh_console) {
    SetDefaults();
    if (cvars::kernel_oobe_on_fresh_console) {
      ApplyOobeState();
      XELOGI(
          "XConfig: fresh console and --kernel_oobe_on_fresh_console - "
          "DashboardInitialized cleared, the dashboard will run its "
          "out-of-box experience on this first launch");
    }

    if (xe::filesystem::CreateEmptyFile(xconfig_path)) {
      FlushToFile();
    }
  }

  FILE* file = xe::filesystem::OpenFile(xconfig_path, "rb");

  if (!file) {
    SetDefaults();
    return;
  }

  // A file written before ConsoleExt was appended is short; the tail keeps its
  // zero default (fread stops at end of file).
  std::memset(&xconfig_data_, 0, sizeof(XConfigData));
  fread(&xconfig_data_, 1, sizeof(XConfigData), file);
  fclose(file);

  if (FILE* side = xe::filesystem::OpenFile(SideRecordPath(xconfig_path), "rb")) {
    fread(xnet_parameters_record_.data(), 1, xnet_parameters_record_.size(),
          side);
    fclose(side);
  }

  // TEST-ONLY: re-run OOBE on a console that has already been set up. Not
  // written back here, but any later guest xconfig write persists the whole
  // block - see the flag's description.
  if (cvars::kernel_oobe_force) {
    ApplyOobeState();
    XELOGW(
        "XConfig: --kernel_oobe_force - DashboardInitialized cleared in "
        "memory; the dashboard will run its out-of-box experience");
  }
}

void XConfig::ApplyOobeState() {
  // MEASURED 1099z165 (runs oobe_base / oobe_clear / oobe_drive, frames
  // looked at): with bit 0x40 of XCONFIG_USER_RETAIL_FLAGS clear, the retail
  // 17559 dashboard loads its 'oobe' section instead of dashmain/hubui/... and
  // renders the out-of-box "press the Guide button" screen.
  xconfig_data_.user.retail_flags =
      xconfig_data_.user.retail_flags.get() &
      ~static_cast<uint32_t>(X_RETAIL_FLAGS::DashboardInitialized);
  // A console out of the box has shown no first-use tutorial either.
  xconfig_data_.console_ext.dash_first_use_tutorial_flags = 0;
}

void XConfig::ResetXnetCategoryIfStale(X_CONFIG_CATEGORY category) {
  if (category != X_CONFIG_CATEGORY::XCONFIG_XNET_MACHINE_ACCOUNT_CATEGORY &&
      category != X_CONFIG_CATEGORY::XCONFIG_XNET_PARAMETERS_CATEGORY) {
    return;
  }
  uint8_t* base = CategoryBase(category);
  if (xe::load_and_swap<uint32_t>(base) == 1) {
    return;
  }
  std::memset(base, 0, 0x1F0);
  xe::store_and_swap<uint32_t>(base, 1);
  XELOGI("XConfig: category {} version was not 1 - reset to version 1 with "
         "zero data (real kernel 80087AC0)",
         static_cast<int>(category));
  FlushToFile();
}

void XConfig::ReadSetting(const X_CONFIG_CATEGORY category,
                          const uint16_t setting_id, void* buffer) {
  const auto setting = FindField(category, setting_id);

  if (!setting) {
    return;
  }

  std::lock_guard<xe_mutex> lock(lock_);
  ResetXnetCategoryIfStale(category);
  std::memcpy(buffer, FieldBase(category, *setting) + setting->block_offset,
              setting->size);
}

void XConfig::WriteSetting(const X_CONFIG_CATEGORY category,
                           const uint16_t setting_id, const void* buffer) {
  const auto setting = FindField(category, setting_id);

  if (!setting) {
    return;
  }

  std::lock_guard<xe_mutex> lock(lock_);
  ResetXnetCategoryIfStale(category);
  std::memcpy(FieldBase(category, *setting) + setting->block_offset, buffer,
              setting->size);

  FlushToFile();
}

uint8_t* XConfig::FieldBase(X_CONFIG_CATEGORY category,
                            const FieldDescriptor& field) {
  if (field.ext) {
    return reinterpret_cast<uint8_t*>(&xconfig_data_.console_ext);
  }
  return CategoryBase(category);
}

uint16_t XConfig::GetSettingSize(const X_CONFIG_CATEGORY category,
                                 const uint16_t setting_id) {
  const auto setting = FindField(category, setting_id);

  if (!setting) {
    return 0;
  }

  return setting->size;
}

void XConfig::WriteXConfig(const XConfigData* data) {
  xconfig_data_ = *data;
  FlushToFile();
}

void XConfig::SetDefaults() {
  xconfig_data_ = {};

  xconfig_data_.secured.av_region = X_AV_REGION::NTSCM;
  xconfig_data_.user.language = static_cast<uint32_t>(XLanguage::kEnglish);
  xconfig_data_.user.country =
      static_cast<uint8_t>(XOnlineCountry::kUnitedStates);
  xconfig_data_.user.audio_flags = DolbyDigital | DolbyProLogic;
  xconfig_data_.user.av_pack_hdmi_sz = XHDTVResolution.at(1).to_host();
  xconfig_data_.user.av_pack_component_sz = XHDTVResolution.at(1).to_host();
  xconfig_data_.user.av_pack_vga_sz = XVGAResolution.at(3).to_host();
  // Phase 1099z11: signin.xex (9011B5F0) asks for Xbox Live privacy/update
  // consent on a dashboard sign-in whenever bit 0x4 is clear, network or not;
  // accepting writes 0x4 | 0x10000000 (9011B650). A console that has been
  // set up has accepted, so a fresh config starts that way.
  xconfig_data_.user.retail_flags =
      DashboardInitialized | LiveConsentAccepted | LiveConsentAccepted2;
  xconfig_data_.user.video_flags = RatioNormal;
  xconfig_data_.user.parental_control_flags =
      XBLAllowed | XBLMembershipCreationAllowed;
  xconfig_data_.user.parental_control_game = NoGameRestrictions;
  xconfig_data_.user.music_volume = 0.7f;

  const auto& tz = kTimezones[0x19];

  xconfig_data_.user.time_zone_bias = tz.timezone_bias;
  memcpy(xconfig_data_.user.tz_std_name.data(), tz.tz_std_name.data(), 4);
  memcpy(xconfig_data_.user.tz_dlt_name.data(), tz.tz_dlt_name.data(), 4);
  memcpy(xconfig_data_.user.tz_std_date.data(), tz.tz_std_date.data(), 4);
  memcpy(xconfig_data_.user.tz_dlt_date.data(), tz.tz_dlt_date.data(), 4);
  xconfig_data_.user.tz_std_bias = tz.tz_std_bias;
  xconfig_data_.user.tz_dlt_bias = tz.tz_dlt_bias;

  xe::string_util::copy_and_swap_truncating(
      xconfig_data_.iptv.service_provider_name.data(), u"Xenia TV",
      xconfig_data_.iptv.service_provider_name.size());
}

void XConfig::FlushToFile() {
  if (!std::filesystem::exists(file_path_)) {
    return;
  }

  FILE* file = xe::filesystem::OpenFile(file_path_, "wb");
  if (!file) {
    return;
  }

  fwrite(&xconfig_data_, 1, sizeof(XConfigData), file);
  fclose(file);

  if (FILE* side = xe::filesystem::OpenFile(SideRecordPath(file_path_), "wb")) {
    fwrite(xnet_parameters_record_.data(), 1, xnet_parameters_record_.size(),
           side);
    fclose(side);
  }
}

const XConfig::FieldDescriptor* XConfig::FindField(X_CONFIG_CATEGORY category,
                                                   uint16_t setting) {
  auto it = std::ranges::find_if(kFields, [&](const FieldDescriptor& f) {
    return f.category == category && f.setting == setting;
  });

  return it == std::ranges::end(kFields) ? nullptr : &*it;
}

uint8_t* XConfig::CategoryBase(X_CONFIG_CATEGORY category) {
  switch (category) {
    case X_CONFIG_CATEGORY::XCONFIG_STATIC_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.static_settings);
    case X_CONFIG_CATEGORY::XCONFIG_STATISTIC_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.statistic);
    case X_CONFIG_CATEGORY::XCONFIG_SECURED_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.secured);
    case X_CONFIG_CATEGORY::XCONFIG_USER_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.user);
    case X_CONFIG_CATEGORY::XCONFIG_XNET_MACHINE_ACCOUNT_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.xnet_machine_account);
    case X_CONFIG_CATEGORY::XCONFIG_XNET_PARAMETERS_CATEGORY:
      return xnet_parameters_record_.data();
    case X_CONFIG_CATEGORY::XCONFIG_MEDIA_CENTER_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.media_center);
    case X_CONFIG_CATEGORY::XCONFIG_CONSOLE_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.console);
    case X_CONFIG_CATEGORY::XCONFIG_DVD_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.dvd);
    case X_CONFIG_CATEGORY::XCONFIG_IPTV_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.iptv);
    case X_CONFIG_CATEGORY::XCONFIG_SYSTEM_CATEGORY:
      return reinterpret_cast<uint8_t*>(&xconfig_data_.system);
  }
  return nullptr;
}

const uint8_t* XConfig::CategoryBase(X_CONFIG_CATEGORY category) const {
  return const_cast<XConfig*>(this)->CategoryBase(category);
}

}  // namespace kernel
}  // namespace xe
