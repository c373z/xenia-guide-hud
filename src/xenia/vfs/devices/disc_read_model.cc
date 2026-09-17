/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/disc_read_model.h"

#include <chrono>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

DEFINE_int32(disc_read_rate_kbps, 16200,
             "Optical drive model: disc image read rate in KB/s (estimate, "
             "~12x DVD). 0 = reads are instant.",
             "Storage");
DEFINE_int32(disc_seek_ms, 80,
             "Optical drive model: time for a non-sequential disc read "
             "(estimate).",
             "Storage");
DEFINE_int32(disc_cache_mb, 16,
             "Optical drive model: size of the file-system cache in front of "
             "the drive (ESTIMATE, phase 1099z158). Reads of data already in "
             "the cache return without touching the drive: no seek, no read "
             "time, no wait behind another thread's read. 0 = no cache.",
             "Storage");
DEFINE_bool(disc_synth_security_block, false,
            "HOST-SIDE (phase 1099z164): a plain disc image has no security "
            "sector, whose IOCTL 0x24090 data (a hash-tree root sector and "
            "its SHA-1) the dashboard's Install to Hard Drive checks. With "
            "this on, \\Device\\CdRom0 reports a virtual 16-sector block of "
            "zeros just past the game partition (no hash tables, verification "
            "flag clear) and serves it on raw reads. Not the disc's data.",
            "Storage");

namespace xe {
namespace vfs {

namespace {

// 64 KB cache blocks, least recently used evicted first.
constexpr uint64_t kCacheBlock = 64 * 1024;
std::mutex cache_mutex;
std::list<uint64_t> cache_lru;  // front = most recent
std::unordered_map<uint64_t, std::list<uint64_t>::iterator> cache_index;

bool CacheHasRange(uint64_t offset, size_t length) {
  const uint64_t first = offset / kCacheBlock;
  const uint64_t last = (offset + length - 1) / kCacheBlock;
  std::lock_guard<std::mutex> lock(cache_mutex);
  for (uint64_t b = first; b <= last; ++b) {
    if (cache_index.find(b) == cache_index.end()) return false;
  }
  for (uint64_t b = first; b <= last; ++b) {
    cache_lru.splice(cache_lru.begin(), cache_lru, cache_index[b]);
  }
  return true;
}

void CacheAddRange(uint64_t offset, size_t length) {
  const size_t capacity =
      size_t(std::max(cvars::disc_cache_mb, 0)) * 1024 * 1024 / kCacheBlock;
  if (!capacity) return;
  const uint64_t first = offset / kCacheBlock;
  const uint64_t last = (offset + length - 1) / kCacheBlock;
  std::lock_guard<std::mutex> lock(cache_mutex);
  for (uint64_t b = first; b <= last; ++b) {
    auto it = cache_index.find(b);
    if (it != cache_index.end()) {
      cache_lru.splice(cache_lru.begin(), cache_lru, it->second);
      continue;
    }
    cache_lru.push_front(b);
    cache_index[b] = cache_lru.begin();
    while (cache_lru.size() > capacity) {
      cache_index.erase(cache_lru.back());
      cache_lru.pop_back();
    }
  }
}

}  // namespace

void DiscReadModelAccount(uint64_t disc_offset, size_t length) {
  if (cvars::disc_read_rate_kbps <= 0 || length == 0) {
    return;
  }
  // Phase 1099z158: without a cache in front of the drive, the dash's main
  // thread re-reading the start of default.xex every frame while another thread
  // read deep into the image made every read seek (head ping-pong) and made the
  // main thread wait behind the drive lock each frame: ~5 fps for ~90 s after
  // inserting Fable III. Cached ranges skip the drive entirely.
  if (CacheHasRange(disc_offset, length)) {
    return;
  }
  // One head: reads queue behind each other. Phase 1099z159: the queue is a
  // "drive busy until" time reserved under the lock, and the wait happens
  // AFTER the lock is released. Sleeping while holding the lock (the old code)
  // left it locked forever when a title terminate killed a reader mid-sleep,
  // so every later disc read hung: Fable III black screen when launched ~2 s
  // after inserting the disc, while the dash was still reading it.
  static std::mutex drive_mutex;
  static uint64_t head = 0;
  static std::chrono::steady_clock::time_point busy_until;
  std::chrono::steady_clock::time_point done;
  {
    std::lock_guard<std::mutex> lock(drive_mutex);
    double ms = double(length) / (double(cvars::disc_read_rate_kbps) * 1024.0) *
                1000.0;
    const uint64_t distance =
        disc_offset > head ? disc_offset - head : head - disc_offset;
    if (distance > (1u << 20)) {  // beyond read-ahead: the head seeks
      ms += cvars::disc_seek_ms;
    }
    head = disc_offset + length;
    if (length >= (4u << 20)) {
      static int logs = 0;
      if (++logs <= 20) {
        XELOGI("DiscReadModel: {:X} bytes at disc offset {:X} -> {:.0f} ms",
               length, disc_offset, ms);
      }
    }
    const auto now = std::chrono::steady_clock::now();
    const auto start = busy_until > now ? busy_until : now;
    busy_until =
        start + std::chrono::microseconds(static_cast<int64_t>(ms * 1000.0));
    done = busy_until;
  }
  std::this_thread::sleep_until(done);
  CacheAddRange(disc_offset, length);
}

}  // namespace vfs
}  // namespace xe
