/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_VIDEO_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_VIDEO_H_

#include "xenia/kernel/kernel.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

void VdQueryVideoMode(X_VIDEO_MODE* video_mode, bool is_internal_resolution);

// Runs a guest function on the title's own render thread from inside
// VdCallGraphicsNotificationRoutines. The title's D3D device is thread-affine
// ("the current thread is trying to use a D3D device object that is owned by
// a different thread"), so the Guide can only draw into the title's frame
// from there. Pass fn = 0 to clear.
extern thread_local bool in_xam_createdevice_scope;

void SetGuideDrawHook(uint32_t fn, uint32_t self);

// Queue the whole XUI bootstrap to run on the title's render thread. The
// title's D3D device is thread-affine, so every XUI call that touches it -
// render host, CreateDC, hud's init - has to happen there, not just the draw.
// True once the title-thread bootstrap has finished its device-touching
// work. Scene creation waits for this and then runs off the render thread.
bool GuideBootstrapReady();

void QueueGuideBootstrap(uint32_t hud_base, uint32_t guide_obj,
                         bool use_title_device, uint32_t skin_module);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_VIDEO_H_
