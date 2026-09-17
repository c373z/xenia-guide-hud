/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_SYSTEM_UPDATE_IMPORT_H_
#define XENIA_APP_SYSTEM_UPDATE_IMPORT_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xe {
namespace app {

// 2026-09-16: the emulator's own port of research/import_system_update.py.
// An official Xbox 360 SystemUpdate zip placed in <exe>\updates is expanded
// into <exe>\systems\<build> (flash files, loose hard-drive files, _packages,
// _update, _sysext, system.json) the first time the exe is opened, so a
// portable build needs no Python and no scripts.
struct SystemImportResult {
  std::filesystem::path zip;
  std::filesystem::path system_dir;  // empty when the build was not known
  uint32_t build = 0;
  std::string xam_version;
  // Named payloads that could not be read or are missing. A system with any
  // failure is not booted automatically.
  std::vector<std::string> failures;
  // Problems that do not stop the system booting (a delta patch whose base
  // module the base firmware does not have, e.g. 2.0.8955's aac.xexp).
  std::vector<std::string> warnings;
  // Why the whole zip was refused (not a full update, delta-patch xam, ...).
  std::string error;
};

// Imports every *.zip in updates_dir that no systems_dir\*\system.json names
// as its source. fonts_dir holds the base .xtt fonts an update does not carry.
// firmware_root (optional) holds base firmwares (see ImportBaseFirmware); an
// update that ships delta patches is imported on the matching one, and is
// refused as before when there is none.
std::vector<SystemImportResult> ImportPendingSystemUpdates(
    const std::filesystem::path& updates_dir,
    const std::filesystem::path& systems_dir,
    const std::filesystem::path& fonts_dir,
    const std::filesystem::path& firmware_root = {});

// 2026-09-16: pre-2010 updates (2.0.8955 and older) ship most flash modules
// as $flash_*.xexp delta patches against the console's shipped flash
// (2.0.1888.0), which every console keeps. What a zip needs, read without
// importing it.
struct SystemUpdateInfo {
  uint32_t build = 0;
  std::string version;       // "2.0.8955.0"
  std::string needs_base;    // "2.0.1888.0" for a delta-patch update, else ""
  std::string error;         // not a readable system update
};
SystemUpdateInfo InspectSystemUpdate(const std::filesystem::path& zip);

// A base firmware: the console's shipped flash files (xam.xex, hud.xex, ...),
// from a zip, a folder, or a .rar (read with 7-Zip when it is installed).
// Copied to firmware_root\<version>\ (e.g. firmware\2.0.1888.0).
struct BaseFirmware {
  std::filesystem::path dir;
  std::string version;
  std::string error;
};
BaseFirmware ImportBaseFirmware(const std::filesystem::path& source,
                                const std::filesystem::path& firmware_root);
// firmware_root\<version> when it holds that firmware's xam.xex, else empty.
std::filesystem::path FindBaseFirmware(
    const std::filesystem::path& firmware_root, const std::string& version);

// Imports one update zip into systems_dir\<build>. base_firmware_dir is the
// folder FindBaseFirmware returned (empty when the update needs none).
SystemImportResult ImportSystemUpdate(
    const std::filesystem::path& zip, const std::filesystem::path& systems_dir,
    const std::filesystem::path& fonts_dir,
    const std::filesystem::path& base_firmware_dir);

struct ImportedSystem {
  std::filesystem::path dir;
  uint32_t build = 0;
  std::string xam_version;
  bool has_sysext = false;
  bool complete = false;  // no failures recorded
  std::filesystem::file_time_type imported;
};

// Every systems_dir\<build> with a readable system.json, newest import first.
std::vector<ImportedSystem> ListImportedSystems(
    const std::filesystem::path& systems_dir);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_SYSTEM_UPDATE_IMPORT_H_
