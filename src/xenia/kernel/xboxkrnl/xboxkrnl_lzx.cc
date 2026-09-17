/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Phase 1099z143: the kernel's LZX block decompressor (LDI*), after the 17489
// kernel (80169E80..8016A0A4). The dash's disc title reader uses it for XEXs
// with "Normal" (LZX) compression (Sonic Generations); with the exports
// undefined it never got the title name, icon or Game Details.
//
// Guest context (0x2FF8 bytes, from the caller's workspace or its allocator):
//   +0x0000 'CIDL'  +0x0004 max output block  +0x0008 window size
//   +0x000C decoder state (host-side here)
//   +0x2EEC alloc fn  +0x2EF0 free fn  +0x2EF4 workspace after the context
// Decoding uses libmspack's lzxd; the decoder state lives on the host, keyed
// by the context address. Each LDIDecompress call is one LZX frame (<= 32 KB
// output), as in XEX "Normal" compression chunks.

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/power_reset.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

extern "C" {
#include "third_party/mspack/lzx.h"
#include "third_party/mspack/mspack.h"
}

namespace xe {
namespace kernel {
namespace xboxkrnl {

namespace {

constexpr uint32_t kCidlMagic = 0x4349444C;  // 'CIDL'
constexpr uint32_t kContextSize = 0x2FF8;

struct LdiIo {
  mspack_system sys;  // must be first: mspack passes it back as the system
  const uint8_t* in = nullptr;
  size_t in_len = 0;
  size_t in_pos = 0;
  uint8_t* out = nullptr;
  size_t out_len = 0;
  size_t out_pos = 0;
};

// mspack_file* handed to lzxd is the LdiIo itself for both directions; the
// read/write callbacks tell them apart by the function called.
int LdiRead(mspack_file* file, void* buffer, int chars) {
  auto* io = reinterpret_cast<LdiIo*>(file);
  size_t n = std::min<size_t>(chars, io->in_len - io->in_pos);
  std::memcpy(buffer, io->in + io->in_pos, n);
  io->in_pos += n;
  return static_cast<int>(n);
}
int LdiWrite(mspack_file* file, void* buffer, int chars) {
  auto* io = reinterpret_cast<LdiIo*>(file);
  size_t n = std::min<size_t>(chars, io->out_len - io->out_pos);
  std::memcpy(io->out + io->out_pos, buffer, n);
  io->out_pos += n;
  return static_cast<int>(n);
}
void* LdiAlloc(mspack_system*, size_t chars) { return std::calloc(chars, 1); }
void LdiFree(void* ptr) { std::free(ptr); }
void LdiCopy(void* src, void* dest, size_t chars) {
  std::memmove(dest, src, chars);
}
void LdiMessage(mspack_file*, const char* fmt, ...) {}

struct LdiState {
  std::unique_ptr<LdiIo> io;
  lzxd_stream* lzx = nullptr;
  uint32_t window_bits = 15;
  bool allocated_by_guest_fn = false;
};

std::mutex ldi_lock;
std::map<uint32_t, LdiState> ldi_states;

bool LdiOpenStream(LdiState* s) {
  if (s->lzx) lzxd_free(s->lzx);
  s->lzx = lzxd_init(&s->io->sys, reinterpret_cast<mspack_file*>(s->io.get()),
                     reinterpret_cast<mspack_file*>(s->io.get()),
                     s->window_bits, 0, 0x8000, 0, 0);
  return s->lzx != nullptr;
}

uint32_t CallGuest(uint32_t fn, uint64_t arg) {
  uint64_t args[] = {arg};
  return static_cast<uint32_t>(kernel_state()->processor()->Execute(
      XThread::GetCurrentThread()->thread_state(), fn, args, 1));
}

}  // namespace

// 80169E80: (pcbDataBlockMax, &{WindowSize, CpuType}, pfnAlloc, pfnFree,
// workspace, pcbSrcBufferMin, phHandle). Returns 0, or 1 on failure.
dword_result_t LDICreateDecompression_entry(lpdword_t max_block_ptr,
                                            lpdword_t config_ptr,
                                            dword_t alloc_fn, dword_t free_fn,
                                            dword_t workspace,
                                            lpdword_t src_min_ptr,
                                            lpdword_t handle_ptr) {
  const uint32_t max_block = *max_block_ptr;
  src_min_ptr[0] = max_block + 0x1800;
  if (!handle_ptr) return 0;
  handle_ptr[0] = 0;
  uint32_t ctx = 0;
  bool guest_alloc = false;
  if (alloc_fn) {
    ctx = CallGuest(alloc_fn, kContextSize);
    guest_alloc = true;
  } else {
    ctx = workspace;
  }
  if (!ctx) return 1;
  const uint32_t window = config_ptr[0];
  uint32_t window_bits = 0;
  if (!xe::bit_scan_forward(window, &window_bits) || window_bits < 15 ||
      window_bits > 21) {
    XELOGE("LDICreateDecompression: unsupported window {:08X}", window);
    return 1;
  }
  auto* mem = kernel_memory();
  xe::store_and_swap<uint32_t>(mem->TranslateVirtual(ctx + 0x2EEC), alloc_fn);
  xe::store_and_swap<uint32_t>(mem->TranslateVirtual(ctx + 0x2EF0), free_fn);
  xe::store_and_swap<uint32_t>(mem->TranslateVirtual(ctx + 0x2EF4),
                               alloc_fn ? uint32_t(workspace)
                                        : uint32_t(workspace) + kContextSize);
  xe::store_and_swap<uint32_t>(mem->TranslateVirtual(ctx + 4), max_block);
  xe::store_and_swap<uint32_t>(mem->TranslateVirtual(ctx + 8), window);

  LdiState state;
  state.io = std::make_unique<LdiIo>();
  state.io->sys.read = LdiRead;
  state.io->sys.write = LdiWrite;
  state.io->sys.alloc = LdiAlloc;
  state.io->sys.free = LdiFree;
  state.io->sys.copy = LdiCopy;
  state.io->sys.message = LdiMessage;
  state.window_bits = window_bits;
  state.allocated_by_guest_fn = guest_alloc;
  if (!LdiOpenStream(&state)) return 1;
  xe::store_and_swap<uint32_t>(mem->TranslateVirtual(ctx), kCidlMagic);
  {
    std::lock_guard<std::mutex> lock(ldi_lock);
    auto& slot = ldi_states[ctx];
    if (slot.lzx) lzxd_free(slot.lzx);
    slot = std::move(state);
  }
  handle_ptr[0] = ctx;
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(LDICreateDecompression, kNone, kImplemented);

// 80169F58: (h, src, src_len, dst, pcbResult in: bytes wanted, out: produced).
// 2 bad handle, 3 more than the max block, 4 decode error.
dword_result_t LDIDecompress_entry(dword_t handle, lpvoid_t src,
                                   dword_t src_len, lpvoid_t dst,
                                   lpdword_t result_ptr) {
  auto* mem = kernel_memory();
  if (!handle ||
      xe::load_and_swap<uint32_t>(mem->TranslateVirtual(handle)) != kCidlMagic) {
    return 2;
  }
  const uint32_t wanted = *result_ptr;
  if (wanted > xe::load_and_swap<uint32_t>(mem->TranslateVirtual(handle + 4))) {
    return 3;
  }
  std::lock_guard<std::mutex> lock(ldi_lock);
  auto it = ldi_states.find(handle);
  if (it == ldi_states.end() || !it->second.lzx) return 2;
  auto& s = it->second;
  auto* lzx = s.lzx;
  auto* io = s.io.get();
  io->in = src.as<const uint8_t*>();
  io->in_len = src_len;
  io->in_pos = 0;
  io->out = dst.as<uint8_t*>();
  io->out_len = wanted;
  io->out_pos = 0;
  // A frame shorter than 32 KB is the stream's last: tell lzxd its length.
  if (wanted < 0x8000) lzx->length = lzx->offset + wanted;
  // lzxd_decompress(n) decodes frames up to (offset + n) / 32K + 1, i.e. one
  // frame too many when offset + n ends exactly on a frame boundary - which
  // reads into the next block's bytes that this call does not have. Ask for
  // all but the last byte (decodes exactly this frame), then take the last
  // byte from lzxd's stored frame output.
  int err = MSPACK_ERR_OK;
  if (wanted > 1) err = lzxd_decompress(lzx, wanted - 1);
  if (err == MSPACK_ERR_OK && wanted) err = lzxd_decompress(lzx, 1);
  // Blocks are independent in the input: drop lzxd's lookahead (bits it
  // peeked past this block's end, or the zero padding it injected).
  lzx->i_ptr = lzx->i_end = &lzx->inbuf[0];
  lzx->bit_buffer = 0;
  lzx->bits_left = 0;
  lzx->input_end = 0;
  *result_ptr = static_cast<uint32_t>(io->out_pos);
  if (err != MSPACK_ERR_OK) {
    XELOGE("LDIDecompress: lzxd error {} (in {:X} bytes, wanted {:X}, got {:X})",
           err, uint32_t(src_len), wanted, uint32_t(io->out_pos));
    lzx->error = MSPACK_ERR_OK;
    return 4;
  }
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDecompress, kNone, kImplemented);

// 80169FF0: reset the decoder (new stream, same window).
dword_result_t LDIResetDecompression_entry(dword_t handle) {
  auto* mem = kernel_memory();
  if (!handle ||
      xe::load_and_swap<uint32_t>(mem->TranslateVirtual(handle)) != kCidlMagic) {
    return 2;
  }
  std::lock_guard<std::mutex> lock(ldi_lock);
  auto it = ldi_states.find(handle);
  if (it != ldi_states.end()) LdiOpenStream(&it->second);
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(LDIResetDecompression, kNone, kImplemented);

// 8016A038: clear the magic, release the decoder, free the context through the
// caller's free function when its allocator made it.
dword_result_t LDIDestroyDecompression_entry(dword_t handle) {
  auto* mem = kernel_memory();
  if (!handle ||
      xe::load_and_swap<uint32_t>(mem->TranslateVirtual(handle)) != kCidlMagic) {
    return 2;
  }
  xe::store_and_swap<uint32_t>(mem->TranslateVirtual(handle), 0);
  bool guest_alloc = false;
  {
    std::lock_guard<std::mutex> lock(ldi_lock);
    auto it = ldi_states.find(handle);
    if (it != ldi_states.end()) {
      if (it->second.lzx) lzxd_free(it->second.lzx);
      guest_alloc = it->second.allocated_by_guest_fn;
      ldi_states.erase(it);
    }
  }
  const uint32_t free_fn =
      xe::load_and_swap<uint32_t>(mem->TranslateVirtual(handle + 0x2EF0));
  if (guest_alloc && free_fn) CallGuest(free_fn, handle);
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDestroyDecompression, kNone, kImplemented);

void ResetLzxStateForPowerOff() {
  std::lock_guard<std::mutex> lock(ldi_lock);
  for (auto& kv : ldi_states) {
    if (kv.second.lzx) lzxd_free(kv.second.lzx);
  }
  ldi_states.clear();
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Lzx);
