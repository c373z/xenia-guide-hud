/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/host_path_device.h"

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/kernel/xfile.h"
#include "xenia/vfs/devices/host_path_entry.h"

namespace xe {
namespace vfs {

HostPathDevice::HostPathDevice(const std::string_view mount_path,
                               const std::filesystem::path& host_path,
                               bool read_only)
    : Device(mount_path),
      name_("STFS"),
      host_path_(host_path),
      read_only_(read_only) {}

HostPathDevice::~HostPathDevice() = default;

uint32_t HostPathDevice::total_allocation_units() const {
  if (!model_bytes_) return 128 * 1024;
  const uint64_t unit = uint64_t(sectors_per_allocation_unit()) * bytes_per_sector();
  return uint32_t(std::min<uint64_t>(model_bytes_ / unit, UINT32_MAX));
}

uint32_t HostPathDevice::available_allocation_units() const {
  if (!model_bytes_) return 128 * 1024;
  std::error_code ec;
  const auto space = std::filesystem::space(host_path_, ec);
  const uint64_t free_bytes =
      ec ? 0 : std::min<uint64_t>(space.available, model_bytes_);
  const uint64_t unit = uint64_t(sectors_per_allocation_unit()) * bytes_per_sector();
  return uint32_t(std::min<uint64_t>(free_bytes / unit, UINT32_MAX));
}

bool HostPathDevice::Initialize() {
  if (!std::filesystem::exists(host_path_)) {
    if (!read_only_) {
      // Create the path.
      std::filesystem::create_directories(host_path_);
    } else {
      XELOGE("Host path does not exist");
      return false;
    }
  }

  auto root_entry = new HostPathEntry(this, nullptr, "", host_path_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);
  PopulateEntry(root_entry);

  return true;
}

void HostPathDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* HostPathDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  XELOGFS("HostPathDevice::ResolvePath({})", path);
  return root_entry_->ResolvePath(path);
}

void HostPathDevice::PopulateEntry(HostPathEntry* parent_entry) {
  auto child_infos = xe::filesystem::ListFiles(parent_entry->host_path());
  for (auto& child_info : child_infos) {
    auto child = HostPathEntry::Create(
        this, parent_entry, parent_entry->host_path() / child_info.name,
        child_info);
    parent_entry->children_.push_back(std::unique_ptr<Entry>(child));

    if (child_info.type == xe::filesystem::FileInfo::Type::kDirectory) {
      PopulateEntry(child);
    }
  }
}

void HostPathDevice::RefreshFromHost(Entry* entry) {
  if (!entry || entry->device() != this ||
      !(entry->attributes() & kFileAttributeDirectory)) {
    return;
  }
  auto global_lock = global_critical_region_.Acquire();
  auto* parent_entry = static_cast<HostPathEntry*>(entry);
  auto child_infos = xe::filesystem::ListFiles(parent_entry->host_path());
  for (auto& child_info : child_infos) {
    const bool is_directory =
        child_info.type == xe::filesystem::FileInfo::Type::kDirectory;
    if (auto* existing =
            parent_entry->GetChild(xe::path_to_utf8(child_info.name))) {
      if (is_directory) {
        RefreshFromHost(existing);
      }
      continue;
    }
    auto child = HostPathEntry::Create(
        this, parent_entry, parent_entry->host_path() / child_info.name,
        child_info);
    parent_entry->children_.push_back(std::unique_ptr<Entry>(child));
    if (is_directory) {
      PopulateEntry(child);
    }
  }
}

}  // namespace vfs
}  // namespace xe
