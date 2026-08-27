/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <ranges>

#include <mutex>
#include <thread>

#include "xenia/base/mutex.h"

#include "xenia/emulator.h"

// For naming OS threads in the Guide thread probe (Windows-only file paths
// already: this translation unit uses CONTEXT/SuspendThread directly).
#include <tlhelp32.h>

#include "config.h"
#include "third_party/fmt/include/fmt/format.h"
#include "third_party/tabulate/single_include/tabulate/tabulate.hpp"
#include "third_party/zarchive/include/zarchive/zarchivecommon.h"
#include "third_party/zarchive/include/zarchive/zarchivewriter.h"
#include "third_party/zarchive/src/sha_256.h"
#include "xenia/apu/audio_system.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/thread_debug_info.h"
#include "xenia/cpu/backend/null_backend.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/xam/achievement_manager.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_video.h"

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include "xenia/kernel/xam/xdbf/spa_info.h"
#include "xenia/kernel/xbdm/xbdm_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/memory.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_host_notification.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/disc_zarchive_device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/virtual_file_system.h"

#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_backend.h"
#endif  // XE_ARCH

DEFINE_double(time_scalar, 1.0,
              "Scalar used to speed or slow time (1x, 2x, 1/2x, etc).",
              "General");

DEFINE_string(
    launch_module, "",
    "Executable to launch from the .iso or the package instead of default.xex "
    "or the module specified by the game. Leave blank to launch the default "
    "module.",
    "General");

DEFINE_bool(allow_dll_module_launch, false,
            "Allow launching a non-executable (DLL) XEX module directly, such "
            "as the Xbox 360 Guide (hud.xex) or xam.xex. Normally these are "
            "loaded by xam rather than booted.",
            "General");

DEFINE_bool(allow_game_relative_writes, false,
            "Not useful to non-developers. Allows code to write to paths "
            "relative to game://. Used for "
            "generating test data to compare with original hardware. ",
            "General");

DECLARE_bool(allow_plugins);

DEFINE_int32(priority_class, 0,
             "Forces Xenia to use different process priority than default one. "
             "It might affect performance and cause unexpected bugs. Possible "
             "values: 0 - Normal, 1 - Above normal, 2 - High",
             "General");

DECLARE_int32(console_type);

namespace xe {
using namespace xe::literals;

Emulator::GameConfigLoadCallback::GameConfigLoadCallback(Emulator& emulator)
    : emulator_(emulator) {
  emulator_.AddGameConfigLoadCallback(this);
}

Emulator::GameConfigLoadCallback::~GameConfigLoadCallback() {
  emulator_.RemoveGameConfigLoadCallback(this);
}

Emulator::Emulator(const std::filesystem::path& command_line,
                   const std::filesystem::path& storage_root,
                   const std::filesystem::path& content_root,
                   const std::filesystem::path& cache_root)
    : on_launch(),
      on_terminate(),
      on_exit(),
      command_line_(command_line),
      storage_root_(storage_root),
      content_root_(content_root),
      cache_root_(cache_root),
      title_name_(),
      title_version_(),
      display_window_(nullptr),
      memory_(),
      audio_system_(),
      audio_media_player_(),
      graphics_system_(),
      input_system_(),
      export_resolver_(),
      file_system_(),
      kernel_state_(),
      main_thread_(),
      title_id_(std::nullopt),
      game_info_database_(),
      paused_(false),
      restoring_(false),
      restore_fence_() {
  if (cvars::priority_class != 0) {
    if (SetProcessPriorityClass(cvars::priority_class)) {
      XELOGI("Higher priority class request: Successful. New priority: {}",
             cvars::priority_class);
    }
  }

#if XE_PLATFORM_WIN32 == 1
  // Show a disclaimer that links to the quickstart
  // guide the first time they ever open the emulator
  uint64_t persistent_flags = GetPersistentEmulatorFlags();
  if (!(persistent_flags & EmulatorFlagDisclaimerAcknowledged)) {
    if ((MessageBoxW(
             nullptr,
             L"DISCLAIMER: Xenia is not for enabling illegal activity, and "
             "support is unavailable for illegally obtained software.\n\n"
             "Please respect this policy as no further reminders will be "
             "given.\n\nThe quickstart guide explains how to use digital or "
             "physical games from your Xbox 360 console.\n\nWould you like "
             "to open it?",
             L"Xenia", MB_YESNO | MB_ICONQUESTION) == IDYES)) {
      LaunchWebBrowser(
          "https://github.com/xenia-canary/xenia-canary/wiki/"
          "Quickstart#how-to-rip-games");
    }
    SetPersistentEmulatorFlags(persistent_flags |
                               EmulatorFlagDisclaimerAcknowledged);
  }
#endif
}

Emulator::~Emulator() {
  // Note that we delete things in the reverse order they were initialized.

  // Give the systems time to shutdown before we delete them.
  if (graphics_system_) {
    graphics_system_->Shutdown();
  }
  if (audio_system_) {
    audio_system_->Shutdown();
  }

  input_system_.reset();
  graphics_system_.reset();
  audio_system_.reset();
  audio_media_player_.reset();

  kernel_state_.reset();
  file_system_.reset();

  processor_.reset();

  export_resolver_.reset();

  ExceptionHandler::Uninstall(Emulator::ExceptionCallbackThunk, this);
}

X_STATUS Emulator::Setup(
    ui::Window* display_window, ui::ImGuiDrawer* imgui_drawer,
    bool require_cpu_backend,
    std::function<std::unique_ptr<apu::AudioSystem>(cpu::Processor*)>
        audio_system_factory,
    std::function<std::unique_ptr<gpu::GraphicsSystem>()>
        graphics_system_factory,
    std::function<std::vector<std::unique_ptr<hid::InputDriver>>(ui::Window*)>
        input_driver_factory) {
  X_STATUS result = X_STATUS_UNSUCCESSFUL;

  display_window_ = display_window;
  imgui_drawer_ = imgui_drawer;

  // Initialize clock.
  // 360 uses a 50MHz clock.
  Clock::set_guest_tick_frequency(50000000);
  // We could reset this with save state data/constant value to help replays.
  Clock::set_guest_system_time_base(Clock::QueryHostSystemTime());
  // This can be adjusted dynamically, as well.
  Clock::set_guest_time_scalar(cvars::time_scalar);

  // Before we can set thread affinity we must enable the process to use all
  // logical processors.
  xe::threading::EnableAffinityConfiguration();

  XELOGI("{}: Initializing Memory...", __func__);
  // Create memory system first, as it is required for other systems.
  memory_ = std::make_unique<Memory>();
  if (!memory_->Initialize()) {
    XELOGE("{}: Cannot initalize memory!", __func__);
    return result;
  }

  XELOGI("{}: Initializing Exports...", __func__);
  // Shared export resolver used to attach and query for HLE exports.
  export_resolver_ = std::make_unique<xe::cpu::ExportResolver>();

  std::unique_ptr<xe::cpu::backend::Backend> backend;
#if XE_ARCH_AMD64
  if (cvars::cpu == "x64") {
    backend.reset(new xe::cpu::backend::x64::X64Backend());
  }
#elif XE_ARCH_ARM64
  if (cvars::cpu == "a64") {
    backend.reset(new xe::cpu::backend::a64::A64Backend());
  }
#endif  // XE_ARCH
  if (cvars::cpu == "any") {
    if (!backend) {
#if XE_ARCH_AMD64
      backend.reset(new xe::cpu::backend::x64::X64Backend());
#elif XE_ARCH_ARM64
      backend.reset(new xe::cpu::backend::a64::A64Backend());
#endif  // XE_ARCH
    }
  }
  if (!backend && !require_cpu_backend) {
    backend.reset(new xe::cpu::backend::NullBackend());
  }

  XELOGI("{}: Initializing Processor...", __func__);
  // Initialize the CPU.
  processor_ = std::make_unique<xe::cpu::Processor>(memory_.get(),
                                                    export_resolver_.get());
  if (!processor_->Setup(std::move(backend))) {
    XELOGE("{}: Cannot initalize processor!", __func__);
    return X_STATUS_UNSUCCESSFUL;
  }

  XELOGI("{}: Initializing Audio...", __func__);
  // Initialize the APU.
  if (audio_system_factory) {
    audio_system_ = audio_system_factory(processor_.get());
    if (!audio_system_) {
      XELOGE("{}: Cannot initalize audio_system!", __func__);
      return X_STATUS_NOT_IMPLEMENTED;
    }
  }

  XELOGI("{}: Initializing Graphics...", __func__);
  // Initialize the GPU.
  graphics_system_ = graphics_system_factory();
  if (!graphics_system_) {
    XELOGE("{}: Cannot initalize graphics_system!", __func__);
    return X_STATUS_NOT_IMPLEMENTED;
  }

  XELOGI("{}: Initializing HID...", __func__);
  // Initialize the HID.
  input_system_ = std::make_unique<xe::hid::InputSystem>(display_window_);
  if (!input_system_) {
    XELOGE("{}: Cannot initalize input_system!", __func__);
    return X_STATUS_NOT_IMPLEMENTED;
  }
  if (input_driver_factory) {
    auto input_drivers = input_driver_factory(display_window_);
    for (size_t i = 0; i < input_drivers.size(); ++i) {
      input_system_->AddDriver(std::move(input_drivers[i]));
    }
  }

  result = input_system_->Setup();
  if (result) {
    return result;
  }

  // Add inputSystem to UI
  imgui_drawer_->LoadInputSystem(input_system_.get());

  XELOGI("{}: Initializing VFS...", __func__);
  // Bring up the virtual filesystem used by the kernel.
  file_system_ = std::make_unique<xe::vfs::VirtualFileSystem>();

  patcher_ = std::make_unique<xe::patcher::Patcher>(storage_root_);

  XELOGI("{}: Initializing Kernel...", __func__);
  // Shared kernel state.
  kernel_state_ = std::make_unique<xe::kernel::KernelState>(this);
#define LOAD_KERNEL_MODULE(t) \
  static_cast<void>(kernel_state_->LoadKernelModule<kernel::t>())
  // HLE kernel modules.
  LOAD_KERNEL_MODULE(xboxkrnl::XboxkrnlModule);
  LOAD_KERNEL_MODULE(xam::XamModule);

  // 415608C3 anti-cheat checks if XDBM is loaded.
  if (cvars::console_type >= 0) {
    LOAD_KERNEL_MODULE(xbdm::XbdmModule);
  }
#undef LOAD_KERNEL_MODULE
  plugin_loader_ = std::make_unique<xe::patcher::PluginLoader>(
      kernel_state_.get(), storage_root() / "plugins");

  XELOGI("{}: Starting graphics_system...", __func__);
  // Setup the core components.
  result = graphics_system_->Setup(
      processor_.get(), kernel_state_.get(),
      display_window_ ? &display_window_->app_context() : nullptr,
      display_window_ != nullptr);
  if (result) {
    XELOGE("{}: Failed to setup graphics_system!", __func__);
    return result;
  }

  if (audio_system_) {
    XELOGI("{}: Starting audio_system...", __func__);
    result = audio_system_->Setup(kernel_state_.get());
    if (result) {
      XELOGE("{}: Failed to setup audio_system!", __func__);
      return result;
    }
    audio_media_player_ = std::make_unique<apu::AudioMediaPlayer>(
        audio_system_.get(), kernel_state_.get());
    audio_media_player_->Setup();
  }

  // Initialize emulator fallback exception handling last.
  ExceptionHandler::Install(Emulator::ExceptionCallbackThunk, this);

  return result;
}

X_STATUS Emulator::TerminateTitle() {
  if (!is_title_open()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  kernel_state_->TerminateTitle();
  title_id_ = std::nullopt;
  title_name_ = "";
  title_version_ = "";
  on_terminate();
  return X_STATUS_SUCCESS;
}

const std::unique_ptr<vfs::Device> Emulator::CreateVfsDevice(
    const std::filesystem::path& path, const std::string_view mount_path) {
  // Must check if the type has changed e.g. XamSwapDisc
  switch (GetFileSignature(path)) {
    case FileSignatureType::XEX0:
    case FileSignatureType::XEXQ:
    case FileSignatureType::XEXH:
    case FileSignatureType::XEX25:
    case FileSignatureType::XEX1:
    case FileSignatureType::XEX2:
    case FileSignatureType::ELF: {
      auto parent_path = path.parent_path();
      return std::make_unique<vfs::HostPathDevice>(
          mount_path, parent_path, !cvars::allow_game_relative_writes);
    } break;
    case FileSignatureType::LIVE:
    case FileSignatureType::CON:
    case FileSignatureType::PIRS: {
      return vfs::XContentContainerDevice::CreateContentDevice(mount_path,
                                                               path);
    } break;
    case FileSignatureType::XISO: {
      return std::make_unique<vfs::DiscImageDevice>(mount_path, path);
    } break;
    case FileSignatureType::ZAR: {
      return std::make_unique<vfs::DiscZarchiveDevice>(mount_path, path);
    } break;
    case FileSignatureType::XBE:
    case FileSignatureType::EXE:
    case FileSignatureType::Unknown:
    default:
      return nullptr;
      break;
  }
}

uint64_t Emulator::GetPersistentEmulatorFlags() {
#if XE_PLATFORM_WIN32 == 1
  uint64_t value = 0;
  DWORD value_size = sizeof(value);
  HKEY xenia_hkey = nullptr;
  LSTATUS lstat =
      RegOpenKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Xenia", &xenia_hkey);
  if (!xenia_hkey) {
    // let the Set function create the key and initialize it to 0
    SetPersistentEmulatorFlags(0ULL);
    return 0ULL;
  }

  lstat = RegQueryValueExA(xenia_hkey, "XEFLAGS", 0, NULL,
                           reinterpret_cast<LPBYTE>(&value), &value_size);
  RegCloseKey(xenia_hkey);
  if (lstat) {
    return 0ULL;
  }
  return value;
#else
  return EmulatorFlagDisclaimerAcknowledged;
#endif
}
void Emulator::SetPersistentEmulatorFlags(uint64_t new_flags) {
#if XE_PLATFORM_WIN32 == 1
  uint64_t value = new_flags;
  DWORD value_size = sizeof(value);
  HKEY xenia_hkey = nullptr;
  LSTATUS lstat =
      RegOpenKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Xenia", &xenia_hkey);
  if (!xenia_hkey) {
    lstat = RegCreateKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Xenia", &xenia_hkey);
  }

  lstat = RegSetValueExA(xenia_hkey, "XEFLAGS", 0, REG_QWORD,
                         reinterpret_cast<const BYTE*>(&value), 8);
  RegFlushKey(xenia_hkey);
  RegCloseKey(xenia_hkey);
#endif
}

X_STATUS Emulator::MountPath(const std::filesystem::path& path,
                             const std::string_view mount_path) {
  auto device = CreateVfsDevice(path, mount_path);
  if (!device || !device->Initialize()) {
    XELOGE(
        "Unable to mount the selected file, it is an unsupported format or "
        "corrupted.");
    return X_STATUS_NO_SUCH_FILE;
  }
  if (!file_system_->RegisterDevice(std::move(device))) {
    XELOGE("Unable to register the input file to {}.", mount_path);
    return X_STATUS_NO_SUCH_FILE;
  }

  file_system_->UnregisterSymbolicLink(kDefaultPartitionSymbolicLink);
  file_system_->UnregisterSymbolicLink(kDefaultGameSymbolicLink);
  file_system_->UnregisterSymbolicLink("plugins:");

  // Create symlinks to the device.
  file_system_->RegisterSymbolicLink(kDefaultGameSymbolicLink, mount_path);
  file_system_->RegisterSymbolicLink(kDefaultPartitionSymbolicLink, mount_path);

  return X_STATUS_SUCCESS;
}

Emulator::FileSignatureType Emulator::GetFileSignature(
    const std::filesystem::path& path) {
  FILE* file = xe::filesystem::OpenFile(path, "rb");

  if (!file) {
    return FileSignatureType::Unknown;
  }

  const uint64_t file_size = std::filesystem::file_size(path);
  constexpr int64_t header_size = 4;

  if (file_size < header_size) {
    return FileSignatureType::Unknown;
  }

  char file_magic[header_size];
  fread(file_magic, sizeof(file_magic), 1, file);

  fourcc_t magic_value =
      make_fourcc(file_magic[0], file_magic[1], file_magic[2], file_magic[3]);

  fclose(file);

  switch (magic_value) {
    case xe::cpu::kXEX0Signature:
      return FileSignatureType::XEX0;
    case xe::cpu::kXEXQSignature:
      return FileSignatureType::XEXQ;
    case xe::cpu::kXEXHSignature:
      return FileSignatureType::XEXH;
    case xe::cpu::kXEX25Signature:
      return FileSignatureType::XEX25;
    case xe::cpu::kXEX1Signature:
      return FileSignatureType::XEX1;
    case xe::cpu::kXEX2Signature:
      return FileSignatureType::XEX2;
    case xe::vfs::kCONSignature:
      return FileSignatureType::CON;
    case xe::vfs::kLIVESignature:
      return FileSignatureType::LIVE;
    case xe::vfs::kPIRSSignature:
      return FileSignatureType::PIRS;
    case xe::vfs::kXSFSignature:
      return FileSignatureType::XISO;
    case xe::cpu::kXBESignature:
      return FileSignatureType::XBE;
    case xe::cpu::kElfSignature:
      return FileSignatureType::ELF;
    default:
      break;
  }

  magic_value = make_fourcc(file_magic[0], file_magic[1], 0, 0);

  if (xe::kernel::kEXESignature == magic_value) {
    return FileSignatureType::EXE;
  }

  file = xe::filesystem::OpenFile(path, "rb");
  xe::filesystem::Seek(file, -header_size, SEEK_END);
  fread(file_magic, 1, header_size, file);
  fclose(file);

  magic_value =
      make_fourcc(file_magic[0], file_magic[1], file_magic[2], file_magic[3]);

  if (xe::vfs::kZarMagic == magic_value) {
    return FileSignatureType::ZAR;
  }

  // Check if XISO
  std::unique_ptr<vfs::Device> device =
      std::make_unique<vfs::DiscImageDevice>("", path);

  XELOGI("Checking for XISO");

  if (device->Initialize()) {
    return FileSignatureType::XISO;
  }

  XELOGE("{}: {} ({:08X})", __func__, path.extension(), magic_value);
  return FileSignatureType::Unknown;
}

X_STATUS Emulator::LaunchPath(const std::filesystem::path& path) {
  X_STATUS mount_result = X_STATUS_SUCCESS;

  switch (GetFileSignature(path)) {
    case FileSignatureType::XEX0:
    case FileSignatureType::XEXQ:
    case FileSignatureType::XEXH:
    case FileSignatureType::XEX25:
    case FileSignatureType::XEX1:
    case FileSignatureType::XEX2:
    case FileSignatureType::ELF: {
      mount_result = MountPath(path, "\\Device\\Harddisk0\\Partition1");
      return mount_result ? mount_result : LaunchXexFile(path);
    } break;
    case FileSignatureType::LIVE:
    case FileSignatureType::CON:
    case FileSignatureType::PIRS: {
      mount_result = MountPath(path, "\\Device\\Package_0");
      return mount_result ? mount_result : LaunchStfsContainer(path);
    } break;
    case FileSignatureType::XISO: {
      mount_result = MountPath(path, "\\Device\\Cdrom0");
      return mount_result ? mount_result : LaunchDiscImage(path);
    } break;
    case FileSignatureType::XBE: {
      XELOGE("OG Xbox games are not supported");
      return X_STATUS_NOT_SUPPORTED;
    } break;
    case FileSignatureType::ZAR: {
      mount_result = MountPath(path, "\\Device\\Cdrom0");
      return mount_result ? mount_result : LaunchDiscArchive(path);
    } break;
    case FileSignatureType::EXE:
    case FileSignatureType::Unknown:
    default:
      return X_STATUS_NOT_SUPPORTED;
      break;
  }
}

X_STATUS Emulator::LaunchXexFile(const std::filesystem::path& path) {
  // We create a virtual filesystem pointing to its directory and symlink
  // that to the game filesystem.
  // e.g., /my/files/foo.xex will get a local fs at:
  // \\Device\\Harddisk0\\Partition1
  // and then get that symlinked to game:\, so
  // -> game:\foo.xex
  // Get just the filename (foo.xex).
  auto file_name = path.filename();

  // Launch the game.
  auto fs_path = fmt::format("{}\\", kDefaultGameSymbolicLink) +
                 xe::path_to_utf8(file_name);
  X_STATUS result = CompleteLaunch(path, fs_path);

  if (XFAILED(result)) {
    return result;
  }

  kernel_state_->deployment_type_ = XDeploymentType::kInstalledToHDD;

  if (!kernel::IsSystemTitle(kernel_state_->title_id())) {
    return result;
  }

  const std::string mount_path =
      utf8::find_base_guest_path(kernel_state_->GetExecutableModule()->path());

  // System related symlinks. This should point to dashboard location in the
  // future.
  XELOGI("System title: registering SystemRoot -> '{}' (exe '{}')", mount_path, kernel_state_->GetExecutableModule()->path());
  file_system_->RegisterSymbolicLink("\\SystemRoot", mount_path);

  auto module = kernel_state_->LoadUserModule("xam.xex");

  if (!module) {
    module = kernel_state_->LoadUserModule("$flash_xam.xex");
  }

  if (module) {
    result = kernel_state_->FinishLoadingUserModule(module, false);
  }

  return result;
}

X_STATUS Emulator::LaunchDiscImage(const std::filesystem::path& path) {
  std::string module_path = FindLaunchModule();
  X_STATUS result = CompleteLaunch(path, module_path);

  if (result == X_STATUS_NOT_FOUND && !cvars::launch_module.empty()) {
    return LaunchDefaultModule(path);
  }
  kernel_state_->deployment_type_ = XDeploymentType::kOpticalDisc;
  return result;
}

X_STATUS Emulator::LaunchDiscArchive(const std::filesystem::path& path) {
  std::string module_path = FindLaunchModule();
  X_STATUS result = CompleteLaunch(path, module_path);

  if (result == X_STATUS_NOT_FOUND && !cvars::launch_module.empty()) {
    return LaunchDefaultModule(path);
  }
  kernel_state_->deployment_type_ = XDeploymentType::kOpticalDisc;
  return result;
}

X_STATUS Emulator::LaunchStfsContainer(const std::filesystem::path& path) {
  std::string module_path = FindLaunchModule();
  X_STATUS result = CompleteLaunch(path, module_path);

  if (result == X_STATUS_NOT_FOUND && !cvars::launch_module.empty()) {
    return LaunchDefaultModule(path);
  }
  kernel_state_->deployment_type_ = XDeploymentType::kDownload;
  return result;
}

X_STATUS Emulator::LaunchDefaultModule(const std::filesystem::path& path) {
  cvars::launch_module = "";
  std::string module_path = FindLaunchModule();
  X_STATUS result = CompleteLaunch(path, module_path);

  if (XSUCCEEDED(result)) {
    kernel_state_->deployment_type_ = XDeploymentType::kInstalledToHDD;
    auto title_id = kernel_state_->title_id();
    if (!kernel::IsSystemTitle(title_id)) {
      // Assumption that any loaded game is loaded as a disc.
      kernel_state_->deployment_type_ = XDeploymentType::kOpticalDisc;
    }
  }
  return result;
}

X_STATUS Emulator::DataMigration(const uint64_t xuid) {
  uint32_t failure_count = 0;
  const std::string xuid_string = fmt::format("{:016X}", xuid);
  const std::string common_xuid_string = fmt::format("{:016X}", 0);
  const std::filesystem::path path_to_profile_data =
      content_root_ / xuid_string / "FFFE07D1" / "00010000" / xuid_string;
  // Filter directories inside. First we need to find any content type
  // directories.
  // Savefiles must go to user specific directory
  // Everything else goes to common
  const auto titles_to_move = xe::filesystem::FilterByName(
      xe::filesystem::ListDirectories(content_root_),
      std::regex("[A-F0-9]{8}"));

  for (const auto& title : titles_to_move) {
    if (xe::path_to_utf8(title.name) == "FFFE07D1" ||
        xe::path_to_utf8(title.name) == "00000000") {
      // SKip any dashboard/profile related data that was previously installed
      continue;
    }

    const auto content_type_dirs = xe::filesystem::FilterByName(
        xe::filesystem::ListDirectories(title.path / title.name),
        std::regex("[A-F0-9]{8}"));

    for (const auto& content_type : content_type_dirs) {
      const std::string used_xuid =
          xe::path_to_utf8(content_type.name) == "00000001"
              ? xuid_string
              : common_xuid_string;

      const auto previous_path = content_root_ / title.name / content_type.name;
      const auto path = content_root_ / used_xuid / title.name;

      if (!std::filesystem::exists(path)) {
        std::filesystem::create_directories(path);
      }

      std::error_code ec;
      std::filesystem::rename(previous_path, path / content_type.name, ec);

      if (ec) {
        failure_count++;
        XELOGW("{}: Moving from: {} to: {} failed! Error message: {} ({:08X})",
               __func__, previous_path, path / content_type.name, ec.message(),
               ec.value());
      }
    }
    // Other directories:
    // Headers - Just copy everything to both common and xuid locations
    // profile - ?
    if (std::filesystem::exists(title.path / title.name / "Headers")) {
      const auto xuid_path =
          content_root_ / xuid_string / title.name / "Headers";

      std::filesystem::create_directories(xuid_path);

      std::error_code ec;
      // Copy to specific user
      std::filesystem::copy(title.path / title.name / "Headers", xuid_path,
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::skip_existing,
                            ec);
      if (ec) {
        failure_count++;
        XELOGW("{}: Copying from: {} to: {} failed! Error message: {} ({:08X})",
               __func__, title.path / title.name / "Headers", xuid_path,
               ec.message(), ec.value());
      }

      const auto header_types =
          xe::filesystem::ListDirectories(title.path / title.name / "Headers");

      if (!(header_types.size() == 1 &&
            header_types.at(0).name == "00000001")) {
        const auto common_path =
            content_root_ / common_xuid_string / title.name / "Headers";

        std::filesystem::create_directories(common_path);

        // Copy to common, skip cases where only savefile header is available
        std::filesystem::copy(title.path / title.name / "Headers", common_path,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::skip_existing,
                              ec);
        if (ec) {
          failure_count++;
          XELOGW(
              "{}: Copying from: {} to: {} failed! Error message: {} ({:08X})",
              __func__, title.path / title.name / "Headers", common_path,
              ec.message(), ec.value());
        }
      }

      if (!ec) {
        // Remove previous directory
        std::error_code ec;
        std::filesystem::remove_all(title.path / title.name / "Headers", ec);
      }
    }

    if (std::filesystem::exists(title.path / title.name / "profile")) {
      // Find directory with previous username. There should be only one!
      const auto old_profile_data =
          xe::filesystem::ListDirectories(title.path / title.name / "profile");

      xe::filesystem::FileInfo entry_to_copy = xe::filesystem::FileInfo();
      if (old_profile_data.size() != 1) {
        for (const auto& entry : old_profile_data) {
          if (entry.name == "User") {
            entry_to_copy = entry;
          }
        }
      } else {
        entry_to_copy = old_profile_data.front();
      }

      const auto path_from =
          title.path / title.name / "profile" / entry_to_copy.name;
      std::error_code ec;
      // Move files from inside to outside for convenience
      std::filesystem::rename(path_from, path_to_profile_data / title.name, ec);
      if (ec) {
        failure_count++;
        XELOGW("{}: Moving from: {} to: {} failed! Error message: {} ({:08X})",
               __func__, path_from, path_to_profile_data / title.name,
               ec.message(), ec.value());
      } else {
        std::error_code ec;
        std::filesystem::remove_all(title.path / title.name / "profile", ec);
      }
    }

    const auto remaining_file_list =
        xe::filesystem::ListDirectories(title.path / title.name);

    if (remaining_file_list.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(title.path / title.name, ec);
    }
  }

  std::string migration_status_message =
      fmt::format("Migration finished with {} {}.", failure_count,
                  failure_count == 1 ? "error" : "errors");

  if (failure_count) {
    migration_status_message.append(
        " For more information check xenia.log file.");
  }
  new xe::ui::HostNotificationWindow(imgui_drawer_, "Migration Status",
                                     migration_status_message, 0);
  return X_STATUS_SUCCESS;
}

X_STATUS Emulator::ProcessContentPackageHeader(
    const std::filesystem::path& path, ContentInstallEntry& installation_info) {
  installation_info.name_ = "Invalid Content Package!";
  installation_info.content_type_ = XContentType::kInvalid;
  installation_info.data_installation_path_ = xe::path_to_utf8(path.filename());

  const auto header = vfs::XContentContainerDevice::ReadContainerHeader(path);

  if (!header || !header->content_header.is_magic_valid()) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_result_ = X_STATUS_INVALID_PARAMETER;
    installation_info.installation_error_message_ = "Invalid Package Type!";
    XELOGE("Failed to initialize device");
    return X_STATUS_INVALID_PARAMETER;
  }

  // Always install savefiles to user signed to slot 0.
  const auto profile =
      kernel_state_->xam_state()->profile_manager()->GetProfile(
          static_cast<uint8_t>(0));

  uint64_t xuid = header->content_metadata.profile_id;
  if (header->content_metadata.content_type == XContentType::kSavedGame &&
      profile) {
    xuid = profile->xuid();
  }

  installation_info.data_installation_path_ = fmt::format(
      "{:016X}/{:08X}/{:08X}/{}", xuid,
      header->content_metadata.execution_info.title_id.get(),
      static_cast<uint32_t>(header->content_metadata.content_type.get()),
      path.filename());

  installation_info.header_installation_path_ = fmt::format(
      "{:016X}/{:08X}/Headers/{:08X}/{}", xuid,
      header->content_metadata.execution_info.title_id.get(),
      static_cast<uint32_t>(header->content_metadata.content_type.get()),
      path.filename());

  installation_info.name_ =
      xe::to_utf8(header->content_metadata.display_name(XLanguage::kEnglish));
  installation_info.content_type_ =
      static_cast<XContentType>(header->content_metadata.content_type);
  installation_info.content_size_ = header->content_metadata.content_size;
  installation_info.installation_state_ = InstallState::pending;

  installation_info.icon_ = imgui_drawer_->LoadImGuiIcon(
      std::span<const uint8_t>(header->content_metadata.title_thumbnail,
                               header->content_metadata.title_thumbnail_size));
  return X_STATUS_SUCCESS;
}

X_STATUS Emulator::InstallContentPackage(
    const std::filesystem::path& path, ContentInstallEntry& installation_info) {
  installation_info.installation_state_ = InstallState::preparing;

  std::unique_ptr<vfs::XContentContainerDevice> device =
      vfs::XContentContainerDevice::CreateContentDevice("", path);

  if (!device || !device->Initialize()) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_error_message_ =
        "Device initialization failed!";
    installation_info.installation_result_ = X_STATUS_ACCESS_DENIED;
    XELOGE("Failed to initialize device");
    return X_STATUS_INVALID_PARAMETER;
  }

  const std::filesystem::path installation_path =
      content_root() / installation_info.data_installation_path_;

  const std::filesystem::path header_path =
      content_root() / installation_info.header_installation_path_;

  if (!std::filesystem::exists(content_root())) {
    const std::error_code ec = xe::filesystem::CreateFolder(content_root());
    if (ec) {
      installation_info.installation_state_ = InstallState::failed;
      installation_info.installation_error_message_ = ec.message();
      installation_info.installation_result_ = X_STATUS_ACCESS_DENIED;
      return X_STATUS_ACCESS_DENIED;
    }
  }

  const auto disk_space = std::filesystem::space(content_root());
  if (disk_space.available < installation_info.content_size_ * 1.1f) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_error_message_ = "Insufficient disk space!";
    installation_info.installation_result_ = X_STATUS_DISK_FULL;
    return X_STATUS_DISK_FULL;
  }

  if (std::filesystem::exists(installation_path)) {
    // TODO(Gliniak): Popup
    // Do you want to overwrite already existing data?
  } else {
    std::error_code error_code;
    std::filesystem::create_directories(installation_path, error_code);
    if (error_code) {
      installation_info.installation_state_ = InstallState::failed;
      installation_info.installation_error_message_ =
          "Cannot Create Content Directory!";
      installation_info.installation_result_ = error_code.value();
      return error_code.value();
    }
  }

  installation_info.content_size_ = device->data_size();
  installation_info.installation_state_ = InstallState::installing;

  vfs::VirtualFileSystem::ExtractContentHeader(device.get(), header_path);

  X_STATUS error_code = vfs::VirtualFileSystem::ExtractContentFiles(
      device.get(), installation_path,
      installation_info.currently_installed_size_);
  if (error_code != X_ERROR_SUCCESS) {
    installation_info.installation_state_ = InstallState::failed;
    return error_code;
  }

  installation_info.installation_state_ = InstallState::installed;
  installation_info.currently_installed_size_ = installation_info.content_size_;
  kernel_state()->BroadcastNotification(kXNotificationLiveContentInstalled, 0);

  if (installation_info.content_type_ == XContentType::kProfile) {
    kernel_state_->xam_state()->profile_manager()->ReloadProfiles();
  }

  return error_code;
}

X_STATUS Emulator::ExtractZarchivePackage(
    const std::filesystem::path& path,
    const std::filesystem::path& extract_dir) {
  std::unique_ptr<vfs::Device> device =
      std::make_unique<vfs::DiscZarchiveDevice>("", path);
  if (!device->Initialize()) {
    XELOGE("Failed to initialize device");
    return X_STATUS_INVALID_PARAMETER;
  }

  if (std::filesystem::exists(extract_dir)) {
    // TODO(Gliniak): Popup
    // Do you want to overwrite already existing data?
  } else {
    std::error_code error_code;
    std::filesystem::create_directories(extract_dir, error_code);
    if (error_code) {
      return error_code.value();
    }
  }

  uint64_t progress = 0;
  return vfs::VirtualFileSystem::ExtractContentFiles(device.get(), extract_dir,
                                                     progress);
}

X_STATUS Emulator::CreateZarchivePackage(
    const std::filesystem::path& inputDirectory,
    const std::filesystem::path& outputFile) {
  std::vector<uint8_t> buffer;
  buffer.resize(64 * 1024);

  std::error_code ec;
  PackContext packContext;
  packContext.outputFilePath = outputFile;

  ZArchiveWriter zWriter(
      [](int32_t partIndex, void* ctx) {
        PackContext* packContext = reinterpret_cast<PackContext*>(ctx);
        packContext->currentOutputFile =
            std::ofstream(packContext->outputFilePath, std::ios::binary);

        if (!packContext->currentOutputFile.is_open()) {
          XELOGI("Failed to create output file: {}\n",
                 packContext->outputFilePath.string());
          packContext->hasError = true;
        }
      },
      [](const void* data, size_t length, void* ctx) {
        PackContext* packContext = reinterpret_cast<PackContext*>(ctx);
        packContext->currentOutputFile.write(
            reinterpret_cast<const char*>(data), length);
      },
      &packContext);

  if (packContext.hasError) {
    return X_STATUS_UNSUCCESSFUL;
  }

  for (auto const& dirEntry :
       std::filesystem::recursive_directory_iterator(inputDirectory)) {
    std::filesystem::path pathEntry =
        std::filesystem::relative(dirEntry.path(), inputDirectory, ec);

    if (ec) {
      XELOGI("Failed to get relative path {}\n", pathEntry.string());
      return X_STATUS_UNSUCCESSFUL;
    }

    if (dirEntry.is_directory()) {
      if (!zWriter.MakeDir(pathEntry.generic_string().c_str(), false)) {
        XELOGI("Failed to create directory {}\n", pathEntry.string());
        return X_STATUS_UNSUCCESSFUL;
      }
    } else if (dirEntry.is_regular_file()) {
      // Don't pack itself to prevent infinite packing.
      if (dirEntry == outputFile) {
        continue;
      }

      XELOGI("Adding file: {}\n", pathEntry.string());

      if (!zWriter.StartNewFile(pathEntry.generic_string().c_str())) {
        XELOGI("Failed to create archive file {}\n", pathEntry.string());
        return X_STATUS_UNSUCCESSFUL;
      }

      std::filesystem::path file_to_pack_path = inputDirectory / pathEntry;
      FILE* file = xe::filesystem::OpenFile(file_to_pack_path, "rb");

      if (!file) {
        XELOGI("Failed to open input file {}\n", pathEntry.string());
        return X_STATUS_UNSUCCESSFUL;
      }

      const uint64_t file_size = std::filesystem::file_size(file_to_pack_path);
      uint64_t total_bytes_read = 0;

      while (total_bytes_read < file_size) {
        uint64_t bytes_read = fread(buffer.data(), 1, buffer.size(), file);

        total_bytes_read += bytes_read;

        zWriter.AppendData(buffer.data(), bytes_read);
      }

      fclose(file);
    }

    if (packContext.hasError) {
      return X_STATUS_UNSUCCESSFUL;
    }
  }

  zWriter.Finalize();

  return X_STATUS_SUCCESS;
}

static void InstallGuideStoreTraces(xe::kernel::KernelState* ks);
static void ArmGuideThreadProbe(xe::kernel::KernelState* ks, int pdelay);
static void ReportXamTextPopulation(Memory* memory, const char* when);

void Emulator::on_guide_button_pressed(uint8_t user_index) {
  XELOGI("Guide button: pressed (user {}), handler={:08X} buf={:08X} "
         "out_sz={:08X}",
         user_index, guide_handler_, guide_buf_, guide_out_sz_);
  // Drive the Guide open sequence if hud.xex is loaded and registered. This
  // runs the message dispatch on a guest thread - hud's handler must not be
  // called from the host UI thread. It does not yet produce a visible Guide
  // (hud needs to be hosted by xam to build and draw its scenes) but it is
  // the real entry point, so this is where an open belongs once hosting
  // exists.
  if (guide_handler_ && guide_buf_ && guide_out_sz_ && kernel_state_) {
    auto* ks = kernel_state_.get();
    uint32_t handler = guide_handler_;
    uint32_t buf = guide_buf_;
    uint32_t osz = guide_out_sz_;
    uint32_t hud_base = guide_hud_base_;
    uint32_t skin_mod = guide_skin_module_;
    auto t = kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
        ks, 512 * 1024, 0, [ks, handler, buf, osz, hud_base, skin_mod]() -> int {
          auto* ts = kernel::XThread::GetCurrentThread()->thread_state();
          uint64_t a[] = {0x80000004ull, buf, osz};
          XELOGI("Guide button: dispatching open to {:08X}", handler);
          uint64_t r =
              ks->processor()->Execute(ts, handler, a, xe::countof(a));
          XELOGI("Guide button: handler returned {:08X}",
                 static_cast<uint32_t>(r));
          // Then drive hud's own XUI init and render loop. hud is a system
          // app: the system normally creates its thread and calls these. Its
          // entry points sit at fixed offsets from the module base -
          // base+0xA898 calls XuiInit and XuiRenderCreateDC, base+0xAB28
          // calls XuiRenderBegin/End/Present - and both take the Guide object
          // in r3 and only read from it.
          if (hud_base) {
            uint32_t obj = xe::load_and_swap<uint32_t>(
                ks->memory()->TranslateVirtual(0x91400690u));
            if (obj) {
              // Dump the Guide object's vtable. The draw entry points used
              // below were guessed from static scanning; the object's own
              // vtable is the authoritative list of its virtual methods.
              uint32_t vt = xe::load_and_swap<uint32_t>(
                  ks->memory()->TranslateVirtual(obj));
              XELOGI("Guide button: obj={:08X} vtable={:08X}", obj, vt);
              if (vt) {
                for (int i = 0; i < 48; ++i) {
                  uint32_t fn = xe::load_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(vt + i * 4));
                  XELOGI("Guide button: vtable[{}] = {:08X}", i, fn);
                }
              }
              auto rd = [&](uint32_t a) {
                return xe::load_and_swap<uint32_t>(
                    ks->memory()->TranslateVirtual(a));
              };
              // Dump hud's XUI import thunks. The import-table dump marks
              // these "!!" (no HLE implementation), which says nothing about
              // where the LLE override actually pointed them.
              for (uint32_t th : {0x913FE7E4u, 0x913FE7F4u, 0x913FE874u}) {
                XELOGI("Guide button: thunk {:08X}: {:08X} {:08X} {:08X} "
                       "{:08X}",
                       th, rd(th), rd(th + 4), rd(th + 8), rd(th + 12));
              }
              XELOGI("Guide button: pre-init  +8={:08X} +12={:08X} "
                     "+20={:08X}",
                     rd(obj + 16 + 8), rd(obj + 16 + 12),
                     rd(obj + 16 + 20));
              // [obj+20] gates DC creation in the init at hud+0xA898 and is
              // never dereferenced there (the register is reloaded from
              // [obj+12] immediately after the test), so forcing it non-zero
              // is safe and lets XuiRenderCreateDC run.
              // The XUI render context global (81D6C978) is written inside
              // the function containing runtime 818FF278. Find which export
              // that is by listing the XUI ordinal range.
              {
                // Does the REAL xam export ordinal 0x506? The image is a
                // XEX with its own export mechanism, not a PE .edata, so
                // parsing it offline does not work - ask the loader.
                auto bm = ks->GetModule("xam.xex", true);
                uint32_t ba = bm ? bm->GetProcAddressByOrdinal(0x506) : 0;
                XELOGI("Guide button: XamInputSendXenonButtonPress "
                       "(ord 506) -> {:08X}", ba);
                if (ba && cvars::guide_xam_button_api >= 0) {
                  // The wrapper loads its context from [81D4F610] and
                  // the worker dereferences it at +0x10, so a null there
                  // faults. 817C2B68 takes no arguments, asserts the
                  // global is still 0, allocates a 36-byte context and
                  // stores it. Run it first if nothing else has.
                  uint32_t ictx = xe::load_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(0x81D4F610u));
                  if (!ictx) {
                    uint64_t ia2[] = {0};
                    uint64_t ir2 = ks->processor()->Execute(
                        ts, 0x817C2B68u, ia2, xe::countof(ia2));
                    ictx = xe::load_and_swap<uint32_t>(
                        ks->memory()->TranslateVirtual(0x81D4F610u));
                    XELOGI("Guide button: xam input ctx init -> {:08X}, "
                           "[81D4F610] now {:08X}",
                           static_cast<uint32_t>(ir2), ictx);
                  }
                  uint64_t ba_args[] = {
                      uint64_t(uint32_t(cvars::guide_xam_button_api))};
                  uint64_t br = ks->processor()->Execute(
                      ts, ba, ba_args, xe::countof(ba_args));
                  XELOGI("Guide button: xam button API({}, {:X}) -> "
                         "{:08X}",
                         0,
                         uint32_t(cvars::guide_xam_button_api),
                         static_cast<uint32_t>(br));
                }
              }
              auto xm = ks->GetModule("xam.xex", true);
              if (xm) {
                for (uint32_t ord = 0x340; ord <= 0x358; ++ord) {
                  uint32_t fa = xm->GetProcAddressByOrdinal(ord);
                  if (fa) {
                    XELOGI("Guide button: xam ord {:03X} -> {:08X}", ord, fa);
                  }
                }
              }
              if (cvars::guide_call_xuiinit) {
                // Target read out of hud's XuiInit thunk at 913FE7E4
                // (lis 0x8195 / ori 0x3760). XuiInit accepts null params -
                // that case branches straight to the real init - and returns
                // 1 if XUI was already initialised.
                XELOGI("Guide button: XUI ctx before = {:08X}",
                       rd(0x81D6C978u));
                uint64_t xa[] = {0};
                uint64_t xr = ks->processor()->Execute(ts, 0x81953760u, xa,
                                                       xe::countof(xa));
                XELOGI("Guide button: XuiInit returned {:08X}, ctx now {:08X}",
                       static_cast<uint32_t>(xr), rd(0x81D6C978u));
              }
                // Armed early instead (see ArmGuideThreadProbe): a probe
                // armed from this handler cannot fire on a freeze that
                // happens before the handler is ever published.
                // Arm again here: breakpoints set before the target is
                // JIT-translated do not take, so the xam-load call is
                // inert for anything not yet executed.
                InstallGuideStoreTraces(ks);
                static std::unique_ptr<cpu::Breakpoint> pump_bp;
                if (cvars::guide_trace_pump && !pump_bp) {
                  pump_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      uint64_t(uint32_t(cvars::guide_trace_pump)),
                      [](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                         uint64_t host_pc) {
                        static std::atomic<uint32_t> n{0};
                        uint32_t k = ++n;
                        if (k <= 3 || k % 500 == 0) {
                          auto* th = kernel::XThread::GetCurrentThread();
                          auto* c = th ? th->thread_state()->context()
                                       : nullptr;
                          // r3 at the DC vtable slots is the device
                          // context: [dc+0x1CC] is its device wrapper and
                          // [dev+0x32A0] its RT0. Follow the chain so the
                          // device xam picks for itself is visible.
                          auto* pm = th ? th->kernel_state()->memory()
                                        : nullptr;
                          auto rp = [pm](uint32_t a) -> uint32_t {
                            if (!pm) return 0;
                            if (a < 0x1000u) return 0;
                            auto* hp = pm->LookupHeap(a);
                            if (!hp || hp->QueryRangeAccess(a, a + 4) ==
                                           xe::memory::PageAccess::kNoAccess)
                              return 0;
                            return xe::load_and_swap<uint32_t>(
                                pm->TranslateVirtual(a));
                          };
                          uint32_t dc = c ? uint32_t(c->r[3]) : 0;
                          uint32_t wrap = rp(dc + 0x1CCu);
                          uint32_t dev = rp(wrap + 12u);
                          XELOGI("GuidePump hit #{} thread {:08X} r3={:08X} "
                                 "lr={:08X} [dc+1CC]={:08X} dev={:08X} "
                                 "RT0={:08X} [3F74]={:08X} [134]={:08X}",
                                 k, th ? th->handle() : 0, dc,
                                 c ? uint32_t(c->lr) : 0, wrap, dev,
                                 rp(dev + 0x32A0u), rp(dev + 0x3F74u),
                                 rp(dc + 0x134u));
                        }
                      });
                  ks->processor()->AddBreakpoint(pump_bp.get());
                  XELOGI("GuidePump: counting breakpoint at {:08X}",
                         uint32_t(cvars::guide_trace_pump));
                }
                static std::unique_ptr<cpu::Breakpoint> srt_bp;
                if (cvars::guide_trace_setrendertarget && !srt_bp) {
                  srt_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x819F31A8ull,
                      [](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                         uint64_t host_pc) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) return;
                        auto* c = th->thread_state()->context();
                        static std::atomic<uint32_t> n{0};
                        uint32_t k = ++n;
                        if (k > 40) return;
                        XELOGI("SetRenderTarget #{}: dev={:08X} index={} "
                               "surface={:08X} lr={:08X}  <- {}",
                               k, static_cast<uint32_t>(c->r[3]),
                               static_cast<uint32_t>(c->r[4]),
                               static_cast<uint32_t>(c->r[5]),
                               static_cast<uint32_t>(c->lr),
                               c->r[5] ? "BIND" : "unbind");
                      });
                  ks->processor()->AddBreakpoint(srt_bp.get());
                  XELOGI("SetRenderTarget trace installed at 819F31A8");
                }
                // 819F7F20 passes its 4th argument (r6) down to 819F5D18 as
                // r8, which becomes r14 there and is dereferenced at +32
                // without a guard. 819F7F20 itself guards the same read. Log
                // r6 and lr to see which caller supplies the null.
                // 81A0FA80 allocates the front buffer (819E7310) and stores
                // it at [r29+3F74] at 81A0FCD4, then binds RT0 further down.
                // The bind is observed to happen but no device ends up with a
                // front buffer, so either the store runs against a third
                // object or the bind is reachable without it. Break on both.
                static std::unique_ptr<cpu::Breakpoint> fb_bp;
                if (cvars::guide_trace_setrendertarget && !fb_bp) {
                  fb_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x81A0FCD4ull,
                      [](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                         uint64_t host_pc) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) return;
                        auto* c = th->thread_state()->context();
                        static std::atomic<uint32_t> n{0};
                        if (++n > 10) return;
                        XELOGI("FrontBufferStore: [r29={:08X} +3F74] = r3={:08X}"
                               "  r31={:08X} lr={:08X}",
                               static_cast<uint32_t>(c->r[29]),
                               static_cast<uint32_t>(c->r[3]),
                               static_cast<uint32_t>(c->r[31]),
                               static_cast<uint32_t>(c->lr));
                      });
                  ks->processor()->AddBreakpoint(fb_bp.get());
                  XELOGI("FrontBufferStore trace installed at 81A0FCD4");
                }
                static std::unique_ptr<cpu::Breakpoint> r6_bp;
                if (cvars::guide_trace_setrendertarget && !r6_bp) {
                  r6_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x819F7F20ull,
                      [](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                         uint64_t host_pc) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) return;
                        auto* c = th->thread_state()->context();
                        static std::atomic<uint32_t> n{0};
                        uint32_t k = ++n;
                        if (k > 30) return;
                        XELOGI("819F7F20 #{}: r3={:08X} r4={:08X} r5={:08X} "
                               "r6={:08X} r7={:08X} lr={:08X} {}",
                               k, static_cast<uint32_t>(c->r[3]),
                               static_cast<uint32_t>(c->r[4]),
                               static_cast<uint32_t>(c->r[5]),
                               static_cast<uint32_t>(c->r[6]),
                               static_cast<uint32_t>(c->r[7]),
                               static_cast<uint32_t>(c->lr),
                               c->r[6] ? "" : "  <-- r6 NULL");
                      });
                  ks->processor()->AddBreakpoint(r6_bp.get());
                  XELOGI("819F7F20 trace installed");
                }
              if (cvars::guide_force_cmdbuf_complete > 0) {
                static bool fc_started = false;
                if (!fc_started) {
                  fc_started = true;
                  auto* fm4 = ks->memory();
                  int delay = cvars::guide_force_cmdbuf_complete;
                  std::thread([fm4, delay]() {
                    xe::threading::set_name("CmdBufComplete");
                    std::this_thread::sleep_for(std::chrono::seconds(delay));
                    auto rd4 = [fm4](uint32_t a) {
                      return a ? xe::load_and_swap<uint32_t>(
                                     fm4->TranslateVirtual(a)) : 0u;
                    };
                    for (int i = 0; i < 4000; ++i) {
                      for (uint32_t slot : {0x81D43684u, 0x801E6FC8u}) {
                        uint32_t dev = rd4(slot);
                        if (!dev) continue;
                        auto* p = fm4->TranslateVirtual(dev + 0x2B3Du);
                        if (p && !(*p & 0x02)) {
                          *p = static_cast<uint8_t>(*p | 0x02);
                          XELOGI("CmdBufComplete: set bit1 of [{:08X}+2B3D]",
                                 dev);
                        }
                      }
                      std::this_thread::sleep_for(
                          std::chrono::milliseconds(5));
                    }
                  }).detach();
                  XELOGI("CmdBufComplete: armed, {}s", delay);
                }
              }
              if (cvars::guide_watch_null_render) {
                static bool nr_started = false;
                if (!nr_started) {
                  nr_started = true;
                  auto* nm = ks->memory();
                  std::thread([nm]() {
                    xe::threading::set_name("NullRenderWatch");
                    auto rd5 = [nm](uint32_t a) {
                      return a ? xe::load_and_swap<uint32_t>(
                                     nm->TranslateVirtual(a)) : 0u;
                    };
                    uint32_t last_ctx = 0, last_flag = 0xFFFFFFFF;
                    for (int i = 0; i < 120000; ++i) {
                      uint32_t ctx = rd5(0x81D6C978u);
                      if (ctx != last_ctx) {
                        XELOGI("NullRenderWatch: ctx {:08X} -> {:08X}",
                               last_ctx, ctx);
                        last_ctx = ctx;
                        last_flag = 0xFFFFFFFF;
                      }
                      if (ctx) {
                        uint32_t f = rd5(ctx + 0x1Cu);
                        if (f != last_flag) {
                          XELOGI("NullRenderWatch: [ctx+1C] {:08X} -> {:08X}",
                                 last_flag, f);
                          last_flag = f;
                        }
                      }
                      std::this_thread::sleep_for(
                          std::chrono::microseconds(250));
                    }
                  }).detach();
                  XELOGI("NullRenderWatch: armed");
                }
              }
              if (cvars::guide_watch_front_buffer) {
                static bool watch_started = false;
                if (!watch_started) {
                  watch_started = true;
                  auto* wm = ks->memory();
                  std::thread([wm]() {
                    xe::threading::set_name("FrontBufferWatch");
                    auto rdw = [wm](uint32_t a) {
                      return a ? xe::load_and_swap<uint32_t>(
                                     wm->TranslateVirtual(a))
                               : 0u;
                    };
                    uint32_t last_dev[2] = {0, 0}, last_fb[2] = {0, 0};
                    const uint32_t slots[2] = {0x81D43684u, 0x801E6FC8u};
                    const char* names[2] = {"81D43684", "801E6FC8"};
                    for (int i = 0; i < 60000; ++i) {
                      for (int k = 0; k < 2; ++k) {
                        uint32_t dev = rdw(slots[k]);
                        uint32_t fb = dev ? rdw(dev + 0x3F74u) : 0;
                        if (dev != last_dev[k]) {
                          XELOGI("FBWatch[{}]: device {:08X} -> {:08X}",
                                 names[k], last_dev[k], dev);
                          last_dev[k] = dev;
                          last_fb[k] = 0;
                        }
                        if (fb != last_fb[k]) {
                          XELOGI("FBWatch[{}]: dev {:08X} front buffer "
                                 "{:08X} -> {:08X}",
                                 names[k], dev, last_fb[k], fb);
                          last_fb[k] = fb;
                        }
                      }
                      std::this_thread::sleep_for(
                          std::chrono::microseconds(500));
                    }
                    XELOGI("FBWatch: finished");
                  }).detach();
                  XELOGI("FBWatch: started");
                }
              }
              if (cvars::guide_bootstrap_before_device &&
                  cvars::guide_bootstrap_on_title_thread) {
                // The mode-1 creator below never returns, so anything after it
                // is dead code in that configuration - including the queue
                // call that starts the Guide bootstrap.
                XELOGI("Guide button: queueing bootstrap BEFORE device creation");
                kernel::xboxkrnl::QueueGuideBootstrap(
                    hud_base, obj, cvars::guide_use_title_device, skin_mod);
              }
              if (cvars::guide_create_xam_device) {
                uint32_t gate_ptr = rd(0x815F048Cu);
                uint32_t gate = gate_ptr ? rd(gate_ptr) : 0;
                XELOGI("Guide button: VdGlobalDevice(801E6FC4) = {:08X}",
                       rd(0x801E6FC4u));
                // 817439D0 registers callbacks by dispatching through
                // [[815F044C]]->vtable[6]; if that object is null it returns
                // having registered nothing. 81723D98 registers 81723D70
                // there, and 81723D70 is the head of the chain that would
                // eventually run 817915A0 and signal the three events.
                {
                  uint32_t slot = rd(0x815F044Cu);
                  uint32_t obj = slot ? rd(slot) : 0;
                  XELOGI("Guide button: callback registry [815F044C]={:08X} "
                         "-> obj {:08X} {}",
                         slot, obj,
                         obj ? "(present)" : "(NULL - registration is a no-op)");
                }
                XELOGI("Guide button: device gate [815F048C]={:08X} "
                       "[*]={:08X} bit200={}",
                       gate_ptr, gate, (gate & 0x200) ? "set" : "clear");
                uint64_t ca[] = {0};
                // 8178F748 asks 819F4D28 for a mode-2 device, which skips the
                // ring buffer bring-up by design. 8178E9F0 asks for mode 1,
                // which takes it - and which nothing inside xam ever calls, so
                // on hardware it comes from the system boot.
                uint32_t create_fn =
                    cvars::guide_create_primary_device ? 0x8178E9F0u
                                                       : 0x8178F748u;
                // Mode 1 calls KeGetCurrentProcessType and asserts unless the
                // matching device global is still empty. As SYSTEM it looks at
                // VdGlobalXamDevice (empty); as anything else at VdGlobalDevice
                // (the title's, already set).
                uint8_t saved_pt = 0, saved_ptd = 0;
                kernel::XThread* cur = kernel::XThread::GetCurrentThread();
                if (cvars::guide_system_process_type && cur) {
                  auto* kt = cur->guest_object<kernel::X_KTHREAD>();
                  saved_pt = kt->process_type;
                  saved_ptd = kt->process_type_dup;
                  kt->process_type = kernel::X_PROCTYPE_SYSTEM;
                  kt->process_type_dup = kernel::X_PROCTYPE_SYSTEM;
                  XELOGI("Guide button: process type {} -> SYSTEM for device "
                         "creation", saved_pt);
                }
                if (cvars::guide_call_boot_entry) {
                  // 81751428 crashed on a null global at 81D3C8E8. The only
                  // code that takes that global's address is 81727500, which
                  // hands it to an initialiser - and 81727500 is reached only
                  // from 81750FA8, another entry point with no caller inside
                  // xam. So the dependency is derived, not guessed: run the
                  // initialiser first.
                  // 81750FA8 already runs every session, but with its first
                  // argument zero - and that argument is r29, which gates the
                  // call to 81727500 that initialises 81D3C8E8
                  // ("cmpwi cr6,r29,0 / bne -> 81751294"). Calling it with 0,
                  // as the previous attempt did, takes the same path that
                  // skips the initialiser. Pass 1.
                  for (uint32_t entry : {0x81750FA8u, 0x81751428u}) {
                    uint64_t ba[] = {entry == 0x81750FA8u ? 1u : 0u};
                    uint64_t br = ks->processor()->Execute(ts, entry, ba,
                                                           xe::countof(ba));
                    XELOGI("Guide button: boot entry {:08X} returned {:08X}; "
                           "[81D3C8E8]={:08X}",
                           entry, static_cast<uint32_t>(br),
                           xe::load_and_swap<uint32_t>(
                               ks->memory()->TranslateVirtual(0x81D3C8E8u)));
                  }
                }
                if (cvars::guide_restore_title_ring) {
                  kernel::xboxkrnl::GuideSaveTitleRing();
                }
                XELOGI("Guide button: calling device creator {:08X}", create_fn);
                {
                  // Snapshot the ring before the creator: mode 1 reaches
                  // VdInitializeRingBuffer and the title stops swapping.
                  // If the ring is merely repointed, restoring it may let
                  // the title carry on; if it is torn down some other way,
                  // that shows here instead.
                  auto* rgs = ks->emulator()->graphics_system();
                  if (rgs && rgs->command_processor()) {
                    uint32_t rp = 0, rs = 0, rw = 0;
                    rgs->command_processor()->GuideRingState(&rp, &rs, &rw);
                    XELOGI("GuideRing: before creator ptr={:08X} "
                           "size={:08X} wb={:08X}", rp, rs, rw);
                    std::thread([rgs, rp, rs, rw]() {
                      xe::threading::set_name("GuideRingWatch");
                      for (int i = 0; i < 60; ++i) {
                        xe::threading::Sleep(std::chrono::seconds(1));
                        uint32_t p2 = 0, s2 = 0, w2 = 0;
                        rgs->command_processor()->GuideRingState(&p2, &s2,
                                                                &w2);
                        if (p2 != rp || s2 != rs || w2 != rw) {
                          XELOGI("GuideRing: CHANGED after {}s ptr {:08X}"
                                 "->{:08X} size {:08X}->{:08X} wb "
                                 "{:08X}->{:08X}",
                                 i + 1, rp, p2, rs, s2, rw, w2);
                          if (cvars::guide_restore_title_ring) {
                            // Isolated test of the core claim: the ring
                            // is re-pointed, not destroyed, so handing
                            // the registers back should let the title
                            // resume. Give mode-1 bring-up a moment to
                            // finish before taking the ring back.
                            xe::threading::Sleep(
                                std::chrono::seconds(3));
                            kernel::xboxkrnl::GuideRestoreTitleRing();
                          }
                          return;
                        }
                      }
                      XELOGI("GuideRing: unchanged for 60s");
                    }).detach();
                  }
                }
                if (cvars::guide_stall_probe_seconds > 0 && cur) {
                  // CreateDevice may never return, so the probe has to live on
                  // a host thread of its own. Collect raw RIPs first and
                  // resolve them only after resuming - LookupFunction takes the
                  // code cache lock, and holding a suspended thread across that
                  // is a deadlock waiting to happen.
                  void* nh = cur->thread() ? cur->thread()->native_handle()
                                           : nullptr;
                  int delay = cvars::guide_stall_probe_seconds;
                  auto* proc = ks->processor();
                  if (nh) {
                    std::thread([nh, delay, proc, ksp = ks]() {
                      xe::threading::set_name("GuideStallProbe");
                      std::this_thread::sleep_for(std::chrono::seconds(delay));
                      uint64_t rips[8] = {};
                      uint32_t gr[8][4] = {};
                      for (int i = 0; i < 8; ++i) {
                        CONTEXT ctx = {};
                        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                        if (SuspendThread(reinterpret_cast<HANDLE>(nh)) !=
                            static_cast<DWORD>(-1)) {
                          if (GetThreadContext(reinterpret_cast<HANDLE>(nh),
                                               &ctx)) {
                            rips[i] = ctx.Rip;
                            // The x64 backend keeps the PPCContext pointer in
                            // rsi (X64Emitter::GetContextReg) and the guest
                            // membase in rdi. PPCContext is host memory, so
                            // the guest GPRs can be read straight out of it
                            // while the thread is held.
                            auto* gc =
                                reinterpret_cast<cpu::ppc::PPCContext*>(
                                    ctx.Rsi);
                            if (gc) {
                              gr[i][0] = static_cast<uint32_t>(gc->r[3]);
                              gr[i][1] = static_cast<uint32_t>(gc->r[11]);
                              gr[i][2] = static_cast<uint32_t>(gc->r[29]);
                              gr[i][3] = static_cast<uint32_t>(gc->r[31]);
                            }
                          }
                          ResumeThread(reinterpret_cast<HANDLE>(nh));
                        }
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(120));
                      }
                      // 819F4488 polls [[device+2B10]] against [arg+8] with
                      // the 819F3FC8 delay between reads. Sample the polled
                      // word itself so "never advances" is measured, not
                      // assumed. 81D43684 is xam's device slot.
                      {
                        auto* m = ksp->memory();
                        auto rdp = [m](uint32_t a) {
                          return xe::load_and_swap<uint32_t>(
                              m->TranslateVirtual(a));
                        };
                        // Use the object the poll actually holds in r29,
                        // not the device global - they are different objects,
                        // and reading the global is what made an earlier pass
                        // report [2B10]=0 and reject a correct guess.
                        uint32_t dv = gr[0][2] ? gr[0][2] : rdp(0x81D43684u);
                        uint32_t frame = gr[0][3];
                        if (frame) {
                          XELOGI("StallProbe: poll arg r31={:08X} [r31+8]={:08X}",
                                 frame, rdp(frame + 8));
                        }
                        uint32_t idp = dv ? rdp(dv + 0x2B10u) : 0;
                        XELOGI("StallProbe: device={:08X} [2B10]={:08X}", dv,
                               idp);
                        if (idp) {
                          uint32_t a = rdp(idp);
                          std::this_thread::sleep_for(
                              std::chrono::milliseconds(1500));
                          uint32_t b = rdp(idp);
                          XELOGI("StallProbe: polled word [{:08X}] = {:08X} "
                                 "then {:08X} ({})",
                                 idp, a, b,
                                 a == b ? "UNCHANGED" : "advanced");
                          if (cvars::guide_fake_gpu_writeback) {
                            auto wr = [m](uint32_t addr, uint32_t v) {
                              xe::store_and_swap<uint32_t>(
                                  m->TranslateVirtual(addr), v);
                            };
                            uint32_t v = b;
                            for (int k = 0; k < 24; ++k) {
                              wr(idp, ++v);
                              std::this_thread::sleep_for(
                                  std::chrono::milliseconds(100));
                              CONTEXT c2 = {};
                              c2.ContextFlags = CONTEXT_CONTROL;
                              uint32_t g2 = 0;
                              if (SuspendThread(reinterpret_cast<HANDLE>(nh)) !=
                                  static_cast<DWORD>(-1)) {
                                if (GetThreadContext(
                                        reinterpret_cast<HANDLE>(nh), &c2)) {
                                  auto* f2 =
                                      proc->backend()->code_cache()
                                          ->LookupFunction(c2.Rip);
                                  g2 = f2 ? f2->MapMachineCodeToGuestAddress(
                                                c2.Rip)
                                          : 0;
                                }
                                ResumeThread(reinterpret_cast<HANDLE>(nh));
                              }
                              bool in_spin =
                                  g2 >= 0x819F3FC8u && g2 <= 0x819F4004u;
                              if (k % 6 == 0 || !in_spin) {
                                XELOGI("FakeWriteback[{}]: wrote {:08X}, guest "
                                       "PC {:08X} {}",
                                       k, v, g2,
                                       in_spin ? "(still spinning)"
                                               : "<-- LEFT THE SPIN");
                              }
                              if (!in_spin) break;
                            }
                          }
                        }
                      }
                      auto* cc = proc->backend()->code_cache();
                      for (int i = 0; i < 8; ++i) {
                        if (!rips[i]) {
                          XELOGI("StallProbe[{}]: no sample", i);
                          continue;
                        }
                        auto* f = cc->LookupFunction(rips[i]);
                        uint32_t g =
                            f ? f->MapMachineCodeToGuestAddress(rips[i]) : 0;
                        XELOGI("StallProbe[{}]: guest {:08X}  r3={:08X} "
                               "r11={:08X} r29={:08X} r31={:08X}{}",
                               i, g, gr[i][0], gr[i][1], gr[i][2], gr[i][3],
                               f ? "" : "  (not guest code)");
                      }
                    }).detach();
                  }
                }
                kernel::xboxkrnl::in_xam_createdevice_scope = true;
                uint64_t cr = ks->processor()->Execute(ts, create_fn, ca,
                                                       xe::countof(ca));
                kernel::xboxkrnl::in_xam_createdevice_scope = false;
                if (cvars::guide_system_process_type && cur) {
                  auto* kt = cur->guest_object<kernel::X_KTHREAD>();
                  kt->process_type = saved_pt;
                  kt->process_type_dup = saved_ptd;
                }
                XELOGI("Guide button: xam CreateDevice returned {:08X}, "
                       "device now {:08X}",
                       static_cast<uint32_t>(cr), rd(0x81D43684u));
                // Publish it as VdGlobalXamDevice (kernel global 801E6FC8).
                // Xenia stores 0 there with the comment "Pointer to the XAM
                // D3D device, which we don't have", and KernelState has a
                // matching TODO to run graphics notifications as
                // X_PROCTYPE_SYSTEM when it is non-zero. We have one now.
                uint32_t xam_dev = rd(0x81D43684u);
                if (xam_dev) {
                  xe::store_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(0x801E6FC8u), xam_dev);
                  XELOGI("Guide button: VdGlobalXamDevice = {:08X}",
                         rd(0x801E6FC8u));
                }
              }
              if (cvars::guide_bootstrap_on_title_thread &&
                  !cvars::guide_bootstrap_before_device) {
                kernel::xboxkrnl::QueueGuideBootstrap(
                    hud_base, obj, cvars::guide_use_title_device,
                    skin_mod);
                // xam's UI startup and everything under it check that the
                // caller is xam's recorded UI thread (81D42520). Spoofing that
                // check satisfied the comparison but not the thread's own
                // state, so queue the startup as an APC on the real thread.
                if (cvars::guide_xam_ui_startup) {
                  uint32_t ui_thread = xe::load_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(0x81D42520u));
                  auto threads =
                      ks->object_table()->GetObjectsByType<kernel::XThread>(
                          kernel::XObject::Type::Thread);
                  bool queued = false;
                  for (auto& th : threads) {
                    if (th->guest_object() == ui_thread) {
                      XELOGI("Guide button: queueing xam UI startup {:08X} as "
                             "an APC on xam's UI thread {:08X}",
                             uint32_t(cvars::guide_xam_ui_startup), ui_thread);
                      th->EnqueueApc(cvars::guide_xam_ui_startup, 0, 0, 0);
                      queued = true;
                      break;
                    }
                  }
                  if (!queued) {
                    XELOGW("Guide button: xam UI thread {:08X} not found among "
                           "{} threads",
                           ui_thread, threads.size());
                  }
                }
                XELOGI("Guide button: queued XUI bootstrap for the title "
                       "thread (hud {:08X}, obj {:08X})",
                       hud_base, obj);
                if (cvars::guide_scene_off_thread) {
                  // Wait for the title thread to finish the device-touching
                  // part, then build the scene here. XUI scene loading is
                  // asynchronous, so it must not run while we hold the
                  // renderer.
                  for (int i = 0; i < 400; ++i) {
                    if (kernel::xboxkrnl::GuideBootstrapReady()) {
                      break;
                    }
                    xe::threading::Sleep(std::chrono::milliseconds(10));
                  }
                  if (!kernel::xboxkrnl::GuideBootstrapReady()) {
                    XELOGW("Guide button: bootstrap never became ready");
                    return 0;
                  }
                  uint32_t ovt = xe::load_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(obj));
                  uint32_t sfn =
                      ovt ? xe::load_and_swap<uint32_t>(
                                ks->memory()->TranslateVirtual(ovt + 27 * 4))
                          : 0;
                  // Step the scene creator's own sequence instead of calling
                  // it whole, so the blocking sub-call is identifiable:
                  // init -> XamEnableSystemAppInput -> ... -> XuiSceneCreate.
                  auto rdm = [&](uint32_t addr) {
                    return xe::load_and_swap<uint32_t>(
                        ks->memory()->TranslateVirtual(addr));
                  };
                  // Synchronous CPU readings bracket the known-good steps,
                  // so the probe has a control: if the thread accrues time
                  // across steps 1-3 the instrument detects work, and a flat
                  // reading during step 4 then means something.
                  auto cpu_ms = [](void* nh) -> std::pair<uint64_t, uint64_t> {
                    FILETIME c0, e0, k0, u0;
                    if (!nh || !GetThreadTimes(reinterpret_cast<HANDLE>(nh),
                                               &c0, &e0, &k0, &u0)) {
                      return {0, 0};
                    }
                    uint64_t k =
                        (uint64_t(k0.dwHighDateTime) << 32) | k0.dwLowDateTime;
                    uint64_t u =
                        (uint64_t(u0.dwHighDateTime) << 32) | u0.dwLowDateTime;
                    return {k / 10000, u / 10000};
                  };
                  void* nh_self =
                      kernel::XThread::GetCurrentThread()->thread()
                          ? kernel::XThread::GetCurrentThread()
                                ->thread()
                                ->native_handle()
                          : nullptr;
                  if (cvars::guide_step_scene) {
                    auto t0 = cpu_ms(nh_self);
                    XELOGI("CpuMark before steps: kernel={}ms user={}ms",
                           t0.first, t0.second);
                    // CONTROL: start the watchdog before the known-good steps.
                    // Xenia keeps guest registers in host registers while
                    // running, so a "frozen" context may only mean the context
                    // is not written back. If it also looks frozen during
                    // steps 1-3, which demonstrably complete, then sampling it
                    // proves nothing about whether the thread is executing.
                    {
                      // Per-thread CPU time. Unlike sampling the PPC context
                      // (stale during JIT execution) this distinguishes a
                      // spinning thread from a blocked one, and the samples
                      // during steps 1-3 act as the control: they must show
                      // time accruing while work is demonstrably happening.
                      auto* wt0 = kernel::XThread::GetCurrentThread();
                      void* nh = wt0->thread() ? wt0->thread()->native_handle()
                                               : nullptr;
                      std::thread([nh]() {
                        for (int i = 0; i < 30; ++i) {
                          xe::threading::Sleep(std::chrono::milliseconds(500));
                          if (!nh) {
                            continue;
                          }
                          // Is the thread blocked, or dead? A terminated
                          // thread also shows flat CPU, no kernel calls, no
                          // waits and no faults - and would leave xam's XUI
                          // critical section held forever, which is what the
                          // title thread then blocks on.
                          // Host instruction pointer of the stuck thread.
                          // Suspending briefly to read RIP is safe here (the
                          // thread is making no progress) and gives a direct
                          // answer instead of probing candidate locks one at a
                          // time. Symbolize offline as RIP - module_base
                          // against the PDB.
                          {
                            HANDLE hh = reinterpret_cast<HANDLE>(nh);
                            if (SuspendThread(hh) != (DWORD)-1) {
                              CONTEXT ctx;
                              ctx.ContextFlags = CONTEXT_CONTROL;
                              if (GetThreadContext(hh, &ctx)) {
                                auto base = reinterpret_cast<uint64_t>(
                                    GetModuleHandleW(nullptr));
                                // Which module is RIP in?
                                wchar_t modname[MAX_PATH] = {};
                                HMODULE hm = nullptr;
                                if (GetModuleHandleExW(
                                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        reinterpret_cast<LPCWSTR>(ctx.Rip),
                                        &hm)) {
                                  GetModuleFileNameW(hm, modname, MAX_PATH);
                                }
                                XELOGI("HostRip {}: rip={:X} in '{}' +{:X} "
                                       "rsp={:X}",
                                       i, ctx.Rip,
                                       xe::to_utf8(std::u16string(
                                           reinterpret_cast<const char16_t*>(
                                               modname))),
                                       hm ? ctx.Rip -
                                                reinterpret_cast<uint64_t>(hm)
                                          : 0,
                                       ctx.Rsp);
                                // Poor man's stack walk: scan the stack for
                                // return addresses inside the exe, which names
                                // the Xenia code that called into the DLL.
                                if (i == 0) {
                                  // Symbolize with dbghelp so the frames are
                                  // names rather than offsets.
                                  static bool sym_ready = false;
                                  if (!sym_ready) {
                                    SymSetOptions(SYMOPT_UNDNAME |
                                                  SYMOPT_DEFERRED_LOADS);
                                    sym_ready = SymInitialize(
                                                    GetCurrentProcess(),
                                                    nullptr, TRUE) != FALSE;
                                  }
                                  auto sym_name = [](uint64_t addr) {
                                    char buf[sizeof(SYMBOL_INFO) + 512] = {};
                                    auto* si =
                                        reinterpret_cast<SYMBOL_INFO*>(buf);
                                    si->SizeOfStruct = sizeof(SYMBOL_INFO);
                                    si->MaxNameLen = 500;
                                    DWORD64 disp = 0;
                                    if (SymFromAddr(GetCurrentProcess(), addr,
                                                    &disp, si)) {
                                      return std::string(si->Name) + "+" +
                                             std::to_string(disp);
                                    }
                                    return std::string("<no symbol>");
                                  };
                                  XELOGI("  rip sym: {}", sym_name(ctx.Rip));
                                  auto* sp = reinterpret_cast<uint64_t*>(
                                      ctx.Rsp);
                                  int found = 0;
                                  for (int w = 0; w < 96 && found < 6; ++w) {
                                    uint64_t v = sp[w];
                                    if (v > base && v < base + 0x4000000) {
                                      XELOGI("  stack[{}] exe+{:X}  {}", w,
                                             v - base, sym_name(v));
                                      ++found;
                                    }
                                  }
                                }
                              }
                              ResumeThread(hh);
                            }
                          }
                          // Is Xenia's global critical region held while the
                          // Guide thread is stuck? If TryAcquire succeeds the
                          // thread is not blocked on it, which rules out the
                          // most likely host lock.
                          {
                            auto probe =
                                xe::global_critical_region::TryAcquire();
                            XELOGI("GlobalLock {}: {}", i,
                                   probe.owns_lock() ? "FREE" : "HELD");
                          }
                          DWORD exit_code = 0;
                          if (GetExitCodeThread(reinterpret_cast<HANDLE>(nh),
                                                &exit_code)) {
                            XELOGI("ThreadState {}: {}", i,
                                   exit_code == STILL_ACTIVE
                                       ? "STILL_ACTIVE"
                                       : fmt::format("EXITED code={}",
                                                     exit_code));
                          }
                          FILETIME c0, e0, k0, u0;
                          if (GetThreadTimes(reinterpret_cast<HANDLE>(nh), &c0,
                                             &e0, &k0, &u0)) {
                            uint64_t k = (uint64_t(k0.dwHighDateTime) << 32) |
                                         k0.dwLowDateTime;
                            uint64_t u = (uint64_t(u0.dwHighDateTime) << 32) |
                                         u0.dwLowDateTime;
                            XELOGI("CpuProbe {}: kernel={}ms user={}ms", i,
                                   k / 10000, u / 10000);
                          }
                        }
                      }).detach();
                    }
                    XELOGI("Guide button: step 1 init(render_obj,0)");
                    uint64_t ia2[] = {obj + 16, 0};
                    uint64_t ir2 = ks->processor()->Execute(
                        ts, hud_base + 0xA898u, ia2, xe::countof(ia2));
                    XELOGI("Guide button: step 1 init -> {:08X}",
                           static_cast<uint32_t>(ir2));
                    uint32_t inp = rdm(obj + 72);
                    XELOGI("Guide button: step 2 XamEnableSystemAppInput({:08X}"
                           ", 1)",
                           inp);
                    uint64_t ea[] = {inp, 1};
                    uint64_t er = ks->processor()->Execute(
                        ts, 0x913FE724u, ea, xe::countof(ea));
                    XELOGI("Guide button: step 2 -> {:08X}",
                           static_cast<uint32_t>(er));
                    // Step 3: build the resource locator exactly as hud
                    // does - XamBuildResourceLocator([guide+4], "hud",
                    // [91400168] = "strings.xus", buf, 128).
                    uint32_t pbuf = ks->memory()->SystemHeapAlloc(256, 16);
                    uint32_t outh = ks->memory()->SystemHeapAlloc(16, 16);
                    uint64_t ba[] = {rdm(obj + 4), 0x913E1B24u,
                                     rdm(0x91400168u), pbuf, 128};
                    uint64_t br = ks->processor()->Execute(
                        ts, 0x913FE8C4u, ba, xe::countof(ba));
                    std::string loc;
                    for (int i = 0; i < 80; ++i) {
                      uint16_t ch = xe::load_and_swap<uint16_t>(
                          ks->memory()->TranslateVirtual(pbuf + i * 2));
                      if (!ch) break;
                      loc.push_back(static_cast<char>(ch & 0x7F));
                    }
                    XELOGI("Guide button: step 3 locator -> {:08X} '{}'",
                           static_cast<uint32_t>(br), loc);
                    // Step 4: XuiSceneCreate(basePath, sceneFile, 0, &out)
                    auto t3 = cpu_ms(nh_self);
                    XELOGI("CpuMark after steps 1-3: kernel={}ms user={}ms",
                           t3.first, t3.second);
                    XELOGI("Guide button: step 4 XuiSceneCreate calling");
                    // Watchdog: sample this thread's guest context from a host
                    // thread. If lr/r1 move, guest code is still executing (a
                    // loop); if they are frozen, the thread is not running at
                    // all. That is the distinction the CPU and kernel-call
                    // measurements could not make.
                    {
                      auto* wt = kernel::XThread::GetCurrentThread();
                      std::thread([wt]() {
                        for (int i = 0; i < 12; ++i) {
                          xe::threading::Sleep(std::chrono::seconds(2));
                          auto* c = wt->thread_state()->context();
                          XELOGI("Watchdog {}: lr={:08X} r1={:08X} r3={:08X} "
                                 "r4={:08X}",
                                 i, static_cast<uint32_t>(c->lr),
                                 static_cast<uint32_t>(c->r[1]),
                                 static_cast<uint32_t>(c->r[3]),
                                 static_cast<uint32_t>(c->r[4]));
                        }
                      }).detach();
                    }
                    uint64_t sca[] = {pbuf, rdm(0x91400170u), 0, outh};
                    uint64_t scr = ks->processor()->Execute(
                        ts, 0x913FE6D4u, sca, xe::countof(sca));
                    XELOGI("Guide button: step 4 XuiSceneCreate -> {:08X} "
                           "scene={:08X}",
                           static_cast<uint32_t>(scr), rdm(outh));
                    XELOGI("Guide button: steps done");
                    return 0;
                  }
                  XELOGI("Guide button: scene creator {:08X} off-thread", sfn);
                  if (sfn) {
                    uint64_t sa[] = {obj, 0, 0};
                    uint64_t sr =
                        ks->processor()->Execute(ts, sfn, sa, xe::countof(sa));
                    XELOGI("Guide button: off-thread scene creator -> {:08X}",
                           static_cast<uint32_t>(sr));
                  }
                }
                return 0;
              }
              if (cvars::guide_use_title_device) {
                uint32_t title_dev = rd(0x801E6FC4u);
                if (title_dev) {
                  xe::store_and_swap<uint32_t>(
                      ks->memory()->TranslateVirtual(0x81D43684u), title_dev);
                  XELOGI("Guide button: xam device global -> title device "
                         "{:08X}",
                         title_dev);
                }
              }
              if (cvars::guide_call_render_host) {
                XELOGI("Guide button: D3D device global 81D43684 = {:08X}",
                       rd(0x81D43684u));
                uint64_t ha[] = {0};
                uint64_t hr = ks->processor()->Execute(ts, 0x8178DC58u, ha,
                                                       xe::countof(ha));
                XELOGI("Guide button: render host returned {:08X}, "
                       "XUI ctx now {:08X}",
                       static_cast<uint32_t>(hr), rd(0x81D6C978u));
                uint32_t xctx = rd(0x81D6C978u);
                if (xctx) {
                  uint32_t cvt = rd(xctx);
                  XELOGI("Guide button: XUI ctx {:08X} vtable {:08X}", xctx,
                         cvt);
                  for (int i = 0; i < 8; ++i) {
                    XELOGI("Guide button: ctxvt[{}] = {:08X}", i,
                           rd(cvt + i * 4));
                  }
                }
              }
              {
                // Call xam's XuiRenderCreateDC (R 818FB038) directly with our
                // own out-pointer, so its return value is attributable to it
                // rather than to hud's init wrapper.
                uint32_t dcp = ks->memory()->SystemHeapAlloc(16, 16);
                uint64_t da2[] = {dcp};
                uint64_t dr2 = ks->processor()->Execute(ts, 0x818FB038u, da2,
                                                        xe::countof(da2));
                XELOGI("Guide button: direct XuiRenderCreateDC -> {:08X}, "
                       "dc={:08X}",
                       static_cast<uint32_t>(dr2), rd(dcp));
              }
              if (cvars::guide_force_render_gate) {
                xe::store_and_swap<uint32_t>(
                    ks->memory()->TranslateVirtual(obj + 16 + 20), 1u);
              }
              uint64_t ia[] = {obj + 16, 0};
              uint64_t ir = ks->processor()->Execute(ts, hud_base + 0xA898u,
                                                     ia, xe::countof(ia));
              if (cvars::guide_create_scene) {
                uint32_t cvt2 = rd(obj);
                uint32_t scene_fn = cvt2 ? rd(cvt2 + 27 * 4) : 0;
                XELOGI("Guide button: scene fn (vtable[27]) = {:08X}",
                       scene_fn);
                if (scene_fn) {
                  uint64_t sa[] = {obj};
                  uint64_t sr = ks->processor()->Execute(ts, scene_fn, sa,
                                                         xe::countof(sa));
                  XELOGI("Guide button: scene create -> {:08X}, +8 now {:08X}",
                         static_cast<uint32_t>(sr), rd(obj + 16 + 8));
                }
              }
              XELOGI("Guide button: post-init +8={:08X} +12={:08X} "
                     "+20={:08X}",
                     rd(obj + 16 + 8), rd(obj + 16 + 12),
                     rd(obj + 16 + 20));
              XELOGI("Guide button: XUI init returned {:08X}",
                     static_cast<uint32_t>(ir));
              // Hand the draw to the graphics-notification path so it runs
              // on the title's render thread. The title's D3D device is
              // thread-affine, so drawing it from this thread is refused by
              // the guest D3D runtime.
              kernel::xboxkrnl::SetGuideDrawHook(hud_base + 0xAB28u, obj + 16);
              XELOGI("Guide button: draw hook installed ({:08X}, {:08X})",
                     hud_base + 0xAB28u, obj + 16);
              for (int frame = 0; frame < 0; ++frame) {
                uint64_t da[] = {obj + 16};
                uint64_t dr = ks->processor()->Execute(
                    ts, hud_base + 0xAB28u, da, xe::countof(da));
                if (frame < 3) {
                  XELOGI("Guide button: draw frame {} returned {:08X}", frame,
                         static_cast<uint32_t>(dr));
                }
                xe::threading::Sleep(std::chrono::milliseconds(16));
              }
              XELOGI("Guide button: draw loop ended");
            } else {
              XELOGW("Guide button: no Guide object at 91400690");
            }
          }
          return 0;
        },
        ks->GetSystemProcess()));
    t->set_name("Guide button dispatch");
    if (XFAILED(t->Create())) {
      XELOGE("Guide button: failed to create dispatch thread");
    }
  }

  // Report what the Guide press can and cannot do in this build, so the
  // button is no longer silently swallowed. Opening the real Guide needs
  // hud.xex hosted by xam - see research/FINDINGS.md.
  XELOGI("Guide button: user={} lle_xam={} hud={}", user_index,
         cvars::lle_xam.empty() ? "off" : "on",
         cvars::guide_hud_path.empty() ? "not loaded" : "loaded");
  if (cvars::guide_hud_path.empty()) {
    XELOGI(
        "Guide button: no hud.xex configured - set guide_hud_path to load it");
  }
}

void Emulator::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // Don't hold the lock on this (so any waits follow through)
  graphics_system_->Pause();
  audio_system_->Pause();

  auto lock = global_critical_region::AcquireDirect();
  auto threads =
      kernel_state()->object_table()->GetObjectsByType<kernel::XThread>(
          kernel::XObject::Type::Thread);
  auto current_thread = kernel::XThread::IsInThread()
                            ? kernel::XThread::GetCurrentThread()
                            : nullptr;
  for (auto thread : threads) {
    // Don't pause ourself or host threads.
    if (thread == current_thread || !thread->can_debugger_suspend()) {
      continue;
    }

    if (thread->is_running()) {
      thread->thread()->Suspend(nullptr);
    }
  }

  XELOGD("! EMULATOR PAUSED !");
}

void Emulator::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;
  XELOGD("! EMULATOR RESUMED !");

  graphics_system_->Resume();
  audio_system_->Resume();

  auto threads =
      kernel_state()->object_table()->GetObjectsByType<kernel::XThread>(
          kernel::XObject::Type::Thread);
  for (auto thread : threads) {
    if (!thread->can_debugger_suspend()) {
      // Don't pause host threads.
      continue;
    }

    if (!thread->is_running()) {
      thread->thread()->Resume(nullptr);
    }
  }
}

bool Emulator::SaveToFile(const std::filesystem::path& path) {
  Pause();

  filesystem::CreateEmptyFile(path);
  auto map = MappedMemory::Open(path, MappedMemory::Mode::kReadWrite, 0, 2_GiB);
  if (!map) {
    return false;
  }

  // Save the emulator state to a file
  ByteStream stream(map->data(), map->size());
  stream.Write(kEmulatorSaveSignature);
  stream.Write(title_id_.has_value());
  if (title_id_.has_value()) {
    stream.Write(title_id_.value());
  }

  // It's important we don't hold the global lock here! XThreads need to step
  // forward (possibly through guarded regions) without worry!
  processor_->Save(&stream);
  graphics_system_->Save(&stream);
  audio_system_->Save(&stream);
  kernel_state_->Save(&stream);
  memory_->Save(&stream);
  map->Close(stream.offset());

  Resume();
  return true;
}

bool Emulator::RestoreFromFile(const std::filesystem::path& path) {
  // Restore the emulator state from a file
  auto map = MappedMemory::Open(path, MappedMemory::Mode::kReadWrite);
  if (!map) {
    return false;
  }

  restoring_ = true;

  // Terminate any loaded titles.
  Pause();
  kernel_state_->TerminateTitle();

  auto lock = global_critical_region::AcquireDirect();
  ByteStream stream(map->data(), map->size());
  if (stream.Read<uint32_t>() != kEmulatorSaveSignature) {
    return false;
  }

  auto has_title_id = stream.Read<bool>();
  std::optional<uint32_t> title_id;
  if (!has_title_id) {
    title_id = {};
  } else {
    title_id = stream.Read<uint32_t>();
  }
  if (title_id_.has_value() != title_id.has_value() ||
      title_id_.value() != title_id.value()) {
    // Swapping between titles is unsupported at the moment.
    assert_always();
    return false;
  }

  if (!processor_->Restore(&stream)) {
    XELOGE("Could not restore processor!");
    return false;
  }
  if (!graphics_system_->Restore(&stream)) {
    XELOGE("Could not restore graphics system!");
    return false;
  }
  if (!audio_system_->Restore(&stream)) {
    XELOGE("Could not restore audio system!");
    return false;
  }
  if (!kernel_state_->Restore(&stream)) {
    XELOGE("Could not restore kernel state!");
    return false;
  }
  if (!memory_->Restore(&stream)) {
    XELOGE("Could not restore memory!");
    return false;
  }

  // Update the main thread.
  auto threads =
      kernel_state_->object_table()->GetObjectsByType<kernel::XThread>();
  for (auto thread : threads) {
    if (thread->main_thread()) {
      main_thread_ = thread;
      break;
    }
  }

  Resume();

  restore_fence_.Signal();
  restoring_ = false;

  return true;
}

const std::filesystem::path Emulator::GetNewDiscPath(
    std::string window_message) {
  std::filesystem::path path = "";

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(false);
  file_picker->set_title(!window_message.empty() ? window_message
                                                 : "Select Content Package");
  file_picker->set_extensions({
      {"Supported Files", "*.iso;*.xex;*.xcp;*.*"},
      {"Disc Image (*.iso)", "*.iso"},
      {"Xbox Executable (*.xex)", "*.xex"},
      {"All Files (*.*)", "*.*"},
  });

  if (file_picker->Show()) {
    auto selected_files = file_picker->selected_files();
    if (!selected_files.empty()) {
      path = selected_files[0];
    }
  }
  return path;
}

bool Emulator::ExceptionCallbackThunk(Exception* ex, void* data) {
  return reinterpret_cast<Emulator*>(data)->ExceptionCallback(ex);
}

bool Emulator::ExceptionCallback(Exception* ex) {
  // Check to see if the exception occurred in guest code.
  auto code_cache = processor()->backend()->code_cache();
  auto code_base = code_cache->execute_base_address();
  auto code_end = code_base + code_cache->total_size();

  if (!processor()->is_debugger_attached() && debugging::IsDebuggerAttached()) {
    // If Xenia's debugger isn't attached but another one is, pass it to that
    // debugger.
    return false;
  } else if (processor()->is_debugger_attached()) {
    // Let the debugger handle this exception. It may decide to continue past
    // it (if it was a stepping breakpoint, etc).
    return processor()->OnUnhandledException(ex);
  }

  if (!(ex->pc() >= code_base && ex->pc() < code_end)) {
    // Didn't occur in guest code. Let it pass - but say so first. A host-side
    // fault otherwise produces no log line at all: the only trace is Xenia's
    // modal "Unhandled Exception" dialog, which reports a bare module+offset
    // and no caller. Raw addresses are logged rather than resolved here
    // because resolving would take the loader lock, which is not safe from an
    // exception handler. Feed exe-relative frames to tools/sym.ps1 offline.
    static std::atomic<uint32_t> host_faults{0};
    if (++host_faults <= 4) {
      void* frames[32] = {};
      USHORT n = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
      uint64_t exe_base =
          reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
      std::string bt;
      for (USHORT i = 0; i < n; ++i) {
        uint64_t a = reinterpret_cast<uint64_t>(frames[i]);
        if (a >= exe_base && a < exe_base + 0x2000000ull) {
          bt += fmt::format("exe+{:X} ", a - exe_base);
        } else {
          bt += fmt::format("{:X} ", a);
        }
      }
      XELOGE("HOST FAULT: pc={:X} (exe+{:X}) fault_addr={:X} exe_base={:X}",
             ex->pc(),
             ex->pc() >= exe_base ? ex->pc() - exe_base : 0ull,
             ex->code() == Exception::Code::kAccessViolation
                 ? ex->fault_address()
                 : 0,
             exe_base);
      XELOGE("HOST FAULT: frames: {}", bt);
    }
    return false;
  }

  // Log the essentials BEFORE pausing. Pause() waits for the graphics system
  // and command processor to acknowledge, and if either is blocked - which is
  // likely when a guest thread has just faulted mid-frame - it never returns
  // and the crash dump below is never written. That turns a diagnosable crash
  // into a silent freeze.
  {
    auto* early_thread = kernel::XThread::GetCurrentThread();
    auto* early_fn = code_cache->LookupFunction(ex->pc());
    uint32_t guest_pc =
        early_fn ? early_fn->MapMachineCodeToGuestAddress(ex->pc()) : 0;
    const char* code_str =
        ex->code() == Exception::Code::kAccessViolation ? "access violation"
        : ex->code() == Exception::Code::kIllegalInstruction
            ? "illegal instruction"
            : "other";
    auto* ectx =
        early_thread ? early_thread->thread_state()->context() : nullptr;
    XELOGE(
        "GUEST CRASH: {} at guest PC {:08X} (host {:X}), thread '{}', "
        "fault_addr {:016X}",
        code_str, guest_pc, ex->pc(),
        early_thread ? early_thread->name() : std::string("<none>"),
        ex->code() == Exception::Code::kAccessViolation ? ex->fault_address()
                                                        : 0);
    if (ectx) {
      // LR identifies the caller, which matters when several call sites reach
      // the same function - picking one by "it was compiled just before" is
      // not evidence.
      XELOGE("GUEST CRASH: lr={:08X} r3={:016X} r4={:016X} r5={:016X}",
             static_cast<uint32_t>(ectx->lr), ectx->r[3], ectx->r[4],
             ectx->r[5]);
      // Xenon MSVC keeps "this" and the other long-lived pointers in the
      // callee-saved range, and by the time a load faults r3-r5 are usually
      // already clobbered. Without these it is not possible to tell which
      // object a faulting "lwz rX,off(rY)" was reading from - which is
      // exactly the question a null deref raises.
      // Poor-man's backtrace: scan the guest stack for words that look like
      // xam .text addresses. Breakpoints would give an exact caller, but
      // installing one changes scheduling enough that the code path under
      // investigation stops being taken - so the crash path can only be
      // observed without them.
      {
        auto* mm = kernel_state() ? kernel_state()->memory() : nullptr;
        uint32_t sp = static_cast<uint32_t>(ectx->r[1]);
        if (mm && sp) {
          // Every read below walks addresses derived from a *faulted*
          // thread's stack, so none of them can be assumed mapped. Reading
          // off the end of the stack region faulted inside this handler,
          // which replaced the real crash report with a second exception and
          // destroyed the evidence for the first.
          auto readable = [&](uint32_t a, uint32_t len) {
            auto* hp = mm->LookupHeap(a);
            return hp && hp->QueryRangeAccess(a, a + len - 1) !=
                             xe::memory::PageAccess::kNoAccess;
          };
          // Proper unwind first. PPC keeps a back chain at [sp], and these
          // prologues save LR with "stw r12,-8(r1)" before the stwu, so a
          // frame's return address sits at [caller_sp - 8]. Walking that is
          // exact, unlike the scan below, which cannot tell a live frame from
          // a stale word left by an earlier deeper call.
          {
            std::string bt;
            uint32_t cur = sp;
            for (int f = 0; f < 12 && cur; ++f) {
              if (!readable(cur, 4)) break;
              uint32_t caller_sp =
                  xe::load_and_swap<uint32_t>(mm->TranslateVirtual(cur));
              if (caller_sp <= cur || caller_sp - cur > 0x10000) break;
              if (caller_sp < 8 || !readable(caller_sp - 8, 4)) break;
              uint32_t ra = xe::load_and_swap<uint32_t>(
                  mm->TranslateVirtual(caller_sp - 8));
              if (ra < 0x81000000u || ra >= 0x92000000u) break;
              bt += fmt::format("{:08X} ", ra);
              cur = caller_sp;
            }
            if (!bt.empty()) {
              XELOGE("GUEST CRASH: unwind (back chain): {}", bt);
            }
          }
          std::string line;
          int shown = 0;
          // Cover several frames. 96 words was too short by eight for
          // 819F5D18 alone, whose prologue is stwu r1,-0x1A0(r1) - 104 words -
          // so the direct caller's return address fell outside the window and
          // its absence was misread as proof the call was indirect.
          for (uint32_t i = 0; i < 400 && shown < 24; ++i) {
            uint32_t a = sp + i * 4;
            if (!readable(a, 4)) break;
            uint32_t v = xe::load_and_swap<uint32_t>(mm->TranslateVirtual(a));
            if (v >= 0x81700000u && v < 0x81E00000u) {
              line += fmt::format("{:08X}(+{:X}) ", v, i * 4);
              ++shown;
            }
          }
          if (!line.empty()) {
            XELOGE("GUEST CRASH: stack code refs: {}", line);
          }
        }
      }
      // All 32 GPRs. Picking a subset means the one register the faulting
      // instruction actually used is the one that is missing - which is
      // exactly what happened with an "lwz r11,32(r14)" fault when only
      // r27-r31 were dumped.
      for (int base = 0; base < 32; base += 8) {
        XELOGE("GUEST CRASH: r{:<2}-r{:<2} {:08X} {:08X} {:08X} {:08X} "
               "{:08X} {:08X} {:08X} {:08X}",
               base, base + 7, static_cast<uint32_t>(ectx->r[base + 0]),
               static_cast<uint32_t>(ectx->r[base + 1]),
               static_cast<uint32_t>(ectx->r[base + 2]),
               static_cast<uint32_t>(ectx->r[base + 3]),
               static_cast<uint32_t>(ectx->r[base + 4]),
               static_cast<uint32_t>(ectx->r[base + 5]),
               static_cast<uint32_t>(ectx->r[base + 6]),
               static_cast<uint32_t>(ectx->r[base + 7]));
      }
      // LR here is the function's own __savegprlr return, not the caller.
      // That helper stores the real LR at [r1-8] of the caller's frame before
      // the stwu, so with a 0xC0 frame it is at r1+0xB8. Scan a window in case
      // the frame size differs.
      XELOGE("GUEST CRASH: [81D3F924] = {:08X}",
             xe::load_and_swap<uint32_t>(
                 memory()->TranslateVirtual(0x81D3F924u)));
      uint32_t sp = static_cast<uint32_t>(ectx->r[1]);
      for (uint32_t off = 0xA0; off <= 0xE0; off += 8) {
        uint32_t v = xe::load_and_swap<uint32_t>(
            memory()->TranslateVirtual(sp + off));
        if (v >= 0x81000000 && v < 0x82000000) {
          XELOGE("GUEST CRASH: saved lr candidate [r1+{:X}] = {:08X}", off, v);
        }
      }
    }
  }

  // Within range. Pause the emulator and eat the exception.
  Pause();

  // Dump information into the log.
  auto current_thread = kernel::XThread::GetCurrentThread();
  assert_not_null(current_thread);

  auto guest_function = code_cache->LookupFunction(ex->pc());
  assert_not_null(guest_function);

  auto context = current_thread->thread_state()->context();

  std::string crash_msg;
  crash_msg.append("==== CRASH DUMP ====\n");
  crash_msg.append(fmt::format("Thread ID (Host: 0x{:08X} / Guest: 0x{:08X})\n",
                               current_thread->thread()->system_id(),
                               current_thread->thread_id()));
  crash_msg.append(
      fmt::format("Thread Handle: 0x{:08X}\n", current_thread->handle()));
  crash_msg.append(
      fmt::format("PC: 0x{:08X}\n",
                  guest_function->MapMachineCodeToGuestAddress(ex->pc())));
  if (ex->code() == Exception::Code::kAccessViolation) {
    const char* op_str = "unknown";
    if (ex->access_violation_operation() ==
        Exception::AccessViolationOperation::kRead) {
      op_str = "read";
    } else if (ex->access_violation_operation() ==
               Exception::AccessViolationOperation::kWrite) {
      op_str = "write";
    }
    crash_msg.append(fmt::format("Access Violation: {} at 0x{:016X}\n", op_str,
                                 ex->fault_address()));
  } else if (ex->code() == Exception::Code::kIllegalInstruction) {
    crash_msg.append("Illegal Instruction\n");
  }
  crash_msg.append("Registers:\n");
  for (int i = 0; i < 32; i++) {
    crash_msg.append(fmt::format(" r{:<3} = {:016X}\n", i, context->r[i]));
  }
  for (int i = 0; i < 32; i++) {
    crash_msg.append(fmt::format(" f{:<3} = {:016X} = (double){} = (float){}\n",
                                 i,
                                 *reinterpret_cast<uint64_t*>(&context->f[i]),
                                 context->f[i], *(float*)&context->f[i]));
  }
  for (int i = 0; i < 128; i++) {
    crash_msg.append(
        fmt::format(" v{:<3} = [0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X}]\n", i,
                    context->v[i].u32[0], context->v[i].u32[1],
                    context->v[i].u32[2], context->v[i].u32[3]));
  }
  XELOGE("{}", crash_msg);
  std::string crash_dlg = fmt::format(
      "The guest has crashed.\n\n"
      "Xenia has now paused itself.\n\n"
      "{}",
      crash_msg);
  // Display a dialog telling the user the guest has crashed.
  if (display_window_ && imgui_drawer_) {
    display_window_->app_context().CallInUIThreadSynchronous([this,
                                                              &crash_dlg]() {
      xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_, "Uh-oh!", crash_dlg);
    });
  }

  // Now suspend ourself (we should be a guest thread).
  current_thread->Suspend(nullptr);

  // We should not arrive here!
  assert_always();
  return false;
}

void Emulator::WaitUntilExit() {
  while (true) {
    if (main_thread_) {
      xe::threading::Wait(main_thread_->thread(), false);
    }

    if (restoring_) {
      restore_fence_.Wait();
    } else {
      // Not restoring and the thread exited. We're finished.
      break;
    }
  }

  on_exit();
}

void Emulator::AddGameConfigLoadCallback(GameConfigLoadCallback* callback) {
  assert_not_null(callback);
  // Game config load callbacks handling is entirely in the UI thread.
  assert_true(!display_window_ ||
              display_window_->app_context().IsInUIThread());
  // Check if already added.
  if (std::ranges::find(std::as_const(game_config_load_callbacks_), callback) !=
      game_config_load_callbacks_.cend()) {
    return;
  }
  game_config_load_callbacks_.push_back(callback);
}

void Emulator::RemoveGameConfigLoadCallback(GameConfigLoadCallback* callback) {
  assert_not_null(callback);
  // Game config load callbacks handling is entirely in the UI thread.
  assert_true(!display_window_ ||
              display_window_->app_context().IsInUIThread());
  auto it =
      std::ranges::find(std::as_const(game_config_load_callbacks_), callback);
  if (it == game_config_load_callbacks_.cend()) {
    return;
  }
  if (game_config_load_callback_loop_next_index_ != SIZE_MAX) {
    // Actualize the next callback index after the erasure from the vector.
    size_t existing_index =
        size_t(std::distance(game_config_load_callbacks_.cbegin(), it));
    if (game_config_load_callback_loop_next_index_ > existing_index) {
      --game_config_load_callback_loop_next_index_;
    }
  }
  game_config_load_callbacks_.erase(it);
}

std::string Emulator::FindLaunchModule() {
  std::string path(fmt::format("{}\\", kDefaultGameSymbolicLink));

  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");

  if (!xam->loader_data().launch_path.empty()) {
    std::string symbolic_link_path;
    if (kernel_state_->file_system()->FindSymbolicLink(kDefaultGameSymbolicLink,
                                                       symbolic_link_path)) {
      std::filesystem::path file_path = symbolic_link_path;
      // Remove previous symbolic links.
      // Some titles can provide root within specific directory.
      kernel_state_->file_system()->UnregisterSymbolicLink(
          kDefaultPartitionSymbolicLink);
      kernel_state_->file_system()->UnregisterSymbolicLink(
          kDefaultGameSymbolicLink);

      file_path /= std::filesystem::path(xam->loader_data().launch_path);

      kernel_state_->file_system()->RegisterSymbolicLink(
          kDefaultPartitionSymbolicLink,
          xe::path_to_utf8(file_path.parent_path()));
      kernel_state_->file_system()->RegisterSymbolicLink(
          kDefaultGameSymbolicLink, xe::path_to_utf8(file_path.parent_path()));

      return xe::path_to_utf8(file_path);
    }
  }

  if (!cvars::launch_module.empty()) {
    return path + cvars::launch_module;
  }

  return path + "default.xex";
}

static std::string format_version(xex2_version version) {
  // fmt::format doesn't like bit fields we use + to bypass it
  return fmt::format("{}.{}.{}.{}", +version.major, +version.minor,
                     +version.build, +version.qfe);
}

// Installing these at the Guide button press misses anything that happens
// during xam and hud initialisation - XUI class registration among it - so
// arm them as soon as the title is loaded instead.
// Sample every guest thread's PC. Split into two passes on purpose: the raw
// registers are logged before anything touches the code cache, because
// LookupFunction takes the code cache lock and that is exactly the lock a
// JIT/loader deadlock is likely to be holding. Resolving first would hang the
// probe and lose the only evidence.
// Report how much of xam's .text reads back as zero. Called at more than one
// point: a page that is populated at load and zero later means something is
// clobbering the image, which is a different bug from it never being loaded.
static void ReportXamTextPopulation(Memory* memory, const char* when) {
  const uint32_t kTextStart = 0x81770000u;
  const uint32_t kTextEnd = 0x81D60000u;
  uint32_t zero_pages = 0, total_pages = 0, run = 0, best_run = 0;
  uint32_t best_start = 0, first_zero = 0, last_zero = 0;
  for (uint32_t pg = kTextStart; pg < kTextEnd; pg += 0x1000) {
    auto* hp = memory->LookupHeap(pg);
    if (!hp || hp->QueryRangeAccess(pg, pg + 0xFFF) ==
                   xe::memory::PageAccess::kNoAccess) {
      continue;
    }
    ++total_pages;
    const uint32_t* w = memory->TranslateVirtual<const uint32_t*>(pg);
    bool all_zero = true;
    for (uint32_t i = 0; i < 0x1000 / 4; ++i) {
      if (w[i]) {
        all_zero = false;
        break;
      }
    }
    if (all_zero) {
      ++zero_pages;
      if (!first_zero) first_zero = pg;
      last_zero = pg;
      if (++run > best_run) {
        best_run = run;
        best_start = pg - (run - 1) * 0x1000;
      }
    } else {
      run = 0;
    }
  }
  // Whole-page counting misses a hole inside an otherwise populated page,
  // which is exactly what a bogus function start landing in inter-function
  // padding would look like. Dump the specific addresses the scanner has
  // tripped on so they can be diffed against the image on disk.
  for (uint32_t probe_addr : {0x8186E528u, 0x818936B8u, 0x81747D70u}) {
    auto* hp = memory->LookupHeap(probe_addr);
    if (!hp || hp->QueryRangeAccess(probe_addr, probe_addr + 31) ==
                   xe::memory::PageAccess::kNoAccess) {
      XELOGI("xam probe {:08X} ({}): unmapped", probe_addr, when);
      continue;
    }
    std::string words;
    for (uint32_t i = 0; i < 8; ++i) {
      words += fmt::format(
          "{:08X} ", xe::load_and_swap<uint32_t>(
                         memory->TranslateVirtual(probe_addr + i * 4)));
    }
    XELOGI("xam probe {:08X} ({}): {}", probe_addr, when, words);
  }
  XELOGI(
      "xam .text population ({}): {} of {} mapped pages are entirely zero "
      "({:.1f}%); longest zero run {} pages at {:08X}; first {:08X} "
      "last {:08X}",
      when, zero_pages, total_pages,
      total_pages ? 100.0 * zero_pages / total_pages : 0.0, best_run,
      best_start, first_zero, last_zero);
}

static void ArmGuideThreadProbe(xe::kernel::KernelState* ks, int pdelay) {
  // Two threads, because enumerating the object table is exactly what a
  // freeze blocks on. The cacher keeps a fresh list of thread objects and
  // will itself wedge on the object table lock once the hang hits - that is
  // expected. The sampler only ever touches the cached references, so it can
  // still read thread contexts after everything else is stuck. Holding
  // object_refs keeps those threads alive, so the handles stay valid.
  struct Shared {
    std::mutex mu;
    std::vector<kernel::object_ref<kernel::XThread>> threads;
    uint32_t generation = 0;
  };
  auto shared = std::make_shared<Shared>();

  std::thread([shared, ksp = ks]() {
    xe::threading::set_name("GuideProbeCache");
    for (;;) {
      auto ths = ksp->object_table()->GetObjectsByType<kernel::XThread>(
          kernel::XObject::Type::Thread);
      {
        std::lock_guard<std::mutex> lk(shared->mu);
        shared->threads = std::move(ths);
        shared->generation++;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }).detach();

  auto* pproc = ks->processor();
  std::thread([pdelay, pproc, shared]() {
    xe::threading::set_name("GuideThreadProbe");
    std::this_thread::sleep_for(std::chrono::seconds(pdelay));
    // Compare against the same scan taken at load: a page populated then and
    // zero now means the image is being clobbered after loading, which is a
    // different bug from it never being loaded.
    ReportXamTextPopulation(pproc->memory(), "at probe time");
    // Plain file, not XELOGI: if the logger were wedged these markers would
    // be the only evidence that the probe ran at all.
    FILE* pf = fopen("probe.txt", "w");
    std::vector<kernel::object_ref<kernel::XThread>> ths;
    uint32_t gen = 0;
    {
      std::lock_guard<std::mutex> lk(shared->mu);
      ths = shared->threads;
      gen = shared->generation;
    }
    if (pf) {
      fprintf(pf, "probe: awake, snapshot gen=%u threads=%zu%s", gen,
              ths.size(), "\n");
      // Who is sitting on the global critical region? It is supposed to be
      // held only for very short bursts, so a stable owner here is the hang.
      fprintf(pf, "probe: GCR owner_tid=%lu recursion=%u%s",
              static_cast<unsigned long>(
                  xe::global_critical_region::mutex().owner_thread_id()),
              xe::global_critical_region::mutex().recursion_count(),
              "\n");
      // The GCR owner is an OS thread id, and the holder is often a thread
      // created after the cached snapshot. Enumerate the process's threads
      // and read their names straight from the OS, which needs no kernel
      // lock at all.
      DWORD gcr_owner =
          xe::global_critical_region::mutex().owner_thread_id();
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        DWORD pid = GetCurrentProcessId();
        if (Thread32First(snap, &te)) {
          do {
            if (te.th32OwnerProcessID != pid) continue;
            std::string nm;
            HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                   te.th32ThreadID);
            if (th) {
              PWSTR desc = nullptr;
              if (SUCCEEDED(GetThreadDescription(th, &desc)) && desc) {
                for (PWSTR q = desc; *q; ++q) {
                  nm += (*q < 128) ? char(*q) : '?';
                }
                LocalFree(desc);
              }
              CloseHandle(th);
            }
            fprintf(pf, "os-thread tid=%-6lu %-28s%s%s",
                    static_cast<unsigned long>(te.th32ThreadID), nm.c_str(),
                    te.th32ThreadID == gcr_owner ? "  <== GCR OWNER" : "",
                    "\n");
          } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
        fflush(pf);
      }
      // Map host addresses to loaded modules, so the owner's rip and return
      // addresses name a DLL instead of a bare number.
      struct Mod {
        uint64_t base, size;
        std::string name;
      };
      std::vector<Mod> mods;
      HANDLE msnap = CreateToolhelp32Snapshot(
          TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
      if (msnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me;
        me.dwSize = sizeof(me);
        if (Module32FirstW(msnap, &me)) {
          do {
            std::string nm;
            for (PWSTR q = me.szModule; *q; ++q) {
              nm += (*q < 128) ? char(*q) : '?';
            }
            mods.push_back({reinterpret_cast<uint64_t>(me.modBaseAddr),
                            me.modBaseSize, nm});
          } while (Module32NextW(msnap, &me));
        }
        CloseHandle(msnap);
      }
      auto where = [&mods](uint64_t a) -> std::string {
        for (auto& m : mods) {
          if (a >= m.base && a < m.base + m.size) {
            return fmt::format("{}+{:X}", m.name, a - m.base);
          }
        }
        return "?";
      };

      // Sample the GCR owner directly by tid. It is typically a guest thread
      // created after the cached snapshot, so this is the only way to see
      // where it stopped.
      if (gcr_owner) {
        HANDLE oh = OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
            FALSE, gcr_owner);
        if (oh) {
          CONTEXT oc = {};
          oc.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
          if (SuspendThread(oh) != static_cast<DWORD>(-1)) {
            if (GetThreadContext(oh, &oc)) {
              fprintf(pf, "GCR owner rip=%016llX  %s%s",
                      static_cast<unsigned long long>(oc.Rip),
                      where(oc.Rip).c_str(), "\n");
              fflush(pf);
              // Walk a little of its stack for return addresses; the guest
              // frame that took the lock should be in here somewhere.
              for (int d = 0; d < 48; ++d) {
                uint64_t slot = 0;
                SIZE_T got = 0;
                if (!ReadProcessMemory(
                        GetCurrentProcess(),
                        reinterpret_cast<LPCVOID>(oc.Rsp + d * 8), &slot,
                        sizeof(slot), &got) ||
                    got != sizeof(slot)) {
                  break;
                }
                if (slot > 0x10000) {
                  std::string w = where(slot);
                  if (w != "?") {
                    fprintf(pf, "GCR owner stack[%02d]=%016llX  %s%s", d,
                            static_cast<unsigned long long>(slot), w.c_str(),
                            "\n");
                  }
                }
              }
              fflush(pf);
            }
            ResumeThread(oh);
          }
          CloseHandle(oh);
        }
      }
    }
    struct Row {
      uint32_t h;
      std::string nm;
      uint64_t rip;
      uint32_t lr, r3;
      uint32_t tid;
    };
    std::vector<Row> rows;
    for (auto& th : ths) {
      void* nh2 = th->thread() ? th->thread()->native_handle() : nullptr;
      if (!nh2) continue;
      uint64_t rip = 0;
      uint32_t lr = 0, r3 = 0;
      CONTEXT c2 = {};
      c2.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      if (SuspendThread(reinterpret_cast<HANDLE>(nh2)) !=
          static_cast<DWORD>(-1)) {
        if (GetThreadContext(reinterpret_cast<HANDLE>(nh2), &c2)) {
          rip = c2.Rip;
        }
        ResumeThread(reinterpret_cast<HANDLE>(nh2));
      }
      auto* tc = th->thread_state() ? th->thread_state()->context() : nullptr;
      if (tc) {
        lr = static_cast<uint32_t>(tc->lr);
        r3 = static_cast<uint32_t>(tc->r[3]);
      }
      rows.push_back({th->handle(), th->thread_name(), rip, lr, r3,
                      th->thread() ? th->thread()->system_id() : 0});
    }
    if (pf) {
      for (auto& r : rows) {
        fprintf(pf, "%08X %-26s tid=%-6u rip=%016llX lr=%08X r3=%08X\n",
                r.h, r.nm.c_str(), r.tid,
                static_cast<unsigned long long>(r.rip), r.lr, r.r3);
      }
      fprintf(pf, "probe: sampled %zu threads\n", rows.size());
      fflush(pf);
    }
    // Resolution last: LookupFunction takes the code cache lock and may never
    // return under a deadlock. Everything above is already on disk by now.
    auto* cc2 = pproc->backend()->code_cache();
    for (auto& r : rows) {
      auto* f2 = r.rip ? cc2->LookupFunction(r.rip) : nullptr;
      uint32_t g2 = f2 ? f2->MapMachineCodeToGuestAddress(r.rip) : 0;
      if (pf) {
        fprintf(pf, "resolved %08X guest %08X%s", r.h, g2, "\n");
        fflush(pf);
      }
      XELOGI("ThreadProbe: {:08X} {:26} guest {:08X} lr={:08X} r3={:08X}{}",
             r.h, r.nm, g2, r.lr, r.r3,
             f2 ? "" : "  (host: in a kernel call)");
    }
  }).detach();
  XELOGI("ThreadProbe: armed for {}s", pdelay);
}

static void InstallGuideStoreTraces(xe::kernel::KernelState* ks) {
  static std::vector<std::unique_ptr<cpu::Breakpoint>> st_bps;
  if (!cvars::guide_trace_stores.empty() && st_bps.empty()) {
    std::string spec = cvars::guide_trace_stores;
    size_t pos = 0;
    while (pos <= spec.size()) {
      size_t comma = spec.find(',', pos);
      std::string tok = spec.substr(
          pos, comma == std::string::npos ? std::string::npos
                                          : comma - pos);
      if (!tok.empty()) {
        uint32_t addr =
            uint32_t(std::strtoul(tok.c_str(), nullptr, 16));
        if (addr) {
          auto bp = std::make_unique<cpu::Breakpoint>(
              ks->processor(),
              cpu::Breakpoint::AddressType::kGuest,
              uint64_t(addr),
              [](cpu::Breakpoint* bp,
                 cpu::ThreadDebugInfo* ti, uint64_t hpc) {
                auto* th = kernel::XThread::GetCurrentThread();
                auto* c =
                    th ? th->thread_state()->context() : nullptr;
                static std::atomic<uint32_t> sn{0};
                uint32_t s = ++sn;
                if (s > 60) return;
                // Dump the UTF-16 string at r3 as well:
                // these sites pass resource locators, and
                // the locator is what decides whether a
                // scene loads any visual content.
                std::string wstr;
                auto* mm = th ? th->kernel_state()->memory()
                              : nullptr;
                uint32_t sp = c ? uint32_t(c->r[3]) : 0;
                if (mm && sp > 0x1000u) {
                  auto* hp = mm->LookupHeap(sp);
                  if (hp && hp->QueryRangeAccess(sp, sp + 64) !=
                                xe::memory::PageAccess::kNoAccess) {
                    for (uint32_t w = 0; w < 48; ++w) {
                      uint16_t ch = xe::load_and_swap<uint16_t>(
                          mm->TranslateVirtual(sp + w * 2));
                      if (!ch) break;
                      wstr += (ch >= 0x20 && ch < 0x7F)
                                  ? char(ch) : '?';
                    }
                  }
                }
                std::string wstr2;
                uint32_t sp2 = c ? uint32_t(c->r[4]) : 0;
                if (mm && sp2 > 0x1000u) {
                  auto* hp2 = mm->LookupHeap(sp2);
                  if (hp2 &&
                      hp2->QueryRangeAccess(sp2, sp2 + 64) !=
                          xe::memory::PageAccess::kNoAccess) {
                    for (uint32_t w = 0; w < 48; ++w) {
                      uint16_t ch = xe::load_and_swap<uint16_t>(
                          mm->TranslateVirtual(sp2 + w * 2));
                      if (!ch) break;
                      wstr2 += (ch >= 0x20 && ch < 0x7F)
                                   ? char(ch) : '?';
                    }
                  }
                }
                XELOGI("StoreTrace {:08X} #{}: r3={:08X} "
                       "\"{}\"  r4={:08X} \"{}\"  lr={:08X}",
                       bp->guest_address(), s, sp, wstr,
                       sp2, wstr2, c ? uint32_t(c->lr) : 0);
              });
          ks->processor()->AddBreakpoint(bp.get());
          st_bps.push_back(std::move(bp));
        }
      }
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    XELOGI("StoreTrace: installed {} breakpoints",
           st_bps.size());
  }
}

X_STATUS Emulator::CompleteLaunch(const std::filesystem::path& path,
                                  const std::string_view module_path) {
  // Making changes to the UI (setting the icon) and executing game config
  // load callbacks which expect to be called from the UI thread.
  // If not on UI thread, dispatch to it synchronously.
  if (!display_window_->app_context().IsInUIThread()) {
    X_STATUS result = X_STATUS_UNSUCCESSFUL;
    display_window_->app_context().CallInUIThreadSynchronous(
        [this, &path, &module_path, &result]() {
          result = CompleteLaunch(path, module_path);
        });
    return result;
  }

  // Setup NullDevices for raw HDD partition accesses
  // Cache/STFC code baked into games tries reading/writing to these
  // By using a NullDevice that just returns success to all IO requests it
  // should allow games to believe cache/raw disk was accessed successfully

  // NOTE: this should probably be moved to xenia_main.cc, but right now we
  // need to register the \Device\Harddisk0\ NullDevice _after_ the
  // \Device\Harddisk0\Partition1 HostPathDevice, otherwise requests to
  // Partition1 will go to this. Registering during CompleteLaunch allows us
  // to make sure any HostPathDevices are ready beforehand. (see comment above
  // cache:\ device registration for more info about why)
  auto null_paths = {std::string("\\Partition0"), std::string("\\Cache0"),
                     std::string("\\Cache1")};
  auto null_device =
      std::make_unique<vfs::NullDevice>("\\Device\\Harddisk0", null_paths);
  if (null_device->Initialize()) {
    file_system_->RegisterDevice(std::move(null_device));
  }

  // Reset state.
  title_id_ = std::nullopt;
  title_name_ = "";
  title_version_ = "";
  display_window_->SetIcon(nullptr, 0);

  // Allow xam to request module loads.
  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");

  // Register \SystemRoot before the title runs. LaunchXexFile registers it
  // only after CompleteLaunch returns, by which time the guest has already
  // queried it - the dashboard looks for \SystemRoot\systemupdate.xex during
  // startup and gets "device not found". Doing it here makes the link exist
  // when the title first asks.
  if (cvars::system_root_early && !module_path.empty()) {
    std::string base(utf8::find_base_guest_path(module_path));
    if (!base.empty()) {
      file_system_->RegisterSymbolicLink("\\SystemRoot", base);
      XELOGI("Early SystemRoot -> '{}'", base);
    }
  }

  // LLE xam bootstrap: load the real xam.xex as a guest module before the main
  // module, so the main module's xam imports bind against xam's real export
  // table instead of Xenia's HLE xam. See XexModule::SetupLibraryImports.
  if (!cvars::guide_system_root.empty()) {
    // xam.xex and hud.xex normally have to sit on the title's own GAME:
    // device, which only works when the title is the dashboard folder.
    // Mount them separately so a real game disc can be the title.
    auto sys_device = std::make_unique<vfs::HostPathDevice>(
        "\\SYS", xe::to_path(cvars::guide_system_root), true);
    if (sys_device->Initialize() &&
        file_system_->RegisterDevice(std::move(sys_device))) {
      file_system_->RegisterSymbolicLink("SYS:", "\\SYS");
      XELOGI("Guide: mounted system root {} as SYS:",
             cvars::guide_system_root);
    } else {
      XELOGE("Guide: failed to mount system root {}",
             cvars::guide_system_root);
    }
  }
  if (!cvars::lle_xam.empty()) {
    XELOGI("LLE xam: loading guest xam from {}", cvars::lle_xam);
    lle_xam_module_ = kernel_state_->LoadUserModule(cvars::lle_xam, false);
    auto xam_module = lle_xam_module_;
    if (!xam_module) {
      XELOGE("LLE xam: failed to load {}", cvars::lle_xam);
      return X_STATUS_NOT_FOUND;
    }
    // call_entry=false: DllMain is run later on a real guest thread by the
    // bootstrap below. CompleteLaunch runs on the UI thread, which has no
    // guest thread state, so executing guest code here is not valid.
    X_RESULT xam_result =
        kernel_state_->FinishLoadingUserModule(xam_module, false);
    if (XFAILED(xam_result)) {
      XELOGE("LLE xam: failed to finish loading {}", cvars::lle_xam);
      return xam_result;
    }
    XELOGI("LLE xam: loaded at {:08X}", xam_module->hmodule_ptr());
    if (cvars::guide_patch_cmdbuf_reset) {
      // 81A01464  stw r30,0x2B4C(r31)  ; zeroes the cmdbuf write cursor
      const uint32_t kRAddr = 0x81A01464u;
      const uint32_t kROrig = 0x93DF2B4Cu;
      auto* rw = memory()->TranslateVirtual<uint32_t*>(kRAddr);
      uint32_t rcur = xe::load_and_swap<uint32_t>(rw);
      if (rcur == kROrig) {
        void* rpage = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(rw) & ~uintptr_t(0xFFF));
        xe::memory::PageAccess rold = xe::memory::PageAccess::kReadOnly;
        if (xe::memory::Protect(rpage, 0x1000,
                                xe::memory::PageAccess::kReadWrite,
                                &rold)) {
          xe::store_and_swap<uint32_t>(rw, 0x60000000u);
          xe::memory::Protect(rpage, 0x1000, rold, nullptr);
          XELOGI("Guide: patched {:08X} {:08X} -> 60000000 (cmdbuf "
                 "cursor reset removed)",
                 kRAddr, rcur);
        }
      } else {
        XELOGW("Guide: NOT patching {:08X}: found {:08X}, expected {:08X}",
               kRAddr, rcur, kROrig);
      }
    }
    ReportXamTextPopulation(memory(), "after xam load");
    // These addresses hold correct code here and read back as zero later in
    // the run, then are correct again by 30s. Poll them so the transition is
    // timestamped against the surrounding log rather than inferred.
    {
      Memory* wmem = memory();
      std::thread([wmem]() {
        xe::threading::set_name("XamTextWatch");
        const uint32_t addrs[] = {0x8186E528u, 0x818936B8u, 0x81747D70u,
                                  0x818AE538u};
        uint32_t last[4] = {};
        bool primed = false;
        for (int iter = 0; iter < 120000; ++iter) {
          for (int i = 0; i < 4; ++i) {
            auto* hp = wmem->LookupHeap(addrs[i]);
            if (!hp || hp->QueryRangeAccess(addrs[i], addrs[i] + 3) ==
                           xe::memory::PageAccess::kNoAccess) {
              continue;
            }
            uint32_t v = xe::load_and_swap<uint32_t>(
                wmem->TranslateVirtual(addrs[i]));
            if (primed && v != last[i]) {
              // Ask the OS what the page actually is at the moment it
              // changes. If the guard is still PAGE_READONLY then nothing
              // wrote through this view; if the protection has been reset,
              // the mapping was replaced rather than written.
              MEMORY_BASIC_INFORMATION mbi = {};
              void* hostp = wmem->TranslateVirtual(addrs[i]);
              SIZE_T got = VirtualQuery(hostp, &mbi, sizeof(mbi));
              XELOGE(
                  "XamTextWatch: {:08X} changed {:08X} -> {:08X} "
                  "(host={} protect={:X} state={:X} type={:X} allocbase={} "
                  "regionsize={:X})",
                  addrs[i], last[i], v, hostp,
                  got ? mbi.Protect : 0u, got ? mbi.State : 0u,
                  got ? mbi.Type : 0u, got ? mbi.AllocationBase : nullptr,
                  got ? static_cast<uint64_t>(mbi.RegionSize) : 0ull);
            }
            last[i] = v;
          }
          primed = true;
          std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
      }).detach();
      XELOGI("XamTextWatch: polling 4 xam .text addresses");
      // Opt-in (XENIA_XAM_RO=1): make xam's .text read-only at the host level
      // so whatever writes zeros over it traps instead of succeeding. The
      // existing fault logging then names the writer. Off by default because
      // it deliberately turns a silent corruption into a crash.
      if (std::getenv("XENIA_XAM_RO")) {
        auto* host = memory()->TranslateVirtual(0x81740000u);
        xe::memory::PageAccess old_access = xe::memory::PageAccess::kReadWrite;
        bool ok = xe::memory::Protect(host, 0x818C0000u - 0x81740000u,
                                      xe::memory::PageAccess::kReadOnly,
                                      &old_access);
        XELOGI("XamTextWatch: read-only guard over 81740000-818C0000 -> {}",
               ok ? "armed" : "FAILED");
      }
    }
    // Opt-in (XENIA_EFAIL_TAG=1). Every E_FAIL site returns the same
    // 0x80004005, so a failing XuiSceneCreate cannot say where it came from,
    // and breakpointing the loader stops the path being taken at all. Give
    // each of the 18 E_FAIL construction sites in xam's XUI region its own
    // low word instead: the HRESULT that comes back then names the site.
    // Tag N corresponds to index N below, i.e. 0x80004010 + N.
    if (std::getenv("XENIA_EFAIL_TAG")) {
      static const uint32_t kOriSites[] = {
          0x81938128u, 0x81939A9Cu, 0x8193AE44u, 0x8193B458u, 0x8193C098u,
          0x8193CCFCu, 0x8193D2D0u, 0x819560B4u, 0x819577D8u, 0x8195D85Cu,
          0x8195D8ECu, 0x8195E360u, 0x81963758u, 0x81968B7Cu, 0x8196BD30u,
          0x8196CA5Cu, 0x8196ED70u, 0x8196FB74u,
      };
      uint32_t tagged = 0;
      for (uint32_t i = 0; i < xe::countof(kOriSites); ++i) {
        auto* w = memory()->TranslateVirtual<uint32_t*>(kOriSites[i]);
        uint32_t cur = xe::load_and_swap<uint32_t>(w);
        // Match any "ori rX,rX,0x4005" - the constant is built into r29,
        // r30 and r31 as well as r3 - and rewrite only the immediate so the
        // destination register is preserved.
        if ((cur & 0xFC00FFFFu) != 0x60004005u) {
          XELOGW("EFailTag: NOT patching {:08X}: found {:08X}", kOriSites[i],
                 cur);
          continue;
        }
        void* pg = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(w) &
                                           ~uintptr_t(0xFFF));
        xe::memory::PageAccess old_access = xe::memory::PageAccess::kReadOnly;
        if (xe::memory::Protect(pg, 0x1000,
                                xe::memory::PageAccess::kReadWrite,
                                &old_access)) {
          xe::store_and_swap<uint32_t>(w, (cur & 0xFFFF0000u) |
                                                (0x4010u + i));
          xe::memory::Protect(pg, 0x1000, old_access, nullptr);
          ++tagged;
        }
      }
      XELOGI("EFailTag: tagged {} of {} XUI E_FAIL sites (0x80004010 + index)",
             tagged, xe::countof(kOriSites));
    }
    if (cvars::guide_patch_null_render) {
      // 818FDEF0  lwz r11,0x1C(r27)   ; XUI context's null-render flag
      // 818FDF14  stw r11,0x134(r30)  ; over the device context's copy
      const uint32_t kAddr = 0x818FDF14u;
      const uint32_t kOrig = 0x917E0134u;
      XELOGI("Guide: patch step 1, memory()={}",
             static_cast<const void*>(memory()));
      auto* pw = memory()->TranslateVirtual<uint32_t*>(kAddr);
      XELOGI("Guide: patch step 2, host ptr={}", static_cast<void*>(pw));
      uint32_t cur = xe::load_and_swap<uint32_t>(pw);
      XELOGI("Guide: patch step 3, cur={:08X}", cur);
      if (cur == kOrig) {
        // Guest code pages are mapped without write access, so the store
        // faults unless the page is temporarily made writable.
        void* page = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pw) &
                                             ~uintptr_t(0xFFF));
        xe::memory::PageAccess old_access = xe::memory::PageAccess::kReadOnly;
        bool unprotected = xe::memory::Protect(
            page, 0x1000, xe::memory::PageAccess::kReadWrite, &old_access);
        if (!unprotected) {
          XELOGE("Guide: could not unprotect {:08X} for patching", kAddr);
          return X_STATUS_UNSUCCESSFUL;
        }
        xe::store_and_swap<uint32_t>(pw, 0x60000000u);  // nop
        xe::memory::Protect(page, 0x1000, old_access, nullptr);
        XELOGI("Guide: patched {:08X} {:08X} -> 60000000 (null-render copy "
               "removed)",
               kAddr, cur);
      } else {
        XELOGW("Guide: NOT patching {:08X}: found {:08X}, expected {:08X}",
               kAddr, cur, kOrig);
      }
    }
  }

  if (cvars::guide_auto_press_seconds > 0) {
    int delay = cvars::guide_auto_press_seconds;
    std::thread([this, delay]() {
      xe::threading::set_name("GuideAutoPress");
      xe::threading::Sleep(std::chrono::seconds(delay));
      XELOGI("Guide button: auto-press firing after {}s", delay);
      on_guide_button_pressed(0);
    }).detach();
    XELOGI("Guide button: auto-press armed for {}s", delay);
  }
  // Arm the thread probe here rather than only from the Guide button path.
  // The button handler is gated on a handler that a freeze during hud load
  // never publishes, so a probe armed there can never fire on the very hang
  // it exists to diagnose.
  if (cvars::guide_probe_threads_seconds > 0) {
    ArmGuideThreadProbe(kernel_state_.get(),
                        cvars::guide_probe_threads_seconds);
  }
  XELOGI("Loading module {}", module_path);
  auto module = kernel_state_->LoadUserModule(module_path);
  // xam code that was correct right after xam loaded reads back as zero later
  // in the run. Bracket the title load, which is the largest thing that
  // happens in between.
  ReportXamTextPopulation(memory(), "after title load");
  if (!module) {
    XELOGE("Failed to load user module {}", path);
    return X_STATUS_NOT_FOUND;
  }

  if (!module->is_executable()) {
    if (!cvars::allow_dll_module_launch) {
      kernel_state_->UnloadUserModule(module, false);
      XELOGE("Failed to load user module {}", path);
      return X_STATUS_NOT_SUPPORTED;
    }
    // System DLL modules (hud.xex, xam.xex, ...) are normally loaded by xam
    // rather than booted. Allow launching them directly so their imports can
    // be resolved and reported.
    XELOGW("Launching non-executable (DLL) module {}", path);
  }

  X_RESULT result = kernel_state_->ApplyTitleUpdate(module);
  if (XFAILED(result)) {
    XELOGE("Failed to apply title update! Cannot run module {}", path);
    return result;
  }

  result = kernel_state_->FinishLoadingUserModule(module);
  if (XFAILED(result)) {
    XELOGE("Failed to initialize user module {}", path);
    return result;
  }
  // Grab the current title ID.
  xex2_opt_execution_info* info = nullptr;
  uint32_t workspace_address = 0;
  module->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &info);

  // The title workspace (/XEXWORKSPACE) is addressed by the guest as the
  // region immediately following the module image, not as an arbitrary
  // allocation. xam derives its heap base from the image end, so placing this
  // anywhere else leaves xam's heaps uninitialized and every xam allocation
  // fails. Try the fixed address first and only fall back to a floating
  // allocation if that range is unavailable.
  if (module->xex_module()) {
    workspace_address = xe::round_up(
        module->xex_module()->base_address() + module->xex_module()->image_size(),
        0x1000);
    auto* heap = kernel_state_->memory()->LookupHeap(workspace_address);
    if (!heap ||
        !heap->AllocFixed(workspace_address, module->workspace_size(), 0x1000,
                          kMemoryAllocationReserve | kMemoryAllocationCommit,
                          kMemoryProtectRead | kMemoryProtectWrite)) {
      XELOGW("Title workspace: could not reserve {} bytes at {:08X}",
             module->workspace_size(), workspace_address);
      workspace_address = 0;
    } else {
      XELOGI("Title workspace: {:08X}-{:08X} ({} bytes)", workspace_address,
             workspace_address + module->workspace_size(),
             module->workspace_size());
    }
  }
  if (!workspace_address) {
    kernel_state_->memory()
        ->LookupHeapByType(false, 0x1000)
        ->Alloc(module->workspace_size(), 0x1000,
                kMemoryAllocationReserve | kMemoryAllocationCommit,
                kMemoryProtectRead | kMemoryProtectWrite, false,
                &workspace_address);
  }

  if (!info) {
    title_id_ = 0;
  } else {
    title_id_ = info->title_id;
    auto title_version = info->version();
    if (title_version.value != 0) {
      title_version_ = format_version(title_version);
    }
  }

  // Try and load the resource database (xex only).
  if (module->title_id()) {
    auto title_id = fmt::format("{:08X}", module->title_id());

    // Load the per-game configuration file and make sure updates are handled
    // by the callbacks.
    config::LoadGameConfig(title_id);
    assert_true(game_config_load_callback_loop_next_index_ == SIZE_MAX);
    game_config_load_callback_loop_next_index_ = 0;
    while (game_config_load_callback_loop_next_index_ <
           game_config_load_callbacks_.size()) {
      game_config_load_callbacks_[game_config_load_callback_loop_next_index_++]
          ->PostGameConfigLoad();
    }
    game_config_load_callback_loop_next_index_ = SIZE_MAX;

    const auto db = kernel_state_->module_xdbf(module);

    game_info_database_ =
        std::make_unique<kernel::util::GameInfoDatabase>(db.get());
    kernel_state_->xam_state()->LoadSpaInfo(db.get());

    kernel_state_->xam_state()->user_tracker()->AddTitleToPlayedList();

    if (game_info_database_->IsValid()) {
      title_name_ = game_info_database_->GetTitleName(static_cast<XLanguage>(
          kernel_state_->xconfig()->ReadSetting<uint32_t>(
              kernel::XCONFIG_USER_CATEGORY, kernel::XCONFIG_USER_LANGUAGE)));
      XELOGI("Title name: {}", title_name_);

      // Show achievments data
      tabulate::Table table;
      table.format().multi_byte_characters(true);
      table.add_row({"ID", "Title", "Description", "Type", "Gamerscore"});

      const std::vector<kernel::util::GameInfoDatabase::Achievement>
          achievement_list = game_info_database_->GetAchievements();
      for (const kernel::util::GameInfoDatabase::Achievement& entry :
           achievement_list) {
        const std::string type = GetAchievementTypeName(
            kernel::xam::GetAchievementType(entry.flags));

        table.add_row({fmt::format("{}", entry.id), entry.label,
                       entry.description, type,
                       fmt::format("{}", entry.gamerscore)});
      }
      XELOGI("\n-------------------- ACHIEVEMENTS --------------------\n{}",
             table.str());

      const std::vector<kernel::util::GameInfoDatabase::Property>
          properties_list = game_info_database_->GetProperties();

      // 4D5307DC SPA contains a lot of properties, limit properties to log.
      const auto properties_list_limit =
          properties_list | std::views::take(150);

      table = tabulate::Table();
      table.format().multi_byte_characters(true);
      table.add_row({"ID", "Name", "Matchmaking", "Data Size"});

      for (const kernel::util::GameInfoDatabase::Property& entry :
           properties_list_limit) {
        std::string label =
            string_util::remove_eol(string_util::trim(entry.description));

        table.add_row({fmt::format("{:08X}", entry.id), label,
                       entry.is_matchmaking ? "True" : "False",
                       fmt::format("{}", entry.data_size)});
      }

      std::string properties_totals;

      if (properties_list.size() > properties_list_limit.size()) {
        properties_totals =
            fmt::format("\nProperties: {}/{}", properties_list_limit.size(),
                        properties_list.size());
      }

      XELOGI("\n-------------------- PROPERTIES --------------------{}\n{}",
             properties_totals.c_str(), table.str());

      const std::vector<kernel::util::GameInfoDatabase::Context> contexts_list =
          game_info_database_->GetContexts();

      table = tabulate::Table();
      table.format().multi_byte_characters(true);
      table.add_row(
          {"ID", "Name", "Matchmaking", "Default Value", "Max Value"});

      for (const kernel::util::GameInfoDatabase::Context& entry :
           contexts_list) {
        std::string label =
            string_util::remove_eol(string_util::trim(entry.description));

        table.add_row({fmt::format("{:08X}", entry.id), label,
                       entry.is_matchmaking ? "True" : "False",
                       fmt::format("{}", entry.default_value),
                       fmt::format("{}", entry.max_value)});
      }
      XELOGI("\n-------------------- CONTEXTS --------------------\n{}",
             table.str());

      const std::vector<kernel::util::GameInfoDatabase::StatsView> stats_views =
          game_info_database_->GetStatsViews();

      // 4D5307EA SPA contains a lot of stats, limit views to log.
      const auto stats_views_limit = stats_views | std::views::take(100);

      table = tabulate::Table();
      table.format().multi_byte_characters(true);
      table.add_row({"ID", "View Type", "Name", "Skilled", "Arbitrated",
                     "Hidden", "Team View", "Online Only"});

      for (const kernel::util::GameInfoDatabase::StatsView& entry :
           stats_views_limit) {
        const std::string name =
            string_util::remove_eol(string_util::trim(entry.view.name));

        const std::string view_type =
            kernel::xam::GetViewTypeName(entry.view.view_type);

        table.add_row({fmt::format("{:08X}", entry.view.id), view_type, name,
                       entry.view.skilled ? "True" : "False",
                       entry.view.arbitrated ? "True" : "False",
                       entry.view.hidden ? "True" : "False",
                       entry.view.team_view ? "True" : "False",
                       entry.view.online_only ? "True" : "False"});
      }

      std::string stats_view_totals;

      if (stats_views.size() > stats_views_limit.size()) {
        stats_view_totals = fmt::format(
            "\nViews: {}/{}", stats_views_limit.size(), stats_views.size());
      }
      XELOGI("\n-------------------- STATS VIEWS --------------------{}\n{}",
             stats_view_totals.c_str(), table.str());

      const std::vector<kernel::util::GameInfoDatabase::PresenceMode>
          presence_modes = game_info_database_->GetPresenceModes();

      table = tabulate::Table();
      table.format().multi_byte_characters(true);
      table.add_row({"Context Value", "Contexts Count", "Properties Count"});

      for (const kernel::util::GameInfoDatabase::PresenceMode& entry :
           presence_modes) {
        table.add_row(
            {fmt::format("{}", entry.context_value),
             fmt::format("{}", entry.property_bag.contexts.size()),
             fmt::format("{}", entry.property_bag.properties.size())});
      }
      XELOGI("\n-------------------- PRESENCE MODES --------------------\n{}",
             table.str());

      auto icon_block = game_info_database_->GetIcon();
      if (!icon_block.empty()) {
        display_window_->SetIcon(icon_block.data(), icon_block.size());
      }
    }
  }

  // Initialize shader storage asynchronously - pipeline compilation happens in
  // background while the game goes through its normal startup (loading screens,
  // intro videos, etc.). With async_shader_compilation enabled, draws are
  // skipped until pipelines are ready, so this is safe. By the time actual
  // gameplay starts, most cached pipelines should be compiled.
  if (graphics_system_) {
    on_shader_storage_initialization(true);
    graphics_system_->InitializeShaderStorage(
        cache_root_, title_id_.value(), false,
        [this]() { on_shader_storage_initialization(false); });
  }

  // If a real xam was loaded, its DllMain must run on a guest thread before
  // the title starts, otherwise the title spins waiting for an uninitialized
  // xam. Do this synchronously so xam is ready before the main thread exists.
  if (module->is_executable() && lle_xam_module_ &&
      lle_xam_module_->entry_point()) {
    auto* ks = kernel_state_.get();
    auto xam_mod = lle_xam_module_;
    // SetExecutableModule first: InitializeGuestObject acquires the title
    // process thread_list_spinlock, which is only valid once initialized.
    kernel_state_->SetExecutableModule(module);
    // Neutralise the heap selector's app-id trap (Ghidra 817BAE38, runtime
    // 817B3C38: xam .text is shifted by 0x7200 because Xenia maps the XEX
    // basefile flat while Ghidra honours PE raw offsets). The trap fires when
    // the current app id does not match the requested one; the value the
    // selector actually returns is derived from flag bits further down, so
    // branching onto the OK path yields what a matching context would have.
    // This must happen before DllMain runs, or the JIT may already have cached
    // the untranslated block.
    // 817BAE38 is the app-id mismatch trap; 817BADFC terminates the 20-entry
    // flag-mask table scan that runs before it. Both sit on the path to a real
    // heap id, so probe them together.
    for (uint32_t ghidra_trap :
         {0x817BAE38u, 0x817BADFCu}) {
      if (!cvars::lle_xam_heap_patch) {
        break;
      }
      const uint32_t trap_addr = ghidra_trap - 0x7200u;
      auto* p = memory()->TranslateVirtual(trap_addr);
      // Guest code pages are mapped read-only, so the store faults unless the
      // page is temporarily made writable first.
      auto* heap = memory()->LookupHeap(trap_addr);
      uint32_t old_protect = 0;
      bool unprotected =
          heap && heap->Protect(trap_addr, 4,
                                kMemoryProtectRead | kMemoryProtectWrite,
                                &old_protect);
      XELOGI("LLE xam: trap {:08X} was {:08X}, unprotect={}", ghidra_trap,
             xe::load_and_swap<uint32_t>(p), unprotected);
      if (unprotected) {
        xe::store_and_swap<uint32_t>(p, 0x48000004u);
        XELOGI("LLE xam: trap {:08X} now {:08X}", ghidra_trap,
               xe::load_and_swap<uint32_t>(p));
        heap->Protect(trap_addr, 4, old_protect, nullptr);
      } else {
        XELOGE("LLE xam: could not unprotect {:08X}", ghidra_trap);
      }
    }
    // xam selects its heap from the "current app id", which its getter
    // derives from KeGetCurrentProcessType when there is no per-thread app
    // context: SYSTEM (2) yields 0xFE, anything else 0xEE. On hardware xam
    // initialises inside the system process, so run its DllMain there or the
    // heap selector traps on an app-id mismatch.
    auto xam_boot =
        kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
            ks, 1024 * 1024, 0, [ks, xam_mod]() -> int {
              auto* ts = kernel::XThread::GetCurrentThread()->thread_state();
              uint64_t args[] = {xam_mod->handle(), 1 /* PROCESS_ATTACH */, 0};
              XELOGI("LLE xam: DllMain entry={:08X}", xam_mod->entry_point());
              ks->processor()->Execute(ts, xam_mod->entry_point(), args,
                                       xe::countof(args));
              XELOGI("LLE xam: DllMain returned");
              // xam's heap descriptors (array at 0x81D4E1B0, 10 x 408 bytes)
              // are still all-zero after DllMain: the "Unable to commit %d
              // bytes for heap." path never runs, so creation is not failing,
              // it is never attempted. 817BBD70 (runtime 817B4B70) is the
              // routine that builds them - it calls the single-heap creator
              // five times - and it has no callers inside xam, so on hardware
              // something outside the module drives it. Take one argument.
              if (cvars::lle_xam_heap_init) {
                uint64_t hargs[] = {0};
                XELOGI("LLE xam: calling heap init 817B4B70");
                ks->processor()->Execute(ts, 0x817B4B70u, hargs,
                                         xe::countof(hargs));
                XELOGI("LLE xam: heap init returned");
              }
              return 0;
            },
            ks->GetSystemProcess()));
    xam_boot->set_name("LLE xam init");
    if (XSUCCEEDED(xam_boot->Create())) {
      XELOGI("LLE xam: waiting for init thread");
      xam_boot->Wait(0, 0, 0, nullptr);
      XELOGI("LLE xam: init complete");
      // The tag->index mapper (817BA1D8) returns a single flag bit as the
      // index for the 0x10000000 request class, so requests like 0x18100000
      // land on heap[0] deterministically - and heap[0] is the 0xCCCC
      // placeholder. Copy heap[1]'s descriptor over it to test whether that
      // is the whole story: if so the failures go away while xam stays up.
      if (cvars::lle_xam_heap0_alias) {
        auto* d0 = memory()->TranslateVirtual(0x81D4E1B0u);
        auto* d1 = memory()->TranslateVirtual(0x81D4E1B0u + 408u);
        std::memcpy(d0, d1, 408);
        xe::store_and_swap<uint32_t>(d0, 0);  // keep id field as index 0
        XELOGI("LLE xam: aliased heap[0] to heap[1]");
      }
      // Read the heap descriptors straight out of guest memory rather than
      // inferring their state from allocation-failure counts. Array is at
      // 0x81D4E1B0 (a constant embedded in xam's code, so already a flat
      // runtime address), 10 entries of 408 bytes. Field +4 is the "created"
      // flag: every reader in xam early-outs when it is zero.
      for (uint32_t i = 0; i < 10; ++i) {
        uint32_t desc = 0x81D4E1B0u + i * 408u;
        auto* dp = memory()->TranslateVirtual(desc);
        XELOGI("LLE xam: heap[{}] @{:08X} +0={:08X} +4={:08X} +8={:08X} "
               "+1C={:08X} +24={:08X}",
               i, desc, xe::load_and_swap<uint32_t>(dp),
               xe::load_and_swap<uint32_t>(dp + 4),
               xe::load_and_swap<uint32_t>(dp + 8),
               xe::load_and_swap<uint32_t>(dp + 0x1C),
               xe::load_and_swap<uint32_t>(dp + 0x24));
      }
      // xam's current-app-id getter (81783270) reads a global sentinel first:
      // when it holds -1 the getter short-circuits to 0xFE (XamApp) instead of
      // falling through to KeGetCurrentProcessType, which yields 0xEE on title
      // threads and makes the heap selector trap. Title code calling into xam
      // therefore lands on heap 0, the zero-sized placeholder.
      if (cvars::lle_xam_appid_sentinel) {
        auto* p = memory()->TranslateVirtual(0x81D227F0);
        XELOGI("LLE xam: app-id sentinel was {:08X}, forcing FFFFFFFF",
               xe::load_and_swap<uint32_t>(p));
        xe::store_and_swap<uint32_t>(p, 0xFFFFFFFFu);
      }
    } else {
      XELOGE("LLE xam: failed to create init thread");
    }
  }

  kernel::object_ref<kernel::XThread> main_thread;
  if (!module->is_executable() && cvars::allow_dll_module_launch) {
    // DLL modules have no title entry point to launch, and their DllMain must
    // not run on the UI thread (no guest thread state there). Run the
    // DLL_PROCESS_ATTACH sequence on a real guest thread instead.
    auto* ks = kernel_state_.get();
    auto xam_mod = lle_xam_module_;
    auto dll_mod = module;
    auto boot = kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
        ks, 1024 * 1024, 0, [ks, xam_mod, dll_mod]() -> int {
          auto* ts = kernel::XThread::GetCurrentThread()->thread_state();
          auto attach =
              [&](const kernel::object_ref<kernel::UserModule>& m) {
                if (!m || !m->entry_point()) {
                  return;
                }
                uint64_t args[] = {m->handle(), 1 /* DLL_PROCESS_ATTACH */, 0};
                XELOGI("Bootstrap: DllMain {} entry={:08X}", m->name(),
                       m->entry_point());
                ks->processor()->Execute(ts, m->entry_point(), args,
                                         xe::countof(args));
                XELOGI("Bootstrap: DllMain {} returned", m->name());
              };
          attach(xam_mod);
          attach(dll_mod);
          XELOGI("Bootstrap: attach sequence complete");

          // If a system app registered a message handler (hud.xex registers
          // as app 0xFF), optionally dispatch the Guide open message to it.
          if (cvars::lle_show_guide) {
            uint32_t guide_handler = ks->sys_app_handler(0xFF);
            if (guide_handler) {
              auto* mem = ks->memory();
              // hud copies 0x47C bytes out of the inner struct and writes
              // the result size through the third argument, so that must be
              // a pointer, not a size.
              uint32_t inner = mem->SystemHeapAlloc(0x500, 16);
              uint32_t buf = mem->SystemHeapAlloc(0x40, 16);
              uint32_t out_sz = mem->SystemHeapAlloc(0x10, 16);
              XELOGI("Guide: buffers inner={:08X} buf={:08X} out_sz={:08X}",
                     inner, buf, out_sz);
              std::memset(mem->TranslateVirtual(inner), 0, 0x500);
              std::memset(mem->TranslateVirtual(buf), 0, 0x40);
              std::memset(mem->TranslateVirtual(out_sz), 0, 0x10);
              auto* iw = mem->TranslateVirtual<xe::be<uint32_t>*>(inner);
              iw[2] = static_cast<uint32_t>(cvars::guide_subcommand);
              auto* bw = mem->TranslateVirtual<xe::be<uint32_t>*>(buf);
              bw[0] = 1;
              bw[1] = inner;
              XELOGI("Bootstrap: Guide dispatch msg=80000004 subcmd={} -> {:08X}",
                     int32_t(cvars::guide_subcommand), guide_handler);
              uint64_t gargs[] = {static_cast<uint64_t>(cvars::guide_message), buf, out_sz};
              uint64_t gres = ks->processor()->Execute(ts, guide_handler, gargs,
                                                       xe::countof(gargs));
              XELOGI("Bootstrap: Guide handler returned {:08X}",
                     static_cast<uint32_t>(gres));
            } else {
              XELOGW("Bootstrap: no system app 0xFF handler registered");
            }
          }

          if (cvars::lle_show_guide && xam_mod) {
            // XamShowGuideUI == xam ordinal 0x304.
            uint32_t guide_addr = xam_mod->GetProcAddressByOrdinal(0x304);
            XELOGI("Bootstrap: XamShowGuideUI (ord 0x304) -> {:08X}",
                   guide_addr);
            if (guide_addr) {
              uint64_t guide_args[] = {0};
              uint64_t guide_ret = ks->processor()->Execute(
                  ts, guide_addr, guide_args, xe::countof(guide_args));
              XELOGI("Bootstrap: XamShowGuideUI returned {:08X}",
                     static_cast<uint32_t>(guide_ret));
            } else {
              XELOGE("Bootstrap: could not resolve XamShowGuideUI");
            }
          }
          return 0;
        }));
    boot->set_name("Guide Bootstrap");
    // Must happen before Create(): SetExecutableModule initializes the title
    // X_KPROCESS, and XThread::InitializeGuestObject acquires that process's
    // thread_list_spinlock. Creating a thread first spins on an uninitialized
    // lock forever. This mirrors the ordering in KernelState::LaunchModule.
    kernel_state_->SetExecutableModule(module);
    XELOGI("Bootstrap: creating thread");
    X_STATUS boot_status = boot->Create();
    XELOGI("Bootstrap: Create() returned {:08X}", boot_status);
    if (XFAILED(boot_status)) {
      XELOGE("Failed to create Guide bootstrap thread");
      return X_STATUS_UNSUCCESSFUL;
    }
    main_thread = kernel::object_ref<kernel::XThread>(boot.release());
  } else {
    main_thread = kernel_state_->LaunchModule(module);
  }
  if (!main_thread) {
    return X_STATUS_UNSUCCESSFUL;
  }
  main_thread_ = main_thread;

  // Optionally load the Guide (hud.xex) as a system app alongside the title.
  // It is an overlay: it needs a title running underneath for graphics, so
  // this happens after the main thread exists.
  if (!cvars::guide_hud_path.empty()) {
    auto* ks = kernel_state_.get();
    std::string hud_path = cvars::guide_hud_path;
    auto xam_mod_for_guide = lle_xam_module_;
    auto hud_boot =
        kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
            ks, 1024 * 1024, 0, [this, ks, hud_path, xam_mod_for_guide]() -> int {
              // Give the title time to bring up graphics before overlaying.
              xe::threading::Sleep(std::chrono::seconds(8));
              XELOGI("Guide: loading {}", hud_path);
              auto hud = ks->LoadUserModule(hud_path, false);
              if (!hud) {
                XELOGE("Guide: failed to load {}", hud_path);
                return 1;
              }
              if (XFAILED(ks->FinishLoadingUserModule(hud, false))) {
                XELOGE("Guide: failed to finish loading");
                return 1;
              }
              auto* ts = kernel::XThread::GetCurrentThread()->thread_state();
              // DllMain's first argument is the module's hmodule, not a
              // kernel object handle. hud keeps it and later hands it to
              // XexGetModuleSection to find its own "hud" resource section;
              // passing the handle made that lookup fail with
              // "no module for hmodule F8000494".
              uint64_t args[] = {hud->hmodule_ptr(), 1 /* PROCESS_ATTACH */,
                                 0};
              XELOGI("Guide: DllMain entry={:08X}", hud->entry_point());
              ks->processor()->Execute(ts, hud->entry_point(), args,
                                       xe::countof(args));
              XELOGI("Guide: DllMain returned");

              // Load hud's XUI skin package. hud asks
              // XamBuildResourceLocator for a locator into this module; with
              // no module the locator comes back empty and no scene loads.
              // hud.xex carries its own XUI skin as a resource section named
              // "hud" (91401000, 167581b in the XEX resource table), which is
              // exactly the container it passes to XamBuildResourceLocator.
              // So the module it wants is itself, not a separate package.
              guide_skin_module_ = hud->hmodule_ptr();
              XELOGI("Guide: hud handle={:08X} hmodule_ptr={:08X}",
                     hud->handle(), hud->hmodule_ptr());
              if (cvars::guide_static_locator) {
                // hud picks the locator builder at 913EB994:
                //   cmpwi cr6,r3,-1 ; bneq -> dynamic (module = [obj+8])
                // [obj+8] is 0, giving "section://@0,...". Nopping the
                // branch forces the static builder, which takes its
                // module from [obj+4]. Patching hud itself covers the
                // xam-driven path too - setting [obj+8] only works when
                // our own bootstrap runs.
                const uint32_t kBAddr = 0x913EB994u;
                const uint32_t kBOrig = 0x409A0020u;
                auto* bw = memory()->TranslateVirtual<uint32_t*>(kBAddr);
                uint32_t bcur = xe::load_and_swap<uint32_t>(bw);
                if (bcur == kBOrig) {
                  void* bp2 = reinterpret_cast<void*>(
                      reinterpret_cast<uintptr_t>(bw) & ~uintptr_t(0xFFF));
                  xe::memory::PageAccess bold =
                      xe::memory::PageAccess::kReadOnly;
                  if (xe::memory::Protect(bp2, 0x1000,
                                          xe::memory::PageAccess::kReadWrite,
                                          &bold)) {
                    xe::store_and_swap<uint32_t>(bw, 0x60000000u);
                    xe::memory::Protect(bp2, 0x1000, bold, nullptr);
                    XELOGI("Guide: patched hud {:08X} {:08X} -> 60000000 "
                           "(force static resource locator)",
                           kBAddr, bcur);
                  }
                } else {
                  XELOGW("Guide: NOT patching hud {:08X}: found {:08X}, "
                         "expected {:08X}",
                         kBAddr, bcur, kBOrig);
                }
              }
              if (!cvars::guide_skin_path.empty()) {
                auto skin = ks->LoadUserModule(cvars::guide_skin_path, false);
                if (skin) {
                  ks->FinishLoadingUserModule(skin, false);
                  guide_skin_module_ = skin->hmodule_ptr();
                  XELOGI("Guide: skin override {} -> hmodule {:08X}",
                         cvars::guide_skin_path, guide_skin_module_);
                }
              }

              uint32_t h = ks->sys_app_handler(0xFF);
              if (!h) {
                // Under LLE xam, hud's XamRegisterSysApp import binds to the
                // real guest xam export, so the handler lands in xam's own
                // table and never reaches Xenia's HLE map. hud registers app
                // 0xFF with a handler baked into its code as base + 0x69C0
                // (98007960: lis r11,0x913e / addi r5,r11,27072, against a
                // load base of 913E0000). Derive it from the module base.
                h = hud->xex_module()->base_address() + 0x69C0;
                XELOGW("Guide: HLE map empty (LLE xam owns the registration); "
                       "using hud base {:08X} + 0x69C0 -> {:08X}",
                       hud->xex_module()->base_address(), h);
              }
              auto* mem = ks->memory();
              // hud copies 0x47C bytes out of the inner struct and writes the
              // result size through the third argument, so that is a pointer,
              // not a size.
              uint32_t inner = mem->SystemHeapAlloc(0x500, 16);
              uint32_t buf = mem->SystemHeapAlloc(0x40, 16);
              uint32_t out_sz = mem->SystemHeapAlloc(0x10, 16);
              XELOGI("Guide: buffers inner={:08X} buf={:08X} out_sz={:08X}",
                     inner, buf, out_sz);

              // Publish these immediately - the XUI registration below takes
              // a while, and a Guide button press during that window would
              // otherwise find no handler recorded.
              guide_handler_ = h;
              guide_hud_base_ = hud->xex_module()->base_address();
              guide_buf_ = buf;
              guide_out_sz_ = out_sz;
              std::memset(mem->TranslateVirtual(inner), 0, 0x500);
              std::memset(mem->TranslateVirtual(buf), 0, 0x40);
              std::memset(mem->TranslateVirtual(out_sz), 0, 0x10);
              auto* iw = mem->TranslateVirtual<xe::be<uint32_t>*>(inner);
              iw[2] = static_cast<uint32_t>(cvars::guide_subcommand);
              auto* bw = mem->TranslateVirtual<xe::be<uint32_t>*>(buf);
              bw[0] = 1;
              bw[1] = inner;
              // 0x80000004 constructs the Guide object and stores it at
              // 91400690. Every other message loads that object and would
              // dereference null if it does not exist yet, so always create
              // first.
              // hud's scenes derive from XUI built-ins ("BaseScene" has parent
              // "XuiScene"), and CBaseScene::Register fails with 0x80300006
              // because the parent is not in xam's class registry. The
              // registrars that populate it have no callers anywhere inside
              // xam - something outside the module drives them on hardware,
              // the same shape as the heap-init routine. Drive them here.
              // xam's core XUI class registrars are unreachable from its own
              // code: no bl, no b, and no pointer table outside .pdata. They
              // build a descriptor with the class name at +84 and the parent
              // at +88 (the hud-side wrappers use +100/+104) and call the
              // register entry at 81956318. Drive them directly. The list is
              // run twice because registration resolves the parent at call
              // time, so classes registered in the first pass unblock the
              // ones that depend on them in the second.
              if (cvars::lle_xam_xui_init) {
                static const uint32_t kXuiCore[] = {
                    0x8194F860u, 0x8194F950u, 0x8194FAC0u, 0x8194FBE8u,
                    0x8194FCD8u, 0x8194FDC8u, 0x8194FEB0u, 0x8194FFA0u,
                    0x81950090u, 0x81950178u, 0x81950268u, 0x81950350u,
                    0x819504C0u, 0x819505B0u, 0x819506A0u, 0x819507D0u,
                    0x819508C0u, 0x819509B0u, 0x81950AA0u, 0x81950B88u,
                    0x81950C78u, 0x81950D68u, 0x81950E58u, 0x81950F48u,
                    0x81951038u, 0x81951128u, 0x81951218u, 0x81951308u,
                    0x819513F8u, 0x81952428u, 0x819524D0u, 0x81952580u,
                    0x81952628u, 0x81953298u, 0x81953338u, 0x819533E8u,
                    0x819536B0u, 0x81970290u};
                int prev_ok = -1;
                for (int pass = 0; pass < 8; ++pass) {
                  int ok = 0, fail = 0;
                  for (uint32_t reg : kXuiCore) {
                    uint64_t rargs[] = {0};
                    uint64_t rr = ks->processor()->Execute(ts, reg, rargs,
                                                           xe::countof(rargs));
                    if (static_cast<uint32_t>(rr) & 0x80000000u) {
                      ++fail;
                    } else {
                      ++ok;
                    }
                  }
                  XELOGI("Guide: XUI core pass {}: {} ok, {} failed", pass, ok,
                         fail);
                  if (ok == prev_ok) {
                    break;  // converged - remaining failures are not ordering
                  }
                  prev_ok = ok;
                }
                for (uint32_t reg : {0x817503E8u, 0x8199BE08u, 0x8176B2C8u}) {
                  uint64_t rargs[] = {0};
                  XELOGI("Guide: XUI registrar {:08X}", reg);
                  uint64_t rr = ks->processor()->Execute(ts, reg, rargs,
                                                         xe::countof(rargs));
                  XELOGI("Guide: registrar {:08X} returned {:08X}", reg,
                         static_cast<uint32_t>(rr));
                }
              }
              // Read xam's XUI class registry directly instead of inferring
              // its state from HRESULTs. 81950D60 takes the critical section
              // at 0x81D6D030 and searches the structure at 0x81D6D508; if
              // that is all zeroes the registry was never initialised, which
              // is a different problem from a class merely being absent.
              {
                auto* mem2 = ks->memory();
                std::string cs, tb;
                for (int i = 0; i < 8; ++i) {
                  cs += fmt::format("{:08X} ", xe::load_and_swap<uint32_t>(
                      mem2->TranslateVirtual(0x81D6D030u + i * 4)));
                  tb += fmt::format("{:08X} ", xe::load_and_swap<uint32_t>(
                      mem2->TranslateVirtual(0x81D6D508u + i * 4)));
                }
                XELOGI("Guide: XUI crit @81D6D030: {}", cs);
                XELOGI("Guide: XUI registry @81D6D508: {}", tb);
              }
              // Prefer xam's own entry point over a hand-built message.
              // Phase 18: the payload layout was guessed, and xam stores
              // the buffer as a typed sub-object and virtual-calls it,
              // which faults. XamShowGuideUI (ordinal 0x304) makes xam
              // construct its own correctly-shaped message.
              if (cvars::lle_show_guide && xam_mod_for_guide) {
                // Nothing inside xam registers its system apps: the
                // static descriptor table at 0x81604368 (ids 0xEF-0xFD)
                // is walked by 8177FE50, reached only from the callerless
                // root 81751428. XamShowGuideUI sends to app 0xFE, which
                // is absent from that table, so drive the root first and
                // see what the table looks like afterwards.
                if (cvars::lle_xam_sysapp_init) {
                  // 81751428 is the outer root, but it spawns a worker and
                  // waits on it, which never completes here. 8177FE50 is the
                  // function that actually walks the descriptor table and
                  // fills in the app entries, so call that directly.
                  uint64_t sa[] = {0};
                  // Walker first (fills 0xEF-0xFD), then the outer root,
                  // which creates XamApp's thread - the only thing that can
                  // register 0xFE. The root blocks, so run it last.
                  XELOGI("Guide: sysapp table walk 8177FE50");
                  ks->processor()->Execute(ts, 0x8177FE50u, sa,
                                           xe::countof(sa));
                  // 81751428 turned out to be a teardown path: it runs an
                  // app whose main is the ExTerminateTitleProcess wrapper.
                  // 81A34E78 is the factory that constructs XamApp itself -
                  // it calls XamApp's constructor (81A4E640) three times and
                  // has no callers anywhere in xam.
                  // XamApp's factory runs the app's message pump, so it
                  // never returns - that is what a system app does. Run it on
                  // its own guest thread and carry on, otherwise the Guide
                  // sequence blocks here forever.
                  auto app_thread = kernel::object_ref<kernel::XHostThread>(
                      new kernel::XHostThread(ks, 1024 * 1024, 0, [ks]() -> int {
                        auto* ats =
                            kernel::XThread::GetCurrentThread()->thread_state();
                                                // 81A34E78 is a constructor: it stores through r3.
                        // Give it a real zeroed buffer instead of address 0.
                        // Size it properly: the factory addresses sub-objects
                        // with addis/addi pairs (addis +0x10000 then a negative
                        // addi), so a naive scan of small stw displacements
                        // reported 76 bytes when the real span is 33956
                        // (0x84A4). Allocate 64 KiB - comfortably past that.
                        const uint32_t kSelfSize = 0x10000;
                        uint32_t self =
                            ks->memory()->SystemHeapAlloc(kSelfSize, 128);
                        std::memset(ks->memory()->TranslateVirtual(self), 0,
                                    kSelfSize);
                        XELOGI("Guide: XamApp factory 81A34E78 this={:08X}",
                               self);
                        uint64_t aa[] = {self};
                        ks->processor()->Execute(ats, 0x81A34E78u, aa,
                                                 xe::countof(aa));
                        XELOGI("Guide: XamApp factory returned");
                        // The factory constructs three XamApp instances at
                        // this+10832, +17880 and +24928 (the three calls to
                        // the ctor 81A4E640). XamApp's entry 81A4E3D0 reads
                        // this+204/+232/+236, so run it on the first instance
                        // - that is the chain that reaches the 0x1015 handler.
                        for (uint32_t inst : {10832u, 17880u, 24928u}) {
                          uint32_t obj = self + inst;
                          XELOGI("Guide: XamApp entry 81A4E3D0 this={:08X} "
                                 "(+{})", obj, inst);
                          uint64_t ea[] = {obj};
                          uint64_t er = ks->processor()->Execute(
                              ats, 0x81A4E3D0u, ea, xe::countof(ea));
                          XELOGI("Guide: XamApp entry returned {:08X}",
                                 static_cast<uint32_t>(er));
                        }

                        // Register app 0xFE by hand. xam locates a system app
                        // at 0x81D4E550 - id*192 (81786078), and the validator
                        // 817863F8 requires BOTH +8 and +16 non-zero, with the
                        // handler at +12. Nothing in xam's own startup runs
                        // here to do this, so point 0xFE straight at xam's real
                        // XamApp message handler 81A5F220 - the dispatcher that
                        // owns the 0x1015 case XamShowGuideUI sends.
                        if (cvars::lle_xam_fake_app_fe) {
                          uint32_t fe = 0x81D4E550u - 0xFEu * 192u;
                          auto* p8 = ks->memory()->TranslateVirtual(fe);
                          uint32_t ctx = self + 10832u;  // first XamApp
                          xe::store_and_swap<uint32_t>(p8 + 8, ctx);
                          xe::store_and_swap<uint32_t>(p8 + 12, 0x81A5F220u);
                          xe::store_and_swap<uint32_t>(p8 + 16, 1u);
                          xe::store_and_swap<uint32_t>(p8 + 24, 0xFEu);
                          XELOGI("Guide: registered app FE @{:08X} "
                                 "handler=81A5F220 ctx={:08X}", fe, ctx);
                          // Same treatment for the apps xam ships in its
                          // static descriptor table at 0x81604368: entries of
                          // {name, appId, handler, flags}, ids 0xEF-0xFD. The
                          // table walker 8177FE50 returns success but leaves
                          // them unregistered, and something polls 0xFC
                          // (XLiveBase) thousands of times a second when it is
                          // missing. Register each entry that has a handler.
                          auto* mem4 = ks->memory();
                          for (uint32_t i = 0; i < 15; ++i) {
                            uint32_t rec = 0x81604368u + i * 16u;
                            uint32_t id = xe::load_and_swap<uint32_t>(
                                mem4->TranslateVirtual(rec + 4));
                            uint32_t handler = xe::load_and_swap<uint32_t>(
                                mem4->TranslateVirtual(rec + 8));
                            if (id < 0xEFu || id > 0xFDu || !handler) {
                              continue;
                            }
                            uint32_t e = 0x81D4E550u - id * 192u;
                            auto* ep = mem4->TranslateVirtual(e);
                            xe::store_and_swap<uint32_t>(ep + 8, ctx);
                            xe::store_and_swap<uint32_t>(ep + 12, handler);
                            xe::store_and_swap<uint32_t>(ep + 16, 1u);
                            xe::store_and_swap<uint32_t>(ep + 24, id);
                            XELOGI("Guide: registered app {:02X} @{:08X} "
                                   "handler={:08X}", id, e, handler);
                          }
                          // 81A5F220 is not merely a dispatcher - it is
                          // XamApp's run function. It initialises (81A63BB8,
                          // 81A63D28, 81A64438) and the 0x1015 comparison sits
                          // inside its message loop, so this is the pump that
                          // would drain what XMsgStartIORequest queues. It
                          // takes this in r3 and a 2060-byte scratch buffer in
                          // r4 (zeroed at entry). Run it on its own thread; it
                          // is a loop and will not return.
                          uint32_t scratch =
                              ks->memory()->SystemHeapAlloc(0x1000, 128);
                          std::memset(ks->memory()->TranslateVirtual(scratch),
                                      0, 0x1000);
                          auto pump = kernel::object_ref<kernel::XHostThread>(
                              new kernel::XHostThread(
                                  ks, 1024 * 1024, 0, [ks, ctx, scratch]() -> int {
                                    auto* pts = kernel::XThread::
                                        GetCurrentThread()->thread_state();
                                    uint64_t pa[] = {ctx, scratch};
                                    XELOGI("Guide: XamApp pump 81A5F220 "
                                           "this={:08X} buf={:08X}", ctx,
                                           scratch);
                                    ks->processor()->Execute(
                                        pts, 0x81A5F220u, pa, xe::countof(pa));
                                    XELOGI("Guide: XamApp pump returned");
                                    return 0;
                                  },
                                  ks->GetSystemProcess()));
                          pump->set_name("XamApp pump");
                          if (XFAILED(pump->Create())) {
                            XELOGE("Guide: failed to create pump thread");
                          }
                          xe::threading::Sleep(std::chrono::seconds(3));
                          // Re-read app 0xFE after the pump has had time to
                          // run. If the pump publishes its wait event (or a
                          // queue) into the app entry, it will appear here -
                          // that is the field XMsgStartIORequest would need in
                          // order to wake it, and the one our hand-written
                          // entry omits.
                          {
                            uint32_t fe2 = 0x81D4E550u - 0xFEu * 192u;
                            auto* q = ks->memory()->TranslateVirtual(fe2);
                            std::string d1, d2;
                            for (int i = 0; i < 12; ++i) {
                              d1 += fmt::format("{:08X} ",
                                  xe::load_and_swap<uint32_t>(q + i * 4));
                            }
                            for (int i = 12; i < 24; ++i) {
                              d2 += fmt::format("{:08X} ",
                                  xe::load_and_swap<uint32_t>(q + i * 4));
                            }
                            XELOGI("Guide: app FE after pump [0..47]:  {}", d1);
                            XELOGI("Guide: app FE after pump [48..95]: {}", d2);
                          }
                        }
                        return 0;
                      }, ks->GetSystemProcess()));
                  app_thread->set_name("XamApp");
                  if (XFAILED(app_thread->Create())) {
                    XELOGE("Guide: failed to create XamApp thread");
                  }
                  // Give it time to construct and register before we look.
                  xe::threading::Sleep(std::chrono::seconds(5));
                }
                // xam locates a system app as 0x81D4E550 - appid*192
                // (81786078), then 817863F8 requires fields +8 and +16
                // to be set. XamShowGuideUI sends to app 0xFE, whose
                // entry is 0x81D426D0. Read it before calling.
                {
                  auto* m3 = ks->memory();
                  for (uint32_t id : {0xFEu, 0xFFu}) {
                    uint32_t e = 0x81D4E550u - id * 192u;
                    std::string f;
                    for (int i = 0; i < 8; ++i) {
                      f += fmt::format("{:08X} ", xe::load_and_swap<uint32_t>(
                          m3->TranslateVirtual(e + i * 4)));
                    }
                    XELOGI("Guide: app {:02X} entry @{:08X}: {}", id, e, f);
                  }
                  for (uint32_t ord : {0x24Bu, 0x24Cu, 0x304u}) {
                    XELOGI("Guide: xam ordinal {:X} -> {:08X}", ord,
                           xam_mod_for_guide->GetProcAddressByOrdinal(ord));
                  }
                  XELOGI("Guide: current-app ptr @81D426C8 = {:08X}",
                         xe::load_and_swap<uint32_t>(
                             m3->TranslateVirtual(0x81D426C8u)));
                }
                uint32_t g = xam_mod_for_guide->GetProcAddressByOrdinal(0x304);
                // Xenia declares the whole app lifecycle API in its xam
                // table - XamAppLoad (0x244), XamAppRequestLoad (0x248),
                // XamAppLoadPass2SysApps (0x254), XamAppRequestLoadEx (0x299)
                // - and implements none of it. That API *is* the hosting
                // mechanism hud needs: something has to load and host system
                // apps, and nothing in Xenia does. Real xam has these, so try
                // driving them directly.
                // XamLoadSysApp(0x251) is the specific "load a system app"
                // entry; it takes (id, arg) and does not store through the
                // incoming r3, so it is safe to drive. hud registers as app
                // 0xFF, so try that and the ids in xam's descriptor table.
                // Guest breakpoint on xam's sys-app loader (81786788). Its
                // first argument is compared against 4, but the sys-app main
                // appears to pass a table address - the static reading could
                // not resolve which. A breakpoint reads the real registers.
                static std::unique_ptr<cpu::Breakpoint> loader_bp;
                if (cvars::lle_xam_trace_loader && !loader_bp) {
                  loader_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x913FE6D4ull,
                      [](cpu::Breakpoint* bp, cpu::ThreadDebugInfo* ti,
                         uint64_t host_pc) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) {
                          XELOGI("LoaderTrace: hit, no thread context");
                          return;
                        }
                        auto* c = th->thread_state()->context();
                        {
                          uint32_t np = static_cast<uint32_t>(c->r[3]);
                          std::string nm;
                          if (np) {
                            auto* mem = th->kernel_state()->memory();
                            for (int i = 0; i < 40; ++i) {
                              uint16_t ch = xe::load_and_swap<uint16_t>(
                                  mem->TranslateVirtual(np + i * 2));
                              if (!ch) break;
                              nm.push_back(static_cast<char>(ch & 0x7F));
                            }
                          }
                          XELOGI("LoaderTrace: FindClass name '{}'", nm);
                        }
                        XELOGI(
                            "LoaderTrace: SCENECREATE 913FE6D4 lr={:08X} r3={:08X} "
                            "r4={:08X} r5={:08X} r6={:08X} r7={:08X} "
                            "r29={:08X} r30={:08X}",
                            static_cast<uint32_t>(c->lr),
                            static_cast<uint32_t>(c->r[3]),
                            static_cast<uint32_t>(c->r[4]),
                            static_cast<uint32_t>(c->r[5]),
                            static_cast<uint32_t>(c->r[6]),
                            static_cast<uint32_t>(c->r[7]),
                            static_cast<uint32_t>(c->r[29]),
                            static_cast<uint32_t>(c->r[30]));
                      });
                  // AddBreakpoint installs it when the processor is running.
                  ks->processor()->AddBreakpoint(loader_bp.get());
                  XELOGI(
                      "LoaderTrace: bp exec_state={} patched={} host={:X}",
                      static_cast<int>(ks->processor()->execution_state()),
                      loader_bp->backend_data().size(),
                      loader_bp->backend_data().empty()
                          ? 0ull
                          : loader_bp->backend_data()[0].first);
                  // Control: the same mechanism on XamShowGuideUI's worker
                  // (81787430), which is definitely executed. If the control
                  // never fires either, breakpoints are not working here and
                  // the loader result means nothing.
                  static std::unique_ptr<cpu::Breakpoint> ctl_bp;
                  ctl_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x81787430ull,
                      [](cpu::Breakpoint*, cpu::ThreadDebugInfo*, uint64_t) {
                        XELOGI("LoaderTrace: CONTROL hit at 81787430");
                      });
                  // Disabled: a breakpoint hit does not resume, so the entry
                  // control kills the thread before the call site is reached.
                  // ks->processor()->AddBreakpoint(ctl_bp.get());
                  // XamShowGuideUI's unconditional callee: ghidra 8178D730,
                  // runtime 8178D730 - 0x7200 = 81786530.
                  static std::unique_ptr<cpu::Breakpoint> work_bp;
                  work_bp = std::make_unique<cpu::Breakpoint>(
                      ks->processor(), cpu::Breakpoint::AddressType::kGuest,
                      0x81787470ull,
                      [](cpu::Breakpoint*, cpu::ThreadDebugInfo*, uint64_t) {
                        auto* th = kernel::XThread::GetCurrentThread();
                        if (!th) {
                          XELOGI("LoaderTrace: WORKER hit, no thread");
                          return;
                        }
                        auto* c = th->thread_state()->context();
                        XELOGI(
                            "LoaderTrace: CALLSITE 81787470 r3={:08X} r4={:08X} "
                            "r5={:08X}",
                            static_cast<uint32_t>(c->r[3]),
                            static_cast<uint32_t>(c->r[4]),
                            static_cast<uint32_t>(c->r[5]));
                      });
                  // Disabled for this run: one hit per session (see below).
                  // ks->processor()->AddBreakpoint(work_bp.get());
                  XELOGI("LoaderTrace: 81787470 patched={}",
                         work_bp->backend_data().size());
                  XELOGI("LoaderTrace: 81787430 patched={} host={:X}",
                         ctl_bp->backend_data().size(),
                         ctl_bp->backend_data().empty()
                             ? 0ull
                             : ctl_bp->backend_data()[0].first);
                }
                if (cvars::lle_xam_app_host) {
                  uint32_t load_fn =
                      xam_mod_for_guide->GetProcAddressByOrdinal(0x251);
                  XELOGI("Guide: XamLoadSysApp -> {:08X}", load_fn);
                  if (load_fn) {
                    for (uint32_t id : {0xFFu, 0xFEu, 0xF7u}) {
                      uint64_t la[] = {id, 0};
                      uint64_t lr = ks->processor()->Execute(
                          ts, load_fn, la, xe::countof(la));
                      XELOGI("Guide: XamLoadSysApp({:02X}) -> {:08X}", id,
                             static_cast<uint32_t>(lr));
                    }
                  }
                  uint32_t msg_fn =
                      xam_mod_for_guide->GetProcAddressByOrdinal(0x247);
                  XELOGI("Guide: XamSendMessageToLoadedApps -> {:08X}", msg_fn);
                }
                XELOGI("Guide: XamShowGuideUI (ord 0x304) -> {:08X}", g);
                if (g) {
                  uint64_t ga[] = {0};
                  uint64_t gr = ks->processor()->Execute(ts, g, ga,
                                                         xe::countof(ga));
                  XELOGI("Guide: XamShowGuideUI returned {:08X}",
                         static_cast<uint32_t>(gr));
                } else {
                  XELOGE("Guide: could not resolve ordinal 0x304");
                }
                return 0;
              }
              // Remember these so the Guide button can re-dispatch later.
              guide_handler_ = h;
              guide_buf_ = buf;
              guide_out_sz_ = out_sz;
              XELOGI("Guide: create msg=80000004 subcmd={} -> {:08X}",
                     int32_t(cvars::guide_subcommand), h);
              uint64_t cargs[] = {0x80000004ull, buf, out_sz};
              uint64_t cres = ks->processor()->Execute(ts, h, cargs,
                                                       xe::countof(cargs));
              XELOGI("Guide: create returned {:08X}",
                     static_cast<uint32_t>(cres));
              // hud's handler stores the Guide object at 0x91400690 and every
              // non-create message loads it from there, so a null here means
              // the later dispatch would fault rather than draw.
              XELOGI("Guide: object @91400690 = {:08X}",
                     xe::load_and_swap<uint32_t>(
                         mem->TranslateVirtual(0x91400690u)));
              // hud is a system app: on hardware the system creates its
              // thread and runs its render loop. Nothing here does, which is
              // why every message returns success and nothing draws. hud's
              // own XUI entry points, located by scanning .text in flat
              // address space for calls to the import thunks:
              //   base + 0xA898  calls XuiInit + XuiRenderCreateDC
              //   base + 0xAB28  calls XuiRenderBegin/End/Present
              // Both take `this` in r3 and only read from it. Drive them
              // against the Guide object the create message just produced.
              if (cvars::lle_guide_draw) {
                uint32_t hb = hud->xex_module()->base_address();
                uint32_t obj = xe::load_and_swap<uint32_t>(
                    mem->TranslateVirtual(0x91400690u));
                if (obj) {
                  uint64_t ia[] = {obj};
                  if (cvars::guide_force_obj14) {
                    xe::store_and_swap<uint32_t>(
                        mem->TranslateVirtual(obj + 0x14u),
                        cvars::guide_force_obj14);
                    XELOGI("Guide: forced [obj+14] = {:08X} so init takes the "
                           "XuiRenderCreateDC branch",
                           uint32_t(cvars::guide_force_obj14));
                  }
                  XELOGI("Guide: hud XUI init {:08X} this={:08X}", hb + 0xA898u,
                         obj);
                  uint64_t ir = ks->processor()->Execute(ts, hb + 0xA898u, ia,
                                                          xe::countof(ia));
                  XELOGI("Guide: hud XUI init returned {:08X}",
                         static_cast<uint32_t>(ir));
                  // [obj+12] is null on this path, so the device context
                  // XuiRenderCreateDC produced is stored elsewhere in the
                  // object. Dump the head of it to find the pointer: a DC
                  // lives in the 0x40000000 heap like the object itself.
                  {
                    std::string ow;
                    for (uint32_t w = 0; w < 32; ++w) {
                      uint32_t v = xe::load_and_swap<uint32_t>(
                          mem->TranslateVirtual(obj + w * 4));
                      ow += fmt::format("{:02X}:{:08X} ", w * 4, v);
                    }
                    XELOGI("Guide: obj {:08X} head {}", obj, ow);
                  }
                  // The device context hangs off [this+12]; [dc+0x134] is
                  // the null-render flag that decides whether XuiRenderBegin
                  // dispatches vtable[20] and the emitter ever builds a
                  // DRAW_INDX. Report it alongside the GPU draw count so a
                  // cleared flag that still produces no draws is
                  // distinguishable from a flag that never cleared.
                  auto* gsd = ks->emulator()->graphics_system();
                  auto* cpd = gsd ? gsd->command_processor() : nullptr;
                  uint32_t prev_draws =
                      cpd ? cpd->guide_draw_count_ : 0u;
                  for (int frame = 0; frame < 6000; ++frame) {
                    uint64_t da[] = {obj};
                    ks->processor()->Execute(ts, hb + 0xAB28u, da,
                                             xe::countof(da));
                    if (frame < 3 || frame % 500 == 0) {
                      uint32_t dcp = xe::load_and_swap<uint32_t>(
                          mem->TranslateVirtual(obj + 12));
                      uint32_t flag =
                          dcp ? xe::load_and_swap<uint32_t>(
                                    mem->TranslateVirtual(dcp + 0x134u))
                              : 0xFFFFFFFFu;
                      uint32_t now = cpd ? cpd->guide_draw_count_ : 0u;
                      XELOGI("GuideFrame {}: dc={:08X} [134]={:08X} "
                             "gpu_draws +{} (total {})",
                             frame, dcp, flag, now - prev_draws, now);
                      prev_draws = now;
                    }
                    xe::threading::Sleep(std::chrono::milliseconds(16));
                  }
                  XELOGI("Guide: hud draw loop finished");
                  if (cvars::guide_coverage_fn) {
                    auto* cf = ks->processor()->LookupFunction(
                        cvars::guide_coverage_fn);
                    auto* cgf =
                        cf ? dynamic_cast<cpu::GuestFunction*>(cf) : nullptr;
                    if (cgf && cgf->trace_data().is_valid()) {
                      auto& td = cgf->trace_data();
                      auto* cnt = reinterpret_cast<uint64_t*>(
                          td.instruction_execute_counts());
                      uint32_t n = td.instruction_count(), ex = 0, last = 0;
                      for (uint32_t i = 0; i < n; ++i) {
                        if (cnt[i]) {
                          ++ex;
                          last = td.start_address() + i * 4;
                        }
                      }
                      XELOGI("Coverage {:08X}: {}/{} executed, furthest "
                             "{:08X} (+0x{:X})",
                             uint32_t(cvars::guide_coverage_fn), ex, n, last,
                             last - td.start_address());
                    } else {
                      XELOGI("Coverage {:08X}: no counts - lookup={}, "
                             "guest_fn={}, trace_valid={}",
                             uint32_t(cvars::guide_coverage_fn),
                             cf ? "ok" : "null", cgf ? "ok" : "cast-failed",
                             (cgf && cgf->trace_data().is_valid())
                                 ? "yes" : "no");
                    }
                  }
                }
              }

              const uint32_t msg = static_cast<uint32_t>(cvars::guide_message);
              if (msg != 0x80000004u) {
                XELOGI("Guide: dispatch msg={:08X} -> {:08X}", msg, h);
                uint64_t gargs[] = {msg, buf, out_sz};
                uint64_t r = ks->processor()->Execute(ts, h, gargs,
                                                      xe::countof(gargs));
                XELOGI("Guide: handler returned {:08X}",
                       static_cast<uint32_t>(r));
              }
              return 0;
            }));
    hud_boot->set_name("Guide Loader");
    if (XFAILED(hud_boot->Create())) {
      XELOGE("Guide: failed to create loader thread");
    }
  }
  on_launch(title_id_.value(), title_name_);

  // Plugins must be loaded after calling LaunchModule() and
  // FinishLoadingUserModule() which will apply TUs and patching to the main
  // xex.
  if (cvars::allow_plugins) {
    if (plugin_loader_->IsAnyPluginForTitleAvailable(title_id_.value(),
                                                     module->hash().value())) {
      plugin_loader_->LoadTitlePlugins(title_id_.value(),
                                       module->hash().value());
    }
  }

  // Resume the main thread now.
  // If the debugger has requested a suspend this will just decrement the
  // suspend count without resuming it until the debugger wants.
  main_thread_->Resume();

  return X_STATUS_SUCCESS;
}

}  // namespace xe
