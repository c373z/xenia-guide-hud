/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/system_update_import.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/zlib-ng/zlib-ng.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

// Every step mirrors research/import_system_update.py; the comments there
// carry the measurements behind each rule.

namespace xe {
namespace app {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

bool StartsWithI(const std::string& s, std::string_view prefix) {
  return s.size() >= prefix.size() &&
         Lower(s.substr(0, prefix.size())) == Lower(std::string(prefix));
}

bool EndsWithI(const std::string& s, std::string_view suffix) {
  return s.size() >= suffix.size() &&
         Lower(s.substr(s.size() - suffix.size())) ==
             Lower(std::string(suffix));
}

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | p[3];
}
uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t Le32(const uint8_t* p) {
  return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}
uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Le24(const uint8_t* p) {
  return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
}

bool IsStfs(const Bytes& d) {
  return d.size() >= 4 && (!std::memcmp(d.data(), "PIRS", 4) ||
                           !std::memcmp(d.data(), "CON ", 4) ||
                           !std::memcmp(d.data(), "LIVE", 4));
}

Bytes ReadFile(const fs::path& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw std::runtime_error("cannot open " + xe::path_to_utf8(path));
  Bytes out(size_t(f.tellg()));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(out.data()), out.size());
  if (!f) throw std::runtime_error("cannot read " + xe::path_to_utf8(path));
  return out;
}

void WriteFile(const fs::path& path, const uint8_t* data, size_t size) {
  fs::create_directories(path.parent_path());
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(data), size);
  if (!f) throw std::runtime_error("cannot write " + xe::path_to_utf8(path));
}
void WriteFile(const fs::path& path, const Bytes& data) {
  WriteFile(path, data.data(), data.size());
}

// Package payloads nest deep (_packages\FFFE07DF00000006\Theme1\...), and
// the exe does not opt into long paths, so writes go through the \\?\ form.
fs::path LongPath(const fs::path& p) {
#if XE_PLATFORM_WIN32
  std::wstring w = fs::absolute(p).wstring();
  if (w.rfind(L"\\\\?\\", 0) == 0) return fs::path(w);
  if (w.rfind(L"\\\\", 0) == 0) {
    return fs::path(L"\\\\?\\UNC\\" + w.substr(2));
  }
  return fs::path(L"\\\\?\\" + w);
#else
  return p;
#endif
}

fs::path JoinSlashPath(fs::path base, const std::string& slash_path) {
  size_t start = 0;
  while (start <= slash_path.size()) {
    size_t end = slash_path.find('/', start);
    if (end == std::string::npos) end = slash_path.size();
    if (end > start) base /= xe::to_path(slash_path.substr(start, end - start));
    start = end + 1;
  }
  return base;
}

// ---------------------------------------------------------------------------
// Zip (stored and deflate; the update zips are well under 4 GB)
// ---------------------------------------------------------------------------
struct ZipEntry {
  std::string name;  // '/' separated
  uint16_t method = 0;
  uint32_t csize = 0, usize = 0, local_offset = 0;
};

std::vector<ZipEntry> ZipList(const Bytes& z) {
  if (z.size() < 22) throw std::runtime_error("not a zip (too small)");
  size_t lowest = z.size() > 0xFFFF + 22 ? z.size() - (0xFFFF + 22) : 0;
  size_t eocd = SIZE_MAX;
  for (size_t i = z.size() - 22 + 1; i-- > lowest;) {
    if (Le32(&z[i]) == 0x06054B50) {
      eocd = i;
      break;
    }
  }
  if (eocd == SIZE_MAX) throw std::runtime_error("not a zip (no directory)");
  uint16_t count = Le16(&z[eocd + 10]);
  size_t p = Le32(&z[eocd + 16]);
  std::vector<ZipEntry> out;
  for (uint16_t i = 0; i < count; ++i) {
    if (p + 46 > z.size() || Le32(&z[p]) != 0x02014B50) {
      throw std::runtime_error("zip directory is damaged");
    }
    ZipEntry e;
    e.method = Le16(&z[p + 10]);
    e.csize = Le32(&z[p + 20]);
    e.usize = Le32(&z[p + 24]);
    uint16_t nlen = Le16(&z[p + 28]);
    uint16_t elen = Le16(&z[p + 30]);
    uint16_t clen = Le16(&z[p + 32]);
    e.local_offset = Le32(&z[p + 42]);
    if (p + 46 + nlen > z.size()) throw std::runtime_error("zip name past end");
    e.name.assign(reinterpret_cast<const char*>(&z[p + 46]), nlen);
    std::replace(e.name.begin(), e.name.end(), '\\', '/');
    if (e.csize == 0xFFFFFFFF || e.usize == 0xFFFFFFFF ||
        e.local_offset == 0xFFFFFFFF) {
      throw std::runtime_error("zip64 archives are not supported");
    }
    out.push_back(std::move(e));
    p += 46 + nlen + elen + clen;
  }
  return out;
}

Bytes ZipRead(const Bytes& z, const ZipEntry& e) {
  size_t p = e.local_offset;
  if (p + 30 > z.size() || Le32(&z[p]) != 0x04034B50) {
    throw std::runtime_error("bad local header");
  }
  size_t data = p + 30 + Le16(&z[p + 26]) + Le16(&z[p + 28]);
  if (data + e.csize > z.size()) throw std::runtime_error("data past end");
  Bytes out(e.usize);
  if (e.method == 0) {
    if (e.csize != e.usize) throw std::runtime_error("stored size mismatch");
    std::memcpy(out.data(), &z[data], e.usize);
    return out;
  }
  if (e.method != 8) {
    throw std::runtime_error(fmt::format("compression method {}", e.method));
  }
  zng_stream s = {};
  if (zng_inflateInit2(&s, -MAX_WBITS) != Z_OK) {
    throw std::runtime_error("inflate init failed");
  }
  s.next_in = &z[data];
  s.avail_in = e.csize;
  s.next_out = out.data();
  s.avail_out = e.usize;
  int ret = zng_inflate(&s, Z_FINISH);
  size_t produced = s.total_out;
  zng_inflateEnd(&s);
  if (ret != Z_STREAM_END || produced != e.usize) {
    throw std::runtime_error(fmt::format("inflate failed ({})", ret));
  }
  return out;
}

struct Member {
  std::string name;
  Bytes data;
};

// A zip holding $SystemUpdate/ at any depth, or a zip of its contents (flat,
// dash.xex at the root).
std::vector<Member> SourceMembers(const Bytes& z,
                                  std::vector<std::string>& failures) {
  auto entries = ZipList(z);
  std::vector<std::pair<const ZipEntry*, std::string>> picked, flat;
  bool flat_has_dash = false;
  for (auto& e : entries) {
    if (e.name.empty() || e.name.back() == '/') continue;
    size_t slash = e.name.rfind('/');
    std::string leaf =
        slash == std::string::npos ? e.name : e.name.substr(slash + 1);
    if (slash == std::string::npos) {
      flat.emplace_back(&e, leaf);
      if (Lower(leaf) == "dash.xex") flat_has_dash = true;
      continue;
    }
    std::string dir = e.name.substr(0, slash);
    size_t dslash = dir.rfind('/');
    std::string parent =
        dslash == std::string::npos ? dir : dir.substr(dslash + 1);
    if (Lower(parent) == "$systemupdate") picked.emplace_back(&e, leaf);
  }
  if (picked.empty() && flat_has_dash) picked = flat;
  if (picked.empty()) {
    throw std::runtime_error("no $SystemUpdate payloads in the zip");
  }
  std::sort(picked.begin(), picked.end(),
            [](auto& a, auto& b) { return a.second < b.second; });
  std::vector<Member> out;
  for (auto& [e, name] : picked) {
    try {
      out.push_back({name, ZipRead(z, *e)});
    } catch (const std::exception& ex) {
      failures.push_back(
          fmt::format("{}: could not be read out of the zip: {}", name,
                      ex.what()));
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// STFS (mirrors stfs_container_device.cc, as the Python importer does)
// ---------------------------------------------------------------------------
constexpr size_t kBlock = 0x1000;
constexpr uint32_t kPerLevel[3] = {170, 28900, 4913000};
constexpr uint32_t kEndOfChain = 0xFFFFFF;

struct StfsEntry {
  std::string name, path;
  bool dir = false;
  uint32_t start = 0, blocks = 0, length = 0;
  uint16_t parent = 0;
};

class Stfs {
 public:
  explicit Stfs(const Bytes& d) : d_(d) {
    if (!IsStfs(d) || d.size() < 0x3A0) {
      throw std::runtime_error("not an STFS package");
    }
    uint32_t header_size = Be32(&d[0x340]);
    const size_t vd = 0x379;
    uint8_t flags = d[vd + 2];
    read_only_ = flags & 1;
    root_active_ = flags & 2;
    ft_count_ = Le16(&d[vd + 3]);
    ft_block_ = Le24(&d[vd + 5]);
    total_blocks_ = Be32(&d[vd + 0x1C]);
    bpht_ = read_only_ ? 1 : 2;
    step_[0] = kPerLevel[0] + bpht_;
    step_[1] = kPerLevel[1] + (kPerLevel[0] + 1) * bpht_;
    base_ = (header_size + kBlock - 1) / kBlock * kBlock;
  }

  std::vector<StfsEntry> Paths() {
    auto ents = Entries();
    for (auto& e : ents) {
      std::vector<std::string> parts = {e.name};
      uint16_t p = e.parent;
      size_t guard = 0;
      while (p != 0xFFFF) {
        if (p >= ents.size() || ++guard > ents.size()) {
          throw std::runtime_error("bad parent index");
        }
        parts.push_back(ents[p].name);
        p = ents[p].parent;
      }
      std::string path;
      for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!path.empty()) path += '/';
        path += *it;
      }
      e.path = path;
    }
    return ents;
  }

  Bytes Read(const StfsEntry& e) {
    Bytes out;
    out.reserve(e.length);
    uint32_t block = e.start;
    size_t left = e.length;
    while (left && block != kEndOfChain) {
      size_t n = std::min(kBlock, left);
      size_t off = BlockToOffset(block);
      Need(off, n);
      out.insert(out.end(), d_.begin() + off, d_.begin() + off + n);
      left -= n;
      block = HashEntry(block) & 0xFFFFFF;
    }
    if (left) {
      throw std::runtime_error(fmt::format("{} bytes missing", left));
    }
    return out;
  }

 private:
  void Need(size_t off, size_t n) const {
    if (off + n > d_.size()) throw std::runtime_error("read past end");
  }

  size_t BlockToOffset(uint32_t index) const {
    uint64_t block = index;
    for (uint32_t base : kPerLevel) {
      block += ((uint64_t(index) + base) / base) * bpht_;
      if (index < base) break;
    }
    return size_t(base_ + (block << 12));
  }

  uint64_t HashBlockNumber(uint32_t index, int level) const {
    if (level == 2) return step_[1];
    if (index < kPerLevel[level]) return level == 0 ? 0 : step_[level - 1];
    uint64_t block = uint64_t(index / kPerLevel[level]) * step_[level];
    if (level == 0) {
      block += (uint64_t(index / kPerLevel[1]) + 1) * bpht_;
      if (index < kPerLevel[1]) return block;
    }
    return block + bpht_;
  }

  uint32_t HashEntry(uint32_t index) {
    size_t secondary = root_active_ ? kBlock : 0;
    int levels = 0;
    for (int lvl = 0; lvl < 3; ++lvl) {
      if (total_blocks_ < kPerLevel[lvl]) {
        levels = lvl;
        break;
      }
    }
    if (read_only_) {
      secondary = 0;
      levels = 0;
    }
    for (int lvl = levels; lvl >= 0; --lvl) {
      size_t off = size_t(base_ + (HashBlockNumber(index, lvl) << 12));
      auto it = cache_.find(off);
      if (it == cache_.end()) it = cache_.emplace(off, off + secondary).first;
      uint32_t rec = lvl == 0 ? index % kPerLevel[0]
                              : (index / kPerLevel[lvl - 1]) % kPerLevel[0];
      size_t at = it->second + rec * 0x18 + 0x14;
      Need(at, 4);
      uint32_t info = Be32(&d_[at]);
      secondary = (info & 0x40000000) ? kBlock : 0;
    }
    size_t off0 = size_t(base_ + (HashBlockNumber(index, 0) << 12));
    size_t at = cache_.at(off0) + (index % kPerLevel[0]) * 0x18 + 0x14;
    Need(at, 4);
    return Be32(&d_[at]);
  }

  std::vector<StfsEntry> Entries() {
    std::vector<StfsEntry> out;
    uint32_t block = ft_block_;
    for (uint32_t i = 0; i < ft_count_; ++i) {
      size_t off = BlockToOffset(block);
      for (size_t m = 0; m < kBlock / 0x40; ++m) {
        size_t e = off + m * 0x40;
        Need(e, 0x40);
        if (d_[e] == 0) break;
        uint8_t flags = d_[e + 40];
        StfsEntry ent;
        // cp1252; system file names are ASCII.
        ent.name.assign(reinterpret_cast<const char*>(&d_[e]), flags & 0x3F);
        ent.dir = flags & 0x80;
        ent.start = Le24(&d_[e + 47]);
        ent.blocks = Le24(&d_[e + 44]);
        ent.parent = Be16(&d_[e + 50]);
        ent.length = Be32(&d_[e + 52]);
        out.push_back(std::move(ent));
      }
      block = HashEntry(block) & 0xFFFFFF;
      if (block == kEndOfChain) break;
    }
    return out;
  }

  const Bytes& d_;
  bool read_only_ = false, root_active_ = false;
  uint32_t ft_count_ = 0, ft_block_ = 0, total_blocks_ = 0, bpht_ = 1;
  uint64_t step_[2] = {};
  uint64_t base_ = 0;
  // Hash block offset -> offset of the copy in use (primary or secondary).
  std::map<size_t, size_t> cache_;
};

// XEX2 XEX_HEADER_EXECUTION_INFO (0x00040006) version.
bool XexVersion(const Bytes& d, uint32_t out[4]) {
  if (d.size() < 0x18 || std::memcmp(d.data(), "XEX2", 4)) return false;
  uint32_t count = Be32(&d[0x14]);
  for (uint32_t i = 0; i < count; ++i) {
    size_t h = 0x18 + size_t(i) * 8;
    if (h + 8 > d.size()) return false;
    if (Be32(&d[h]) == 0x00040006) {
      size_t v_at = size_t(Be32(&d[h + 4])) + 4;
      if (v_at + 4 > d.size()) return false;
      uint32_t v = Be32(&d[v_at]);
      out[0] = v >> 28;
      out[1] = (v >> 24) & 0xF;
      out[2] = (v >> 8) & 0xFFFF;
      out[3] = v & 0xFF;
      return true;
    }
  }
  return false;
}

// (name, version) for every file a system.manifest (XMNP) lists.
std::vector<std::pair<std::string, uint32_t>> ManifestEntries(const Bytes& d) {
  std::vector<std::pair<std::string, uint32_t>> out;
  std::set<std::pair<std::string, uint32_t>> seen;
  size_t i = 0x138;
  while (i + 6 < d.size()) {
    uint16_t n = Be16(&d[i]);
    size_t end = i + 2 + n;
    if (n >= 2 && n <= 0x80 && end + 4 <= d.size() && d[end - 1] == 0) {
      std::string name(reinterpret_cast<const char*>(&d[i + 2]), n - 1);
      bool printable = std::all_of(name.begin(), name.end(), [](char c) {
        return uint8_t(c) >= 0x20 && uint8_t(c) < 0x7F;
      });
      if (printable && name.find('.') != std::string::npos) {
        uint32_t ver = Be32(&d[end]);
        if ((ver >> 28) >= 1 && (ver >> 28) <= 9 && ver != 0xFFFFFFFF) {
          if (seen.insert({name, ver}).second) out.push_back({name, ver});
          i = end + 4;
          continue;
        }
      }
    }
    ++i;
  }
  return out;
}

std::string JsonString(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default:
        if (c < 0x20) {
          out += fmt::format("\\u{:04x}", c);
        } else {
          out += char(c);
        }
    }
  }
  return out + "\"";
}

std::string JsonList(const std::vector<std::string>& v) {
  std::string out = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    out += (i ? ", " : "") + JsonString(v[i]);
  }
  return out + "]";
}

std::string GenericPath(const fs::path& p) {
  std::string s = xe::path_to_utf8(p);
  std::replace(s.begin(), s.end(), '\\', '/');
  return s;
}

const char* const kRequired[] = {"xam.xex",       "dash.xex",
                                 "hud.xex",       "signin.xex",
                                 "bootanim.xex",  "huduiskin.xex",
                                 "createprofile.xex", "vk.xex"};
const char* const kRequiredLang[] = {"L.dash.xex.dashmain.xzp",
                                     "L.xam.xex.xam.xzp"};
const char* const kRequiredFonts[] = {"xenonjklatin.xtt", "xenonclatin.xtt"};
const char* const kUpdateOnly[] = {"systemupdate.xex", "systemupdate2pre.xex",
                                   "oddupd1.xex", "oddupd2.xex",
                                   "oddupd3.xex"};

// XEX2 delta patch descriptor (0x000005FF): source (base) and target
// versions. 2026-09-16: 2.0.8955's $flash_*.xexp name source 2.0.1888.0.
bool XexpVersions(const Bytes& d, uint32_t source[4], uint32_t target[4]) {
  if (d.size() < 0x18 || std::memcmp(d.data(), "XEX2", 4)) return false;
  uint32_t count = Be32(&d[0x14]);
  for (uint32_t i = 0; i < count; ++i) {
    size_t h = 0x18 + size_t(i) * 8;
    if (h + 8 > d.size()) return false;
    if (Be32(&d[h]) == 0x000005FF) {
      size_t at = size_t(Be32(&d[h + 4]));
      if (at + 12 > d.size()) return false;
      auto split = [](uint32_t v, uint32_t out[4]) {
        out[0] = v >> 28;
        out[1] = (v >> 24) & 0xF;
        out[2] = (v >> 8) & 0xFFFF;
        out[3] = v & 0xFF;
      };
      split(Be32(&d[at + 4]), target);
      split(Be32(&d[at + 8]), source);
      return true;
    }
  }
  return false;
}

std::string VersionString(const uint32_t v[4]) {
  return fmt::format("{}.{}.{}.{}", v[0], v[1], v[2], v[3]);
}

// An update zip's payloads, sorted the way the console installs them.
struct UpdateParts {
  std::vector<Member> members;  // owns the raw package bytes
  // Case-sensitive maps like the Python dicts; lookups that the console does
  // case-insensitively go through Lower().
  std::map<std::string, Bytes> flash, loose, update_extra;
  std::map<std::string, std::map<std::string, Bytes>> packages;
  std::map<std::string, const Bytes*> packages_raw;
};

#if XE_PLATFORM_WIN32
bool ExtractWithSevenZip(const fs::path& archive, const fs::path& out,
                         std::string& error);
#endif

// 2026-09-16: a .rar/.7z update (Digiex ships 2.0.7357 as a .rar) read the
// same way as a zip: the files of its $SystemUpdate folder at any depth, or
// the archive root when it holds the payloads themselves.
std::vector<Member> ArchiveFolderMembers(const fs::path& archive) {
#if XE_PLATFORM_WIN32
  std::error_code ec;
  const fs::path tmp =
      fs::temp_directory_path(ec) /
      fmt::format("xenia_sysupdate_{:06x}", std::rand() & 0xFFFFFF);
  fs::remove_all(tmp, ec);
  std::string err;
  if (!ExtractWithSevenZip(archive, tmp, err)) {
    fs::remove_all(tmp, ec);
    throw std::runtime_error(err);
  }
  fs::path root;
  auto holds_payloads = [&](const fs::path& dir) {
    for (auto& de : fs::directory_iterator(dir, ec)) {
      std::string n = Lower(xe::path_to_utf8(de.path().filename()));
      if (de.is_regular_file() &&
          (n.rfind("su20076000", 0) == 0 || n == "dash.xex")) {
        return true;
      }
    }
    return false;
  };
  if (holds_payloads(tmp)) root = tmp;
  for (auto it = fs::recursive_directory_iterator(tmp, ec);
       root.empty() && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (ec) break;
    if (it->is_directory() &&
        Lower(xe::path_to_utf8(it->path().filename())) == "$systemupdate" &&
        holds_payloads(it->path())) {
      root = it->path();
    }
  }
  std::vector<Member> out;
  if (!root.empty()) {
    std::vector<fs::path> files;
    for (auto& de : fs::directory_iterator(root, ec)) {
      if (de.is_regular_file()) files.push_back(de.path());
    }
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
      out.push_back({xe::path_to_utf8(f.filename()), ReadFile(f)});
    }
  }
  fs::remove_all(tmp, ec);
  if (out.empty()) {
    throw std::runtime_error("no $SystemUpdate payloads in the archive");
  }
  return out;
#else
  throw std::runtime_error("only .zip system updates are supported here");
#endif
}

bool IsUpdateArchiveName(const std::string& name) {
  return EndsWithI(name, ".zip") || EndsWithI(name, ".rar") ||
         EndsWithI(name, ".7z");
}

void CollectUpdate(const fs::path& zip_path, UpdateParts& parts,
                   std::vector<std::string>& failures) {
  if (EndsWithI(xe::path_to_utf8(zip_path), ".zip")) {
    Bytes zip = ReadFile(zip_path);
    parts.members = SourceMembers(zip, failures);
  } else {
    parts.members = ArchiveFolderMembers(zip_path);
  }
  auto& flash = parts.flash;
  auto& loose = parts.loose;
  auto& update_extra = parts.update_extra;
  auto& packages = parts.packages;
  auto& packages_raw = parts.packages_raw;
  for (auto& m : parts.members) {
    if (!IsStfs(m.data)) {
      loose[m.name] = std::move(m.data);
      continue;
    }
    std::vector<StfsEntry> entries;
    Stfs pkg(m.data);
    try {
      for (auto& e : pkg.Paths()) {
        if (!e.dir) entries.push_back(e);
      }
    } catch (const std::exception& ex) {
      failures.push_back(fmt::format(
          "{}: STFS package could not be parsed: {}", m.name, ex.what()));
      continue;
    }
    std::map<std::string, Bytes> files;
    for (auto& e : entries) {
      try {
        files[e.path] = pkg.Read(e);
      } catch (const std::exception& ex) {
        failures.push_back(fmt::format("{}/{}: payload could not be extracted: {}",
                                       m.name, e.path, ex.what()));
      }
    }
    if (StartsWithI(m.name, "su20076000")) {
      for (auto& [p, blob] : files) {
        if (p.rfind("$flash_", 0) == 0) {
          flash[p.substr(7)] = std::move(blob);
        } else {
          update_extra[p] = std::move(blob);
        }
      }
    } else {
      packages[m.name] = std::move(files);
      packages_raw[m.name] = &m.data;
    }
  }
}

// The console's shipped flash files, by lower-case name.
std::map<std::string, Bytes> ReadBaseFirmwareFiles(const fs::path& dir) {
  std::map<std::string, Bytes> files;
  std::error_code ec;
  for (auto& de : fs::directory_iterator(dir, ec)) {
    if (!de.is_regular_file()) continue;
    std::string name = Lower(xe::path_to_utf8(de.path().filename()));
    if (EndsWithI(name, ".bin")) continue;  // fsroot dumps, not SYS: files
    files[name] = ReadFile(de.path());
  }
  return files;
}

void ImportOne(const fs::path& zip_path, const fs::path& systems_dir,
               const fs::path& fonts_dir, const fs::path& base_firmware_dir,
               SystemImportResult& r) {
  auto& failures = r.failures;
  XELOGI("SystemUpdate: importing {}", xe::path_to_utf8(zip_path));
  UpdateParts parts;
  CollectUpdate(zip_path, parts, failures);
  auto& flash = parts.flash;
  auto& loose = parts.loose;
  auto& update_extra = parts.update_extra;
  auto& packages = parts.packages;
  auto& packages_raw = parts.packages_raw;

  // 2026-09-16: a delta-patch update is installed on the base firmware: its
  // files come first, whole files from the update replace them, and each
  // $flash_<name>.xexp is kept beside <name>.xex, which the kernel patches
  // when it loads the module (kernel_system_flash_patches).
  const bool delta_update = !flash.count("xam.xex") && flash.count("xam.xexp");
  std::string base_version;
  if (delta_update && base_firmware_dir.empty()) {
    throw std::runtime_error(
        "this update ships xam as a delta patch ($flash_xam.xexp) that needs "
        "the console's shipped firmware (2.0.1888.0); select it first "
        "(Console > Firmware)");
  }
  if (delta_update) {
    std::set<std::string> have_flash;
    for (auto& [n, b] : flash) have_flash.insert(Lower(n));
    for (auto& [n, b] : ReadBaseFirmwareFiles(base_firmware_dir)) {
      if (!have_flash.count(n)) flash[n] = std::move(b);
    }
    uint32_t bv[4] = {};
    if (XexVersion(flash["xam.xex"], bv)) base_version = VersionString(bv);
    for (auto& [n, b] : flash) {
      if (!EndsWithI(n, ".xexp")) continue;
      uint32_t src[4] = {}, dst[4] = {};
      const bool have_versions = XexpVersions(b, src, dst);
      const std::string base_name = Lower(n.substr(0, n.size() - 1));
      auto it = std::find_if(flash.begin(), flash.end(), [&](auto& e) {
        return Lower(e.first) == base_name;
      });
      uint32_t have[4] = {};
      if (it == flash.end()) {
        r.warnings.push_back(fmt::format(
            "{}: no {} in the base firmware to patch (it patches {}); that "
            "module will not load",
            n, base_name, have_versions ? VersionString(src) : "?"));
      } else if (have_versions && XexVersion(it->second, have) &&
                 VersionString(have) != VersionString(src)) {
        r.warnings.push_back(fmt::format(
            "{}: patches {} but the base firmware's {} is {}; that module "
            "will not load",
            n, VersionString(src), base_name, VersionString(have)));
      }
    }
  }
  if (!flash.count("xam.xex")) {
    throw std::runtime_error(
        "no $flash_xam.xex in any su20076000 package - not a full system "
        "update");
  }
  for (const char* n : kUpdateOnly) {
    for (auto it = flash.begin(); it != flash.end();) {
      if (Lower(it->first) == n) {
        update_extra["$flash_" + it->first] = std::move(it->second);
        it = flash.erase(it);
      } else {
        ++it;
      }
    }
  }

  uint32_t ver[4] = {};
  bool have_ver = XexVersion(flash["xam.xex"], ver);
  if (delta_update) {
    // xam.xex is still the base image; the build is what the patch makes it.
    uint32_t src[4] = {};
    have_ver = XexpVersions(flash["xam.xexp"], src, ver);
  }
  r.build = have_ver ? ver[2] : 0;
  r.xam_version =
      have_ver ? fmt::format("{}.{}.{}.{}", ver[0], ver[1], ver[2], ver[3])
               : "";
  fs::path out = systems_dir / fmt::format("{}", r.build);
  std::error_code ec;
  if (fs::exists(out) && !fs::is_empty(out, ec)) {
    throw std::runtime_error(fmt::format(
        "{} already exists (build {} was imported from another zip)",
        xe::path_to_utf8(out), r.build));
  }
  // Written under a temporary name and renamed at the end, so a half-written
  // system is never picked to boot.
  fs::path work =
      LongPath(systems_dir / fmt::format("{}.importing", r.build));
  fs::remove_all(work, ec);
  fs::create_directories(work);

  // root_files: flash first, then loose files the flash does not provide.
  std::map<std::string, const Bytes*> root_files;
  std::set<std::string> root_is_loose;
  for (auto& [n, b] : flash) root_files[n] = &b;
  for (auto& [n, b] : loose) {
    if (root_files.emplace(n, &b).second) root_is_loose.insert(n);
  }
  std::set<std::string> have;
  for (auto& [n, b] : root_files) have.insert(Lower(n));

  // Base .xtt fonts the update only supplements with .xttp.
  std::map<std::string, Bytes> fonts;
  std::vector<std::string> base_flash_files;
  std::map<std::string, fs::path> avail;
  if (fs::is_directory(fonts_dir, ec)) {
    for (auto& de : fs::directory_iterator(fonts_dir, ec)) {
      avail[Lower(xe::path_to_utf8(de.path().filename()))] = de.path();
    }
  }
  for (auto& [n, b] : flash) {
    if (!EndsWithI(n, ".xttp")) continue;
    std::string base = n.substr(0, n.size() - 1);
    if (have.count(Lower(base))) continue;
    auto it = avail.find(Lower(base));
    if (it == avail.end()) {
      failures.push_back(fmt::format(
          "{}: base font not in {} (the update only carries the .xttp "
          "supplement); strings in that typeface will be blank",
          base, xe::path_to_utf8(fonts_dir)));
      continue;
    }
    fonts[base] = ReadFile(it->second);
    base_flash_files.push_back(base);
  }
  for (auto& [n, b] : fonts) {
    root_files[n] = &b;
    have.insert(Lower(n));
  }

  for (auto& [n, b] : root_files) WriteFile(work / xe::to_path(n), *b);
  size_t package_files = 0;
  for (auto& [pkg, files] : packages) {
    for (auto& [p, blob] : files) {
      WriteFile(JoinSlashPath(work / "_packages" / xe::to_path(pkg), p), blob);
      ++package_files;
    }
  }
  for (auto& [p, blob] : update_extra) {
    WriteFile(JoinSlashPath(work / "_update", p), blob);
  }

  // The hard drive's SystemExtPartition as an installed update leaves it.
  bool sysext = false;
  size_t sysext_files = 0;
  std::vector<std::string> sysext_missing, sysext_packages;
  const Bytes* manifest = nullptr;
  std::map<std::string, std::string> loose_ci;
  for (auto& [n, b] : loose) {
    loose_ci[Lower(n)] = n;
    if (Lower(n) == "system.manifest") manifest = &b;
  }
  if (manifest) {
    sysext = true;
    fs::path sx = work / "_sysext";
    WriteFile(sx / "system.manifest", *manifest);
    for (auto& [name, fver] : ManifestEntries(*manifest)) {
      auto it = loose_ci.find(Lower(name));
      if (it == loose_ci.end()) {
        sysext_missing.push_back(fmt::format("{:08x}\\{}", fver, name));
        continue;
      }
      const std::string& src = it->second;
      fs::path dst = sx / fmt::format("{:08x}", fver) / xe::to_path(name);
      fs::remove(dst, ec);
      fs::create_directories(dst.parent_path());
      bool linked = false;
      if (root_is_loose.count(src)) {
        fs::create_hard_link(work / xe::to_path(src), dst, ec);
        linked = !ec;
      }
      if (!linked) WriteFile(dst, loose[src]);
      ++sysext_files;
    }
    fs::path content =
        sx / "Content" / "0000000000000000" / "FFFE07DF" / "00008000";
    for (auto& [pkg, blob] : packages_raw) {
      if (StartsWithI(pkg, "FFFE07DF")) {
        WriteFile(content / xe::to_path(pkg), *blob);
        sysext_packages.push_back(pkg);
      }
    }
  } else {
    XELOGW("SystemUpdate: no system.manifest - no SystemExtPartition built");
  }

  bool any_lang = false;
  for (auto& n : have) {
    if (n.rfind("l.", 0) == 0 && EndsWithI(n, ".xzp")) any_lang = true;
  }
  std::vector<std::string> required(std::begin(kRequired),
                                    std::end(kRequired));
  if (any_lang) {
    required.insert(required.end(), std::begin(kRequiredLang),
                    std::end(kRequiredLang));
  }
  for (auto& n : required) {
    if (!have.count(Lower(n))) {
      failures.push_back(
          n + ": missing from the update - the system will not boot without it");
    }
  }
  for (const char* n : kRequiredFonts) {
    if (!have.count(n)) {
      failures.push_back(std::string(n) +
                         ": base font missing; text in that typeface is blank");
    }
  }

  std::vector<std::string> flash_names, loose_names, update_names;
  for (auto& [n, b] : flash) flash_names.push_back(n);
  for (auto& [n, b] : loose) loose_names.push_back(n);
  for (auto& [n, b] : update_extra) update_names.push_back(n);
  std::string pkg_counts;
  for (auto& [n, f] : packages) {
    pkg_counts += fmt::format("{}{}: {}", pkg_counts.empty() ? "" : ", ",
                              JsonString(n), f.size());
  }
  std::string fail_json;
  for (auto& f : failures) {
    fail_json += (fail_json.empty() ? "" : ", ") + JsonString(f);
  }
  const std::string base_json =
      delta_update
          ? fmt::format("{{\"path\": {}, \"version\": {}}}",
                        JsonString(GenericPath(fs::absolute(base_firmware_dir))),
                        JsonString(base_version))
          : std::string("null");
  std::string root = GenericPath(out);
  std::string json = fmt::format(
      "{{\n"
      "  \"build\": {},\n"
      "  \"xam_version\": {},\n"
      "  \"source_zip\": {},\n"
      "  \"source_name\": {},\n"
      "  \"source_size\": {},\n"
      "  \"imported_by\": \"xenia (system_update_import.cc)\",\n"
      "  \"flash_files\": {},\n"
      "  \"loose_files\": {},\n"
      "  \"packages\": {{{}}},\n"
      "  \"update_files\": {},\n"
      "  \"base_flash_files\": {},\n"
      "  \"system_ext\": {},\n"
      "  \"settings\": {{\n"
      "    \"guide_system_root\": {},\n"
      "    \"lle_xam\": \"SYS:\\\\xam.xex\",\n"
      "    \"guide_cold_boot_path\": \"SYS:\\\\bootanim.xex\",\n"
      "    \"guide_boot_target\": {},\n"
      "    \"kernel_build_version\": {}\n"
      "  }},\n"
      "  \"base_firmware\": {},\n"
      "  \"warnings\": {},\n"
      "  \"failures\": [{}]\n"
      "}}\n",
      r.build, have_ver ? JsonString(r.xam_version) : "null",
      JsonString(GenericPath(fs::absolute(zip_path))),
      JsonString(xe::path_to_utf8(zip_path.filename())),
      fs::file_size(zip_path), JsonList(flash_names), JsonList(loose_names),
      pkg_counts, JsonList(update_names), JsonList(base_flash_files),
      sysext ? fmt::format("{{\"path\": {}, \"files\": {}, \"missing\": {}, "
                           "\"packages\": {}}}",
                           JsonString(root + "/_sysext"), sysext_files,
                           JsonList(sysext_missing),
                           JsonList(sysext_packages))
             : "null",
      JsonString(root), JsonString(root + "/dash.xex"), r.build, base_json,
      JsonList(r.warnings), fail_json);
  WriteFile(work / "system.json",
            reinterpret_cast<const uint8_t*>(json.data()), json.size());

  fs::remove_all(LongPath(out), ec);  // empty (checked above) or absent
  fs::rename(work, LongPath(out));
  r.system_dir = out;
  XELOGI(
      "SystemUpdate: build {} -> {}: flash {}, loose {}, {} package files in "
      "{}, update extras {}, sysext {} files, {} failure(s)",
      r.xam_version, xe::path_to_utf8(out), flash.size(), loose.size(),
      package_files, packages.size(), update_extra.size(), sysext_files,
      failures.size());
  for (auto& f : failures) XELOGE("SystemUpdate:   {}", f);
}

bool ReadSystemJson(const fs::path& path, rapidjson::Document& doc) {
  Bytes raw;
  try {
    raw = ReadFile(path);
  } catch (...) {
    return false;
  }
  doc.Parse(reinterpret_cast<const char*>(raw.data()), raw.size());
  return !doc.HasParseError() && doc.IsObject();
}

}  // namespace

std::vector<ImportedSystem> ListImportedSystems(const fs::path& systems_dir) {
  std::vector<ImportedSystem> out;
  std::error_code ec;
  if (!fs::is_directory(systems_dir, ec)) return out;
  for (auto& de : fs::directory_iterator(systems_dir, ec)) {
    if (!de.is_directory() || de.path().extension() == ".importing") continue;
    fs::path json = de.path() / "system.json";
    rapidjson::Document doc;
    if (!ReadSystemJson(json, doc)) continue;
    ImportedSystem s;
    s.dir = de.path();
    if (doc.HasMember("build") && doc["build"].IsUint()) {
      s.build = doc["build"].GetUint();
    }
    if (doc.HasMember("xam_version") && doc["xam_version"].IsString()) {
      s.xam_version = doc["xam_version"].GetString();
    }
    s.has_sysext = doc.HasMember("system_ext") && doc["system_ext"].IsObject();
    s.complete = !doc.HasMember("failures") || !doc["failures"].IsArray() ||
                 doc["failures"].Empty();
    s.imported = fs::last_write_time(json, ec);
    out.push_back(std::move(s));
  }
  std::sort(out.begin(), out.end(),
            [](auto& a, auto& b) { return a.imported > b.imported; });
  return out;
}

std::vector<SystemImportResult> ImportPendingSystemUpdates(
    const fs::path& updates_dir, const fs::path& systems_dir,
    const fs::path& fonts_dir, const fs::path& firmware_root) {
  std::vector<SystemImportResult> results;
  std::error_code ec;
  if (!fs::is_directory(updates_dir, ec)) return results;

  // Zips already imported, by file name (and size when recorded; systems the
  // Python importer made record only the path).
  std::set<std::pair<std::string, uint64_t>> done;
  std::set<std::string> done_any_size;
  for (auto& s : ListImportedSystems(systems_dir)) {
    rapidjson::Document doc;
    if (!ReadSystemJson(s.dir / "system.json", doc)) continue;
    std::string name;
    if (doc.HasMember("source_name") && doc["source_name"].IsString()) {
      name = doc["source_name"].GetString();
    } else if (doc.HasMember("source_zip") && doc["source_zip"].IsString()) {
      std::string z = doc["source_zip"].GetString();
      size_t cut = z.find_last_of("/\\");
      name = cut == std::string::npos ? z : z.substr(cut + 1);
    }
    if (name.empty()) continue;
    if (doc.HasMember("source_size") && doc["source_size"].IsUint64()) {
      done.insert({Lower(name), doc["source_size"].GetUint64()});
    } else {
      done_any_size.insert(Lower(name));
    }
  }

  std::vector<fs::path> zips;
  for (auto& de : fs::directory_iterator(updates_dir, ec)) {
    if (de.is_regular_file() &&
        IsUpdateArchiveName(xe::path_to_utf8(de.path().filename()))) {
      zips.push_back(de.path());
    }
  }
  std::sort(zips.begin(), zips.end());
  fs::create_directories(systems_dir, ec);
  for (auto& zip : zips) {
    std::string name = Lower(xe::path_to_utf8(zip.filename()));
    uint64_t size = fs::file_size(zip, ec);
    if (done_any_size.count(name) || done.count({name, size})) continue;
    SystemImportResult r;
    r.zip = zip;
    try {
      fs::path base;
      if (!firmware_root.empty()) {
        const SystemUpdateInfo info = InspectSystemUpdate(zip);
        if (!info.needs_base.empty()) {
          base = FindBaseFirmware(firmware_root, info.needs_base);
        }
      }
      ImportOne(zip, systems_dir, fonts_dir, base, r);
    } catch (const std::exception& ex) {
      r.error = ex.what();
      XELOGE("SystemUpdate: {} not imported: {}", xe::path_to_utf8(zip),
             r.error);
      if (r.build) {
        fs::remove_all(systems_dir / fmt::format("{}.importing", r.build), ec);
      }
    }
    results.push_back(std::move(r));
  }
  return results;
}

SystemUpdateInfo InspectSystemUpdate(const fs::path& zip) {
  SystemUpdateInfo info;
  try {
    UpdateParts parts;
    std::vector<std::string> failures;
    CollectUpdate(zip, parts, failures);
    uint32_t v[4] = {}, src[4] = {};
    if (parts.flash.count("xam.xex") && XexVersion(parts.flash["xam.xex"], v)) {
      info.build = v[2];
      info.version = VersionString(v);
    } else if (parts.flash.count("xam.xexp") &&
               XexpVersions(parts.flash["xam.xexp"], src, v)) {
      info.build = v[2];
      info.version = VersionString(v);
      info.needs_base = VersionString(src);
    } else {
      info.error = "no $flash_xam.xex or $flash_xam.xexp - not a full system "
                   "update";
    }
  } catch (const std::exception& ex) {
    info.error = ex.what();
  }
  return info;
}

fs::path FindBaseFirmware(const fs::path& firmware_root,
                          const std::string& version) {
  std::error_code ec;
  fs::path dir = firmware_root / version;
  return fs::is_regular_file(dir / "xam.xex", ec) ? dir : fs::path();
}

namespace {
#if XE_PLATFORM_WIN32
// A .rar is extracted with 7-Zip (HOST-SIDE convenience: Digiex ships the
// 2.0.1888.0 filesystem as a rar). Returns false when 7-Zip is not there.
bool ExtractWithSevenZip(const fs::path& archive, const fs::path& out,
                         std::string& error) {
  std::vector<fs::path> candidates;
  for (const char* env : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"}) {
    if (const char* base = std::getenv(env)) {
      candidates.push_back(fs::path(base) / "7-Zip" / "7z.exe");
    }
  }
  std::error_code ec;
  fs::path tool;
  for (auto& c : candidates) {
    if (fs::is_regular_file(c, ec)) {
      tool = c;
      break;
    }
  }
  if (tool.empty()) {
    error = "reading a .rar needs 7-Zip (7-zip.org); install it, or extract "
            "the archive and select the folder, or select a .zip";
    return false;
  }
  std::wstring cmd = L"\"" + tool.wstring() + L"\" x -y -o\"" + out.wstring() +
                     L"\" \"" + archive.wstring() + L"\"";
  STARTUPINFOW si = {sizeof(si)};
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    error = fmt::format("could not start 7-Zip ({})", GetLastError());
    return false;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code != 0) {
    error = fmt::format("7-Zip could not extract the archive (exit {})", code);
    return false;
  }
  return true;
}
#endif

// The folder in `root` (at any depth) that holds xam.xex.
fs::path FindXamFolder(const fs::path& root) {
  std::error_code ec;
  if (fs::is_regular_file(root / "xam.xex", ec)) return root;
  for (auto it = fs::recursive_directory_iterator(root, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (it->is_regular_file() &&
        Lower(xe::path_to_utf8(it->path().filename())) == "xam.xex") {
      return it->path().parent_path();
    }
  }
  return {};
}
}  // namespace

BaseFirmware ImportBaseFirmware(const fs::path& source,
                                const fs::path& firmware_root) {
  BaseFirmware result;
  std::error_code ec;
  fs::path staging =
      firmware_root / fmt::format("importing.{}", std::rand() & 0xFFFFFF);
  fs::remove_all(staging, ec);
  try {
    // Gather the files into `from`: the folder itself, or an extraction.
    fs::path from;
    const std::string ext = Lower(xe::path_to_utf8(source.extension()));
    if (fs::is_directory(source, ec)) {
      from = FindXamFolder(source);
    } else if (ext == ".zip") {
      Bytes z = ReadFile(source);
      fs::path raw = staging / "raw";
      for (auto& e : ZipList(z)) {
        if (e.name.empty() || e.name.back() == '/') continue;
        // Keep the zip's folders so FindXamFolder picks the flash folder.
        WriteFile(JoinSlashPath(raw, e.name), ZipRead(z, e));
      }
      from = FindXamFolder(raw);
    } else if (ext == ".rar" || ext == ".7z") {
#if XE_PLATFORM_WIN32
      std::string err;
      if (!ExtractWithSevenZip(source, staging / "raw", err)) {
        throw std::runtime_error(err);
      }
      from = FindXamFolder(staging / "raw");
#else
      throw std::runtime_error("select a .zip or a folder");
#endif
    } else {
      throw std::runtime_error("select a .zip (or a folder) of the firmware");
    }
    if (from.empty()) {
      throw std::runtime_error("no xam.xex in it - not a console firmware");
    }
    uint32_t v[4] = {};
    if (!XexVersion(ReadFile(from / "xam.xex"), v)) {
      throw std::runtime_error("its xam.xex is not a readable XEX2 image");
    }
    result.version = VersionString(v);
    fs::path out = firmware_root / result.version;
    fs::path work = staging / "files";
    fs::create_directories(work);
    size_t count = 0;
    for (auto& de : fs::directory_iterator(from, ec)) {
      if (!de.is_regular_file()) continue;
      fs::copy_file(de.path(), work / de.path().filename(),
                    fs::copy_options::overwrite_existing);
      ++count;
    }
    fs::remove_all(out, ec);
    fs::create_directories(firmware_root, ec);
    fs::rename(work, out);
    result.dir = out;
    XELOGI("SystemUpdate: base firmware {} ({} files) -> {}", result.version,
           count, xe::path_to_utf8(out));
  } catch (const std::exception& ex) {
    result.error = ex.what();
    XELOGE("SystemUpdate: base firmware {} not imported: {}",
           xe::path_to_utf8(source), result.error);
  }
  fs::remove_all(staging, ec);
  return result;
}

SystemImportResult ImportSystemUpdate(const fs::path& zip,
                                      const fs::path& systems_dir,
                                      const fs::path& fonts_dir,
                                      const fs::path& base_firmware_dir) {
  SystemImportResult r;
  r.zip = zip;
  std::error_code ec;
  fs::create_directories(systems_dir, ec);
  try {
    ImportOne(zip, systems_dir, fonts_dir, base_firmware_dir, r);
  } catch (const std::exception& ex) {
    r.error = ex.what();
    XELOGE("SystemUpdate: {} not imported: {}", xe::path_to_utf8(zip),
           r.error);
    if (r.build) {
      fs::remove_all(systems_dir / fmt::format("{}.importing", r.build), ec);
    }
  }
  return r;
}

}  // namespace app
}  // namespace xe
