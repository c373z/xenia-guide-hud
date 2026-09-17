/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_HOST_PATH_DEVICE_H_
#define XENIA_VFS_DEVICES_HOST_PATH_DEVICE_H_

#include <string>

#include "xenia/vfs/device.h"

namespace xe {
namespace vfs {

class HostPathEntry;

class HostPathDevice : public Device {
 public:
  HostPathDevice(const std::string_view mount_path,
                 const std::filesystem::path& host_path, bool read_only);
  ~HostPathDevice() override;

  bool Initialize() override;
  void Dump(StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  bool is_read_only() const override { return read_only_; }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }

  uint32_t total_allocation_units() const override;
  uint32_t available_allocation_units() const override;
  uint32_t sectors_per_allocation_unit() const override {
    return model_bytes_ ? 32 : 1;
  }
  uint32_t bytes_per_sector() const override { return 0x200; }

  // Phase 1099z159: report the volume as a drive of this many bytes (FATX
  // geometry: 512-byte sectors, 16 KB clusters), with free space = the host
  // folder's free space capped at that size. 0 = Xenia's fixed 64 MB answer.
  void set_model_bytes(uint64_t bytes) { model_bytes_ = bytes; }

  // 2026-09-16: the entry tree is read from the host folder once, at
  // Initialize, so files the host writes afterwards (the game library
  // installer) are invisible to the guest until the next mount. Add entries for
  // every host file and folder under `entry` (an entry of this device) that the
  // tree does not have yet. Additive only: nothing is removed or re-read.
  void RefreshFromHost(Entry* entry);

 protected:
  friend class HostPathEntry;
  std::filesystem::path host_path() const { return host_path_; }

 private:
  void PopulateEntry(HostPathEntry* parent_entry);

  std::string name_;
  std::filesystem::path host_path_;
  std::unique_ptr<Entry> root_entry_;
  bool read_only_;
  uint64_t model_bytes_ = 0;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_HOST_PATH_DEVICE_H_
