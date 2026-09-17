/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_HOST_DISC_LINK_H_
#define XENIA_VFS_HOST_DISC_LINK_H_

#include <filesystem>
#include <fstream>
#include <string>

#include "xenia/base/string.h"

namespace xe {
namespace vfs {

// Phase 1099z165: HOST-SIDE, nothing like this exists on a console.
//
// An installed game's content package holds its payload in a "<header>.data"
// folder of SVOD fragments (Data0000, Data0001, ...), 6-8 GB copied off the
// disc. The host-side game library writes a real package header but no
// fragments: instead the .data folder holds this one-line text file naming the
// disc image on the host, and the kernel's SvodCreateDevice mounts that image
// where it would have mounted the SVOD package. The guest cannot tell the
// difference through the file system it is given; what it loses is the
// console's property that an installed game survives the disc being removed.
inline constexpr char kHostDiscLinkFileName[] = "xenia_host_disc.link";

// Returns the disc image path named by a link file, or an empty path if
// data_folder holds no link file. data_folder is the "<header>.data" folder.
inline std::filesystem::path ReadHostDiscLink(
    const std::filesystem::path& data_folder) {
  std::error_code ec;
  const std::filesystem::path link_path = data_folder / kHostDiscLinkFileName;
  if (!std::filesystem::is_regular_file(link_path, ec)) {
    return {};
  }
  std::ifstream file(link_path, std::ios::binary);
  if (!file.is_open()) {
    return {};
  }
  std::string line;
  while (std::getline(file, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                             line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }
    if (line.empty() || line[0] == '#') {
      continue;  // comment lines describe what the file is
    }
    return xe::to_path(line);
  }
  return {};
}

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_HOST_DISC_LINK_H_
