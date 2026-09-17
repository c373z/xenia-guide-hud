/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/kernel/power_reset.h"
#include "xenia/kernel/info/file.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xiocompletion.h"
#include "xenia/kernel/xsymboliclink.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/disc_image_entry.h"
#include "xenia/emulator.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/host_disc_link.h"
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "third_party/crypto/TinySHA1.hpp"
#include "xenia/xbox.h"

DECLARE_bool(disc_synth_security_block);

namespace xe {
namespace kernel {
namespace xboxkrnl {

struct CreateOptions {
  // https://processhacker.sourceforge.io/doc/ntioapi_8h.html
  static constexpr uint32_t FILE_DIRECTORY_FILE = 0x00000001;
  // Optimization - files access will be sequential, not random.
  static constexpr uint32_t FILE_SEQUENTIAL_ONLY = 0x00000004;
  static constexpr uint32_t FILE_SYNCHRONOUS_IO_ALERT = 0x00000010;
  static constexpr uint32_t FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020;
  static constexpr uint32_t FILE_NON_DIRECTORY_FILE = 0x00000040;
  // Optimization - file access will be random, not sequential.
  static constexpr uint32_t FILE_RANDOM_ACCESS = 0x00000800;
};

dword_result_t NtCreateFile_entry(lpdword_t handle_out, dword_t desired_access,
                                  pointer_t<X_OBJECT_ATTRIBUTES> object_attrs,
                                  pointer_t<X_IO_STATUS_BLOCK> io_status_block,
                                  lpqword_t allocation_size_ptr,
                                  dword_t file_attributes, dword_t share_access,
                                  dword_t creation_disposition,
                                  dword_t create_options) {
  uint64_t allocation_size = 0;  // is this correct???
  if (allocation_size_ptr) {
    allocation_size = *allocation_size_ptr;
  }

  if (!object_attrs) {
    // ..? Some games do this. This parameter is not optional.
    return X_STATUS_INVALID_PARAMETER;
  }
  assert_not_null(handle_out);

  auto object_name =
      kernel_memory()->TranslateVirtual<X_ANSI_STRING*>(object_attrs->name_ptr);

  vfs::Entry* root_entry = nullptr;

  // Compute path, possibly attrs relative.
  auto target_path = util::TranslateAnsiPath(kernel_memory(), object_name);

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  if (object_attrs->root_directory != 0xFFFFFFFD &&  // ObDosDevices
      object_attrs->root_directory != 0) {
    auto root_file = kernel_state()->object_table()->LookupObject<XFile>(
        object_attrs->root_directory);
    assert_not_null(root_file);
    assert_true(root_file->type() == XObject::Type::File);

    root_entry = root_file->entry();
  }

  // Attempt open (or create).
  vfs::File* vfs_file;
  vfs::FileAction file_action;
  X_STATUS result = kernel_state()->file_system()->OpenFile(
      root_entry, target_path,
      vfs::FileDisposition((uint32_t)creation_disposition), desired_access,
      (create_options & CreateOptions::FILE_DIRECTORY_FILE) != 0,
      (create_options & CreateOptions::FILE_NON_DIRECTORY_FILE) != 0, &vfs_file,
      &file_action);
  object_ref<XFile> file = nullptr;

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  if (XSUCCEEDED(result)) {
    // If true, desired_access SYNCHRONIZE flag must be set.
    bool synchronous =
        (create_options & CreateOptions::FILE_SYNCHRONOUS_IO_ALERT) ||
        (create_options & CreateOptions::FILE_SYNCHRONOUS_IO_NONALERT);
    file = object_ref<XFile>(new XFile(kernel_state(), vfs_file, synchronous));

    // Handle ref is incremented, so return that.
    handle = file->handle();
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information = (uint32_t)file_action;
  }

  *handle_out = handle;

  // Phase 1099n: xam's profile-device enumeration (8172FF20) opens
  // "\Device\Harddisk0\Partition1\" with options 0x800021 before its volume
  // size query, and its cache setup (81727750) creates "<name>Cache". Any
  // failure here removes the drive from the enumeration. Measure device-rooted
  // opens.
  const uint32_t root_dir = object_attrs->root_directory;
  if (target_path.rfind("\\Device\\", 0) == 0 ||
      (root_dir != 0 && root_dir != 0xFFFFFFFD) ||
      target_path.find("Content") != std::string::npos ||
      target_path.find("F1111") != std::string::npos ||
      // 1099z17559-9: and every failed open, whatever the path.
      XFAILED(result)) {
    static std::atomic<uint32_t> olog{0};
    if (olog.fetch_add(1) < 400) {
      XELOGI("NtCreateFile('{}', root {:08X}{}, access {:08X}, disp {}, "
             "options {:08X}, alloc {}) -> {:08X}",
             target_path, root_dir,
             root_entry ? " '" + root_entry->path() + "'" : std::string(),
             uint32_t(desired_access), uint32_t(creation_disposition),
             uint32_t(create_options), allocation_size, uint32_t(result));
    }
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtCreateFile, kFileSystem, kImplemented);

dword_result_t NtOpenFile_entry(
    lpdword_t handle_out, dword_t desired_access,
    pointer_t<X_OBJECT_ATTRIBUTES> object_attributes,
    pointer_t<X_IO_STATUS_BLOCK> io_status_block, dword_t open_options) {
  return NtCreateFile_entry(
      handle_out, desired_access, object_attributes, io_status_block, nullptr,
      0, 0, static_cast<uint32_t>(xe::vfs::FileDisposition::kOpen),
      open_options);
}
DECLARE_XBOXKRNL_EXPORT1(NtOpenFile, kFileSystem, kImplemented);

dword_result_t NtReadFile_entry(dword_t file_handle, dword_t event_handle,
                                lpvoid_t apc_routine_ptr, lpvoid_t apc_context,
                                pointer_t<X_IO_STATUS_BLOCK> io_status_block,
                                lpvoid_t buffer, dword_t buffer_length,
                                lpqword_t byte_offset_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  bool signal_event = false;
  auto ev = kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  if (XSUCCEEDED(result)) {
    if (true || file->is_synchronous()) {
      // Synchronous.
      uint32_t bytes_read = 0;
      result = file->Read(
          buffer.guest_address(), buffer_length,
          byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : -1,
          &bytes_read, apc_context);
      if (io_status_block) {
        io_status_block->status = result;
        io_status_block->information = bytes_read;
      }

      // Queue the APC callback. It must be delivered via the APC mechanism even
      // though were are completing immediately.
      // Low bit probably means do not queue to IO ports.
      if ((uint32_t)apc_routine_ptr & ~1) {
        if (apc_context && result == X_STATUS_SUCCESS) {
          auto thread = XThread::GetCurrentThread();
          thread->EnqueueApc(static_cast<uint32_t>(apc_routine_ptr) & ~1u,
                             apc_context, io_status_block, 0);
        }
      }

      if (!file->is_synchronous() && result != X_STATUS_END_OF_FILE) {
        result = X_STATUS_PENDING;
      }

      // Mark that we should signal the event now. We do this after
      // we have written the info out.
      signal_event = true;

      if (XSUCCEEDED(result)) {
        if (auto patch = kernel_state()->xmp_volume_patch()) {
          auto host_buf =
              kernel_memory()->TranslateVirtual(buffer.guest_address());
          patch->OnFileRead(file->entry()->name(), host_buf, buffer_length,
                            buffer.guest_address());
        }
      }
    } else {
      // TODO(benvanik): async.

      // X_STATUS_PENDING if not returning immediately.
      // XFile is waitable and signalled after each async req completes.
      // reset the input event (->Reset())
      /*xeNtReadFileState* call_state = new xeNtReadFileState();
      XAsyncRequest* request = new XAsyncRequest(
      state, file,
      (XAsyncRequest::CompletionCallback)xeNtReadFileCompleted,
      call_state);*/
      // result = file->Read(buffer.guest_address(), buffer_length, byte_offset,
      //                     request);
      if (io_status_block) {
        io_status_block->status = X_STATUS_PENDING;
        io_status_block->information = 0;
      }

      result = X_STATUS_PENDING;
    }
  }

  if (XFAILED(result) && io_status_block) {
    io_status_block->status = result;
    io_status_block->information = 0;
  }

  if (ev && signal_event) {
    ev->Set(0, false);
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT2(NtReadFile, kFileSystem, kImplemented, kHighFrequency);

dword_result_t NtReadFileScatter_entry(
    dword_t file_handle, dword_t event_handle, lpvoid_t apc_routine_ptr,
    lpvoid_t apc_context, pointer_t<X_IO_STATUS_BLOCK> io_status_block,
    lpdword_t segment_array, dword_t length, lpqword_t byte_offset_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  bool signal_event = false;
  auto ev = kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  if (XSUCCEEDED(result)) {
    if (true || file->is_synchronous()) {
      // Synchronous.
      uint32_t bytes_read = 0;
      result = file->ReadScatter(
          segment_array.guest_address(), length,
          byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : -1,
          &bytes_read, apc_context);
      if (io_status_block) {
        io_status_block->status = result;
        io_status_block->information = bytes_read;
      }

      // Queue the APC callback. It must be delivered via the APC mechanism even
      // though were are completing immediately.
      // Low bit probably means do not queue to IO ports.
      if ((uint32_t)apc_routine_ptr & ~1) {
        if (apc_context) {
          auto thread = XThread::GetCurrentThread();
          thread->EnqueueApc(static_cast<uint32_t>(apc_routine_ptr) & ~1u,
                             apc_context, io_status_block, 0);
        }
      }

      if (!file->is_synchronous()) {
        result = X_STATUS_PENDING;
      }

      // Mark that we should signal the event now. We do this after
      // we have written the info out.
      signal_event = true;
    } else {
      // TODO(benvanik): async.

      // TODO: On Windows it might be worth trying to use Win32 ReadFileScatter
      // here instead of handling it ourselves

      // X_STATUS_PENDING if not returning immediately.
      // XFile is waitable and signalled after each async req completes.
      // reset the input event (->Reset())
      /*xeNtReadFileState* call_state = new xeNtReadFileState();
      XAsyncRequest* request = new XAsyncRequest(
      state, file,
      (XAsyncRequest::CompletionCallback)xeNtReadFileCompleted,
      call_state);*/
      // result = file->Read(buffer.guest_address(), buffer_length, byte_offset,
      //                     request);
      if (io_status_block) {
        io_status_block->status = X_STATUS_PENDING;
        io_status_block->information = 0;
      }

      result = X_STATUS_PENDING;
    }
  }

  if (XFAILED(result) && io_status_block) {
    io_status_block->status = result;
    io_status_block->information = 0;
  }

  if (ev && signal_event) {
    ev->Set(0, false);
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtReadFileScatter, kFileSystem, kImplemented);

dword_result_t NtWriteFile_entry(dword_t file_handle, dword_t event_handle,
                                 function_t apc_routine, lpvoid_t apc_context,
                                 pointer_t<X_IO_STATUS_BLOCK> io_status_block,
                                 lpvoid_t buffer, dword_t buffer_length,
                                 lpqword_t byte_offset_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  // Grab event to signal.
  bool signal_event = false;
  auto ev = kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  // Grab file.
  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  // Execute write.
  if (XSUCCEEDED(result)) {
    // TODO(benvanik): async path.
    if (true || file->is_synchronous()) {
      // Synchronous request.
      uint32_t bytes_written = 0;
      result = file->Write(
          buffer.guest_address(), buffer_length,
          byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : -1,
          &bytes_written, apc_context);

      if (io_status_block) {
        io_status_block->status = result;
        io_status_block->information = static_cast<uint32_t>(bytes_written);
      }

      // Queue the APC callback. It must be delivered via the APC mechanism even
      // though were are completing immediately.
      // Low bit probably means do not queue to IO ports.
      if ((uint32_t)apc_routine & ~1) {
        if (apc_context) {
          auto thread = XThread::GetCurrentThread();
          thread->EnqueueApc(static_cast<uint32_t>(apc_routine) & ~1u,
                             apc_context, io_status_block, 0);
        }
      }

      if (!file->is_synchronous()) {
        result = X_STATUS_PENDING;
      }

      // Mark that we should signal the event now. We do this after
      // we have written the info out.
      signal_event = true;

      if (XSUCCEEDED(result)) {
        if (auto patch = kernel_state()->xmp_volume_patch()) {
          auto host_buf =
              kernel_memory()->TranslateVirtual(buffer.guest_address());
          patch->OnFileWrite(file->entry()->name(), host_buf, buffer_length,
                             buffer.guest_address());
        }
      }
    } else {
      // X_STATUS_PENDING if not returning immediately.
      result = X_STATUS_PENDING;

      if (io_status_block) {
        io_status_block->status = X_STATUS_PENDING;
        io_status_block->information = 0;
      }
    }
  }

  if (XFAILED(result) && io_status_block) {
    io_status_block->status = result;
    io_status_block->information = 0;
  }

  if (ev && signal_event) {
    ev->Set(0, false);
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtWriteFile, kFileSystem, kImplemented);

dword_result_t NtCreateIoCompletion_entry(
    lpdword_t out_handle, dword_t desired_access,
    pointer_t<X_OBJECT_ATTRIBUTES> object_attribs,
    dword_t num_concurrent_threads) {
  auto completion = new XIOCompletion(kernel_state());
  if (out_handle) {
    *out_handle = completion->handle();
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtCreateIoCompletion, kFileSystem, kImplemented);

dword_result_t NtSetIoCompletion_entry(dword_t handle, dword_t key_context,
                                       dword_t apc_context,
                                       dword_t completion_status,
                                       dword_t num_bytes) {
  auto port =
      kernel_state()->object_table()->LookupObject<XIOCompletion>(handle);
  if (!port) {
    return X_STATUS_INVALID_HANDLE;
  }

  XIOCompletion::IONotification notification;
  notification.key_context = key_context;
  notification.apc_context = apc_context;
  notification.num_bytes = num_bytes;
  notification.status = completion_status;

  port->QueueNotification(notification);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(NtSetIoCompletion, kFileSystem, kImplemented,
                         kHighFrequency);

// Dequeues a packet from the completion port.
dword_result_t NtRemoveIoCompletion_entry(
    dword_t handle, lpdword_t key_context, lpdword_t apc_context,
    pointer_t<X_IO_STATUS_BLOCK> io_status_block, lpqword_t timeout) {
  X_STATUS status = X_STATUS_SUCCESS;
  uint32_t info = 0;

  auto port =
      kernel_state()->object_table()->LookupObject<XIOCompletion>(handle);
  if (!port) {
    status = X_STATUS_INVALID_HANDLE;
  }

  uint64_t timeout_ticks =
      timeout ? static_cast<uint32_t>(*timeout)
              : static_cast<uint64_t>(std::numeric_limits<int64_t>::min());
  XIOCompletion::IONotification notification;
  if (port->WaitForNotification(timeout_ticks, &notification)) {
    if (key_context) {
      *key_context = notification.key_context;
    }
    if (apc_context) {
      *apc_context = notification.apc_context;
    }

    if (io_status_block) {
      io_status_block->status = notification.status;
      io_status_block->information = notification.num_bytes;
    }
  } else {
    status = X_STATUS_TIMEOUT;
  }

  return status;
}
DECLARE_XBOXKRNL_EXPORT2(NtRemoveIoCompletion, kFileSystem, kImplemented,
                         kHighFrequency);

dword_result_t NtCancelIoFile_entry(dword_t handle) {
  auto file = kernel_state()->object_table()->LookupObject<XFile>(handle);
  if (!file) {
    return X_STATUS_INVALID_HANDLE;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtCancelIoFile, kFileSystem, kStub);

dword_result_t NtQueryFullAttributesFile_entry(
    pointer_t<X_OBJECT_ATTRIBUTES> obj_attribs,
    pointer_t<X_FILE_NETWORK_OPEN_INFORMATION> file_info) {
  auto object_name =
      kernel_memory()->TranslateVirtual<X_ANSI_STRING*>(obj_attribs->name_ptr);

  object_ref<XFile> root_file;
  if (obj_attribs->root_directory != 0xFFFFFFFD &&  // ObDosDevices
      obj_attribs->root_directory != 0) {
    root_file = kernel_state()->object_table()->LookupObject<XFile>(
        obj_attribs->root_directory);
    assert_not_null(root_file);
    assert_true(root_file->type() == XObject::Type::File);
    assert_always();
  }

  auto target_path = util::TranslateAnsiPath(kernel_memory(), object_name);

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  // Resolve the file using the virtual file system.
  auto entry = kernel_state()->file_system()->ResolvePath(target_path);
  // Phase 1099n: xam's storage gate (8172E088 -> 817ABDA0) is this call on
  // "<device>\"; failing it leaves the device in state 4, never usable. Log
  // device-rooted queries so that gate's answer is measured.
  if (target_path.rfind("\\Device\\", 0) == 0) {
    static std::atomic<uint32_t> dev_logs{0};
    if (dev_logs.fetch_add(1) < 60) {
      XELOGI("NtQueryFullAttributesFile('{}') -> {}", target_path,
             entry ? "found" : "NO_SUCH_FILE");
    }
  }
  if (entry) {
    // Found.
    file_info->creation_time = entry->create_timestamp();
    file_info->last_access_time = entry->access_timestamp();
    file_info->last_write_time = entry->write_timestamp();
    file_info->change_time = entry->write_timestamp();
    file_info->allocation_size = entry->allocation_size();
    file_info->end_of_file = entry->size();
    file_info->attributes = entry->attributes();

    return X_STATUS_SUCCESS;
  }

  return X_STATUS_NO_SUCH_FILE;
}
DECLARE_XBOXKRNL_EXPORT1(NtQueryFullAttributesFile, kFileSystem, kImplemented);

dword_result_t NtQueryDirectoryFile_entry(
    dword_t file_handle, dword_t event_handle, function_t apc_routine,
    lpvoid_t apc_context, pointer_t<X_IO_STATUS_BLOCK> io_status_block,
    pointer_t<X_FILE_DIRECTORY_INFORMATION> file_info_ptr, dword_t length,
    pointer_t<X_ANSI_STRING> file_name, dword_t restart_scan) {
  if (length < 72) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  X_STATUS result = X_STATUS_UNSUCCESSFUL;
  uint32_t info = 0;

  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  auto name = util::TranslateAnsiPath(kernel_memory(), file_name);

  // Enforce that the path is ASCII.
  if (!IsValidPath(name, true)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  if (file) {
    X_FILE_DIRECTORY_INFORMATION dir_info = {0};
    result =
        file->QueryDirectory(file_info_ptr, length, name, restart_scan != 0);
    if (XSUCCEEDED(result)) {
      info = length;
    }
  } else {
    result = X_STATUS_NO_SUCH_FILE;
  }

  if (XFAILED(result)) {
    info = 0;
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information = info;
  }

  // Phase 1099o: after a reboot xam lists 0 profiles although one exists on
  // the virtual HDD; profile enumeration lists Content\ with this call.
  if (file && file->path().find("Content") != std::string::npos) {
    static std::atomic<uint32_t> qlog{0};
    if (qlog.fetch_add(1) < 120) {
      std::string got;
      if (XSUCCEEDED(result) && file_info_ptr) {
        got = std::string(
            file_info_ptr->file_name,
            std::min<uint32_t>(file_info_ptr->file_name_length, 64));
      }
      XELOGI("NtQueryDirectoryFile('{}', mask '{}', len {}, restart {}) -> "
             "{:08X} '{}'",
             file->path(), name, uint32_t(length), uint32_t(restart_scan),
             uint32_t(result), got);
    }
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtQueryDirectoryFile, kFileSystem, kImplemented);

dword_result_t NtFlushBuffersFile_entry(
    dword_t file_handle, pointer_t<X_IO_STATUS_BLOCK> io_status_block_ptr) {
  auto result = X_STATUS_SUCCESS;

  if (io_status_block_ptr) {
    io_status_block_ptr->status = result;
    io_status_block_ptr->information = 0;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtFlushBuffersFile, kFileSystem, kStub);

// https://docs.microsoft.com/en-us/windows/win32/devnotes/ntopensymboliclinkobject
dword_result_t NtOpenSymbolicLinkObject_entry(
    lpdword_t handle_out, pointer_t<X_OBJECT_ATTRIBUTES> object_attrs) {
  if (!object_attrs) {
    return X_STATUS_INVALID_PARAMETER;
  }
  assert_not_null(handle_out);

  assert_true(object_attrs->attributes == 64);  // case insensitive

  auto object_name =
      kernel_memory()->TranslateVirtual<X_ANSI_STRING*>(object_attrs->name_ptr);

  auto target_path = util::TranslateAnsiPath(kernel_memory(), object_name);

  // Phase 1099z83: object-manager qualifiers contain '?', which IsValidPath
  // (a FILE path check) rejects outside patterns - so every "\??\D:" and
  // "\System??\_rand...:" open failed here with OBJECT_NAME_INVALID before the
  // qualifier strip below ever ran, and xam logged "Couldn't resolve symbolic
  // link root name" on each profile/content close. Strip first, then check.
  target_path = xe::utf8::canonicalize_guest_path(target_path);
  if (utf8::starts_with(target_path, "\\??\\")) {
    target_path = target_path.substr(4);
  } else if (utf8::starts_with(target_path, "\\System??\\")) {
    target_path = target_path.substr(10);
  }

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    static std::atomic<uint32_t> logs{0};
    if (++logs <= 10) {
      std::string hex;
      for (unsigned char ch : target_path) hex += fmt::format("{:02X}", ch);
      XELOGW("NtOpenSymbolicLinkObject: invalid name len {} hex {}",
             target_path.size(), hex);
    }
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  if (object_attrs->root_directory != 0) {
    assert_always();
  }

  // Phase 1099q: strip BOTH qualifiers, exactly as ObCreateSymbolicLink does
  // when it registers the link. xam mounts a profile as "\System??\_rand...:"
  // (81739BA8) and on close re-opens that name here (817ABCC0); with only
  // "\??\" stripped the lookup missed, xam logged "Couldn't resolve symbolic
  // link root name" and returned before deleting the link and dereferencing
  // the package (81736EE8) - every profile mount leaked.
  // (The strip itself now runs before IsValidPath above - phase 1099z83.)

  std::string link_path;
  if (!kernel_state()->file_system()->FindSymbolicLink(target_path,
                                                       link_path)) {
    return X_STATUS_NO_SUCH_FILE;
  }

  object_ref<XSymbolicLink> symlink(new XSymbolicLink(kernel_state()));
  symlink->Initialize(target_path, link_path);

  *handle_out = symlink->handle();

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtOpenSymbolicLinkObject, kFileSystem, kImplemented);

// https://docs.microsoft.com/en-us/windows/win32/devnotes/ntquerysymboliclinkobject
dword_result_t NtQuerySymbolicLinkObject_entry(
    dword_t handle, pointer_t<X_ANSI_STRING> target) {
  auto symlink =
      kernel_state()->object_table()->LookupObject<XSymbolicLink>(handle);
  if (!symlink) {
    return X_STATUS_NO_SUCH_FILE;
  }
  auto length = std::min(static_cast<size_t>(target->maximum_length),
                         symlink->target().size());
  if (length > 0) {
    auto target_buf = kernel_memory()->TranslateVirtual(target->pointer);
    std::memcpy(target_buf, symlink->target().c_str(), length);
  }
  target->length = static_cast<uint16_t>(length);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtQuerySymbolicLinkObject, kFileSystem, kImplemented);

dword_result_t FscGetCacheElementCount_entry(dword_t r3) { return 0; }
DECLARE_XBOXKRNL_EXPORT1(FscGetCacheElementCount, kFileSystem, kStub);

dword_result_t FscSetCacheElementCount_entry(dword_t unk_0, dword_t unk_1) {
  // unk_0 = 0
  // unk_1 looks like a count? in what units? 256 is a common value
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(FscSetCacheElementCount, kFileSystem, kStub);

struct X_DRIVE_GEOMETRY {
  xe::be<uint32_t> sector_count;
  xe::be<uint32_t> sector_size;
};
static_assert_size(X_DRIVE_GEOMETRY, 0x8);

struct X_PARTITION_INFO {
  xe::be<uint64_t> unk;
  xe::be<uint64_t> total_size;
};
static_assert_size(X_PARTITION_INFO, 0x10);

// todo: this should fill in the io status block and queue the apc
// Phase 1099z16: the optical drive. xam's media detection (8176CD60) looks up
// \Device\CdRom0 as a device object, sends CHECK_VERIFY (0x24800) through
// IoSynchronousDeviceIoControlRequest and again through NtDeviceIoControlFile
// on an opened handle, then classifies (8176EA80): default.xex headers with
// execution info plus IOCTL 0x240CC returning 4 make it an Xbox 360 game
// disc. The drive always exists; whether a disc is in it is whether a disc
// image is mounted at \Device\CdRom0 (the tray hook mounts it).
static const char kCdRomDevicePath[] = "\\Device\\CdRom0";

static uint32_t g_cdrom_devobj = 0;
uint32_t GuideCdRomDeviceObject() {
  uint32_t& devobj = g_cdrom_devobj;
  if (!devobj) {
    auto* mem = kernel_memory();
    devobj = mem->SystemHeapAlloc(0x80);
    std::memset(mem->TranslateVirtual(devobj), 0, 0x80);
  }
  return devobj;
}

static bool GuideCdRomMediaPresent() {
  return kernel_state()->file_system()->ResolvePath(
             std::string(kCdRomDevicePath) + "\\") != nullptr;
}

static X_STATUS GuideCdRomIoctl(uint32_t code, lpvoid_t out, uint32_t out_len,
                                uint32_t* information) {
  *information = 0;
  const bool media = GuideCdRomMediaPresent();
  X_STATUS status = X_STATUS_SUCCESS;
  switch (code) {
    case 0x24800:  // IOCTL_STORAGE_CHECK_VERIFY
      status = media ? X_STATUS_SUCCESS : X_STATUS(0xC0000013);
      break;
    case 0x240CC:  // disc authentication state; xam accepts 4 as a game disc
      if (!media) {
        status = X_STATUS(0xC0000013);
      } else if (out && out_len >= 4) {
        xe::store_and_swap<uint32_t>(out, 4);
        *information = 4;
      }
      break;
    case 0x24090:
      // Security-sector summary (17489 kernel 800C4700, from the drive's
      // 0x804-byte security sector): +00 u32 hash-tree root sector, +04 u8,
      // +05 u8 flags (bit 4 = dash verifies reads), +08 u32, +0C/+10 layer
      // sector counts, +14 SHA-1 of the 0x8000 bytes at the root sector.
      // HOST-SIDE (disc_synth_security_block): a plain image has no security
      // sector; report a zero root block past the partition (served by
      // DiscImageFile) with verification off. Otherwise zero-filled as before.
      if (!media) {
        status = X_STATUS(0xC0000013);
      } else if (cvars::disc_synth_security_block && out && out_len >= 8) {
        std::memset(out, 0, out_len);
        auto* root = kernel_state()->file_system()->ResolvePath(
            std::string(kCdRomDevicePath) + "\\");
        const uint32_t sectors =
            root ? uint32_t((root->size() + 0x7FF) / 0x800) : 0;
        xe::store_and_swap<uint32_t>(out, sectors);
        if (out_len >= 0x28) {
          // Layer split: the XGD2 break (0x1B3880) when it fits, else half.
          const uint32_t l0 = sectors > 0x1B3880 ? 0x1B3880u : sectors / 2;
          xe::store_and_swap<uint32_t>(out.as<uint8_t*>() + 0x0C, l0);
          xe::store_and_swap<uint32_t>(out.as<uint8_t*>() + 0x10,
                                       sectors - l0);
          static const uint8_t kZeroBlockSha1[20] = {
              0x51, 0x88, 0x43, 0x18, 0x49, 0xb4, 0x61, 0x31, 0x52, 0xfd,
              0x7b, 0xdb, 0xa6, 0xa3, 0xff, 0x0a, 0x4f, 0xd6, 0x42, 0x4b};
          std::memcpy(out.as<uint8_t*>() + 0x14, kZeroBlockSha1, 20);
        }
        *information = out_len >= 0x28 ? 0x28 : (out_len >= 0xC ? 0xC : 8);
      } else if (out && out_len) {
        std::memset(out, 0, out_len);
      }
      break;
    default:
      // Spindle, spin-down, XGD2 auth, reauth: the image needs none of it.
      if (out && out_len) {
        std::memset(out, 0, out_len);
      }
      break;
  }
  static std::unordered_map<uint32_t, uint32_t> logged;
  {
    auto log_lock = xe::global_critical_region::AcquireDirect();
    if (++logged[code] <= 20) {
      XELOGI("CdRom0 IOCTL {:08X} media={} -> {:08X}", code, media,
             uint32_t(status));
    }
  }
  return status;
}

dword_result_t IoSynchronousDeviceIoControlRequest_entry(
    dword_t io_control_code, dword_t device_object, lpvoid_t input_buffer,
    dword_t input_buffer_len, lpvoid_t output_buffer,
    dword_t output_buffer_len, lpdword_t returned_length, dword_t internal) {
  uint32_t info = 0;
  X_STATUS status = X_STATUS_INVALID_PARAMETER;
  if (device_object == GuideCdRomDeviceObject()) {
    status = GuideCdRomIoctl(io_control_code, output_buffer,
                             output_buffer_len, &info);
  } else {
    XELOGW("IoSynchronousDeviceIoControlRequest({:08X}) on unknown device "
           "{:08X}",
           uint32_t(io_control_code), uint32_t(device_object));
  }
  if (returned_length) {
    *returned_length = info;
  }
  return status;
}
DECLARE_XBOXKRNL_EXPORT1(IoSynchronousDeviceIoControlRequest, kFileSystem,
                         kImplemented);

dword_result_t NtDeviceIoControlFile_entry(
    dword_t handle, dword_t event_handle, dword_t apc_routine,
    dword_t apc_context, pointer_t<X_IO_STATUS_BLOCK> io_status_block,
    dword_t io_control_code, lpvoid_t input_buffer, dword_t input_buffer_len,
    lpvoid_t output_buffer, dword_t output_buffer_len) {
  if (auto file = kernel_state()->object_table()->LookupObject<XFile>(handle)) {
    if (cvars::kernel_device_auth && file->device() &&
        utf8::equal_case(file->device()->mount_path(),
                         "\\Device\\DeviceAuth")) {
      // 1099z17559-5: \Device\DeviceAuth. 0x474000 is xam's "next
      // authentication request" call (retail xam 17559 sends it async with an
      // APC and a 4-byte output). It completes only when an accessory needs
      // authenticating; no emulated device ever does, so it stays pending.
      // HOST-SIDE model of the driver: only this IOCTL is handled.
      static std::atomic<uint32_t> alog{0};
      if (alog.fetch_add(1) < 16) {
        XELOGI("DeviceAuth: IOCTL {:08X} in {:08X}/{} out {:08X}/{} apc {:08X}",
               uint32_t(io_control_code), input_buffer.guest_address(),
               uint32_t(input_buffer_len), output_buffer.guest_address(),
               uint32_t(output_buffer_len), uint32_t(apc_routine));
      }
      if (io_control_code == 0x474000) {
        return X_STATUS_PENDING;
      }
      return X_STATUS_INVALID_DEVICE_REQUEST;
    }
    if (file->device() &&
        utf8::equal_case(file->device()->mount_path(), kCdRomDevicePath)) {
      uint32_t info = 0;
      X_STATUS status = GuideCdRomIoctl(io_control_code, output_buffer,
                                        output_buffer_len, &info);
      if (io_status_block) {
        io_status_block->status = status;
        io_status_block->information = info;
      }
      return status;
    }
  }
  // Called by XMountUtilityDrive cache-mounting code
  // (checks if the returned values look valid, values below seem to pass the
  // checks)
  constexpr uint32_t cache_size = 0xFF000;

  if (io_control_code == X_IOCTL_DISK_GET_DRIVE_GEOMETRY) {
    if (output_buffer_len < sizeof(X_DRIVE_GEOMETRY)) {
      assert_always();
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    auto buffer = output_buffer.as<X_DRIVE_GEOMETRY*>();
    buffer->sector_count = cache_size / 0x200;
    buffer->sector_size = 0x200;  // 0x200, 0x1000, 0x4000
  } else if (io_control_code == X_IOCTL_DISK_GET_PARTITION_INFO) {
    if (output_buffer_len < sizeof(X_PARTITION_INFO)) {
      assert_always();
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    auto buffer = output_buffer.as<X_PARTITION_INFO*>();
    buffer->unk = 0;
    buffer->total_size = cache_size;
  } else {
    XELOGD("NtDeviceIoControlFile(0x{:X}) - unhandled IOCTL!",
           uint32_t(io_control_code));
    assert_always();
    return X_STATUS_INVALID_PARAMETER;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtDeviceIoControlFile, kFileSystem, kStub);
// device_extension_size = additional bytes of data (aligned up to 8 byte
// granularity) that will be allocated at the tail of the resulting device
// object. although it is allocated at the tail, it is accessed through a
// pointer at offset 0x18 so in theory a guest could be unaware that its a
// single allocation device_name is optional, extra_device_object_attributes
// gets assigned to the attributes field of the OBJECT_ATTRIBUTES used for
// ObCreateObject

// todo: need device guest object struct + host object for device
struct X_DRIVER_OBJECT {
  xe::be<uint32_t> driver_start_io_ptr;
  xe::be<uint32_t> driver_delete_device_ptr;
  xe::be<uint32_t> driver_dismount_volume_ptr;
  xe::be<uint32_t> major_function_ptr[11];
};
static_assert_size(X_DRIVER_OBJECT, 0x38);

struct X_KDEVICE_QUEUE {
  xe::be<uint16_t> type;                  // 0x0 sz:0x2
  xe::be<uint8_t> padding;                // 0x2 sz:0x1
  xe::be<uint8_t> busy;                   // 0x3 sz:0x1
  xe::be<uint32_t> lock;                  // 0x4 sz:0x4
  xe::be<X_LIST_ENTRY> device_list_head;  // 0x8 sz:0x8
};
static_assert_size(X_KDEVICE_QUEUE, 0x10);

struct X_KDEVICE_QUEUE_ENTRY {
  X_LIST_ENTRY device_list_entry;  // 0x0 sz:0x2
  xe::be<uint32_t> sort_key;       // 0x8 sz:0x4
  xe::be<uint8_t> inserted;        // 0xC sz:0x1
};
static_assert_size(X_KDEVICE_QUEUE_ENTRY, 0x10);

struct X_IRP_ASYNC_PARAM {
  xe::be<uint32_t> user_apc_routine_ptr;  // 0x0 sz:0x4
  xe::be<uint32_t> user_apc_context_ptr;  // 0x4 sz:0x4
};
static_assert_size(X_IRP_ASYNC_PARAM, 0x8);

union X_UNION_IRP_OVERLAY {
  X_IRP_ASYNC_PARAM asynchronous_parameters;
  xe::be<int64_t> allocation_size;
};

struct X_IRP_OVERLAY {
  union {
    X_KDEVICE_QUEUE_ENTRY device_queue_entry;  // 0x0 sz:0x10
    X_LIST_ENTRY device_list_entry;            // 0x0 sz:0x8
    xe::be<uint32_t> driver_context_ptr[4];    // 0x0 sz:0x10
  };
  xe::be<uint32_t> locked_buffer_length;    // 0x10 sz:0x4
  TypedGuestPointer<X_KTHREAD> thread_ptr;  // 0x14 sz:0x4
  X_LIST_ENTRY list_entry;                  // 0x18 sz:0x8
  union {
    xe::be<uint32_t>
        current_stack_location_ptr;  // 0x20 sz:0x4, X_IO_STACK_LOCATION -> 0x24
    xe::be<uint32_t> packet_type;    // 0x20 sz:0x4
  };
  xe::be<uint32_t>
      original_file_object_ptr;  // 0x24 sz:0x4, X_FILE_OBJECT -> 0x68
};
static_assert_size(X_IRP_OVERLAY, 0x28);

union X_IRP_TAIL {
  X_IRP_OVERLAY overlay;                // 0x0 sz:0x28
  xe::be<XAPC> apc;                     // 0x0 sz:0x28
  xe::be<uint32_t> completion_key_ptr;  // 0x0 sz:0x4
};

struct X_IRP {
  xe::be<uint16_t> type;                               // 0x0 sz:0x2
  xe::be<uint16_t> size;                               // 0x2 sz:0x2
  xe::be<uint32_t> flags;                              // 0x4 sz:0x4
  X_LIST_ENTRY thread_list_entry;                      // 0x8 sz:0x8
  X_IO_STATUS_BLOCK io_status;                         // 0x10 sz:0x8
  xe::be<uint8_t> stack_count;                         // 0x18 sz:0x1
  xe::be<uint8_t> current_location;                    // 0x19 sz:0x1
  xe::be<uint8_t> pending_returned;                    // 0x1A sz:0x1
  xe::be<uint8_t> cancel;                              // 0x1B sz:0x1
  xe::be<uint32_t> user_buffer_ptr;                    // 0x1C sz:0x4
  TypedGuestPointer<X_IO_STATUS_BLOCK> user_iosb_ptr;  // 0x20 sz:0x4
  TypedGuestPointer<X_KEVENT> user_event_ptr;          // 0x24 sz:0x4
  X_UNION_IRP_OVERLAY overlay;                         // 0x28 sz:0x8
  X_IRP_TAIL tail;                                     // 0x30 sz:0x28
  xe::be<uint32_t> cancel_routine_ptr;                 // 0x58 sz:0x4
};
static_assert_size(X_IRP, 0x60);

struct X_DEVICE_OBJECT {
  xe::be<uint16_t> type;                                      // 0x0 sz:0x2
  xe::be<uint16_t> device_extension_size;                     // 0x2 sz:0x2
  xe::be<uint32_t> reference_count;                           // 0x4 sz:0x4
  TypedGuestPointer<X_DRIVER_OBJECT> drive_object_ptr;        // 0x8 sz:0x4
  TypedGuestPointer<X_DEVICE_OBJECT> mounted_or_self_device;  // 0xC sz:0x4
  TypedGuestPointer<X_IRP> current_irp_ptr;                   // 0x10 sz:0x4
  xe::be<uint32_t> flags;                                     // 0x14 sz:0x4
  xe::be<uint32_t> device_extension_ptr;                      // 0x18 sz:0x4
  xe::be<uint8_t> device_type;                                // 0x1C sz:0x1
  xe::be<uint8_t> start_io_flags;                             // 0x1D sz:0x1
  xe::be<uint8_t> stack_size;                                 // 0x1E sz:0x1
  xe::be<uint8_t> delete_pending;                             // 0x1F sz:0x1
  xe::be<uint32_t> sector_size;  // 0x20 sz:0x4, set by XamRamDriveCreate
  xe::be<uint32_t>
      alignment;  // 0x24 sz:0x4, NtQueryInformationFile called to verify
  xe::be<X_KDEVICE_QUEUE> device_queue;  // 0x28 sz:0x10
  xe::be<X_KEVENT> device_lock;          // 0x38 sz:0x10
  xe::be<uint32_t> start_io_count;       // 0x48 sz:0x4
  xe::be<uint32_t> start_io_key;         // 0x4C sz:0x4
};
static_assert_size(X_DEVICE_OBJECT, 0x50);

dword_result_t IoCreateDevice_entry(pointer_t<X_DRIVER_OBJECT> driver_object,
                                    dword_t device_extension_size,
                                    pointer_t<X_ANSI_STRING> device_name,
                                    dword_t device_type,
                                    dword_t extra_device_object_attributes,
                                    lpdword_t device_object,
                                    const ppc_context_t& ctx) {
  // Called from XMountUtilityDrive XAM-task code
  // We'll alloc some scratch space for it so it doesn't cause any exceptions
  auto kernel_mem = ctx->kernel_state->memory();

  uint32_t required_size =
      sizeof(X_DEVICE_OBJECT) + xe::align<uint32_t>(device_extension_size, 8);

  auto out_guest = kernel_mem->SystemHeapAlloc(required_size);

  auto out = kernel_mem->TranslateVirtual<X_DEVICE_OBJECT*>(out_guest);

  memset(out, 0, required_size);

  out->type = 3;  // maybe device object's Ob type?

  // this stores the total object size, without alignment!
  out->device_extension_size = device_extension_size + sizeof(X_DEVICE_OBJECT);

  // from 17559
  if (device_type == 7 || device_type == 58 || device_type == 62 ||
      device_type == 45 || device_type == 2 || device_type == 60 ||
      device_type == 61 || device_type == 36 || device_type == 64 ||
      device_type == 65 || device_type == 66 || device_type == 67 ||
      device_type == 68 || device_type == 69 || device_type == 70 ||
      device_type == 72 || device_type == 73) {
    out->mounted_or_self_device = 0;
  } else {
    out->mounted_or_self_device = static_cast<uint32_t>(out_guest);
  }
  out->device_type = static_cast<uint8_t>(device_type);

  uint32_t flags_field_value = 16;
  if (device_name) {
    flags_field_value |= 8;
  }
  out->stack_size = 1;
  out->flags = flags_field_value;
  if (device_extension_size != 0) {
    // pointer to device specific data
    // XMountUtilityDrive writes some kind of header here
    out->device_extension_ptr = out_guest + 80;
  }

  out->drive_object_ptr = static_cast<uint32_t>(driver_object);

  *device_object = static_cast<uint32_t>(out_guest);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(IoCreateDevice, kFileSystem, kStub);

// supposed to invoke a callback on the driver object! its some sort of
// destructor function intended to be called for all devices created from the
// driver
void IoDeleteDevice_entry(pointer_t<X_DEVICE_OBJECT> device_ptr,
                          const ppc_context_t& ctx) {
  if (device_ptr) {
    auto kernel_mem = ctx->kernel_state->memory();
    kernel_mem->SystemHeapFree(device_ptr);
  }
}

DECLARE_XBOXKRNL_EXPORT1(IoDeleteDevice, kFileSystem, kStub);

// Declared in the export table (ordinal 0x3D) but never implemented. A system
// app started by xam's app initialiser calls this in a tight loop against
// \Device\HdDvdRom; with no implementation the call falls through to the
// "undefined extern" path and the app retries forever, producing ~14k calls
// and ~197k failed ObReferenceObjectByHandle in under a minute. There is no
// HD-DVD drive to dismount, so report success and let the caller move on.
// Phase 1095ac: DrvGetContentStorageNotification is declared in the export
// table with NO implementation, so it went through UndefinedCallExtern, which
// returns 0 - and 0 is SUCCESS. xam's task at 817318F0 calls it at 81731934
// with a buffer at r1+0x70, sees "success", and reads a buffer nothing ever
// wrote:
//
//     [r1+0x7C] name = 0   -> the probe logged  name ''
//     [r1+0x80] op   = 0   -> not 1, so the dismount branch at 8173198C is
//                             skipped (coverage: "8173198C+16" unexecuted)
//     [r1+0x84] kind = 0   -> r30 = 0, and the probe logged  kind 0
//
// r30 = 0 is neither 2 nor 0xF, so the guard at 817319E0 lets it through to the
// op-2 call at 817319F4, which walks the still-unlinked list head at 81D3CA08
// and faults at 817286C0. (xam's own `twui` assert at 817319D4 would have
// caught op 0, but Xenia does not honour trap instructions.)
//
// That one fault is what stops 81731B04's KeSetEvent(81D21404), which blocks
// 81750FA8 at 81751120, which is why 81751164 -> 81780A28 never registers the
// device handler or runs the subsystem startup loop (1095ab).
//
// Report the truth instead: there is no content-storage driver here, so the
// call did not succeed. xam then takes its OWN error path at 81731940. This
// fabricates nothing - it stops fabricating a notification that never arrived.
dword_result_t DrvGetContentStorageNotification_entry(lpvoid_t buffer) {
  return X_STATUS_UNSUCCESSFUL;
}
DECLARE_XBOXKRNL_EXPORT1(DrvGetContentStorageNotification, kFileSystem, kStub);

// Phase 1099o: FOLDER-BACKED STFS DRIVER (user's choice: folders now, real STFS
// packages later). Contract decoded from xam's caller 8173B3A0 and the real
// kernel's StfsCreateDevice (800A0528) / StfsControlDevice (800A0318):
//   StfsCreateDevice(params, 0x58)   params:
//     +0x00 ANSI_STRING device name "\Device\Package_<md5 of package path>"
//     +0x18 u32  handle of the package FILE xam just created and wrote
//     +0x1C 0x24 STFS volume descriptor (returned verbatim by control code 3)
//     +0x44 u32  size of the client extension xam wants
//     +0x48 out  device object         +0x4C out  client extension area
// The package's CONTENTS live in a host folder beside the package file
// ("<package file>.stfs"), mounted writable at the device name. xam then links
// "\??\_rand...:" to that name (ObCreateSymbolicLink) and opens files through
// it. NOT byte-compatible with a console package - that is the later STFS
// work. The package file itself (CON header + metadata) is xam's own output.
struct StfsFolderVolume {
  uint8_t descriptor[0x24];
  std::filesystem::path folder;  // phase 1099z146: hashed by control code 3
};

// Phase 1099z99: NtDeleteFile(ObjectAttributes). Was an undefined extern, so
// a game overwriting a save made xam delete the old package (no-op), then
// create it again -> STATUS_OBJECT_NAME_COLLISION ("Couldn't create and mount
// package ... 0xc0000035"): only the first save ever worked. A folder-backed
// package's contents folder ("<file>.stfs", see StfsCreateDevice) goes with it.
dword_result_t NtDeleteFile_entry(pointer_t<X_OBJECT_ATTRIBUTES> object_attrs) {
  if (!object_attrs || !object_attrs->name_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (object_attrs->root_directory != 0) {
    XELOGW("NtDeleteFile: root-relative delete not implemented");
    return X_STATUS_NOT_IMPLEMENTED;
  }
  auto* mem = kernel_memory();
  const std::string path = util::TranslateAnsiPath(
      mem, mem->TranslateVirtual<X_ANSI_STRING*>(object_attrs->name_ptr));
  auto* fs = kernel_state()->file_system();
  auto* entry = fs->ResolvePath(path);
  if (!entry) {
    return X_STATUS_OBJECT_NAME_NOT_FOUND;
  }
  std::filesystem::path companion;
  if (auto* host = dynamic_cast<vfs::HostPathEntry*>(entry)) {
    companion = host->host_path();
    companion += ".stfs";
  }
  if (!fs->DeletePath(path)) {
    XELOGW("NtDeleteFile('{}') failed", path);
    return X_STATUS_ACCESS_DENIED;
  }
  if (!companion.empty()) {
    std::error_code ec;
    if (std::filesystem::is_directory(companion, ec)) {
      std::filesystem::remove_all(companion, ec);
    }
  }
  XELOGI("NtDeleteFile('{}') -> deleted", path);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtDeleteFile, kFileSystem, kImplemented);
static std::mutex stfs_folder_lock;
static std::unordered_map<uint32_t, StfsFolderVolume> stfs_folder_volumes;

dword_result_t StfsCreateDevice_entry(lpvoid_t params, dword_t params_size,
                                      const ppc_context_t& ctx) {
  if (!params || params_size != 0x58) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto* p = params.as<uint8_t*>();
  auto* mem = kernel_memory();
  const X_ANSI_STRING* name_str = params.as<X_ANSI_STRING*>();
  const std::string device_name =
      xe::utf8::canonicalize_guest_path(util::TranslateAnsiPath(mem, name_str));
  const uint32_t file_handle = xe::load_and_swap<uint32_t>(p + 0x18);
  const uint32_t ext_size = xe::load_and_swap<uint32_t>(p + 0x44);

  auto file =
      kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  auto* host_entry =
      file ? dynamic_cast<vfs::HostPathEntry*>(file->entry()) : nullptr;
  auto* disc_entry =
      file ? dynamic_cast<vfs::DiscImageEntry*>(file->entry()) : nullptr;
  std::error_code ec;
  // Phase 1099z176: a package INSIDE the disc image (every NXE-era disc's
  // \nxeart, a PIRS theme holding the dash tile/background art). The real
  // kernel's STFS driver reads it straight off the disc. HOST-SIDE: Xenia's
  // STFS reader only opens host files, so the entry's bytes are copied once
  // to <cache>\disc_packages\<device name> and that copy is mounted read-only.
  // The guest sees the same package contents either way.
  std::filesystem::path package_path;
  bool read_only_source = false;
  if (host_entry) {
    package_path = host_entry->host_path();
    read_only_source = host_entry->device()->is_read_only();
  } else if (disc_entry && disc_entry->mmap() && !device_name.empty()) {
    const std::string leaf = device_name.substr(device_name.rfind('\\') + 1);
    package_path = kernel_state()->emulator()->cache_root() / "disc_packages";
    std::filesystem::create_directories(package_path, ec);
    package_path /= leaf;
    const size_t size = disc_entry->data_size();
    if (std::filesystem::file_size(package_path, ec) != size || ec) {
      std::ofstream out(package_path, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(disc_entry->mmap()->data() +
                                              disc_entry->data_offset()),
                std::streamsize(size));
      if (!out) {
        XELOGE("StfsCreateDevice: could not copy disc package to {}",
               xe::path_to_utf8(package_path));
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    read_only_source = true;
    XELOGI("StfsCreateDevice: disc package {} ({} bytes) -> {}",
           disc_entry->path(), size, xe::path_to_utf8(package_path));
  }
  if (package_path.empty() || device_name.empty()) {
    XELOGE("StfsCreateDevice: '{}' handle {:08X} is not a host file",
           device_name, file_handle);
    return X_STATUS_INVALID_PARAMETER;
  }
  std::filesystem::path folder = package_path;
  folder += ".stfs";
  auto* fs = kernel_state()->file_system();

  // Phase 1099z161: a REAL package on a read-only device (the system update's
  // FFFE07DF packages in SystemExtPartition) is mounted with Xenia's own STFS
  // reader instead of the folder model, which only suits packages this
  // emulator creates and writes (profiles, saves). Without it the avatar
  // asset pack mounted as an empty folder and the Avatar Editor could not open
  // AvatarAssetPack:\AvatarAssetPack.toc.
  bool real_package = false;
  if (read_only_source && !std::filesystem::exists(folder, ec)) {
    std::ifstream in(package_path, std::ios::binary);
    char magic[4] = {};
    in.read(magic, 4);
    real_package = in.gcount() == 4 && (std::memcmp(magic, "PIRS", 4) == 0 ||
                                        std::memcmp(magic, "CON ", 4) == 0 ||
                                        std::memcmp(magic, "LIVE", 4) == 0);
  }
  if (real_package) {
    if (!fs->ResolvePath(device_name)) {
      auto device = vfs::XContentContainerDevice::CreateContentDevice(
          device_name, package_path);
      if (!device || !device->Initialize() ||
          !fs->RegisterDevice(std::move(device))) {
        XELOGE("StfsCreateDevice: could not open package {} at {}",
               xe::path_to_utf8(package_path), device_name);
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    folder.clear();  // StfsControlDevice code 3: the stored descriptor
  } else {
    std::filesystem::create_directories(folder, ec);
  }

  if (!real_package && !fs->ResolvePath(device_name)) {
    auto device =
        std::make_unique<vfs::HostPathDevice>(device_name, folder, false);
    if (!device->Initialize() || !fs->RegisterDevice(std::move(device))) {
      XELOGE("StfsCreateDevice: could not mount {} at {}",
             xe::path_to_utf8(folder), device_name);
      return X_STATUS_UNSUCCESSFUL;
    }
  }

  const uint32_t devobj = mem->SystemHeapAlloc(0x100);
  const uint32_t ext = ext_size ? mem->SystemHeapAlloc(ext_size) : 0;
  if (!devobj || (ext_size && !ext)) {
    return X_STATUS_NO_MEMORY;
  }
  std::memset(mem->TranslateVirtual(devobj), 0, 0x100);
  if (ext) std::memset(mem->TranslateVirtual(ext), 0, ext_size);
  xe::store_and_swap<uint32_t>(p + 0x48, devobj);
  xe::store_and_swap<uint32_t>(p + 0x4C, ext);
  {
    std::lock_guard<std::mutex> lock(stfs_folder_lock);
    auto& vol = stfs_folder_volumes[devobj];
    std::memcpy(vol.descriptor, p + 0x1C, sizeof(vol.descriptor));
    vol.folder = folder;
  }
  XELOGI("StfsCreateDevice: {} -> {} (devobj {:08X}, ext {:08X} x{:X}, "
         "mode {}) lr={:08X}",
         device_name, xe::path_to_utf8(folder), devobj, ext, ext_size,
         uint32_t(p[0x53]), uint32_t(ctx->lr));
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(StfsCreateDevice, kFileSystem, kImplemented);

// Phase 1099z164: NTSTATUS SvodCreateDevice(desc) - mounts a Games-on-Demand
// (SVOD) package, e.g. a disc installed to the hard drive. Contract from the
// 17489 kernel 80194680 and xam's caller 81680AC4 (0x2C bytes):
//   +0x00 ANSI_STRING device name   +0x08 ANSI_STRING package path
//   +0x10 root directory handle     +0x14 -> SVOD volume descriptor (>=0x24)
//   +0x18 cache object              +0x1C u32 caller data size
//   +0x20 OUT device object         +0x24 OUT caller data (in the extension,
//                                   after 0x4C0 + 8 * descriptor[1] bytes)
// The kernel's SVOD file system is Xenia's XContentContainerDevice here; the
// device object and extension are zeroed stand-ins (HOST-SIDE model).
dword_result_t SvodCreateDevice_entry(lpvoid_t desc, const ppc_context_t& ctx) {
  if (!cvars::kernel_svod_create_device || !desc) {
    return X_STATUS_NOT_IMPLEMENTED;
  }
  auto* p = desc.as<uint8_t*>();
  auto* mem = kernel_memory();
  const std::string device_name = xe::utf8::canonicalize_guest_path(
      util::TranslateAnsiPath(mem, desc.as<X_ANSI_STRING*>()));
  std::string package_path = util::TranslateAnsiPath(
      mem, reinterpret_cast<const X_ANSI_STRING*>(p + 8));
  const uint32_t root_handle = xe::load_and_swap<uint32_t>(p + 0x10);
  const uint32_t vol_ptr = xe::load_and_swap<uint32_t>(p + 0x14);
  const uint32_t data_size = xe::load_and_swap<uint32_t>(p + 0x1C);
  const uint32_t cache_count =
      vol_ptr ? *mem->TranslateVirtual<uint8_t*>(vol_ptr + 1) : 0;

  auto* fs = kernel_state()->file_system();
  vfs::Entry* entry = nullptr;
  if (root_handle) {
    if (auto root =
            kernel_state()->object_table()->LookupObject<XFile>(root_handle)) {
      entry = root->entry()->ResolvePath(package_path);
    }
  } else {
    entry = fs->ResolvePath(package_path);
  }
  auto* host_entry = dynamic_cast<vfs::HostPathEntry*>(entry);
  XELOGI("SvodCreateDevice: {} <- '{}' (root {:08X}, cache {}, data {:X}) "
         "lr={:08X}",
         device_name, package_path, root_handle, cache_count, data_size,
         uint32_t(ctx->lr));
  if (!host_entry || device_name.empty()) {
    return X_STATUS_OBJECT_NAME_NOT_FOUND;
  }
  std::filesystem::path header = host_entry->host_path();
  std::filesystem::path data_folder = header;
  if (header.extension() == ".data") {
    header.replace_extension();
  } else {
    data_folder += ".data";
  }
  // Phase 1099z165: HOST-SIDE. A package the host-side game library installed
  // holds a link to a disc image instead of the SVOD fragments a real install
  // would have copied. Mount the image itself; its GDFX file system has the
  // same shape at the device root that the SVOD package would have had.
  const std::filesystem::path linked_disc = vfs::ReadHostDiscLink(data_folder);
  if (!fs->ResolvePath(device_name)) {
    std::unique_ptr<vfs::Device> device;
    if (!linked_disc.empty()) {
      XELOGI("SvodCreateDevice: {} is a host game library link -> {}",
             xe::path_to_utf8(header), xe::path_to_utf8(linked_disc));
      device = std::make_unique<vfs::DiscImageDevice>(device_name, linked_disc);
    } else {
      device =
          vfs::XContentContainerDevice::CreateContentDevice(device_name, header);
    }
    if (!device || !device->Initialize() ||
        !fs->RegisterDevice(std::move(device))) {
      XELOGE("SvodCreateDevice: could not open {} at {}",
             xe::path_to_utf8(header), device_name);
      return X_STATUS_UNSUCCESSFUL;
    }
  }
  const uint32_t ext_size = 0x4C0 + 8 * cache_count + data_size;
  const uint32_t devobj = mem->SystemHeapAlloc(0x100);
  const uint32_t ext = mem->SystemHeapAlloc(ext_size);
  if (!devobj || !ext) {
    return X_STATUS_NO_MEMORY;
  }
  std::memset(mem->TranslateVirtual(devobj), 0, 0x100);
  std::memset(mem->TranslateVirtual(ext), 0, ext_size);
  xe::store_and_swap<uint32_t>(p + 0x20, devobj);
  xe::store_and_swap<uint32_t>(p + 0x24, ext + 0x4C0 + 8 * cache_count);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(SvodCreateDevice, kFileSystem, kSketchy);

dword_result_t StfsControlDevice_entry(dword_t device_object, dword_t code,
                                       lpvoid_t buffer) {
  static std::atomic<uint32_t> clog{0};
  if (clog.fetch_add(1) < 40) {
    XELOGI("StfsControlDevice({:08X}, code {}, buf {:08X})",
           uint32_t(device_object), uint32_t(code), buffer.guest_address());
  }
  switch (uint32_t(code)) {
    case 0:  // lock volume
    case 1:  // unlock volume
    case 2:  // flush - host files are written through as they change
    case 4:  // clear dirty
    case 5:
      return X_STATUS_SUCCESS;
    case 3: {  // report the volume descriptor
      // Phase 1099z146: the kernel returns the LIVE volume descriptor, whose
      // top hash table hash (+0x08..+0x1B) changes as soon as anything is
      // written. xam's commit (817343F0) compares it with the header's copy
      // (817344B0..E4) and, when they differ, hashes, signs and rewrites the
      // package header (8172F1A8). Returning the creation descriptor always
      // compared equal, so new profiles and saves kept a zero header digest,
      // failed "Content digest isn't valid" on reopen and xam deleted them.
      // HOST-SIDE MODEL (not an STFS volume): the folder has no hash tables, so
      // the top-hash field is a SHA-1 of the folder's sorted relative paths,
      // sizes and contents - it changes when the contents change, as the real
      // field does, but is not byte-identical to a console package's.
      std::lock_guard<std::mutex> lock(stfs_folder_lock);
      auto it = stfs_folder_volumes.find(uint32_t(device_object));
      if (it == stfs_folder_volumes.end() || !buffer) {
        return X_STATUS_INVALID_PARAMETER;
      }
      uint8_t descriptor[0x24];
      std::memcpy(descriptor, it->second.descriptor, sizeof(descriptor));
      if (!it->second.folder.empty()) {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (auto e = std::filesystem::recursive_directory_iterator(
                 it->second.folder, ec);
             !ec && e != std::filesystem::recursive_directory_iterator();
             e.increment(ec)) {
          if (e->is_regular_file(ec)) files.push_back(e->path());
        }
        std::sort(files.begin(), files.end());
        sha1::SHA1 s;
        std::vector<char> data;
        for (const auto& f : files) {
          const std::string rel = xe::path_to_utf8(
              std::filesystem::relative(f, it->second.folder, ec));
          s.processBytes(rel.data(), rel.size());
          std::ifstream in(f, std::ios::binary);
          data.assign(std::istreambuf_iterator<char>(in),
                      std::istreambuf_iterator<char>());
          const uint64_t size = data.size();
          s.processBytes(&size, sizeof(size));
          if (!data.empty()) s.processBytes(data.data(), data.size());
        }
        uint8_t digest[20];
        s.finalize(digest);
        std::memcpy(descriptor + 0x08, digest, sizeof(digest));
      }
      std::memcpy(buffer.as<uint8_t*>(), descriptor, sizeof(descriptor));
      return X_STATUS_SUCCESS;
    }
    default:
      return 0xC0000010;  // STATUS_INVALID_DEVICE_REQUEST, as the kernel does
  }
}
DECLARE_XBOXKRNL_EXPORT1(StfsControlDevice, kFileSystem, kImplemented);

dword_result_t IoDismountVolumeByName_entry(lpvoid_t name) {
  // Phase 1095ac: the task at 817318F0 calls this at 817319C0 (return address
  // 817319C4) and then, unless r30 is 2 or 0xF, calls 817316A8 with op 2 -
  // which walks the still-unlinked list head at 81D3CA08 and faults at
  // 817286C0. That one fault stops 81731B04's KeSetEvent(81D21404), which
  // blocks 81750FA8 before 81751164 -> 81780A28, which is what would have built
  // the list in the first place (1095ab). So r30 here decides whether the whole
  // subsystem lives or dies. Read it from the guest.
  if (cvars::guide_bkgnd_watch) {
    auto* th = XThread::GetCurrentThread();
    auto* ctx = (th && th->thread_state()) ? th->thread_state()->context()
                                           : nullptr;
    if (ctx && static_cast<uint32_t>(ctx->lr) == 0x817319C4u) {
      static std::atomic<uint32_t> n{0};
      const uint32_t i = ++n;
      if (i <= 16u) {
        const uint32_t r30 = static_cast<uint32_t>(ctx->r[30]);
        const uint32_t r31 = static_cast<uint32_t>(ctx->r[31]);
        char nm[48] = {0};
        if (name.guest_address()) {
          auto* p8 = kernel_memory()->TranslateVirtual<const char*>(
              name.guest_address());
          for (int k = 0; k < 47 && p8[k]; ++k) nm[k] = p8[k];
        }
        XELOGI("GuideDismount #{}: name@{:08X} '{}' | r30 {} (2 or 0xF would "
               "SKIP the op-2 call at 817319F4) r31 {:08X} | tid {:08X}",
               i, name.guest_address(), nm, r30, r31,
               th ? th->thread_id() : 0u);
      }
    }
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(IoDismountVolumeByName, kFileSystem, kStub);

void ResetIoStateForPowerOff() {
  g_cdrom_devobj = 0;
  std::lock_guard<std::mutex> lock(stfs_folder_lock);
  stfs_folder_volumes.clear();
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Io);
