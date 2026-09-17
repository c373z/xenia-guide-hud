/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_GAME_LIBRARY_H_
#define XENIA_APP_GAME_LIBRARY_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xe {
namespace app {

// Phase 1099z165: what the host can learn about a disc image without booting
// it - the same things the console reads out of the disc's default.xex.
struct DiscTitleInfo {
  bool valid = false;
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  uint32_t version_value = 0;
  uint32_t base_version_value = 0;
  uint8_t disc_number = 1;
  uint8_t disc_count = 1;
  // The XDBF title name, in UTF-8. Empty if the XDBF could not be read.
  std::string title_name;
  // The XDBF title icon, raw PNG bytes. Empty if there is none.
  std::vector<uint8_t> icon_png;
  std::string error;
};

// Mounts the disc image, reads its default.xex headers for the title id and
// decodes the basefile far enough to reach the XDBF resource with the title
// name and icon. Nothing guest-side runs; this is host file I/O only, so it is
// safe to call from a worker thread while a title is running. Slow enough
// (tens to hundreds of ms per disc) that callers should cache the result.
DiscTitleInfo ReadDiscTitleInfo(const std::filesystem::path& disc_path);

// One disc being installed into the emulated console's hard drive.
struct GameLibraryEntry {
  enum class State {
    kPending,
    kReadingDisc,
    kWritingPackage,
    kInstalled,
    kAlreadyInstalled,
    kFailed,
    kCancelled,
  };

  explicit GameLibraryEntry(std::filesystem::path p) : path(std::move(p)) {}

  std::filesystem::path path;
  // Only read by the UI after state leaves kPending/kReadingDisc.
  DiscTitleInfo info;
  std::atomic<State> state{State::kPending};
  // 0..1, the stage this entry has reached. The payload is not copied (see
  // GameLibraryInstaller), so the work is the disc read, not a byte count.
  std::atomic<float> progress{0.0f};
  std::string message;
};

// Phase 1099z165: HOST-SIDE. Installs disc images into the emulated hard drive
// the way the dashboard's own "Install to Hard Drive" does - an Installed Game
// (content type 0x00004000) content package under
//   Content\0000000000000000\<TitleID>\00004000\<32 hex content id>
// - but without copying the 6-8 GB payload. The package header is real and is
// built from the disc's own XEX and XDBF (title id, media id, name, icon), so
// the dashboard enumerates and displays it; the ".data" folder that would hold
// the SVOD Data#### fragments instead holds a one-line link file naming the
// host disc image, which the kernel's SvodCreateDevice follows when the title
// is launched. See research\WORKAROUNDS.md.
class GameLibraryInstaller {
 public:
  GameLibraryInstaller() = default;
  ~GameLibraryInstaller();

  // hdd_partition_root is the emulated drive's Partition1 folder (the same
  // path guide_hdd_path names). Discs already installed are skipped.
  // on_written runs on the install thread after each package is written and
  // once when the run ends, so the caller can make the new files visible to
  // the running guest (the emulated drive's file tree is read at mount).
  void Start(const std::filesystem::path& hdd_partition_root,
             std::vector<std::filesystem::path> disc_paths,
             std::function<void()> on_written = nullptr);

  void Cancel() { cancel_.store(true); }
  bool cancelled() const { return cancel_.load(); }
  bool running() const { return running_.load(); }

  // Stable for the lifetime of the installer: Start() builds the vector once.
  const std::vector<std::unique_ptr<GameLibraryEntry>>& entries() const {
    return entries_;
  }
  size_t installed_count() const { return installed_count_.load(); }

 private:
  void Run(std::filesystem::path hdd_partition_root);
  // Returns false and fills entry->message on failure.
  bool InstallEntry(const std::filesystem::path& hdd_partition_root,
                    GameLibraryEntry* entry);

  std::vector<std::unique_ptr<GameLibraryEntry>> entries_;
  std::function<void()> on_written_;
  std::thread thread_;
  std::atomic<bool> cancel_{false};
  std::atomic<bool> running_{false};
  std::atomic<size_t> installed_count_{0};
};

// Lists the disc images directly inside a folder (non-recursive; a game
// library folder is flat in practice and recursing into an extracted game's
// folders would be slow).
std::vector<std::filesystem::path> FindDiscImagesInFolder(
    const std::filesystem::path& folder);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_GAME_LIBRARY_H_
