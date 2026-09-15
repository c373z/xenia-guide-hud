/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_DISC_READ_MODEL_H_
#define XENIA_VFS_DEVICES_DISC_READ_MODEL_H_

#include <cstddef>
#include <cstdint>

namespace xe {
namespace vfs {

// Phase 1099z109: optical drive timing. Reads from a disc image take the time
// a console's DVD drive would: a seek when the head jumps, then the bytes at
// the drive's read rate. It blocks the reading thread, as a real read does -
// so a game launch from the dash waits on the XEX coming off the disc, and a
// game's own loading screens last as long as they need to.
// The rates are ESTIMATES, not measurements of the user's console:
//   --disc_read_rate_kbps (default 16200 KB/s, ~12x DVD)
//   --disc_seek_ms        (default 80 ms for a non-sequential read)
// 0 for the rate disables the model.
void DiscReadModelAccount(uint64_t disc_offset, size_t length);

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_DISC_READ_MODEL_H_
