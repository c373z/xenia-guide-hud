/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/game_library_art.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/stb/stb_image.h"
#include "third_party/stb/stb_image_write.h"

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/gpu/texture_cache.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
// platform_win.h defines WIN32_LEAN_AND_MEAN; winhttp.h needs nothing else.
#include <winhttp.h>
#endif

#define RAPIDJSON_HAS_STDSTRING 1
#include "third_party/rapidjson/include/rapidjson/document.h"

namespace xe {
namespace app {

namespace {

// The 219x300 cover is the size of the Live-era boxartlg.jpg; BrandedKeyArt is
// 584x800, the same aspect. Both sizes are multiplied by the draw resolution
// scale (the dash samples the file at its own size, so a 2x window needs a 2x
// file to stay sharp - measured 2026-09-16), and capped at the source's size.
constexpr int kLargeWidth = 219;
constexpr int kLargeHeight = 300;
constexpr int kSmallWidth = 85;
constexpr int kSmallHeight = 117;
constexpr int kJpegQuality = 90;
constexpr size_t kMaxSearchProducts = 20;
// A bottom row counts as a black band when this share of its pixels is black
// (every channel below kBlackLevel). Some store covers (both Sonic titles) end
// in 3 solid black rows; Fable III's merely dark art is left alone.
constexpr int kBlackLevel = 32;
constexpr double kBlackRowShare = 0.98;

std::string UrlEncode(const std::string& value) {
  std::string out;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      out += fmt::format("%{:02X}", c);
    }
  }
  return out;
}

#if XE_PLATFORM_WIN32
// One GET over WinHTTP. Returns false on transport errors or a non-200.
bool HttpGet(const std::string& url, std::vector<uint8_t>* body,
             std::string* error) {
  body->clear();
  const std::u16string url16 = xe::to_utf16(url);
  const auto* wurl = reinterpret_cast<const wchar_t*>(url16.c_str());

  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = DWORD(-1);
  parts.dwUrlPathLength = DWORD(-1);
  parts.dwExtraInfoLength = DWORD(-1);
  if (!WinHttpCrackUrl(wurl, 0, 0, &parts)) {
    *error = "bad URL " + url;
    return false;
  }
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.lpszExtraInfo) {
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  }

  bool ok = false;
  HINTERNET session =
      WinHttpOpen(L"Xenia game library", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET connection = nullptr;
  HINTERNET request = nullptr;
  if (session) {
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
  }
  if (connection) {
    request = WinHttpOpenRequest(
        connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  }
  if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr)) {
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    for (;;) {
      DWORD available = 0;
      if (!WinHttpQueryDataAvailable(request, &available) || !available) {
        break;
      }
      const size_t offset = body->size();
      body->resize(offset + available);
      DWORD read = 0;
      if (!WinHttpReadData(request, body->data() + offset, available, &read)) {
        break;
      }
      body->resize(offset + read);
    }
    ok = status == 200 && !body->empty();
    if (!ok) {
      *error = fmt::format("HTTP {} from {}", status, url);
    }
  } else {
    *error = fmt::format("request to {} failed ({})", url, GetLastError());
  }
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  if (session) WinHttpCloseHandle(session);
  return ok;
}
#else
bool HttpGet(const std::string& url, std::vector<uint8_t>* body,
             std::string* error) {
  *error = "no HTTP client on this platform";
  return false;
}
#endif

bool ParseJson(const std::vector<uint8_t>& body, rapidjson::Document* doc) {
  doc->Parse(reinterpret_cast<const char*>(body.data()), body.size());
  return !doc->HasParseError() && doc->IsObject();
}

struct Rgb {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels;  // RGB, row-major.
};

bool Decode(const std::vector<uint8_t>& file, Rgb* out) {
  int w = 0, h = 0, comp = 0;
  stbi_uc* data = stbi_load_from_memory(file.data(), int(file.size()), &w, &h,
                                        &comp, 3);
  if (!data) {
    return false;
  }
  out->width = w;
  out->height = h;
  out->pixels.assign(data, data + size_t(w) * h * 3);
  stbi_image_free(data);
  return true;
}

// Drops solid black rows from the bottom (a band baked into the store image).
int TrimBottomBlackRows(Rgb* image) {
  const int threshold = int(image->width * kBlackRowShare);
  int trimmed = 0;
  while (image->height - trimmed > 1) {
    const uint8_t* row =
        &image->pixels[size_t(image->height - 1 - trimmed) * image->width * 3];
    int black = 0;
    for (int x = 0; x < image->width; ++x) {
      const uint8_t* p = row + x * 3;
      black += p[0] < kBlackLevel && p[1] < kBlackLevel && p[2] < kBlackLevel;
    }
    if (black < threshold) {
      break;
    }
    ++trimmed;
  }
  image->height -= trimmed;
  image->pixels.resize(size_t(image->width) * image->height * 3);
  return trimmed;
}

// Area-average resample (the covers only ever shrink).
Rgb Resize(const Rgb& src, int width, int height) {
  Rgb dst;
  dst.width = width;
  dst.height = height;
  dst.pixels.resize(size_t(width) * height * 3);
  const double sx = double(src.width) / width;
  const double sy = double(src.height) / height;
  for (int y = 0; y < height; ++y) {
    const double y0 = y * sy, y1 = (y + 1) * sy;
    for (int x = 0; x < width; ++x) {
      const double x0 = x * sx, x1 = (x + 1) * sx;
      double sum[3] = {}, area = 0;
      for (int iy = int(y0); iy < std::min(src.height, int(std::ceil(y1)));
           ++iy) {
        const double wy = std::min<double>(iy + 1, y1) - std::max<double>(iy, y0);
        for (int ix = int(x0); ix < std::min(src.width, int(std::ceil(x1)));
             ++ix) {
          const double wgt =
              wy * (std::min<double>(ix + 1, x1) - std::max<double>(ix, x0));
          const uint8_t* p = &src.pixels[(size_t(iy) * src.width + ix) * 3];
          for (int c = 0; c < 3; ++c) sum[c] += p[c] * wgt;
          area += wgt;
        }
      }
      uint8_t* d = &dst.pixels[(size_t(y) * width + x) * 3];
      for (int c = 0; c < 3; ++c) {
        d[c] = uint8_t(std::clamp(sum[c] / area + 0.5, 0.0, 255.0));
      }
    }
  }
  return dst;
}

std::vector<uint8_t> EncodeJpeg(const Rgb& image) {
  std::vector<uint8_t> out;
  stbi_write_jpg_to_func(
      [](void* context, void* data, int size) {
        auto* v = static_cast<std::vector<uint8_t>*>(context);
        v->insert(v->end(), static_cast<uint8_t*>(data),
                  static_cast<uint8_t*>(data) + size);
      },
      &out, image.width, image.height, 3, image.pixels.data(), kJpegQuality);
  return out;
}

// width x height times the render scale, no larger than the source.
Rgb ScaledCover(const Rgb& src, int width, int height) {
  uint32_t scale_x = 1, scale_y = 1;
  gpu::TextureCache::GetConfigDrawResolutionScale(scale_x, scale_y);
  return Resize(src, std::min(src.width, width * int(scale_x)),
                std::min(src.height, height * int(scale_y)));
}

const char* GetString(const rapidjson::Value& object, const char* name) {
  if (!object.IsObject()) {
    return nullptr;
  }
  auto it = object.FindMember(name);
  return it != object.MemberEnd() && it->value.IsString()
             ? it->value.GetString()
             : nullptr;
}

}  // namespace

namespace {

// One name query: search, then the products it returned. Fills *image_uri
// with the BrandedKeyArt of the product that names title_id. Returns false
// only when the lookup could not finish (art->transient is set).
bool LookUpCover(uint32_t title_id, const std::string& query, CoverArt* art,
                 std::string* image_uri) {
  std::vector<uint8_t> body;

  // The name search returns the game and look-alikes (add-ons, other games),
  // so the title id check below decides.
  if (!HttpGet(fmt::format("https://displaycatalog.mp.microsoft.com/v7.0/"
                           "productFamilies/autosuggest?market=US&"
                           "languages=en-US&productFamilyNames=Games&query={}",
                           UrlEncode(query)),
               &body, &art->error)) {
    art->transient = true;
    return false;
  }
  rapidjson::Document search;
  if (!ParseJson(body, &search)) {
    art->error = "unreadable search result";
    art->transient = true;
    return false;
  }
  std::string big_ids;
  size_t product_count = 0;
  auto results = search.FindMember("Results");
  if (results != search.MemberEnd() && results->value.IsArray()) {
    for (const auto& result : results->value.GetArray()) {
      auto products = result.FindMember("Products");
      if (products == result.MemberEnd() || !products->value.IsArray()) {
        continue;
      }
      for (const auto& product : products->value.GetArray()) {
        const char* id = GetString(product, "ProductId");
        if (id && product_count < kMaxSearchProducts) {
          big_ids += (product_count++ ? "," : "") + std::string(id);
        }
      }
    }
  }
  if (!product_count) {
    return true;
  }

  // Every Xbox 360 product record checked names its title id:
  // Properties.ProductGroupName = "[Fission] <name> (<TITLEID>)".
  if (!HttpGet(fmt::format("https://displaycatalog.mp.microsoft.com/v7.0/"
                           "products?bigIds={}&market=US&languages=en-US",
                           big_ids),
               &body, &art->error)) {
    art->transient = true;
    return false;
  }
  rapidjson::Document catalog;
  if (!ParseJson(body, &catalog)) {
    art->error = "unreadable product record";
    art->transient = true;
    return false;
  }
  const std::string wanted = fmt::format("({:08X})", title_id);
  auto products = catalog.FindMember("Products");
  if (products == catalog.MemberEnd() || !products->value.IsArray()) {
    return true;
  }
  for (const auto& product : products->value.GetArray()) {
    auto properties = product.FindMember("Properties");
    const char* group = properties != product.MemberEnd()
                            ? GetString(properties->value, "ProductGroupName")
                            : nullptr;
    std::string group_upper = group ? group : "";
    for (auto& c : group_upper) {
      c = char(std::toupper(static_cast<unsigned char>(c)));
    }
    if (group_upper.find(wanted) == std::string::npos) {
      continue;
    }
    const char* product_id = GetString(product, "ProductId");
    art->product_id = product_id ? product_id : "";
    auto localized = product.FindMember("LocalizedProperties");
    if (localized == product.MemberEnd() || !localized->value.IsArray() ||
        localized->value.Empty()) {
      continue;
    }
    auto images = localized->value[0].FindMember("Images");
    if (images == localized->value[0].MemberEnd() ||
        !images->value.IsArray()) {
      continue;
    }
    for (const auto& image : images->value.GetArray()) {
      const char* purpose = GetString(image, "ImagePurpose");
      const char* uri = GetString(image, "Uri");
      if (purpose && uri && std::string(purpose) == "BrandedKeyArt") {
        *image_uri = uri;
        return true;
      }
    }
  }
  return true;
}

// The disc's title name can be shorter than the store's ("Sonic Transformed"
// vs "Sonic & All-Stars Racing Transformed"), and the store's search matches
// word prefixes, so each word of the name is a further query.
std::vector<std::string> NameWords(const std::string& name) {
  std::vector<std::string> words;
  std::string word;
  for (size_t i = 0; i <= name.size(); ++i) {
    unsigned char c = i < name.size() ? name[i] : ' ';
    if (std::isalnum(c)) {
      word += char(c);
      continue;
    }
    if (word.size() >= 4 &&
        std::find(words.begin(), words.end(), word) == words.end()) {
      words.push_back(word);
    }
    word.clear();
  }
  return words;
}

}  // namespace

CoverArt FetchCoverArt(uint32_t title_id, const std::string& title_name) {
  CoverArt art;
  if (title_name.empty()) {
    art.error = "no title name to search for";
    return art;
  }
  std::vector<std::string> queries = {title_name};
  for (auto& word : NameWords(title_name)) {
    if (word != title_name) {
      queries.push_back(std::move(word));
    }
  }
  std::string image_uri;
  for (const auto& query : queries) {
    if (!LookUpCover(title_id, query, &art, &image_uri)) {
      return art;
    }
    if (!image_uri.empty()) {
      break;
    }
  }
  if (image_uri.empty()) {
    art.error = art.product_id.empty() ? "no store product for this title id"
                                       : "the store product has no cover art";
    return art;
  }
  if (image_uri.rfind("//", 0) == 0) {
    image_uri = "https:" + image_uri;
  }

  // The cover at the store's own size, lossless; trimmed, resized and encoded
  // here.
  std::vector<uint8_t> source_file;
  Rgb source;
  if (!HttpGet(image_uri + "?format=png", &source_file, &art.error)) {
    art.transient = true;
    return art;
  }
  if (!Decode(source_file, &source)) {
    art.error = "undecodable cover image";
    art.transient = true;
    return art;
  }
  const int source_height = source.height;
  const int trimmed = TrimBottomBlackRows(&source);
  const Rgb large_cover = ScaledCover(source, kLargeWidth, kLargeHeight);
  const Rgb small_cover = ScaledCover(source, kSmallWidth, kSmallHeight);
  art.large_jpeg = EncodeJpeg(large_cover);
  art.small_jpeg = EncodeJpeg(small_cover);
  if (art.large_jpeg.empty() || art.small_jpeg.empty()) {
    art.error = "could not encode the cover";
    art.large_jpeg.clear();
    art.small_jpeg.clear();
    art.transient = true;
    return art;
  }
  XELOGI("CoverArt: {:08X} source {}x{}, {} black row(s) trimmed, large {}x{}, "
         "small {}x{}",
         title_id, source.width, source_height, trimmed, large_cover.width,
         large_cover.height, small_cover.width, small_cover.height);
  art.found = true;
  art.error.clear();
  return art;
}

}  // namespace app
}  // namespace xe
