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

void GuideSetMode1Device(uint32_t dev);
uint32_t GuideMode1Device();

// 819F4D28's fifth argument (r7), captured on entry. It is the pointer the
// one real call site of 81A0FE48 passes as that function's second argument
// (819F4E84: mr r4,r28, where r28 <- r7 in the prologue at 819F4D40).
// 819F4D28 runs in both device modes, so this is populated even on the mode-2
// path where 81A0FE48 itself is skipped - which is exactly the case that needs
// it. Passing 0 instead faults at 81A04648 on r30+0x48.
void GuideSetDevCreateArg5(uint32_t ptr);
uint32_t GuideGetDevCreateArg5();

void VdQueryVideoMode(X_VIDEO_MODE* video_mode, bool is_internal_resolution);

// Runs a guest function on the title's own render thread from inside
// VdCallGraphicsNotificationRoutines. The title's D3D device is thread-affine
// ("the current thread is trying to use a D3D device object that is owned by
// a different thread"), so the Guide can only draw into the title's frame
// from there. Pass fn = 0 to clear.
extern thread_local bool in_xam_createdevice_scope;

void SetGuideDrawHook(uint32_t fn, uint32_t self);
// Extent of the loaded xam image, published by the emulator at load. Guide
// code here reads dashroot-derived constants (0x81D42520 and friends); on a
// build whose image ends lower those are unmapped and reading them host-faults.
void SetHudEntries(uint32_t xuiinit, uint32_t render);
void SetXamImageExtent(uint32_t lo, uint32_t hi);
bool XamAddrInImage(uint32_t addr, uint32_t len);
uint32_t XamUiThreadSlot();
uint32_t XamDeviceSlot();
uint32_t XamRenderHost();
uint32_t XamXuiCtxSlot();
uint32_t XamXuiCreateDC();
uint32_t XamProviderSlot();
bool XamIsDashrootLayout();
uint32_t GuideConst(uint32_t addr);
// Patch one guest instruction, unprotecting the host page first - the xam
// text pages are read-only and a bare store faults before it can log.
void GuideInstallAllocStub();
bool GuidePatchWord(uint32_t addr, uint32_t expect, uint32_t value,
                    const char* name);
uint32_t GuideNopFn();
void* GuideStallThread();
void GuidePublishStallThread(void* h);

// Queue the whole XUI bootstrap to run on the title's render thread. The
// title's D3D device is thread-affine, so every XUI call that touches it -
// render host, CreateDC, hud's init - has to happen there, not just the draw.
// True once the title-thread bootstrap has finished its device-touching
// work. Scene creation waits for this and then runs off the render thread.
bool GuideBootstrapReady();

// The bootstrap's own device context - the only one whose [+0x1C8]/[+0x1CC]
// are populated. hud's DC is constructed but never gets the vtable dispatch
// that lands in 818FDE98, so those fields hold allocation garbage.
uint32_t GuideBootDc();

// Bind a freshly created surface as `dev`'s render target. `ts` is a
// xe::cpu::ThreadState*. Returns the surface, or 0 if creation failed.
// Emit the guide_coverage_fn readback immediately (used from the crash
// handler, which no end-of-run readback survives).
void GuideEmitCoverageNow();

uint32_t GuideBindDeviceRt(uint32_t dev, void* ts);

// Allocate and bind a command buffer on `dev` (kb kilobytes). `ts` is a
// xe::cpu::ThreadState*. Returns the buffer, or 0.
uint32_t GuideBindDeviceCmdbuf(uint32_t dev, void* ts, uint32_t kb);

// xam's mode-1 device creator re-points the GPU ring from the title's ring to
// its own, which is why the title stops swapping at the button press. Save the
// title's ring before that call and restore it once the Guide has drawn, so
// the Guide gets a properly brought-up device and the title still presents.
void GuideSaveTitleRing();
void GuideRestoreTitleRing();

void QueueGuideBootstrap(uint32_t hud_base, uint32_t guide_obj,
                         bool use_title_device, uint32_t skin_module);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_VIDEO_H_
