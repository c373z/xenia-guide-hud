/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_GAME_LIBRARY_ART_H_
#define XENIA_APP_GAME_LIBRARY_ART_H_

#include <cstdint>
#include <string>
#include <vector>

namespace xe {
namespace app {

// 2026-09-16, HOST-SIDE: My Games box art. On a console it comes from Xbox
// Live; here it is looked up ONCE, when a game is installed, in Microsoft's
// public store catalog (displaycatalog.mp.microsoft.com), and saved on the
// emulated console. The image is the product's BrandedKeyArt (the 2012
// "XBOX 360" banner cover, no rating block). A game the store does not list
// gets no art at all - the dashboard shows its own placeholder.
struct CoverArt {
  bool found = false;
  std::string product_id;          // Store BigId, for the log.
  std::vector<uint8_t> large_jpeg;
  std::vector<uint8_t> small_jpeg;
  std::string error;               // Why nothing was found.
  // The lookup could not finish (network, service or parse error), as opposed
  // to the store answering that it has no cover for this title.
  bool transient = false;
};

// Blocking; call from a worker thread. title_name is used for the store's
// name search, title_id to reject products that are not this game.
CoverArt FetchCoverArt(uint32_t title_id, const std::string& title_name);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_GAME_LIBRARY_ART_H_
