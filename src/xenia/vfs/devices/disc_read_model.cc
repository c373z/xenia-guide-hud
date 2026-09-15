/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/disc_read_model.h"

#include <chrono>
#include <mutex>
#include <thread>

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

namespace xe {
namespace vfs {

void DiscReadModelAccount(uint64_t disc_offset, size_t length) {
  if (cvars::disc_read_rate_kbps <= 0 || length == 0) {
    return;
  }
  static std::mutex drive_mutex;  // one head: reads queue behind each other
  static uint64_t head = 0;
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
  if (ms >= 1.0) {
    std::this_thread::sleep_for(
        std::chrono::microseconds(static_cast<int64_t>(ms * 1000.0)));
  }
}

}  // namespace vfs
}  // namespace xe
