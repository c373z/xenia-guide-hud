/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/game_library.h"

#include "xenia/app/game_library_art.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <span>

#include "third_party/crypto/TinySHA1.hpp"
#include "third_party/fmt/include/fmt/format.h"

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/string.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/util/xex2_info.h"
#include "xenia/kernel/xam/xdbf/spa_info.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/host_disc_link.h"
#include "xenia/xbox.h"

namespace xe {
namespace app {

// The all-zero XUID folder: an installed game belongs to the console, not to a
// profile. This is where the dashboard's own install puts it.
static constexpr char kMachineXuidFolder[] = "0000000000000000";
// XContentType::kInstalledGame. (0x00007000 is kXbox360Title, a different
// thing - the dashboard does not look for installed games there.)
static constexpr uint32_t kInstalledGameContentType = 0x00004000;
// What the dashboard's install writes: a 0xAD0E header_size in a file padded
// to 0xB000, everything past XContentContainerHeader zero (verified against
// the Fable III package this project's disc install produced).
static constexpr size_t kPackageHeaderFileSize = 0xB000;
static constexpr uint32_t kPackageHeaderSizeField = 0xAD0E;

namespace {

// Pulls the XDBF blob for this title out of an already decoded basefile.
bool FindXdbfResource(const xex2_header* header,
                      const std::vector<uint8_t>& image, uint32_t base_address,
                      uint32_t title_id, const uint8_t** out_data,
                      uint32_t* out_size) {
  xex2_opt_resource_info* resource_header = nullptr;
  if (!cpu::XexModule::GetOptHeader(header, XEX_HEADER_RESOURCE_INFO,
                                    &resource_header)) {
    return false;
  }
  const std::string wanted = fmt::format("{:08X}", title_id);
  const uint32_t count =
      (uint32_t(resource_header->size) - 4) / uint32_t(sizeof(xex2_resource));
  for (uint32_t i = 0; i < count; ++i) {
    const auto& resource = resource_header->resources[i];
    if (std::memcmp(resource.name, wanted.data(), 8) != 0) {
      continue;
    }
    const uint32_t address = resource.address;
    const uint32_t size = resource.size;
    if (address < base_address || !size) {
      return false;
    }
    const uint64_t offset = uint64_t(address) - base_address;
    if (offset + size > image.size()) {
      return false;
    }
    *out_data = image.data() + offset;
    *out_size = size;
    return true;
  }
  return false;
}

// 16 bytes, stable for a given disc, ending in the media id the way a real
// install's content id does.
void MakeContentId(const DiscTitleInfo& info,
                   const std::filesystem::path& disc_path, uint8_t out[16]) {
  const std::string seed =
      fmt::format("{:08X}:{:08X}:{}", info.title_id, info.media_id,
                  xe::path_to_utf8(disc_path.filename()));
  sha1::SHA1 sha;
  sha.processBytes(seed.data(), seed.size());
  uint8_t digest[20];
  sha.finalize(digest);
  std::memcpy(out, digest, 12);
  out[12] = uint8_t(info.media_id >> 24);
  out[13] = uint8_t(info.media_id >> 16);
  out[14] = uint8_t(info.media_id >> 8);
  out[15] = uint8_t(info.media_id);
}

std::string ContentIdToName(const uint8_t id[16]) {
  std::string name;
  name.reserve(32);
  for (int i = 0; i < 16; ++i) {
    name += fmt::format("{:02X}", id[i]);
  }
  return name;
}

// True when content_dir (a title's installed-game folder) holds a package
// header whose execution info names this disc number.
bool HasInstalledPackageForDisc(const std::filesystem::path& content_dir,
                                uint8_t disc_number) {
  std::error_code ec;
  // 0x971A bytes: too big for the stack.
  std::vector<char> buffer(sizeof(vfs::XContentContainerHeader));
  const auto& header =
      *reinterpret_cast<const vfs::XContentContainerHeader*>(buffer.data());
  for (const auto& item :
       std::filesystem::directory_iterator(content_dir, ec)) {
    if (!item.is_regular_file(ec)) {
      continue;
    }
    std::ifstream file(item.path(), std::ios::binary);
    if (!file.read(buffer.data(), buffer.size())) {
      continue;
    }
    if (header.content_header.magic != vfs::XContentPackageType::kCon &&
        header.content_header.magic != vfs::XContentPackageType::kLive &&
        header.content_header.magic != vfs::XContentPackageType::kPirs) {
      continue;
    }
    if (header.content_metadata.execution_info.disc_number == disc_number) {
      return true;
    }
  }
  return false;
}

// 2026-09-16, HOST-SIDE: <Partition1>\BoxArt\<titleid>\ holds the cover
// art fetched once per title (largeboxart.jpg 219x300 and smallboxart.jpg
// 85x117, both times the draw resolution scale at fetch time), or a
// "no-cover-art.txt" note when the store has none, so the store is never
// asked again for that title. See game_library_art.h.
std::filesystem::path BoxArtDir(const std::filesystem::path& hdd_partition_root,
                                uint32_t title_id) {
  return hdd_partition_root / "BoxArt" / fmt::format("{:08x}", title_id);
}

void WriteFile(const std::filesystem::path& path,
               const std::vector<uint8_t>& data) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(data.data()),
             std::streamsize(data.size()));
}

// Only for titles with nothing in BoxArt yet: a new install, or a game
// installed before cover art existed. Returns true when it wrote files.
bool FetchCoverArtOnce(const std::filesystem::path& hdd_partition_root,
                       const DiscTitleInfo& info) {
  if (!cvars::guide_game_library_cover_art || !info.valid) {
    return false;
  }
  const std::filesystem::path dir =
      BoxArtDir(hdd_partition_root, info.title_id);
  std::error_code ec;
  if (std::filesystem::exists(dir, ec)) {
    return false;
  }
  CoverArt art = FetchCoverArt(info.title_id, info.title_name);
  if (!art.found && !art.transient) {
    // A definite "the store has no cover for this title": remember it.
    // Network failures are not remembered, so the next start tries again.
    std::filesystem::create_directories(dir, ec);
    std::ofstream note(dir / "no-cover-art.txt", std::ios::trunc);
    note << info.title_name << ": " << art.error << '\n';
    XELOGI("GameLibrary: no cover art for {:08X} '{}': {}", info.title_id,
           info.title_name, art.error);
    return true;
  }
  if (!art.found) {
    XELOGW("GameLibrary: cover art lookup for {:08X} failed: {}",
           info.title_id, art.error);
    return false;
  }
  std::filesystem::create_directories(dir, ec);
  WriteFile(dir / "largeboxart.jpg", art.large_jpeg);
  WriteFile(dir / "smallboxart.jpg", art.small_jpeg);
  XELOGI("GameLibrary: cover art for {:08X} '{}' from store product {}",
         info.title_id, info.title_name, art.product_id);
  return true;
}

}  // namespace

DiscTitleInfo ReadDiscTitleInfo(const std::filesystem::path& disc_path) {
  DiscTitleInfo info;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(disc_path, ec)) {
    info.error = "not a file";
    return info;
  }

  vfs::DiscImageDevice device("", disc_path);
  if (!device.Initialize()) {
    info.error = "not an Xbox 360 disc image";
    return info;
  }
  // Entry::GetChild matches case-insensitively, so "Default.xex" resolves too.
  vfs::Entry* entry = device.ResolvePath("default.xex");
  if (!entry) {
    info.error = "no default.xex on the disc";
    return info;
  }
  auto map = entry->OpenMapped(MappedMemory::Mode::kRead, 0, 0);
  if (!map || map->size() < sizeof(xex2_header)) {
    info.error = "could not read default.xex";
    return info;
  }

  const auto* header = reinterpret_cast<const xex2_header*>(map->data());
  xex2_opt_execution_info* execution_info = nullptr;
  if (!cpu::XexModule::GetOptHeader(header, XEX_HEADER_EXECUTION_INFO,
                                    &execution_info)) {
    info.error = "default.xex has no execution info";
    return info;
  }
  info.title_id = execution_info->title_id;
  info.media_id = execution_info->media_id;
  info.version_value = execution_info->version_value;
  info.base_version_value = execution_info->base_version_value;
  info.disc_number = execution_info->disc_number;
  info.disc_count = execution_info->disc_count;
  info.valid = true;

  // The name and the icon live in the XDBF resource inside the basefile, so
  // that has to be decrypted and decompressed first. A disc whose key we do
  // not have still gets its title id, which is enough to install it.
  std::vector<uint8_t> image;
  uint32_t base_address = 0;
  if (!cpu::XexModule::ReadImageToHostBuffer(map->data(), map->size(), &image,
                                             &base_address)) {
    info.error = "could not decode default.xex";
    return info;
  }
  const uint8_t* xdbf_data = nullptr;
  uint32_t xdbf_size = 0;
  if (!FindXdbfResource(header, image, base_address, info.title_id, &xdbf_data,
                        &xdbf_size)) {
    info.error = "no XDBF resource";
    return info;
  }
  try {
    kernel::xam::SpaInfo spa(std::span<uint8_t>(
        const_cast<uint8_t*>(xdbf_data), size_t(xdbf_size)));
    spa.Load();
    info.title_name =
        spa.title_name(spa.GetExistingLanguage(XLanguage::kEnglish));
    const auto icon = spa.title_icon();
    info.icon_png.assign(icon.begin(), icon.end());
  } catch (...) {
    info.error = "could not parse the XDBF";
  }
  return info;
}

std::vector<std::filesystem::path> FindDiscImagesInFolder(
    const std::filesystem::path& folder) {
  std::vector<std::filesystem::path> paths;
  std::error_code ec;
  if (!std::filesystem::is_directory(folder, ec)) {
    return paths;
  }
  for (const auto& item : std::filesystem::directory_iterator(folder, ec)) {
    if (!item.is_regular_file(ec)) {
      continue;
    }
    std::string extension = xe::path_to_utf8(item.path().extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (extension == ".iso") {
      paths.push_back(item.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

GameLibraryInstaller::~GameLibraryInstaller() {
  cancel_.store(true);
  if (thread_.joinable()) {
    thread_.join();
  }
}

void GameLibraryInstaller::Start(
    const std::filesystem::path& hdd_partition_root,
    std::vector<std::filesystem::path> disc_paths,
    std::function<void()> on_written) {
  if (running_.load()) {
    return;
  }
  on_written_ = std::move(on_written);
  entries_.clear();
  for (auto& path : disc_paths) {
    entries_.push_back(std::make_unique<GameLibraryEntry>(std::move(path)));
  }
  cancel_.store(false);
  installed_count_.store(0);
  running_.store(true);
  thread_ = std::thread(&GameLibraryInstaller::Run, this, hdd_partition_root);
}

void GameLibraryInstaller::Run(std::filesystem::path hdd_partition_root) {
  xe::threading::set_name("Game Library Install");
  bool wrote_anything = false;
  for (auto& entry : entries_) {
    if (cancel_.load()) {
      entry->state.store(GameLibraryEntry::State::kCancelled);
      continue;
    }
    const bool installed = InstallEntry(hdd_partition_root, entry.get());
    // New cover art must reach the mounted drive's file tree too, even when
    // the game itself was already installed.
    if ((installed || entry->state.load() ==
                          GameLibraryEntry::State::kAlreadyInstalled) &&
        FetchCoverArtOnce(hdd_partition_root, entry->info)) {
      wrote_anything = true;
      if (!installed && on_written_) {
        on_written_();
      }
    }
    if (!installed) {
      if (entry->state.load() != GameLibraryEntry::State::kAlreadyInstalled &&
          entry->state.load() != GameLibraryEntry::State::kCancelled) {
        entry->state.store(GameLibraryEntry::State::kFailed);
        XELOGE("GameLibrary: {} failed: {}", xe::path_to_utf8(entry->path),
               entry->message);
      } else {
        XELOGI("GameLibrary: {} skipped: {}", xe::path_to_utf8(entry->path),
               entry->message);
      }
      continue;
    }
    entry->progress.store(1.0f);
    entry->state.store(GameLibraryEntry::State::kInstalled);
    installed_count_.fetch_add(1);
    wrote_anything = true;
    if (on_written_) {
      on_written_();
    }
  }
  // Once more at the end: a package written while the drive was still being
  // mounted may have been missed by both the mount and the refresh above.
  if (on_written_ && wrote_anything) {
    on_written_();
  }
  running_.store(false);
}

bool GameLibraryInstaller::InstallEntry(
    const std::filesystem::path& hdd_partition_root, GameLibraryEntry* entry) {
  entry->state.store(GameLibraryEntry::State::kReadingDisc);
  entry->progress.store(0.05f);

  entry->info = ReadDiscTitleInfo(entry->path);
  if (!entry->info.valid) {
    entry->message = entry->info.error;
    return false;
  }
  if (cancel_.load()) {
    entry->state.store(GameLibraryEntry::State::kCancelled);
    return false;
  }
  entry->progress.store(0.7f);
  entry->state.store(GameLibraryEntry::State::kWritingPackage);

  std::error_code ec;
  const uint64_t disc_size = std::filesystem::file_size(entry->path, ec);
  if (ec) {
    entry->message = "could not size the disc image";
    return false;
  }

  const std::filesystem::path content_dir =
      hdd_partition_root / "Content" / kMachineXuidFolder /
      fmt::format("{:08X}", entry->info.title_id) /
      fmt::format("{:08X}", kInstalledGameContentType);
  std::filesystem::create_directories(content_dir, ec);
  if (ec) {
    entry->message = "could not create " + xe::path_to_utf8(content_dir);
    return false;
  }

  uint8_t content_id[16];
  MakeContentId(entry->info, entry->path, content_id);
  const std::string package_name = ContentIdToName(content_id);
  const std::filesystem::path header_path = content_dir / package_name;
  const std::filesystem::path data_dir =
      content_dir / (package_name + ".data");

  // Any installed package of this disc - the dashboard's own install, or one
  // this library wrote for the same disc under another file name - means it is
  // already in My Games; a second package only shows up as a duplicate tile.
  // The folder is per title, so match the disc number to keep the other discs
  // of a multi-disc game installable.
  if (std::filesystem::exists(header_path, ec) ||
      HasInstalledPackageForDisc(content_dir, entry->info.disc_number)) {
    entry->state.store(GameLibraryEntry::State::kAlreadyInstalled);
    entry->progress.store(1.0f);
    entry->message = "already installed";
    return false;
  }

  // The package header. Everything in it is real and comes off the disc; the
  // payload description (block and fragment counts) describes the disc image
  // that the link below points at, since no fragments are written.
  std::vector<uint8_t> buffer(kPackageHeaderFileSize, 0);
  auto* package = reinterpret_cast<vfs::XContentContainerHeader*>(buffer.data());
  auto& header = package->content_header;
  auto& metadata = package->content_metadata;

  header.magic = vfs::XContentPackageType::kCon;
  header.header_size = kPackageHeaderSizeField;
  // content_id is filled in at the end: it is a hash of everything after the
  // XContentHeader, and the dashboard checks it (measured - see below).

  // The console certificate and the console licence. A package the dashboard
  // installed itself carries the certificate XeKeysConsolePrivateKeySign
  // builds and one licence held by this console; both are reproduced here
  // byte for byte (checked against the header this project's own disc install
  // wrote). The RSA signature that follows the certificate stays zero - the
  // host accepts any console signature (kernel_accept_console_signatures).
  static constexpr uint8_t kConsoleId[5] = {0x93, 0x01, 0x64, 0xE6, 0x07};
  std::memcpy(header.signature + 0x02, kConsoleId, sizeof(kConsoleId));
  header.signature[0x1B] = 0x02;  // console_type = Retail, big endian
  const uint8_t kManufactureDate[8] = {2, 0, 0, 5, 1, 1, 2, 2};
  std::memcpy(header.signature + 0x1C, kManufactureDate,
              sizeof(kManufactureDate));
  // licensee_id 0xF00000 followed by the console id: "licensed to this
  // console", which is what a disc install gets.
  uint64_t licensee_id = 0xF00000ull;
  for (uint8_t byte : kConsoleId) {
    licensee_id = (licensee_id << 8) | byte;
  }
  header.licenses[0].licensee_id = licensee_id;
  header.licenses[0].license_bits = 0;
  header.licenses[0].license_flags = 0;

  metadata.content_type = XContentType(kInstalledGameContentType);
  metadata.metadata_version = 2;
  // The console that installed this content. Every other package the emulator
  // writes carries it (and the ones off the user's real console carry that
  // console's id); only the installed games left it zero, which is the one
  // field that separated them from content that works. A package claiming a
  // console licence while saying it belongs to console 00000000 is the
  // suspect for "You can play this game only on the console and storage
  // device it was originally installed on" - the licence comparison itself
  // passes (xam 8167F7DC checks the licensee's low 40 bits against
  // XeKeysGetConsoleID, and both are 930164E607).
  std::memcpy(metadata.console_id, kConsoleId, sizeof(kConsoleId));
  // Zero, as in every package the dashboard's own install writes. Measured
  // (runs hlib_a/b, 2026-09-16): with content_size set to the disc size xam's
  // content scan indexes the package under its file name instead of its
  // display name, and My Games leaves it out; zeroing this one field is enough.
  metadata.content_size = 0;
  metadata.execution_info.media_id = entry->info.media_id;
  metadata.execution_info.version_value = entry->info.version_value;
  metadata.execution_info.base_version_value = entry->info.base_version_value;
  metadata.execution_info.title_id = entry->info.title_id;
  metadata.execution_info.disc_number = entry->info.disc_number;
  metadata.execution_info.disc_count = entry->info.disc_count;

  auto& svod = metadata.volume_descriptor.svod;
  svod.descriptor_length = 0x24;
  svod.block_cache_element_count = 5;
  svod.worker_thread_processor = 5;
  svod.worker_thread_priority = 0x11;
  svod.features.bits.enhanced_gdf_layout = 1;
  vfs::store_uint24_le(svod.num_data_blocks_raw,
                       uint32_t(std::min<uint64_t>(disc_size / 0x800, 0xFFFFFF)));
  vfs::store_uint24_le(svod.start_data_block_raw, 0);

  // A real install splits the payload into 0xA290000-byte fragments. This one
  // has a single fragment, hard-linked to the disc image below, so the
  // Data0000 that xam's content scan probes for is there and is the size the
  // header claims.
  metadata.data_file_count = 1;
  metadata.data_file_size = disc_size;
  metadata.volume_type = vfs::XContentVolumeType::kSvod;

  const std::string display_name = entry->info.title_name.empty()
                                       ? xe::path_to_utf8(entry->path.stem())
                                       : entry->info.title_name;
  const std::u16string display_name16 = xe::to_utf16(display_name);
  for (uint32_t language = uint32_t(XLanguage::kEnglish);
       language < uint32_t(XLanguage::kMaxBaseLanguages); ++language) {
    metadata.set_display_name(XLanguage(language), display_name16);
  }
  metadata.set_title_name(display_name16);

  if (!entry->info.icon_png.empty() &&
      entry->info.icon_png.size() <= vfs::XContentMetadata::kThumbLengthV2) {
    const uint32_t icon_size = uint32_t(entry->info.icon_png.size());
    std::memcpy(metadata.thumbnail, entry->info.icon_png.data(), icon_size);
    std::memcpy(metadata.title_thumbnail, entry->info.icon_png.data(),
                icon_size);
    metadata.thumbnail_size = icon_size;
    metadata.title_thumbnail_size = icon_size;
  }

  // The content id is the SHA-1 of the package file from the end of the
  // XContentHeader (0x344) to the end of the file, so it covers the whole
  // metadata block and its padding. Measured, not guessed: it reproduces the
  // content id of the package this project's own disc install wrote, and a
  // package whose content id is anything else - even a full-length random one
  // - is enumerated by xam and then left out of My Games (phase 1099z165,
  // runs lib1099z165t/u/v).
  {
    sha1::SHA1 sha;
    sha.processBytes(buffer.data() + sizeof(vfs::XContentHeader),
                     buffer.size() - sizeof(vfs::XContentHeader));
    uint8_t digest[20];
    sha.finalize(digest);
    std::memcpy(header.content_id, digest, sizeof(header.content_id));
  }

  {
    std::ofstream file(header_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      entry->message = "could not write " + xe::path_to_utf8(header_path);
      return false;
    }
    file.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    if (!file.good()) {
      entry->message = "could not write the package header";
      return false;
    }
  }

  std::filesystem::create_directories(data_dir, ec);
  if (ec && !std::filesystem::is_directory(data_dir)) {
    entry->message = "could not create " + xe::path_to_utf8(data_dir) + ": " +
                     ec.message();
    // Do not leave a header with no payload behind: the dashboard would list
    // a game that cannot be mounted.
    std::filesystem::remove(header_path, ec);
    return false;
  }
  {
    std::ofstream link(data_dir / vfs::kHostDiscLinkFileName,
                       std::ios::binary | std::ios::trunc);
    if (!link.is_open()) {
      entry->message = "could not write the disc link in " +
                       xe::path_to_utf8(data_dir);
      std::filesystem::remove(header_path, ec);
      return false;
    }
    link << "# Xenia host-side game library (HOST-SIDE, phase 1099z165).\n"
         << "# The SVOD fragments a real install would hold are not copied;\n"
         << "# SvodCreateDevice mounts this disc image instead.\n"
         << xe::path_to_utf8(entry->path) << "\n";
  }

  // xam's content scan resolves "<package>.data\Data0000" for every installed
  // game it finds. Give it one, as a hard link - instant, and no second copy
  // of the disc image on disk. It is the disc image, not an SVOD fragment, so
  // it would not parse as one; nothing reads it, because SvodCreateDevice
  // follows the link file above instead. If the link cannot be made (the disc
  // image is on another volume, or the file system has no hard links) the
  // package still works, it just has no Data0000 for the scan to find.
  const std::filesystem::path fragment_path = data_dir / "Data0000";
  std::filesystem::create_hard_link(entry->path, fragment_path, ec);
  if (ec) {
    XELOGW("GameLibrary: no Data0000 hard link for {} ({})",
           xe::path_to_utf8(header_path), ec.message());
  }

  XELOGI("GameLibrary: installed {:08X} '{}' -> {} (link to {})",
         entry->info.title_id, display_name, xe::path_to_utf8(header_path),
         xe::path_to_utf8(entry->path));
  return true;
}

}  // namespace app
}  // namespace xe
