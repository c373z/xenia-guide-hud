/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/virtual_file_system.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/vfs/devices/xcontent_container_device.h"

#include "devices/host_path_entry.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/kernel/xfile.h"

namespace xe {
namespace vfs {

using namespace xe::literals;

VirtualFileSystem::VirtualFileSystem() {}

VirtualFileSystem::~VirtualFileSystem() {
  // Delete all devices.
  // This will explode if anyone is still using data from them.
  Clear();
}

void VirtualFileSystem::Clear() {
  devices_.clear();
  symlinks_.clear();
}

bool VirtualFileSystem::RegisterDevice(std::unique_ptr<Device> device) {
  auto global_lock = global_critical_region_.Acquire();
  devices_.emplace_back(std::move(device));
  return true;
}

bool VirtualFileSystem::UnregisterDevice(const std::string_view path) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto it = devices_.begin(); it != devices_.end(); ++it) {
    if ((*it)->mount_path() == path) {
      XELOGD("Unregistered device: {}", (*it)->mount_path());
      devices_.erase(it);
      return true;
    }
  }
  return false;
}

bool VirtualFileSystem::RegisterSymbolicLink(const std::string_view path,
                                             const std::string_view target) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = std::ranges::find_if(std::as_const(symlinks_), [&](const auto& s) {
    return xe::utf8::equal_case(path, s.first);
  });
  if (it != symlinks_.end()) {
    XELOGE("Trying to re-register already registered symbolic link: {} => {}",
           path, target);
    return false;
  }

  symlinks_.insert({std::string(path), std::string(target)});
  XELOGD("Registered symbolic link: {} => {}", path, target);

  return true;
}

bool VirtualFileSystem::UnregisterSymbolicLink(const std::string_view path) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = std::ranges::find_if(std::as_const(symlinks_), [&](const auto& s) {
    return xe::utf8::equal_case(path, s.first);
  });
  if (it == symlinks_.end()) {
    return false;
  }
  XELOGD("Unregistered symbolic link: {} => {}", it->first, it->second);

  symlinks_.erase(it);
  return true;
}

bool VirtualFileSystem::IsSymbolicLinkRegistered(const std::string_view path) {
  auto it = std::ranges::find_if(std::as_const(symlinks_), [&](const auto& s) {
    return xe::utf8::equal_case(path, s.first);
  });

  return it != symlinks_.cend();
}

bool VirtualFileSystem::FindSymbolicLink(const std::string_view path,
                                         std::string& target) {
  auto it = std::ranges::find_if(std::as_const(symlinks_), [&](const auto& s) {
    return xe::utf8::starts_with_case(path, s.first);
  });
  if (it == symlinks_.cend()) {
    return false;
  }
  target = (*it).second;
  return true;
}

bool VirtualFileSystem::ResolveSymbolicLink(const std::string_view path,
                                            std::string& result) {
  result = path;
  bool was_resolved = false;
  while (true) {
    auto it =
        std::ranges::find_if(std::as_const(symlinks_), [&](const auto& s) {
          return xe::utf8::starts_with_case(result, s.first);
        });
    if (it == symlinks_.cend()) {
      break;
    }
    // Found symlink!
    auto target_path = (*it).second;
    auto relative_path = result.substr((*it).first.size());
    result = target_path + relative_path;
    was_resolved = true;
  }
  return was_resolved;
}

bool VirtualFileSystem::TranslateSymbolicLinks(const std::string_view path,
                                               std::string& out) {
  auto global_lock = global_critical_region_.Acquire();
  std::string normalized(xe::utf8::canonicalize_guest_path(path));
  // ObCreateSymbolicLink registers "\??\name:" and "\System??\name:" links
  // WITHOUT their object-directory qualifier, so look them up the same way.
  // Measured: xam's "\??\_rand...:" lookups failed ("Couldn't resolve symbolic
  // link root name").
  if (xe::utf8::starts_with(normalized, "\\??\\")) {
    normalized = normalized.substr(4);
  } else if (xe::utf8::starts_with(normalized, "\\System??\\")) {
    normalized = normalized.substr(10);
  }
  std::string resolved;
  out = ResolveSymbolicLink(normalized, resolved) ? resolved : normalized;
  // Phase 1099z98: object names are case-insensitive - the dash opens
  // "\Device\Cdrom0\default.xex" (lowercase r) for the disc's title info.
  return std::ranges::any_of(devices_, [&](const auto& d) {
    return xe::utf8::starts_with_case(out, d->mount_path());
  });
}

Entry* VirtualFileSystem::ResolvePath(const std::string_view path) {
  auto global_lock = global_critical_region_.Acquire();

  // Resolve relative paths
  auto normalized_path(xe::utf8::canonicalize_guest_path(path));

  // Resolve symlinks.
  std::string resolved_path;
  if (ResolveSymbolicLink(normalized_path, resolved_path)) {
    normalized_path = resolved_path;
  }

  // Find the device.
  // Phase 1099z98: case-insensitive, like the object manager (dash asks for
  // "\Device\Cdrom0\default.xex"; the device is mounted as "\Device\CdRom0").
  auto it = std::ranges::find_if(std::as_const(devices_), [&](const auto& d) {
    return xe::utf8::starts_with_case(normalized_path, d->mount_path());
  });
  if (it == devices_.cend()) {
    // Supress logging the error for ShaderDumpxe:\CompareBackEnds as this is
    // not an actual problem nor something we care about.
    if (path != "ShaderDumpxe:\\CompareBackEnds") {
      XELOGE("ResolvePath({}) failed - device not found", path);
    }
    return nullptr;
  }

  const auto& device = *it;
  auto relative_path = normalized_path.substr(device->mount_path().size());
  auto* resolved = device->ResolvePath(relative_path);
  // Font lookups decide whether the Guide can rasterise text at all: xam
  // resolves "file://media:/XenonJKLatin.xtt" and XUIFONT::Init has a
  // "must specify a typeface" failure path. Data-file opens are not otherwise
  // logged by name, so a missing font is indistinguishable from silence -
  // which has already caused two wrong conclusions in this work. Log the
  // result explicitly, hit or miss.
  bool is_font =
      xe::utf8::find_first_of(normalized_path, ".xtt") != std::string::npos ||
      xe::utf8::find_first_of(normalized_path, ".XTT") != std::string::npos;
  if (is_font && !resolved) {
    // xam asks for "file://media:/XenonJKLatin.xtt" - media: is the TITLE's
    // device. That happens to work when the title is the dashboard folder,
    // which ships the fonts, but a real game disc has none, so the Guide
    // would have no typeface at all. On hardware these live in flash, not on
    // the game media, so fall back to SYS: (guide_system_root), which is
    // mounted precisely so the Guide does not depend on the title's layout.
    auto slash = normalized_path.find_last_of('\\');
    auto leaf = slash == std::string::npos ? normalized_path
                                           : normalized_path.substr(slash + 1);
    auto sys_it = std::ranges::find_if(std::as_const(devices_), [&](auto& d) {
      return d->mount_path() == "\\SYS";
    });
    if (sys_it != devices_.cend()) {
      resolved = (*sys_it)->ResolvePath("\\" + leaf);
      if (resolved) {
        XELOGI("VFS: font \"{}\" not on the title device; served from SYS:",
               leaf);
      }
    }
  }
  if (is_font) {
    XELOGI("VFS: font lookup \"{}\" -> {}", path,
           resolved ? "FOUND" : "NOT FOUND");
  }
  return resolved;
}

Entry* VirtualFileSystem::CreatePath(const std::string_view path,
                                     uint32_t attributes) {
  // Create all required directories recursively.
  //
  // Phase 1099n: this used to resolve the FIRST path component on its own and
  // walk down from it. That only works when the first component is a symbolic
  // link ("game:"). For a device path, "\Device\Harddisk0\Partition1\Cache"
  // split to "Device", which resolves to nothing, so every create under a
  // \Device\ path returned null -> ACCESS_DENIED (measured: xam's
  // Partition1\Cache create, C0000022, on a writable device). Walk UP from the
  // full path to the deepest ancestor that exists instead, then create the
  // missing components below it.
  std::string full(path);
  while (!full.empty() && (full.back() == '\\' || full.back() == '/')) {
    full.pop_back();
  }
  std::vector<std::string> missing;  // deepest first
  Entry* parent_entry = nullptr;
  std::string cur = full;
  while (!parent_entry) {
    const size_t sep = cur.find_last_of("\\/");
    if (sep == std::string::npos) {
      return nullptr;
    }
    std::string name = cur.substr(sep + 1);
    cur.resize(sep);
    if (!name.empty()) {
      missing.push_back(std::move(name));
    }
    if (cur.empty()) {
      return nullptr;
    }
    parent_entry = ResolvePath(cur);
  }
  if (missing.empty()) {
    return nullptr;
  }
  for (size_t i = missing.size() - 1; i > 0; --i) {
    Entry* child_entry = parent_entry->GetChild(missing[i]);
    if (!child_entry) {
      child_entry = parent_entry->CreateEntry(missing[i],
                                              kFileAttributeDirectory);
    }
    if (!child_entry) {
      return nullptr;
    }
    parent_entry = child_entry;
  }
  return parent_entry->CreateEntry(missing[0], attributes);
}

bool VirtualFileSystem::DeletePath(const std::string_view path) {
  auto entry = ResolvePath(path);
  if (!entry) {
    return false;
  }
  auto parent = entry->parent();
  if (!parent) {
    // Can't delete root.
    return false;
  }
  return parent->Delete(entry);
}

X_STATUS VirtualFileSystem::OpenFile(Entry* root_entry,
                                     const std::string_view path,
                                     FileDisposition creation_disposition,
                                     uint32_t desired_access, bool is_directory,
                                     bool is_non_directory, File** out_file,
                                     FileAction* out_action) {
  // TODO(gibbed): should 'is_directory' remain as a bool or should it be
  // flipped to a generic FileAttributeFlags?

  // Cleanup access.
  if (desired_access & FileAccess::kGenericRead) {
    desired_access |= FileAccess::kFileReadData;
  }
  if (desired_access & FileAccess::kGenericWrite) {
    desired_access |= FileAccess::kFileWriteData;
  }
  if (desired_access & FileAccess::kGenericAll) {
    desired_access |= FileAccess::kFileReadData | FileAccess::kFileWriteData;
  }

  // Lookup host device/parent path.
  // If no device or parent, fail.
  Entry* parent_entry = nullptr;
  Entry* entry = nullptr;

  // Phase 1099z22: a path that IS a device ("\Device\CdRom0", no trailing
  // separator) opens the device itself - xam's media detection (8176CD60)
  // opens the raw drive that way. Splitting it into parent "\Device" and
  // child "CdRom0" found no device and returned OBJECT_PATH_NOT_FOUND, which
  // xam retried forever.
  bool is_device_path = false;
  if (!root_entry) {
    auto global_lock = global_critical_region_.Acquire();
    const auto canonical = xe::utf8::canonicalize_guest_path(path);
    for (const auto& d : devices_) {
      if (xe::utf8::equal_case(canonical, d->mount_path())) {
        is_device_path = true;
        break;
      }
    }
  }
  std::string base_path =
      is_device_path ? std::string()
                     : std::string(xe::utf8::find_base_guest_path(path));
  if (!base_path.empty()) {
    parent_entry = !root_entry ? ResolvePath(base_path)
                               : root_entry->ResolvePath(base_path);
    if (!parent_entry) {
      *out_action = FileAction::kDoesNotExist;
      return X_STATUS_OBJECT_PATH_NOT_FOUND;
    }

    auto file_name = xe::utf8::find_name_from_guest_path(path);
    entry = parent_entry->GetChild(file_name);
  } else {
    entry = !root_entry ? ResolvePath(path) : root_entry->GetChild(path);
  }

  // Phase 1099n: a path with a trailing separator names the directory itself,
  // and "\Device\Harddisk0\Partition1\" names the VOLUME ROOT. Splitting it
  // above gives the parent "\Device\Harddisk0\Partition1" and an empty child
  // name, which never matches, so every open of a volume root failed with
  // OBJECT_NAME_NOT_FOUND while NtQueryFullAttributesFile (which resolves the
  // whole path) found it. xam's profile-device enumeration opens exactly that
  // path (8172FF20, measured 6/6 C0000034), so no storage device was listed.
  if (!entry && !root_entry && !path.empty() &&
      (path.back() == '\\' || path.back() == '/')) {
    if (Entry* whole = ResolvePath(path)) {
      entry = whole;
      parent_entry = nullptr;
    }
  }

  if (entry) {
    if (entry->attributes() & kFileAttributeDirectory && is_non_directory) {
      return X_STATUS_FILE_IS_A_DIRECTORY;
    }

    // If the entry does not exist on the host then remove the cached entry
    if (parent_entry) {
      const xe::vfs::HostPathEntry* host_path =
          dynamic_cast<const xe::vfs::HostPathEntry*>(parent_entry);

      if (host_path) {
        auto const file_path = host_path->host_path() / entry->name();

        if (!std::filesystem::exists(file_path)) {
          // Remove cached entry
          entry->Delete();
          entry = nullptr;
        }
      }
    }
  }

  // Check if exists (if we need it to), or that it doesn't (if it shouldn't).
  switch (creation_disposition) {
    case FileDisposition::kOpen:
    case FileDisposition::kOverwrite:
      // Must exist.
      if (!entry) {
        *out_action = FileAction::kDoesNotExist;
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
      }
      break;
    case FileDisposition::kCreate:
      // Must not exist.
      if (entry) {
        *out_action = FileAction::kExists;
        return X_STATUS_OBJECT_NAME_COLLISION;
      }
      break;
    default:
      // Either way, ok.
      break;
  }

  // Verify permissions.
  bool wants_write = desired_access & FileAccess::kFileWriteData ||
                     desired_access & FileAccess::kFileAppendData;
  if (wants_write && ((parent_entry && parent_entry->is_read_only()) ||
                      (entry && entry->is_read_only()))) {
    // Fail if read only device and wants write.
    // return X_STATUS_ACCESS_DENIED;
    // TODO(benvanik): figure out why games are opening read-only files with
    // write modes.
    assert_always();
    XELOGW("Attempted to open the file/dir for create/write");
    desired_access = FileAccess::kGenericRead | FileAccess::kFileReadData;
  }

  bool created = false;
  if (!entry) {
    // Remember that we are creating this new, instead of replacing.
    created = true;
    *out_action = FileAction::kCreated;
  } else {
    // May need to delete, if it exists.
    switch (creation_disposition) {
      case FileDisposition::kCreate:
        // Shouldn't be possible to hit this.
        assert_always();
        return X_STATUS_ACCESS_DENIED;
      case FileDisposition::kSuperscede:
        // Replace (by delete + recreate).
        if (!entry->Delete()) {
          return X_STATUS_ACCESS_DENIED;
        }
        entry = nullptr;
        *out_action = FileAction::kSuperseded;
        break;
      case FileDisposition::kOpen:
      case FileDisposition::kOpenIf:
        // Normal open.
        *out_action = FileAction::kOpened;
        break;
      case FileDisposition::kOverwrite:
      case FileDisposition::kOverwriteIf:
        // Overwrite (we do by delete + recreate).
        if (!entry->Delete()) {
          return X_STATUS_ACCESS_DENIED;
        }
        entry = nullptr;
        *out_action = FileAction::kOverwritten;
        break;
    }
  }
  if (!entry) {
    // Create if needed (either new or as a replacement).
    entry = CreatePath(
        path, !is_directory ? kFileAttributeNormal : kFileAttributeDirectory);
    if (!entry) {
      return X_STATUS_ACCESS_DENIED;
    }
  }

  // Open.
  auto result = entry->Open(desired_access, out_file);
  if (XFAILED(result)) {
    *out_action = FileAction::kDoesNotExist;
  }
  return result;
}

X_STATUS VirtualFileSystem::ExtractContentFile(Entry* entry,
                                               std::filesystem::path base_path,
                                               uint64_t& progress,
                                               bool extract_to_root) {
  // Allocate a buffer when needed.
  size_t buffer_size = 0;
  uint8_t* buffer = nullptr;

  XELOGI("Extracting file: {}", entry->path());

  auto dest_name =
      base_path / xe::to_path(utf8::fix_path_separators(entry->path()));

  if (extract_to_root) {
    dest_name = base_path / xe::to_path(entry->name());
  }

  if (entry->attributes() & kFileAttributeDirectory) {
    std::error_code error_code;
    std::filesystem::create_directories(dest_name, error_code);
    if (error_code) {
      return error_code.value();
    }
    return 0;
  }

  vfs::File* in_file = nullptr;
  X_STATUS result = entry->Open(FileAccess::kFileReadData, &in_file);
  if (result != X_STATUS_SUCCESS) {
    return result;
  }

  auto file = xe::filesystem::OpenFile(dest_name, "wb");
  if (!file) {
    in_file->Destroy();
    return 1;
  }
  constexpr size_t write_buffer_size = 4_MiB;

  if (entry->can_map()) {
    auto map = entry->OpenMapped(xe::MappedMemory::Mode::kRead);

    size_t remaining_size = map->size();
    size_t offset = 0;

    while (remaining_size > 0) {
      const auto bytes_to_read = std::min(write_buffer_size, remaining_size);
      fwrite(map->data() + offset, bytes_to_read, 1, file);
      offset += bytes_to_read;
      remaining_size -= bytes_to_read;
      progress += bytes_to_read;
    }
    map->Close();
  } else {
    size_t remaining_size = entry->size();
    size_t offset = 0;
    buffer = new uint8_t[write_buffer_size];

    while (remaining_size > 0) {
      size_t bytes_read = 0;
      in_file->ReadSync(std::span<uint8_t>(buffer, write_buffer_size), offset,
                        &bytes_read);
      fwrite(buffer, bytes_read, 1, file);
      offset += bytes_read;
      remaining_size -= bytes_read;
      progress += bytes_read;
    }
  }

  fclose(file);
  in_file->Destroy();

  if (buffer) {
    delete[] buffer;
  }
  return 0;
}

X_STATUS VirtualFileSystem::ExtractContentFiles(Device* device,
                                                std::filesystem::path base_path,
                                                uint64_t& progress) {
  // Run through all the files, breadth-first style.
  std::queue<vfs::Entry*> queue;
  auto root = device->ResolvePath("/");
  queue.push(root);

  while (!queue.empty()) {
    auto entry = queue.front();
    queue.pop();
    for (auto& entry : entry->children()) {
      queue.push(entry.get());
    }

    ExtractContentFile(entry, base_path, progress);
  }
  return X_STATUS_SUCCESS;
}

void VirtualFileSystem::ExtractContentHeader(Device* device,
                                             std::filesystem::path base_path) {
  const XContentContainerDevice* xcontent_device =
      ((XContentContainerDevice*)device);

  if (!std::filesystem::exists(base_path.parent_path())) {
    if (!std::filesystem::create_directories(base_path.parent_path())) {
      return;
    }
  }
  auto header_filename = base_path.filename().string() + ".header";
  auto header_path = base_path.parent_path() / header_filename;
  xe::filesystem::CreateEmptyFile(header_path);

  if (std::filesystem::exists(header_path)) {
    auto file = xe::filesystem::OpenFile(header_path, "wb");
    kernel::xam::XCONTENT_AGGREGATE_DATA data =
        xcontent_device->content_header();
    uint32_t license_mask = xcontent_device->license_mask();

    data.set_file_name(base_path.filename().string());
    fwrite(&data, 1, sizeof(kernel::xam::XCONTENT_AGGREGATE_DATA), file);
    fwrite(&license_mask, 1, sizeof(license_mask), file);
    fclose(file);
  }
  return;
}
}  // namespace vfs
}  // namespace xe
