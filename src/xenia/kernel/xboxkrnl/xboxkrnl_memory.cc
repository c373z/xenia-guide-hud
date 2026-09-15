/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <map>
#include <vector>
#include <string>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <chrono>
#include <algorithm>
#include "xenia/kernel/xboxkrnl/xboxkrnl_memory.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

DEFINE_bool(
    ignore_offset_for_ranged_allocations, false,
    "Allows to ignore 4k offset for physical allocations with provided range. "
    "Certain titles check if result matches provided lower range.",
    "Memory");

namespace xe {
namespace kernel {
namespace xboxkrnl {

uint32_t ToXdkProtectFlags(uint32_t protect) {
  uint32_t result = 0;
  if (!(protect & kMemoryProtectRead) && !(protect & kMemoryProtectWrite)) {
    result = X_PAGE_NOACCESS;
  } else if ((protect & kMemoryProtectRead) &&
             !(protect & kMemoryProtectWrite)) {
    result = X_PAGE_READONLY;
  } else {
    result = X_PAGE_READWRITE;
  }
  if (protect & kMemoryProtectNoCache) {
    result |= X_PAGE_NOCACHE;
  }
  if (protect & kMemoryProtectWriteCombine) {
    result |= X_PAGE_WRITECOMBINE;
  }
  return result;
}

uint32_t FromXdkProtectFlags(uint32_t protect) {
  uint32_t result = 0;
  if ((protect & X_PAGE_READONLY) | (protect & X_PAGE_EXECUTE_READ)) {
    result = kMemoryProtectRead;
  } else if ((protect & X_PAGE_READWRITE) |
             (protect & X_PAGE_EXECUTE_READWRITE)) {
    result = kMemoryProtectRead | kMemoryProtectWrite;
  }
  if (protect & X_PAGE_NOCACHE) {
    result |= kMemoryProtectNoCache;
  }
  if (protect & X_PAGE_WRITECOMBINE) {
    result |= kMemoryProtectWriteCombine;
  }
  return result;
}

dword_result_t NtAllocateVirtualMemory_entry(lpdword_t base_addr_ptr,
                                             lpdword_t region_size_ptr,
                                             dword_t alloc_type,
                                             dword_t protect_bits,
                                             dword_t debug_memory) {
  // NTSTATUS
  // _Inout_  PVOID *BaseAddress,
  // _Inout_  PSIZE_T RegionSize,
  // _In_     ULONG AllocationType,
  // _In_     ULONG Protect
  // _In_     BOOLEAN DebugMemory

  assert_not_null(base_addr_ptr);
  assert_not_null(region_size_ptr);

  // Set to TRUE when allocation is from devkit memory area.
  // We don't support separate devkit memory, so just ignore this flag.
  if (debug_memory) {
    XELOGW(
        "Game is attempting to allocate devkit debug memory (base: {:08X}, "
        "size: {:08X}). Ignoring debug flag and using normal allocation.",
        base_addr_ptr ? uint32_t(*base_addr_ptr) : 0,
        region_size_ptr ? uint32_t(*region_size_ptr) : 0);
  }

  // This allocates memory from the kernel heap, which is initialized on startup
  // and shared by both the kernel implementation and user code.
  // The xe_memory_ref object is used to actually get the memory, and although
  // it's simple today we could extend it to do better things in the future.

  // Must request a size.
  if (!base_addr_ptr || !region_size_ptr || !*region_size_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // Check allocation type.
  if (!(alloc_type & (X_MEM_COMMIT | X_MEM_RESET | X_MEM_RESERVE))) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // If MEM_RESET is set only MEM_RESET can be set.
  if (alloc_type & X_MEM_RESET && (alloc_type & ~X_MEM_RESET)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // Don't allow games to set execute bits.
  if (protect_bits & (X_PAGE_EXECUTE | X_PAGE_EXECUTE_READ |
                      X_PAGE_EXECUTE_READWRITE | X_PAGE_EXECUTE_WRITECOPY)) {
    XELOGW("Game setting EXECUTE bit on allocation");
  }

  uint32_t page_size;
  if (*base_addr_ptr != 0) {
    // ignore specified page size when base address is specified.
    auto heap = kernel_memory()->LookupHeap(*base_addr_ptr);
    // Edge case when title can check for XPS/MMIO range and will receive
    // nullptr.
    if (!heap) {
      // Code returned in this case is unknown but probably this one.
      return X_STATUS_INVALID_PARAMETER;
    }

    if (heap->heap_type() != HeapType::kGuestVirtual) {
      return X_STATUS_INVALID_PARAMETER;
    }
    page_size = heap->page_size();
  } else {
    // Adjust size.
    page_size = 4 * 1024;
    if (alloc_type & X_MEM_LARGE_PAGES) {
      page_size = 64 * 1024;
    }
  }

  // Round the base address down to the nearest page boundary.
  uint32_t adjusted_base = *base_addr_ptr - (*base_addr_ptr % page_size);
  // For some reason, some games pass in negative sizes.
  uint32_t adjusted_size = int32_t(*region_size_ptr) < 0
                               ? -int32_t(region_size_ptr.value())
                               : region_size_ptr.value();

  adjusted_size =
      xe::round_up(adjusted_size, adjusted_base ? page_size : 64 * 1024);

  // Allocate.
  uint32_t allocation_type = 0;
  if (alloc_type & X_MEM_RESERVE) {
    allocation_type |= kMemoryAllocationReserve;
  }
  if (alloc_type & X_MEM_COMMIT) {
    allocation_type |= kMemoryAllocationCommit;
  }
  if (alloc_type & X_MEM_RESET) {
    XELOGE("X_MEM_RESET not implemented");
    assert_always();
  }
  uint32_t protect = FromXdkProtectFlags(protect_bits);
  uint32_t address = 0;
  BaseHeap* heap;
  HeapAllocationInfo prev_alloc_info = {};
  bool was_commited = false;

  if (adjusted_base != 0) {
    heap = kernel_memory()->LookupHeap(adjusted_base);
    if (heap->page_size() != page_size) {
      // Specified the wrong page size for the wrong heap.
      return X_STATUS_ACCESS_DENIED;
    }
    was_commited = heap->QueryRegionInfo(adjusted_base, &prev_alloc_info) &&
                   (prev_alloc_info.state & kMemoryAllocationCommit) != 0;

    if (heap->AllocFixed(adjusted_base, adjusted_size, page_size,
                         allocation_type, protect)) {
      address = adjusted_base;
    }
  } else {
    bool top_down = !!(alloc_type & X_MEM_TOP_DOWN);
    heap = kernel_memory()->LookupHeapByType(false, page_size);
    // Phase 1099z131: a base-less allocation starts on the 64 KB allocation
    // granularity, as its size above is already rounded to it. Aligning only
    // to the page size put bootanim's 0x300000 top-down heap segment at
    // 3FCFF000..3FFFF000; its heap decommits in 64 KB steps assuming the
    // segment ends on that boundary, and wrote the trailing entry header at
    // 3FFFF002, past the allocation (guest crash at 98046CE8).
    heap->Alloc(adjusted_size, std::max<uint32_t>(page_size, 64 * 1024),
                allocation_type, protect, top_down, &address);
  }
  if (!address) {
    // Failed - assume no memory available.
    return X_STATUS_NO_MEMORY;
  }

  // Zero memory, if needed.
  if (address && !(alloc_type & X_MEM_NOZERO)) {
    if (alloc_type & X_MEM_COMMIT) {
      if (!(protect & kMemoryProtectWrite)) {
        heap->Protect(address, adjusted_size,
                      kMemoryProtectRead | kMemoryProtectWrite);
      }
      if (!was_commited) {
        kernel_memory()->Zero(address, adjusted_size);
      }
      if (!(protect & kMemoryProtectWrite)) {
        heap->Protect(address, adjusted_size, protect);
      }
    }
  }

  XELOGD("NtAllocateVirtualMemory = {:08X}", address);

  // Phase 1099z47: record title-owned regions for the ExTerminateTitleProcess
  // Mm slot. The real kernel (8006E3B0) picks the title or system address
  // space from its 5th argument (1 title, 2 system, 0 = the calling thread's
  // process type), which Xenia names debug_memory.
  if (address && (alloc_type & X_MEM_RESERVE)) {
    uint32_t owner = uint32_t(debug_memory);
    if (owner != 1 && owner != 2) {
      owner = 0;
      if (auto* th = XThread::GetCurrentThread()) {
        if (auto* kt = th->guest_object<X_KTHREAD>()) {
          owner = kt->process_type;
        }
      }
    }
    if (owner == X_PROCTYPE_TITLE) {
      kernel_state()->RecordTitleAllocation(address);
    }
  }

  // Phase 1091l: NAME THE ALLOCATOR OF THE HUD MANAGER'S PAGE. [81D43C50+0x28]
  // ends up holding 30052000, a single committed 4 KB page whose allocation
  // base IS that pointer (1091i), written between xam's DllMain returning and
  // the Guide press (1091j). A thread census could not name the writer (1091k)
  // because it may run under a host extern handler, which no census sees.
  // Catching it here is cheaper and exact: log the guest LR - the caller - of
  // any allocation that lands on the page. Gated; the default path logs nothing.
  if (cvars::guide_watch_alloc && address &&
      address == uint32_t(cvars::guide_watch_alloc)) {
    auto* wth = XThread::GetCurrentThread();
    XELOGI("GuideWatchAlloc: NtAllocateVirtualMemory -> {:08X} size {:08X} "
           "type {:08X} protect {:08X} | guest lr {:08X} | thread {:08X} start {:08X}",
           address, adjusted_size, uint32_t(alloc_type), uint32_t(protect_bits),
           wth ? uint32_t(wth->thread_state()->context()->lr) : 0u,
           wth ? wth->thread_id() : 0u, wth ? wth->start_address() : 0u);
  }

  // Phase 1099v: who owns the low 0x40000000 region? xam's title-terminate
  // callback releases 40000000+1F0000 as TITLE memory, and system threads
  // crash on it afterwards. Log the first allocations there with the calling
  // thread's process type.
  if (address >= 0x40000000u && address < 0x40400000u) {
    static std::atomic<uint32_t> alog{0};
    if (alog.fetch_add(1) < 24) {
      auto* cth = XThread::GetCurrentThread();
      auto* kt = (cth && cth->is_guest_thread())
                     ? cth->guest_object<X_KTHREAD>()
                     : nullptr;
      XELOGI("TitleSwitch: NtAllocateVirtualMemory {:08X} size {:08X} type {:X} "
             "by thread '{}' process_type {} lr {:08X}",
             address, adjusted_size, uint32_t(alloc_type),
             cth ? cth->thread_name() : std::string("?"),
             kt ? uint32_t(kt->process_type) : 0xFFu,
             cth ? uint32_t(cth->thread_state()->context()->lr) : 0u);
    }
  }
  // Stash back.
  // Maybe set X_STATUS_ALREADY_COMMITTED if MEM_COMMIT?
  *base_addr_ptr = address;
  *region_size_ptr = adjusted_size;
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtAllocateVirtualMemory, kMemory, kImplemented);

dword_result_t NtProtectVirtualMemory_entry(lpdword_t base_addr_ptr,
                                            lpdword_t region_size_ptr,
                                            dword_t protect_bits,
                                            lpdword_t old_protect,
                                            dword_t debug_memory) {
  // Set to TRUE when this memory refers to devkit memory area.
  assert_true(debug_memory == 0);

  // Must request a size.
  if (!base_addr_ptr || !region_size_ptr || !*region_size_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  // Don't allow games to set execute bits.
  if (protect_bits & (X_PAGE_EXECUTE | X_PAGE_EXECUTE_READ |
                      X_PAGE_EXECUTE_READWRITE | X_PAGE_EXECUTE_WRITECOPY)) {
    XELOGW("Game setting EXECUTE bit on protect");
    return X_STATUS_INVALID_PAGE_PROTECTION;
  }

  auto heap = kernel_memory()->LookupHeap(*base_addr_ptr);
  if (heap->heap_type() != HeapType::kGuestVirtual) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // Adjust the base downwards to the nearest page boundary.
  uint32_t adjusted_base =
      *base_addr_ptr - (*base_addr_ptr % heap->page_size());
  uint32_t adjusted_size = xe::round_up(*region_size_ptr, heap->page_size());
  uint32_t protect = FromXdkProtectFlags(protect_bits);

  uint32_t tmp_old_protect = 0;

  // FIXME: I think it's valid for NtProtectVirtualMemory to span regions, but
  // as of now our implementation will fail in this case. Need to verify.
  if (!heap->Protect(adjusted_base, adjusted_size, protect, &tmp_old_protect)) {
    return X_STATUS_ACCESS_DENIED;
  }

  // Write back output variables.
  *base_addr_ptr = adjusted_base;
  *region_size_ptr = adjusted_size;

  if (old_protect) {
    *old_protect = tmp_old_protect;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtProtectVirtualMemory, kMemory, kImplemented);

dword_result_t NtFreeVirtualMemory_entry(lpdword_t base_addr_ptr,
                                         lpdword_t region_size_ptr,
                                         dword_t free_type,
                                         dword_t debug_memory,
                                         const ppc_context_t& ctx) {
  uint32_t base_addr_value = *base_addr_ptr;
  uint32_t region_size_value = *region_size_ptr;
  // Phase 1099v: log frees while a title switch runs (who freed system pages).
  if (kernel_state()->title_switch_log_budget.load() > 0) {
    kernel_state()->title_switch_log_budget.fetch_sub(1);
    XELOGI("TitleSwitch: NtFreeVirtualMemory base {:08X} size {:08X} type {:X} "
           "lr {:08X}",
           base_addr_value, region_size_value, uint32_t(free_type),
           uint32_t(ctx->lr));
  }
  // X_MEM_DECOMMIT | X_MEM_RELEASE

  // NTSTATUS
  // _Inout_  PVOID *BaseAddress,
  // _Inout_  PSIZE_T RegionSize,
  // _In_     ULONG FreeType
  // _In_     BOOLEAN DebugMemory

  // Set to TRUE when freeing external devkit memory.
  assert_true(debug_memory == 0);

  if (!base_addr_value) {
    return X_STATUS_MEMORY_NOT_ALLOCATED;
  }

  auto heap = kernel_state()->memory()->LookupHeap(base_addr_value);
  if (heap->heap_type() != HeapType::kGuestVirtual) {
    return X_STATUS_INVALID_PARAMETER;
  }
  bool result = false;
  if (free_type == X_MEM_DECOMMIT) {
    // If zero, we may need to query size (free whole region).
    assert_not_zero(region_size_value);

    region_size_value = xe::round_up(region_size_value, heap->page_size());
    result = heap->Decommit(base_addr_value, region_size_value);
  } else {
    result = heap->Release(base_addr_value, &region_size_value);
    if (result) {
      kernel_state()->ForgetTitleAllocation(base_addr_value);
    }
  }
  if (!result) {
    return X_STATUS_UNSUCCESSFUL;
  }

  *base_addr_ptr = base_addr_value;
  *region_size_ptr = region_size_value;
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtFreeVirtualMemory, kMemory, kImplemented);

struct X_MEMORY_BASIC_INFORMATION {
  be<uint32_t> base_address;
  be<uint32_t> allocation_base;
  be<uint32_t> allocation_protect;
  be<uint32_t> region_size;
  be<uint32_t> state;
  be<uint32_t> protect;
  be<uint32_t> type;
};
// chrispy: added region_type ? guessed name, havent seen any except 0 used
dword_result_t NtQueryVirtualMemory_entry(
    dword_t base_address,
    pointer_t<X_MEMORY_BASIC_INFORMATION> memory_basic_information_ptr,
    dword_t region_type) {
  switch (region_type) {
    case 0:
    case 1:
    case 2:
      break;
    default:
      return X_STATUS_INVALID_PARAMETER;
  }
  auto heap = kernel_state()->memory()->LookupHeap(base_address);
  HeapAllocationInfo alloc_info;
  if (heap == nullptr || !heap->QueryRegionInfo(base_address, &alloc_info)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  memory_basic_information_ptr->base_address = alloc_info.base_address;
  memory_basic_information_ptr->allocation_base = alloc_info.allocation_base;
  memory_basic_information_ptr->allocation_protect =
      ToXdkProtectFlags(alloc_info.allocation_protect);
  memory_basic_information_ptr->region_size = alloc_info.region_size;
  // https://docs.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-memory_basic_information
  // State: ... This member can be one of the following values: MEM_COMMIT,
  // MEM_FREE, MEM_RESERVE.
  // State queried by Beautiful Katamari before displaying the loading screen.
  uint32_t x_state;
  if (alloc_info.state & kMemoryAllocationCommit) {
    assert_not_zero(alloc_info.state & kMemoryAllocationReserve);
    x_state = X_MEM_COMMIT;
  } else if (alloc_info.state & kMemoryAllocationReserve) {
    x_state = X_MEM_RESERVE;
  } else {
    x_state = X_MEM_FREE;
  }
  memory_basic_information_ptr->state = x_state;
  memory_basic_information_ptr->protect = ToXdkProtectFlags(alloc_info.protect);
  memory_basic_information_ptr->type = X_MEM_PRIVATE;

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtQueryVirtualMemory, kMemory, kImplemented);

dword_result_t NtAllocateEncryptedMemory_entry(dword_t unk, dword_t region_size,
                                               lpdword_t base_addr_ptr) {
  if (!region_size) {
    return X_STATUS_INVALID_PARAMETER;
  }

  const uint32_t region_size_adjusted =
      xe::round_up(region_size, 64 * 1024, true);

  if (region_size_adjusted > 16 * 1024 * 1024) {
    return X_STATUS_INVALID_PARAMETER;
  }

  uint32_t out_address = 0;
  auto heap = kernel_memory()->LookupHeap(0x8C000000);
  const bool result =
      heap->AllocRange(0x8C000000, 0x8FFFFFFF, region_size_adjusted, 64 * 1024,
                       MemoryAllocationFlag::kMemoryAllocationCommit,
                       MemoryProtectFlag::kMemoryProtectRead |
                           MemoryProtectFlag::kMemoryProtectWrite,
                       false, &out_address);

  if (!result) {
    return X_STATUS_UNSUCCESSFUL;
  }

  XELOGD("NtAllocateEncryptedMemory = {:08X}", out_address);
  *base_addr_ptr = out_address;
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtAllocateEncryptedMemory, kMemory, kImplemented);

dword_result_t NtFreeEncryptedMemory_entry(dword_t region_type,
                                           lpdword_t base_address_ptr) {
  if (!base_address_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  auto heap = kernel_state()->memory()->LookupHeap(0x80000000);
  const uint32_t encrypt_address =
      heap->heap_base() + heap->page_size() * (*base_address_ptr);

  auto encrypt_heap = kernel_state()->memory()->LookupHeap(encrypt_address);

  if (encrypt_heap->heap_type() != HeapType::kGuestXex) {
    return X_STATUS_INVALID_PARAMETER;
  }

  kernel_state()->memory()->SystemHeapFree(encrypt_address);

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtFreeEncryptedMemory, kMemory, kImplemented);

uint32_t xeMmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t region_size,
                                      uint32_t protect_bits,
                                      uint32_t min_addr_range,
                                      uint32_t max_addr_range,
                                      uint32_t alignment) {
  // Type will usually be 0 (user request?), where 1 and 2 are sometimes made
  // by D3D/etc.

  // Check protection bits.
  if (!(protect_bits & (X_PAGE_READONLY | X_PAGE_READWRITE))) {
    XELOGE("MmAllocatePhysicalMemoryEx: bad protection bits");
    return 0;
  }

  // Either may be OR'ed into protect_bits:
  // X_PAGE_NOCACHE
  // X_PAGE_WRITECOMBINE
  // We could use this to detect what's likely GPU-synchronized memory
  // and let the GPU know we're messing with it (or even allocate from
  // the GPU). At least the D3D command buffer is X_PAGE_WRITECOMBINE.

  // Calculate page size.
  // Default            = 4KB
  // X_MEM_LARGE_PAGES  = 64KB
  // X_MEM_16MB_PAGES   = 16MB
  uint32_t page_size = 4 * 1024;
  if (protect_bits & X_MEM_LARGE_PAGES) {
    page_size = 64 * 1024;
  } else if (protect_bits & X_MEM_16MB_PAGES) {
    page_size = 16 * 1024 * 1024;
  }

  // Round up the region size and alignment to the next page.
  uint32_t adjusted_size = xe::round_up(region_size, page_size);
  uint32_t adjusted_alignment = xe::round_up(alignment, page_size);

  uint32_t allocation_type = kMemoryAllocationReserve | kMemoryAllocationCommit;
  uint32_t protect = FromXdkProtectFlags(protect_bits);
  bool top_down = true;
  auto heap = static_cast<PhysicalHeap*>(
      kernel_memory()->LookupHeapByType(true, page_size));
  // min_addr_range/max_addr_range are bounds in physical memory, not virtual.
  uint32_t heap_base = heap->heap_base();
  uint32_t heap_physical_address_offset = heap->GetPhysicalAddress(heap_base);
  // TODO(Gliniak): Games like 545108B4 compares min_addr_range with value
  // returned. 0x1000 offset causes it to go below that minimal range and goes
  // haywire
  if (min_addr_range && max_addr_range &&
      cvars::ignore_offset_for_ranged_allocations) {
    heap_physical_address_offset = 0;
  }

  uint32_t heap_min_addr =
      xe::sat_sub(min_addr_range, heap_physical_address_offset);
  uint32_t heap_max_addr =
      xe::sat_sub(max_addr_range, heap_physical_address_offset);
  uint32_t heap_size = heap->heap_size();
  heap_min_addr = heap_base + std::min(heap_min_addr, heap_size - 1);
  heap_max_addr = heap_base + std::min(heap_max_addr, heap_size - 1);
  uint32_t base_address;
  if (!heap->AllocRange(heap_min_addr, heap_max_addr, adjusted_size,
                        adjusted_alignment, allocation_type, protect, top_down,
                        &base_address)) {
    // Failed - assume no memory available.
    XELOGW("MmAllocatePhysicalMemoryEx: Allocation failed: {:08X} Size: {:08X}",
           base_address, adjusted_size);
    return 0;
  }
  XELOGD("MmAllocatePhysicalMemoryEx = {:08X} Size: {:08X}", base_address,
         adjusted_size);

  return base_address;
}

// Phase 1054 dbg: who allocates physical memory, how often and how much -
// the Guide's paint thread spent ~12% of a paint in the heap's free-block
// rebuild under this export. Tallied by the caller's return address and size,
// reported every 5 s while calls keep coming (guide_alloc_tally).
static void GuideAllocTally(const char* what, uint32_t lr, uint32_t size) {
  if (!cvars::guide_alloc_tally) return;
  static std::mutex mu;
  // one table per export (they used to share one and the label lied)
  static std::map<std::string, std::map<std::pair<uint32_t, uint32_t>, uint32_t>> tables;
  static std::map<std::string, std::map<uint32_t, uint32_t>> lrs;
  static std::map<std::string, uint64_t> totals;
  static std::chrono::steady_clock::time_point t0{};
  std::lock_guard<std::mutex> lk(mu);
  auto& counts = tables[what];
  auto& by_lr = lrs[what];
  auto& total = totals[what];
  ++counts[{lr, size}];
  ++by_lr[lr];
  ++total;
  auto now = std::chrono::steady_clock::now();
  if (!t0.time_since_epoch().count()) t0 = now;
  if (now - t0 >= std::chrono::seconds(5)) {
    for (auto& tb : tables) {
      auto& c = tb.second;
      if (c.empty()) continue;
      std::vector<std::pair<std::pair<uint32_t, uint32_t>, uint32_t>> v(c.begin(), c.end());
      std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
      std::string line;
      for (size_t i = 0; i < std::min<size_t>(v.size(), 10); ++i) {
        line += fmt::format("lr {:08X} arg {:X} x{} | ", v[i].first.first, v[i].first.second, v[i].second);
      }
      XELOGI("GuideAllocTally: {} {} calls in {:.1f} s from {} sites: {}", tb.first, totals[tb.first],
             std::chrono::duration<double>(now - t0).count(), lrs[tb.first].size(), line);
      c.clear();
      lrs[tb.first].clear();
      totals[tb.first] = 0;
    }
    t0 = now;
  }
}

// Phase 1054 alloc: a cache of freed single 4 KB physical pages. xam's debug
// D3D allocates and frees 36-292 byte blocks through MmAllocatePhysicalMemoryEx
// 120-290 times a second (lr 817B27C0 / 817B5218: read-write, 32-byte aligned,
// unrestricted range, freed within the paint); each went through the heap's
// range search under the global memory lock and a VirtualAlloc commit, ~6% of
// a Guide paint. A freed single page whose heap entry is read-write stays
// allocated in the heap and waits here; a single-page request with the same
// converted protection takes it back without touching the heap or the host.
// The heap's own reuse would hand out the same page just as quickly, so the
// guest sees nothing new: the page keeps its size, protection and contents.
namespace {
struct GuidePageCache {
  std::mutex mu;
  std::vector<std::pair<uint32_t, uint32_t>> pages;  // address, heap protect
  // Phase 1054 black: physical page numbers the published stream references.
  // The stream is replayed by the GPU every frame while the Guide is idle, and
  // xam frees the single pages its vertex data lives in within the paint; a
  // page handed straight back to the next allocation was overwritten under
  // the replay (the Guide vanished or went black on alternate frames).
  std::unordered_set<uint32_t> pinned;
  // The GPU thread replays a stream inside the title's resolve and can be a
  // few frames behind the paint that published the next one, so the pins are
  // the union of the last kPinHistory publishes, not the latest alone.
  static constexpr size_t kPinHistory = 16;
  // Phase 1055 bugs: entries carry the publish's pair as a tag; the history
  // is deeper than the pinned window so the stream the GPU is replaying can
  // be found and kept when a stalled replay outlives the window.
  static constexpr size_t kPinHistoryMax = 128;
  std::deque<std::pair<uint64_t, std::vector<uint32_t>>> pin_history;
  uint64_t hits = 0, misses = 0, kept = 0, pin_skips = 0, pin_kept = 0;
};
GuidePageCache g_guide_page_cache;
constexpr uint32_t GuidePhysPage(uint32_t address) {
  return (address & 0x1FFFFFFFu) >> 12;
}
}  // namespace

void GuidePageCachePin(const std::vector<uint32_t>& phys_pages, uint64_t tag,
                       uint64_t keep_tag) {
  auto& c = g_guide_page_cache;
  std::lock_guard<std::mutex> lk(c.mu);
  c.pin_history.emplace_back(tag, phys_pages);
  while (c.pin_history.size() > GuidePageCache::kPinHistoryMax) {
    c.pin_history.pop_front();
  }
  c.pinned.clear();
  const size_t n = c.pin_history.size();
  const size_t from = n > GuidePageCache::kPinHistory ? n - GuidePageCache::kPinHistory : 0;
  for (size_t i = 0; i < n; ++i) {
    const auto& e = c.pin_history[i];
    if (i >= from || (keep_tag && e.first == keep_tag)) {
      c.pinned.insert(e.second.begin(), e.second.end());
    }
  }
}

uint64_t GuidePageCachePinSkips() {
  auto& c = g_guide_page_cache;
  std::lock_guard<std::mutex> lk(c.mu);
  return c.pin_skips;
}

static uint32_t GuidePageCacheTake(uint32_t protect_bits, uint32_t region_size,
                                   uint32_t min_addr_range, uint32_t max_addr_range,
                                   uint32_t alignment) {
  if (!cvars::guide_page_cache) return 0;
  if (region_size == 0 || region_size > 4096u || alignment > 4096u) return 0;
  if (protect_bits & (X_MEM_LARGE_PAGES | X_MEM_16MB_PAGES)) return 0;
  if (min_addr_range != 0 || max_addr_range != 0xFFFFFFFFu) return 0;
  uint32_t want = FromXdkProtectFlags(protect_bits);
  auto& c = g_guide_page_cache;
  std::lock_guard<std::mutex> lk(c.mu);
  // Lowest address first, like the heap's own first fit: the same sequence of
  // requests gets the same pages every paint, so a shader or a vertex buffer
  // keeps its address across paints (last-freed-first rotated them).
  bool skipped = false;
  size_t best = c.pages.size();
  for (size_t i = 0; i < c.pages.size(); ++i) {
    if (c.pages[i].second != want) continue;
    uint32_t a = c.pages[i].first;
    if (!c.pinned.empty() && c.pinned.count(GuidePhysPage(a))) {
      skipped = true;  // the live stream reads this page: leave it
      continue;
    }
    if (best == c.pages.size() || a < c.pages[best].first) best = i;
  }
  if (best != c.pages.size()) {
    uint32_t a = c.pages[best].first;
    c.pages.erase(c.pages.begin() + best);
    ++c.hits;
    return a;
  }
  if (skipped) ++c.pin_skips;
  ++c.misses;
  return 0;
}

static bool GuidePageCacheKeep(uint32_t base_address) {
  if (!cvars::guide_page_cache || !base_address) return false;
  auto heap = kernel_state()->memory()->LookupHeap(base_address);
  if (!heap || heap->heap_type() != HeapType::kGuestPhysical || heap->page_size() != 4096u) return false;
  uint32_t size = 0, protect = 0;
  if (!heap->QuerySizeUnlocked(base_address, &size) || size != 4096u) return false;
  if (!heap->QueryProtectUnlocked(base_address, &protect)) return false;
  auto& c = g_guide_page_cache;
  std::lock_guard<std::mutex> lk(c.mu);
  // A page the live stream reads is kept whatever its protection and beyond
  // the cache's usual size, so the heap cannot hand it out either.
  bool pinned =
      !c.pinned.empty() && c.pinned.count(GuidePhysPage(base_address)) != 0;
  if (pinned) {
    if (c.pages.size() >= 4096) return false;
    c.pages.push_back({base_address, protect});
    ++c.kept;
    ++c.pin_kept;
    return true;
  }
  if (!(protect & kMemoryProtectRead) || !(protect & kMemoryProtectWrite)) return false;
  if (c.pages.size() >= 64 + c.pinned.size()) return false;
  c.pages.push_back({base_address, protect});
  ++c.kept;
  return true;
}

dword_result_t MmAllocatePhysicalMemoryEx_entry(
    dword_t flags, dword_t region_size, dword_t protect_bits,
    dword_t min_addr_range, dword_t max_addr_range, dword_t alignment,
    const ppc_context_t& ctx) {
  GuideAllocTally("MmAllocatePhysicalMemoryEx", uint32_t(ctx->lr), region_size.value());
  if (uint32_t cached = GuidePageCacheTake(protect_bits.value(), region_size.value(), min_addr_range.value(),
                                          max_addr_range.value(), alignment.value())) {
    GuideAllocTally("MmAllocatePhysicalMemoryEx.cached", uint32_t(ctx->lr), region_size.value());
    return cached;
  }
  // the other arguments, for the shape of these allocations
  GuideAllocTally("MmAllocatePhysicalMemoryEx.protect", protect_bits.value(), alignment.value());
  GuideAllocTally("MmAllocatePhysicalMemoryEx.range", min_addr_range.value(), max_addr_range.value());
  uint32_t r = xeMmAllocatePhysicalMemoryEx(flags, region_size, protect_bits,
                                            min_addr_range, max_addr_range,
                                            alignment);
  GuideAllocTally("MmAllocatePhysicalMemoryEx.result", r & 0xFFF00000u, uint32_t(ctx->lr));
  if (r) {
    kernel_state()->RecordPhysicalAllocation(r, region_size, flags,
                                             uint32_t(ctx->lr));
  }
  return r;
}
DECLARE_XBOXKRNL_EXPORT1(MmAllocatePhysicalMemoryEx, kMemory, kImplemented);

dword_result_t MmAllocatePhysicalMemory_entry(dword_t flags,
                                              dword_t region_size,
                                              dword_t protect_bits,
                                              const ppc_context_t& ctx) {
  GuideAllocTally("MmAllocatePhysicalMemory", uint32_t(ctx->lr), region_size.value());
  if (uint32_t cached = GuidePageCacheTake(protect_bits.value(), region_size.value(), 0u, 0xFFFFFFFFu, 0u)) {
    return cached;
  }
  const uint32_t r = xeMmAllocatePhysicalMemoryEx(flags, region_size,
                                                  protect_bits, 0, 0xFFFFFFFFu,
                                                  0);
  if (r) {
    kernel_state()->RecordPhysicalAllocation(r, region_size, flags,
                                             uint32_t(ctx->lr));
  }
  return r;
}
DECLARE_XBOXKRNL_EXPORT1(MmAllocatePhysicalMemory, kMemory, kImplemented);

void MmFreePhysicalMemory_entry(dword_t type, dword_t base_address,
                                const ppc_context_t& ctx) {
  if (kernel_state()->title_switch_log_budget.load() > 0) {
    kernel_state()->title_switch_log_budget.fetch_sub(1);
    XELOGI("TitleSwitch: MmFreePhysicalMemory base {:08X} type {:X} lr {:08X}",
           uint32_t(base_address), uint32_t(type), uint32_t(ctx->lr));
  }
  GuideAllocTally("MmFreePhysicalMemory", uint32_t(ctx->lr), base_address.value() & 0xFFF00000u);
  kernel_state()->ForgetPhysicalAllocation(base_address);
  if (GuidePageCacheKeep(base_address.value())) {
    return;
  }
  // base_address = result of MmAllocatePhysicalMemory.

  assert_true((base_address & 0x1F) == 0);

  auto heap = kernel_state()->memory()->LookupHeap(base_address);
  heap->Release(base_address);
}
DECLARE_XBOXKRNL_EXPORT1(MmFreePhysicalMemory, kMemory, kImplemented);

dword_result_t MmQueryAddressProtect_entry(dword_t base_address,
                                           const ppc_context_t& ctx) {
  GuideAllocTally("MmQueryAddressProtect", uint32_t(ctx->lr), base_address.value() & 0xFFFF0000u);
  auto heap = kernel_state()->memory()->LookupHeap(base_address);
  uint32_t access;
  // Phase 1054 dbg: xam's debug D3D asks this dozens of times a second and
  // the locked query waited on the memory lock for ~0.3 ms each; a racy read
  // of one page's protection is what the caller wants anyway.
  if (!heap->QueryProtectUnlocked(base_address, &access)) {
    access = 0;
  }
  access = !access ? 0 : ToXdkProtectFlags(access);

  return access;
}
DECLARE_XBOXKRNL_EXPORT2(MmQueryAddressProtect, kMemory, kImplemented,
                         kHighFrequency);

void MmSetAddressProtect_entry(lpvoid_t base_address, dword_t region_size,
                               dword_t protect_bits) {
  constexpr uint32_t required_protect_bits =
      X_PAGE_NOACCESS | X_PAGE_READONLY | X_PAGE_READWRITE |
      X_PAGE_EXECUTE_READ | X_PAGE_EXECUTE_READWRITE;

  if (xe::bit_count(protect_bits & required_protect_bits) != 1) {
    // Many titles use invalid combination with zero valid bits set.
    // We're skipping assertion for these cases to prevent unnecessary spam.
    assert_false(xe::bit_count(protect_bits & required_protect_bits) > 1);
    return;
  }

  uint32_t protect = FromXdkProtectFlags(protect_bits);
  auto heap = kernel_memory()->LookupHeap(base_address);

  // More research required: 544307D1 uses it with base_address in xex range,
  // which causes write exception in long term. Probably console disables
  // modification of xex range page protection for security reasons.
  if (heap->heap_type() == HeapType::kGuestXex) {
    return;
  }

  heap->Protect(base_address.guest_address(), region_size, protect);
}
DECLARE_XBOXKRNL_EXPORT1(MmSetAddressProtect, kMemory, kImplemented);

dword_result_t MmQueryAllocationSize_entry(lpvoid_t base_address,
                                           const ppc_context_t& ctx) {
  GuideAllocTally("MmQueryAllocationSize", uint32_t(ctx->lr), base_address.guest_address() & 0xFFF00000u);
  auto heap = kernel_state()->memory()->LookupHeap(base_address);
  uint32_t size;
  if (!heap->QuerySizeUnlocked(base_address, &size)) {
    size = 0;
  }

  return size;
}
DECLARE_XBOXKRNL_EXPORT1(MmQueryAllocationSize, kMemory, kImplemented);

dword_result_t xeMmQueryStatistics(
    pointer_t<X_MM_QUERY_STATISTICS_RESULT> stats_ptr) {
  if (!stats_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  const uint32_t size = sizeof(X_MM_QUERY_STATISTICS_RESULT);

  if (stats_ptr->size != size) {
    return X_STATUS_BUFFER_TOO_SMALL;
  }

  // Zero out the struct.
  stats_ptr.Zero();

  // Set the constants the game is likely asking for.
  // These numbers are mostly guessed. If the game is just checking for
  // memory, this should satisfy it. If it's actually verifying things
  // this won't work :/
  stats_ptr->size = size;

  stats_ptr->total_physical_pages = 0x00020000;  // 512mb / 4kb pages
  stats_ptr->kernel_pages = 0x00000100;          // Previous value 0x300

  uint32_t reserved_pages = 0;
  uint32_t unreserved_pages = 0;
  uint32_t used_pages = 0;
  uint32_t reserved_pages_bytes = 0;
  const BaseHeap* physical_heaps[3] = {
      kernel_memory()->LookupHeapByType(true, 0x1000),
      kernel_memory()->LookupHeapByType(true, 0x10000),
      kernel_memory()->LookupHeapByType(true, 0x1000000)};

  kernel_memory()->GetHeapsPageStatsSummary(
      physical_heaps, std::size(physical_heaps), reserved_pages,
      unreserved_pages, used_pages, reserved_pages_bytes);

  assert_true(used_pages < stats_ptr->total_physical_pages);
  stats_ptr->title.available_pages =
      stats_ptr->total_physical_pages - stats_ptr->kernel_pages - used_pages;
  stats_ptr->title.total_virtual_memory_bytes = 0x2FFE0000;
  stats_ptr->title.reserved_virtual_memory_bytes = reserved_pages_bytes;
  stats_ptr->title.physical_pages = 0x00001000;  // TODO(gibbed): FIXME
  stats_ptr->title.pool_pages = 0x00000010;
  stats_ptr->title.stack_pages = 0x00000100;
  stats_ptr->title.image_pages = 0x00000100;
  stats_ptr->title.heap_pages = 0x00000100;
  stats_ptr->title.virtual_pages = 0x00000100;
  stats_ptr->title.page_table_pages = 0x00000100;
  stats_ptr->title.cache_pages = 0x00000100;

  stats_ptr->system.available_pages = 0x00000000;
  stats_ptr->system.total_virtual_memory_bytes = 0x00000000;
  stats_ptr->system.reserved_virtual_memory_bytes = 0x00000000;
  stats_ptr->system.physical_pages = 0x00000000;
  stats_ptr->system.pool_pages = 0x00000000;
  stats_ptr->system.stack_pages = 0x00000000;
  stats_ptr->system.image_pages = 0x00000000;
  stats_ptr->system.heap_pages = 0x00000000;
  stats_ptr->system.virtual_pages = 0x00000000;
  stats_ptr->system.page_table_pages = 0x00000000;
  stats_ptr->system.cache_pages = 0x00000000;

  stats_ptr->highest_physical_page = 0x0001FFFF;

  return X_STATUS_SUCCESS;
}

dword_result_t MmQueryStatistics_entry(
    pointer_t<X_MM_QUERY_STATISTICS_RESULT> stats_ptr) {
  return xeMmQueryStatistics(stats_ptr);
}
DECLARE_XBOXKRNL_EXPORT2(MmQueryStatistics, kMemory, kImplemented,
                         kHighFrequency);

// https://msdn.microsoft.com/en-us/library/windows/hardware/ff554547(v=vs.85).aspx
dword_result_t MmGetPhysicalAddress_entry(dword_t base_address) {
  // PHYSICAL_ADDRESS MmGetPhysicalAddress(
  //   _In_  PVOID BaseAddress
  // );
  // base_address = result of MmAllocatePhysicalMemory.
  uint32_t physical_address = kernel_memory()->GetPhysicalAddress(base_address);
  assert_true(physical_address != UINT32_MAX);
  if (physical_address == UINT32_MAX) {
    physical_address = 0;
  }
  return physical_address;
}
DECLARE_XBOXKRNL_EXPORT1(MmGetPhysicalAddress, kMemory, kImplemented);

dword_result_t MmMapIoSpace_entry(dword_t unk0, lpvoid_t src_address,
                                  dword_t size, dword_t flags) {
  // I've only seen this used to map XMA audio contexts.
  // The code seems fine with taking the src address, so this just returns that.
  // If others start using it there could be problems.
  assert_true(unk0 == 2);
  assert_true(size == 0x40);
  assert_true(flags == 0x404);

  return src_address.guest_address();
}
DECLARE_XBOXKRNL_EXPORT1(MmMapIoSpace, kMemory, kImplemented);

struct X_POOL_ALLOC_HEADER {
  uint8_t unk_0;
  uint8_t unk_1;
  uint8_t unk_2;  // set this to 170
  uint8_t unk_3;
  xe::be<uint32_t> tag;
};

uint32_t xeAllocatePoolTypeWithTag(PPCContext* context, uint32_t size,
                                   uint32_t tag, uint32_t pool_selector) {
  if (size <= 0xFD8) {
    uint32_t adjusted_size = size + sizeof(X_POOL_ALLOC_HEADER);

    uint32_t addr =
        kernel_state()->memory()->SystemHeapAlloc(adjusted_size, 64);
    if (!addr) {
      XELOGE("ExAllocatePool: system heap exhausted for {} bytes (tag {:08X})",
             adjusted_size, tag);
      return 0;
    }

    auto result_ptr = context->TranslateVirtual<X_POOL_ALLOC_HEADER*>(addr);
    result_ptr->unk_2 = 170;
    result_ptr->tag = tag;

    return addr + sizeof(X_POOL_ALLOC_HEADER);
  } else {
    uint32_t addr = kernel_state()->memory()->SystemHeapAlloc(size, 4096);
    if (!addr) {
      XELOGE("ExAllocatePool: system heap exhausted for {} bytes (tag {:08X})",
             size, tag);
    } else {
      XELOGD("ExAllocatePool: {} bytes (tag {:08X}) -> {:08X}", size, tag,
             addr);
    }
    return addr;
  }
}

dword_result_t ExAllocatePoolTypeWithTag_entry(dword_t size, dword_t tag,
                                               dword_t pool_selector,
                                               const ppc_context_t& context) {
  return xeAllocatePoolTypeWithTag(context, size, tag, pool_selector);
}
DECLARE_XBOXKRNL_EXPORT1(ExAllocatePoolTypeWithTag, kMemory, kImplemented);

dword_result_t ExAllocatePoolWithTag_entry(dword_t numbytes, dword_t tag,
                                           const ppc_context_t& context) {
  return xeAllocatePoolTypeWithTag(context, numbytes, tag, 0);
}
DECLARE_XBOXKRNL_EXPORT1(ExAllocatePoolWithTag, kMemory, kImplemented);

dword_result_t ExAllocatePool_entry(dword_t size,
                                    const ppc_context_t& context) {
  constexpr uint32_t none = 0x656E6F4E;  // 'None'
  return xeAllocatePoolTypeWithTag(context, size, none, 0);
}
DECLARE_XBOXKRNL_EXPORT1(ExAllocatePool, kMemory, kImplemented);

void xeFreePool(PPCContext* context, uint32_t base_address) {
  auto memory = context->kernel_state->memory();
  // if 4kb aligned, there is no pool header!
  if ((base_address & (4096 - 1)) == 0) {
    memory->SystemHeapFree(base_address);
  } else {
    memory->SystemHeapFree(base_address - sizeof(X_POOL_ALLOC_HEADER));
  }
}

void ExFreePool_entry(lpvoid_t base_address, const ppc_context_t& context) {
  xeFreePool(context, base_address.guest_address());
}
DECLARE_XBOXKRNL_EXPORT1(ExFreePool, kMemory, kImplemented);

// hv syscall 15, jumps into (bootloader function table??) alternative table ptr
// offset 224
// this is not a correct implementation. i just wanted to get it to return a
// value thats in the same range as the hv's values that kind of reflects the
// pages index and heap
dword_result_t KeGetImagePageTableEntry_entry(dword_t address,
                                              const ppc_context_t& ctx) {
  auto kernel_state = ctx->kernel_state;
  xe::BaseHeap* image_heap = kernel_state->memory()->LookupHeap(address);
  if (image_heap->heap_type() != HeapType::kGuestXex) {
    return 0;
  }
  uint32_t returned_value = address - image_heap->heap_base();

  // todo: its always a power of two, should shift
  returned_value /= image_heap->page_size();

  if (image_heap->page_size() < 65536) {
    returned_value |= 0x40000000;

    // TODO(Gliniak): Verify if 1 is set when page is marked as read-only. For
    // now there is not enough data, but dashboard 14xxx and above requires that
    // return from this call will have bit 0 set.
    returned_value |= 1;
  }

  return returned_value & 0x400FFFFF;  // this is actually the mask it applies
                                       // to the final
  // result before returning it
}
DECLARE_XBOXKRNL_EXPORT1(KeGetImagePageTableEntry, kMemory, kStub);

dword_result_t KeLockL2_entry() {
  // TODO
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeLockL2, kMemory, kStub);

void KeUnlockL2_entry() {}
DECLARE_XBOXKRNL_EXPORT1(KeUnlockL2, kMemory, kStub);

uint32_t xeMmCreateKernelStack(uint32_t stack_size, uint32_t r4) {
  auto stack_size_aligned = (stack_size + 0xFFF) & 0xFFFFF000;
  uint32_t stack_alignment = (stack_size & 0xF000) ? 0x1000 : 0x10000;

  uint32_t stack_address;
  kernel_memory()
      ->LookupHeap(0x70000000)
      ->AllocRange(0x70000000, 0x7F000000, stack_size_aligned, stack_alignment,
                   kMemoryAllocationReserve | kMemoryAllocationCommit,
                   kMemoryProtectRead | kMemoryProtectWrite, false,
                   &stack_address);
  return stack_address + stack_size;
}
dword_result_t MmCreateKernelStack_entry(dword_t stack_size, dword_t r4) {
  return xeMmCreateKernelStack(stack_size, r4);
}
DECLARE_XBOXKRNL_EXPORT1(MmCreateKernelStack, kMemory, kImplemented);

dword_result_t MmDeleteKernelStack_entry(lpvoid_t stack_base,
                                         lpvoid_t stack_end) {
  // Release the stack (where stack_end is the low address)
  if (kernel_memory()->LookupHeap(0x70000000)->Release(stack_end)) {
    return X_STATUS_SUCCESS;
  }

  return X_STATUS_UNSUCCESSFUL;
}
DECLARE_XBOXKRNL_EXPORT1(MmDeleteKernelStack, kMemory, kImplemented);

dword_result_t MmIsAddressValid_entry(dword_t address,
                                      const ppc_context_t& ctx) {
  auto kernel = ctx->kernel_state;
  auto memory = kernel->memory();
  auto heap = memory->LookupHeap(address);
  if (!heap) {
    return 0;
  }

  return heap->QueryRangeAccess(address, address) !=
         memory::PageAccess::kNoAccess;
}

DECLARE_XBOXKRNL_EXPORT1(MmIsAddressValid, kMemory, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Memory);
