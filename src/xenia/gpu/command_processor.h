/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_COMMAND_PROCESSOR_H_
#define XENIA_GPU_COMMAND_PROCESSOR_H_

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/ring_buffer.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/ui/presenter.h"

namespace xe {

class ByteStream;

namespace gpu {

enum class GPUSetting { ClearMemoryPageState, ReadbackMemexport };

enum class ReadbackResolveMode {
  kDisabled,  // No readback (none)
  kFast,      // Delayed sync, 1 frame behind (fast)
  kFull       // Immediate sync with GPU stall (full)
};

// Occlusion queries - ZPD report mode.
enum class ZPDMode {
  kFake,     // Fake sample counts, no real GPU queries (fake)
  kFast,     // Real queries with speculative cached writes (fast)
  kFastAlt,  // Fast queries, but preserves cached zeroes (fast-alt)
  kStrict,   // Real queries, waits before writeback (strict)
};

void SaveGPUSetting(GPUSetting setting, uint64_t value);
bool GetGPUSetting(GPUSetting setting);
ReadbackResolveMode GetReadbackResolveMode();
void SetReadbackResolveMode(const std::string& mode);
ZPDMode GetZPDMode();
void SetZPDMode(const std::string& mode);

// Shared pool capacity for D3D12 and Vulkan.
constexpr uint32_t kZPDQueryPoolCapacity = 8192;

// Contiguous range of query indices for batched resolve/copy operations.
struct ResolveRange {
  uint32_t start;
  uint32_t count;
};

// Backstop for strict mode. Abandon any pending retires after this many polls
// so EVENT_WRITE_ZPD doesn't keep spinning on an unresolved report.
constexpr uint32_t kStrictZPDRetireMaxStalls = 16;
// Clock backstop used for strict retire if guest polling is sparse.
constexpr uint64_t kStrictZPDRetireDeadlineMs = 2;

// Cap for the fast-mode cached delta map.  Games reuse a small set of report
// addresses so this should never be hit, but prevents unbounded growth if a
// title cycles through unique addresses.  Clearing the cache has no
// correctness impact - it only removes speculative writeback hints.
constexpr size_t kFastZPDCacheMaxEntries = 1024;

class GraphicsSystem;
class Shader;

struct SwapState {
  // Lock must be held when changing data in this structure.
  std::mutex mutex;
  // Dimensions of the framebuffer textures. Should match window size.
  uint32_t width = 0;
  uint32_t height = 0;
  // Current front buffer, being drawn to the screen.
  uintptr_t front_buffer_texture = 0;
  // Current back buffer, being updated by the CP.
  uintptr_t back_buffer_texture = 0;
  // Backend data
  void* backend_data = nullptr;
  // Whether the back buffer is dirty and a swap is pending.
  bool pending = false;
};

enum class SwapMode {
  kNormal,
  kIgnored,
};

enum class GammaRampType {
  kUnknown = 0,
  kTable,
  kPWL,
};

// Phase 528: namespace-scope because the base RenderTargetCache holds no
// reference to the command processor, and the RT-identity question has to be
// answered from inside it.
extern bool g_guide_in_draw_scope;
// Phase 536: set while the captured IB is being replayed, so the render target
// cache can report what state the replayed draws actually get - the suspicion
// is that they run against the title's state, not the Guide's.
extern bool g_guide_replaying;

class CommandProcessor {
 public:
  // Phase 528: EDRAM colour clears, counted so the render target cache can bump
  // it. Public because D3D12RenderTargetCache is not a friend of this class.
  // The question is whether a clear falls between the Guide's draws (which land
  // on the frame boundary, phase 527) and the next resolve - if so, its pixels
  // are written and then erased before anything copies them out.
  uint32_t guide_clear_count_ = 0;
  // Phase 528: also public so the render target cache can report which target
  // the Guide's draws are given. Clears are ruled out (0 across all three
  // paths), leaving RT identity as the remaining explanation for pixels that
  // are written and never resolved.
  bool guide_in_draw_scope_ = false;

 protected:
  RingBuffer
      reader_;  // chrispy: instead of having ringbuffer on stack, have it near
                // the start of the class so we can access it via rel8. This
                // also reduces the number of params we need to pass
 public:
  enum class SwapPostEffect {
    kNone,
    kFxaa,
    kFxaaExtreme,
  };

  CommandProcessor(GraphicsSystem* graphics_system,
                   kernel::KernelState* kernel_state);
  virtual ~CommandProcessor();
  uint32_t counter() const { return counter_; }
  void increment_counter() { counter_++; }

  Shader* active_vertex_shader() const { return active_vertex_shader_; }
  Shader* active_pixel_shader() const { return active_pixel_shader_; }

  virtual bool Initialize();
  virtual void Shutdown();

  void CallInThread(std::function<void()> fn);

  virtual void ClearCaches();

  // "Desired" is for the external thread managing the post-processing effect.
  SwapPostEffect GetDesiredSwapPostEffect() const {
    return swap_post_effect_desired_;
  }
  void SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect);
  // Implementations must not make assumptions that the front buffer will
  // necessarily be a resolve destination - it may be a texture generated by any
  // means like written to by the CPU or loaded from a file (the disclaimer
  // screen right in the beginning of 4D530AA4 is not a resolved render target,
  // for instance).
  virtual void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                         uint32_t frontbuffer_height) {}

  // May be called not only from the command processor thread when the command
  // processor is paused, and the termination of this function may be explicitly
  // awaited.
  virtual void InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
      std::function<void()> completion_callback = nullptr);

  virtual void RequestFrameTrace(const std::filesystem::path& root_path);
  virtual void BeginTracing(const std::filesystem::path& root_path);
  virtual void EndTracing();

  virtual void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) = 0;

  void RestoreRegisters(uint32_t first_register,
                        const uint32_t* register_values,
                        uint32_t register_count, bool execute_callbacks);
  void RestoreGammaRamp(
      const reg::DC_LUT_30_COLOR* new_gamma_ramp_256_entry_table,
      const reg::DC_LUT_PWL_DATA* new_gamma_ramp_pwl_rgb,
      uint32_t new_gamma_ramp_rw_component);
  virtual void RestoreEdramSnapshot(const void* snapshot) = 0;

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2);

  void UpdateWritePointer(uint32_t value);

  void LogRegisterSet(uint32_t register_index, uint32_t value);
  void LogRegisterSets(uint32_t base_register_index, const uint32_t* values,
                       uint32_t n_values);

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

 protected:
  struct IndexBufferInfo {
    xenos::IndexFormat format = xenos::IndexFormat::kInt16;
    xenos::Endian endianness = xenos::Endian::kNone;
    uint32_t count = 0;
    uint32_t guest_base = 0;
    size_t length = 0;
  };

  static constexpr uint32_t kReadbackBufferSizeIncrement = 16 * 1024 * 1024;

  // Eviction policy constants for readback buffer cache
  static constexpr size_t kMaxReadbackBuffers = 64;
  static constexpr uint64_t kReadbackBufferEvictionAgeFrames = 60;

  // Progressive alignment for readback buffers to avoid wasting memory
  static inline uint32_t AlignReadbackBufferSize(uint32_t size) {
    if (size < 1 * 1024 * 1024) {
      return xe::align(size, 256u * 1024u);  // 256KB for < 1MB
    } else if (size < 4 * 1024 * 1024) {
      return xe::align(size, 1u * 1024u * 1024u);  // 1MB for < 4MB
    } else {
      return xe::align(size, kReadbackBufferSizeIncrement);  // 16MB for >= 4MB
    }
  }

  // Generate a cache key for a specific resolve operation
  static inline uint64_t MakeReadbackResolveKey(uint32_t address,
                                                uint32_t length) {
    return (uint64_t(address) << 32) | uint64_t(length);
  }

  void WorkerThreadMain();
  virtual bool SetupContext() = 0;
  virtual void ShutdownContext() = 0;
  // rarely needed, most register writes have no special logic here
  XE_NOINLINE
  void HandleSpecialRegisterWrite(uint32_t index, uint32_t value);

  virtual void WriteRegister(uint32_t index, uint32_t value);

  // mem has big-endian register values
  XE_FORCEINLINE
  virtual void WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                     uint32_t num_registers);

  XE_FORCEINLINE
  virtual void WriteRegisterRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                                          uint32_t num_registers);

  XE_NOINLINE
  void WriteOneRegisterFromRing(
      uint32_t base,
      uint32_t
          num_times);  // repeatedly write a value to one register, presumably a
                       // register with special handling for writes

  void WriteALURangeFromRing(xe::RingBuffer* ring, uint32_t base,
                             uint32_t num_times);

  void WriteFetchRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                               uint32_t num_times);

  void WriteBoolRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                              uint32_t num_times);

  void WriteLoopRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                              uint32_t num_times);

  void WriteREGISTERSRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                                   uint32_t num_times);

  void WriteALURangeFromMem(uint32_t start_index, uint32_t* base,
                            uint32_t num_registers);

  void WriteFetchRangeFromMem(uint32_t start_index, uint32_t* base,
                              uint32_t num_registers);

  void WriteBoolRangeFromMem(uint32_t start_index, uint32_t* base,
                             uint32_t num_registers);

  void WriteLoopRangeFromMem(uint32_t start_index, uint32_t* base,
                             uint32_t num_registers);

  void WriteREGISTERSRangeFromMem(uint32_t start_index, uint32_t* base,
                                  uint32_t num_registers);

  const reg::DC_LUT_30_COLOR* gamma_ramp_256_entry_table() const {
    return gamma_ramp_256_entry_table_;
  }
  const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl_rgb() const {
    return gamma_ramp_pwl_rgb_[0];
  }
  virtual void OnGammaRamp256EntryTableValueWritten() {}
  virtual void OnGammaRampPWLValueWritten() {}

  virtual void MakeCoherent();
  virtual void PrepareForWait();
  virtual void ReturnFromWait();

  virtual void PollCompletedSubmission() {}

  // Used by strict ZPD to distinguish normal in flight latency from a
  // genuinely stuck report.
  virtual uint64_t GetCompletedSubmission() const { return 0; }

  virtual void OnPrimaryBufferEnd() {}

  // TODO(boma): Add tracking for EVENT_WRITE_EXT reports.
  using ReportHandle = uint64_t;
  static constexpr ReportHandle kInvalidReportHandle = 0;

  enum class QueryOpenResult {
    kOpened,
    kDeferred,
    kPoolExhausted,
    kFailed,
  };

  // One active guest report slot. May span multiple host query segments split
  // across submissions or render passes, final value is the normalized sum.
  struct ZPDReport {
    // Guest sample count. Each segment is normalized by its own scale area
    // when it resolves.
    uint64_t accumulated_samples = 0;
    // Submission of the first closed segment.
    uint64_t first_segment_end_submission = 0;
    // Submission containing the most recently closed segment's resolve.
    uint64_t last_segment_end_submission = 0;
    uint64_t slot_sequence_id = 0;
    uint32_t slot_base = 0;
    uint32_t begin_record = 0;
    uint32_t end_record = 0;
    // Snapshotted at BEGIN from zpd_slot_values_.
    uint32_t begin_value = 0;
    uint32_t pending_segments = 0;
    // Last known delta. Carried forward on forced close so slot doesn't
    // briefly look fully occluded. 0 is a valid delta for alternate fast path.
    uint32_t cached_delta = 0;
    bool has_cached_delta = false;
    bool ended = false;
  };

  // Currently open guest lifetime. Retired reports are tracked separately
  // by handle until their query segments resolve. This intentionally models
  // only one logical report at a time. That's enough for conventional ZPD
  // reports, but QueryBatch can have multiple slots in flight, so it doesn't
  // fit this layout. Eventually this probably wants to become something more
  // like a map of active reports keyed by slot and sequence instead.
  struct ActiveZPDSegment {
    ReportHandle report_handle = kInvalidReportHandle;
    uint32_t slot_base = 0;
    uint32_t begin_record = 0;
    uint32_t end_record = 0;
    uint32_t scale_area = 0;
    bool segment_active = false;
    bool segment_pending_begin = false;
    bool logical_active = false;
  };

  struct PendingZPDSlot {
    ReportHandle report_handle = kInvalidReportHandle;
    uint32_t cached_delta = 0;
    bool has_cached_delta = false;
  };

  virtual void EnsureZPDQueryResources() {}
  virtual void ShutdownZPDQueryResources() {}

  virtual bool IsZPDQueryPoolReady() const { return false; }
  virtual bool CanOpenZPDQuery() const { return true; }

  // Backend acquires a pool slot, records BeginQuery, tracks it internally.
  virtual QueryOpenResult OpenZPDQuery(ReportHandle report_handle,
                                       bool can_close_submission) {
    return QueryOpenResult::kFailed;
  }
  // Backend records EndQuery, queues a resolve for the active slot.
  virtual bool CloseZPDQuery(ReportHandle report_handle,
                             uint64_t& out_submission) {
    return false;
  }
  // Backend discards the active query without resolving.
  virtual bool DiscardZPDQuery() { return false; }

  // Backend drains completed resolves and calls OnZPDQueryResolved for each.
  virtual void PumpQueryResolves() {}
  // Backend waits for all pending segments of report_handle to resolve.
  virtual bool AwaitQueryResolve(ReportHandle report_handle,
                                 uint64_t wait_for_submission) {
    return false;
  }

  bool BeginZPDReport(uint32_t report_address);
  bool EndZPDReport(uint32_t report_address, bool guest_forced_end);
  // Opens a new host query segment when CanOpenZPDQuery is true.
  void OpenQuerySegment(bool can_close_submission);
  // Closes the current segment at a submission or render pass boundary.
  // The logical report stays open and a new segment will open at the next
  // opportunity.
  void CloseQuerySegment();
  // Splits the open segment when the draw scale changes so each segment
  // normalizes with one scale.
  void UpdateZPDScale(uint32_t scale_area);

  // Called by backends when a host query resolve completes.  Accumulates
  // the normalized sample count, and if all segments are done, commits the
  // report to guest memory.
  void OnZPDQueryResolved(ReportHandle report_handle, uint64_t raw_samples,
                          uint32_t scale_area);

  // Writes guest report with begin_value read from guest memory.
  // Orphan END path only when no controller snapshot is available.
  void WriteZPDReport(uint32_t begin_record, uint32_t end_record,
                      uint32_t begin_value, uint32_t delta_value,
                      bool write_begin_record);

  // Called from PrepareForWait so strict mode can retire before guest loops
  // again. Gives up after kStrictZPDRetireMaxStalls.
  void PumpPendingRetire();

  // Divides a segment's host count by the scale area it ran under.
  static uint32_t NormalizeSampleCount(uint64_t samples, uint32_t scale_area);

  // Writes the final report to guest memory and advances the slot running
  // total.  Called when a report fully resolves or is abandoned.
  void CommitZPDReport(ZPDReport& report, uint32_t delta_value);
  // Checks that the report's slot sequence is still current (not reused).
  bool IsZPDReportCurrent(const ZPDReport& report) const;
  PendingZPDSlot GetPendingZPDSlot(uint32_t slot_base,
                                   uint32_t end_record) const;

  void ResetZPDState() {
    zpd_active_segment_ = {};
    zpd_next_report_handle_ = 1;
    zpd_slot_sequences_.clear();
    zpd_slot_values_.clear();
    logical_zpd_reports_.clear();
    fast_zpd_report_cached_values_.clear();
    fake_zpd_sample_count_ = 0;
    querybatch_zpd_sample_count_ = UINT32_MAX;
    zpd_pending_retire_handle_ = kInvalidReportHandle;
    zpd_pending_retire_stalls_ = 0;
    zpd_pending_retire_start_ms_ = 0;
    zpd_force_fake_fallback_ = false;
  }

#include "pm4_command_processor_declare.h"

 public:
  // Experimental entry point for running a command buffer the guest built but
  // never submitted - the Guide's case, where xam writes a real PM4 stream
  // into its own buffers and nothing consumes them. ExecuteIndirectBuffer is
  // protected and normally only reached from packet dispatch on the command
  // processor thread; calling this from elsewhere is NOT thread safe and is
  // for investigation only.
  // Counts draw packets actually dispatched. Register checksums proved a
  // poor witness - they under-report writes that rewrite an existing value -
  // whereas a draw either dispatches or it does not.
  uint32_t guide_draw_count_ = 0;
  // Phase 525: draws seen by the GPU thread while the scope flag is set.
  // The flag is set on the TITLE thread around the Guide's Execute, but
  // packets are consumed asynchronously here, so a delta of
  // guide_draw_count_ across that window can be the title's draws. If this
  // counter stays near zero while the delta does not, the 2071 draws that
  // phases 520-522 attributed to the Guide were never the Guide's.
  uint32_t guide_scoped_draws_ = 0;
  // Phase 522: ordering instrumentation. The Guide's geometry reaches the
  // primary EDRAM target (phase 521), but EDRAM is only visible once it is
  // resolved. If the composite draws land after the frame's resolve they
  // are cleared unseen, which fits every observation so far. Count resolves
  // and swaps so the sequence can be read against the draw count.
  uint32_t guide_resolve_count_ = 0;
  uint32_t guide_swap_count_ = 0;
  uint32_t guide_draws_at_last_swap_ = 0;
  bool guide_burst_pending_ = false;  // phase 530
  // Phase 532: the Guide's pixels reach the presented buffer and are then
  // overwritten by the title's own resolve (phase 531). With
  // --readback_resolve=full the buffer lives in guest memory, so the Guide's
  // contribution can be captured as a diff at resolve time and re-applied after
  // the title's resolve, just before the swap. Cheaper and far less risky than
  // replaying ring packets, and it answers the visibility question directly.
  std::vector<uint32_t> guide_fb_before_;
  std::vector<uint32_t> guide_fb_after_;
  uint32_t guide_fb_dest_ = 0;
  bool guide_fb_valid_ = false;
  // Phase 533: the proper fix. Record the ring range of the Guide's burst so it
  // can be replayed at the top of the title's IssueCopy, putting the geometry in
  // EDRAM at the moment the title resolves - rather than compositing stale
  // pixels in CPU memory afterwards.
  // Phase 534: the Guide's packets run from an indirect buffer, so the range is
  // the IB's own address and length (both carried by PM4_INDIRECT_BUFFER), not
  // an offset into the primary ring - phase 533's arithmetic mixed the two.
  bool guide_ib_had_draw_ = false;
  uint32_t guide_replay_start_ = 0;
  uint32_t guide_replay_addr_ = 0;
  uint32_t guide_replay_words_ = 0;
  bool guide_replay_armed_ = false;
  bool guide_replaying_ = false;
  bool guide_prev_scope_ = false;
  // Phase 534: the indirect buffer is transient - replaying it at resolve time
  // faulted reading its own address, so the guest had already reclaimed it.
  // Copy the packets into a persistent scratch buffer at capture time and
  // replay from there.
  uint32_t guide_replay_scratch_ = 0;
  uint32_t guide_replay_scratch_size_ = 0;
  // Phase 535: guard for the within-frame A/B - resolve once without the
  // Guide's geometry, once with, and compare. Answers "does the Guide change
  // the resolved frame" without needing to compare across runs of an animating
  // game, and without needing eyes.
  bool guide_in_ab_ = false;
  // Phase 554: the replay currently runs inside IssueCopy - during a resolve,
  // which is a copy rather than a render pass, so the render target may already
  // be transitioned and correctly-configured draws would still not land in the
  // surface the resolve reads. This drives the replay from a title draw
  // instead, where a render pass is certainly active.
  bool guide_frame_needs_replay_ = false;
  // Phase 538: the replayed draws fetch geometry through constants the replay
  // never writes - it inherits the burst's values with the valid bit cleared
  // (phase 537). Snapshot the whole fetch block during the burst and restore it
  // around the replay, the same save/restore already used for the resolve state.
  // 0x4800..0x48BF is SHADER_CONSTANT_FETCH_00_0 through _31_5, 192 registers.
  uint32_t guide_fetch_[0xC0] = {};
  bool guide_fetch_saved_ = false;
  // Phase 523: the resolve rectangle lives in vertex-fetch slot 0 (a D3D9
  // hack GetResolveInfo depends on) and the copy destination in RB_COPY_*.
  // The Guide's own draws overwrite vf0, so a second IssueCopy after them
  // fails with "Unsupported resolve vertex buffer format" - 3479 times in
  // one run. Snapshot the state at the title's resolve and restore it for
  // the extra one.
  bool guide_resolve_saved_ = false;
  bool guide_resolve_replay_ = false;
  uint32_t guide_saved_copy_[4] = {};    // RB_COPY_CONTROL..DEST_INFO
  uint32_t guide_saved_vf0_[2] = {};     // SHADER_CONSTANT_FETCH_00_0/_1
  uint32_t guide_saved_surface_[2] = {}; // RB_SURFACE_INFO, RB_COLOR_INFO

  // Ring-buffer state, for checking what xam's mode-1 device creator
  // takes from a running title: mode 1 reaches VdInitializeRingBuffer
  // and the title stops swapping at the button press.
  struct GuideRing {
    uint32_t ptr, size, wb, rptr, freq;
  };
  GuideRing GuideRingSave() {
    return {primary_buffer_ptr_, primary_buffer_size_, read_ptr_writeback_ptr_,
            read_ptr_index_, read_ptr_update_freq_};
  }
  // Restores the ring registers WITHOUT the memset InitializeRingBuffer does:
  // the point is to hand the GPU back the title's existing ring with its
  // contents and read position intact, not a cleared one.
  void GuideRingRestore(const GuideRing& r) {
    primary_buffer_ptr_ = r.ptr;
    primary_buffer_size_ = r.size;
    read_ptr_writeback_ptr_ = r.wb;
    read_ptr_index_ = r.rptr;
    read_ptr_update_freq_ = r.freq;
  }
  void GuideRingState(uint32_t* ptr, uint32_t* size, uint32_t* wb) {
    *ptr = primary_buffer_ptr_;
    *size = primary_buffer_size_;
    *wb = read_ptr_writeback_ptr_;
  }

  void ExecuteGuestBufferUnsafe(uint32_t ptr, uint32_t count) {
    ExecuteIndirectBuffer(ptr, count);
  }

  // A command stream the Guide has finished building, to be run on the GPU
  // thread just before the title's next swap. Set by the Guide draw hook on
  // the title thread; consumed and cleared in ExecutePacketType3_XE_SWAP.
  //
  // Running it from the title thread - which is what every experiment up to
  // now did - drives the command processor from the wrong thread while the
  // GPU thread may be mid-packet, and lands the work at an arbitrary point in
  // the title's frame. An overlay wants the opposite: the title's frame fully
  // built, its render target still bound, and the Guide's geometry drawn on
  // top immediately before present. The swap packet is exactly that point.
  volatile uint32_t guide_overlay_ptr_ = 0;
  volatile uint32_t guide_overlay_words_ = 0;

  // Execute a PM4 stream that lives in the guest VIRTUAL address space.
  //
  // ExecuteIndirectBuffer resolves its pointer with TranslatePhysical, which
  // is `physical_membase_ + (addr & 0x1FFFFFFF)`. That is correct for a real
  // indirect buffer, whose address always comes out of physical memory. The
  // Guide's command buffer does not: xam allocates it through 81A01358 and it
  // lands at 3009C000, inside the 4KB-page virtual heap, which is a separate
  // host mapping. Masking that address yields physical 1009C000 - unrelated
  // memory that happens to be zeroed - so every attempt to run the Guide's
  // stream so far has parsed zeros and correctly reported nothing.
  void ExecuteGuestBufferVirtualUnsafe(uint32_t ptr, uint32_t count) {
    if (!count) {
      return;
    }
    RingBuffer old_reader = reader_;
    new (&reader_) RingBuffer(memory_->TranslateVirtual(ptr),
                              count * sizeof(uint32_t));
    reader_.set_write_offset(count * sizeof(uint32_t));
    do {
      if (!ExecutePacket()) {
        XELOGE("**** GUIDE BUFFER: failed to execute packet.");
        break;
      }
    } while (reader_.read_count());
    reader_ = old_reader;
  }

 protected:

  virtual Shader* LoadShader(xenos::ShaderType shader_type,
                             uint32_t guest_address,
                             const uint32_t* host_address,
                             uint32_t dword_count) {
    return nullptr;
  }

  virtual bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                         IndexBufferInfo* index_buffer_info,
                         bool major_mode_explicit) {
    return false;
  }
  virtual bool IssueCopy() { return false; }

  // "Actual" is for the command processor thread, to be read by the
  // implementations.
  SwapPostEffect GetActualSwapPostEffect() const {
    return swap_post_effect_actual_;
  }

  virtual void InitializeTrace();

  Memory* memory_ = nullptr;
  kernel::KernelState* kernel_state_ = nullptr;
  GraphicsSystem* graphics_system_ = nullptr;
  RegisterFile* XE_RESTRICT register_file_ = nullptr;

  ReportHandle zpd_next_report_handle_ = 1;
  std::unordered_map<uint32_t, uint64_t> zpd_slot_sequences_;
  std::unordered_map<uint32_t, uint32_t> zpd_slot_values_;
  std::unordered_map<ReportHandle, ZPDReport> logical_zpd_reports_;
  ActiveZPDSegment zpd_active_segment_{};

  // Cached delta per END.
  // Fast mode uses this for speculative writeback and orphaned END replay.
  std::unordered_map<uint32_t, uint32_t> fast_zpd_report_cached_values_;

  uint32_t querybatch_zpd_sample_count_ = UINT32_MAX;
  bool zpd_force_fake_fallback_ = false;

  // Strict mode defers guest completion until the queued END has retired.
  ReportHandle zpd_pending_retire_handle_ = kInvalidReportHandle;
  uint32_t zpd_pending_retire_stalls_ = 0;
  // Uptime in ms when zpd_pending_retire_handle_ was first set.
  uint64_t zpd_pending_retire_start_ms_ = 0;

  // Set by the backend when resolution scale changes.
  uint32_t zpd_draw_resolution_scale_x_ = 1;
  uint32_t zpd_draw_resolution_scale_y_ = 1;

  uint32_t zpd_draw_resolution_scale_x() const {
    return zpd_draw_resolution_scale_x_;
  }
  uint32_t zpd_draw_resolution_scale_y() const {
    return zpd_draw_resolution_scale_y_;
  }
  // Scale area for the segment being closed.
  uint32_t GetZPDScaleArea() const {
    return zpd_active_segment_.scale_area
               ? zpd_active_segment_.scale_area
               : zpd_draw_resolution_scale_x_ * zpd_draw_resolution_scale_y_;
  }

  uint32_t fake_zpd_sample_count_ = 0;

  TraceWriter trace_writer_;
  enum class TraceState {
    kDisabled,
    kStreaming,
    kSingleFrame,
  };
  TraceState trace_state_ = TraceState::kDisabled;
  std::filesystem::path trace_stream_path_;
  std::filesystem::path trace_frame_path_;

  std::atomic<bool> worker_running_;
  kernel::object_ref<kernel::XHostThread> worker_thread_;

  std::queue<std::function<void()>> pending_fns_;

  // MicroEngine binary from PM4_ME_INIT
  std::vector<uint32_t> me_bin_;

  uint32_t counter_ = 0;

  uint32_t primary_buffer_ptr_ = 0;
  uint32_t primary_buffer_size_ = 0;

  uint32_t read_ptr_index_ = 0;
  uint32_t read_ptr_update_freq_ = 0;
  uint32_t read_ptr_writeback_ptr_ = 0;

  std::unique_ptr<xe::threading::Event> write_ptr_index_event_;
  std::atomic<uint32_t> write_ptr_index_;

  uint64_t bin_select_ = 0xFFFFFFFFull;
  uint64_t bin_mask_ = 0xFFFFFFFFull;

  Shader* active_vertex_shader_ = nullptr;
  Shader* active_pixel_shader_ = nullptr;

  bool paused_ = false;

  // By default (such as for tools), post-processing is disabled.
  // "Desired" is for the external thread managing the post-processing effect.
  SwapPostEffect swap_post_effect_desired_ = SwapPostEffect::kNone;
  SwapPostEffect swap_post_effect_actual_ = SwapPostEffect::kNone;

 private:
  reg::DC_LUT_30_COLOR gamma_ramp_256_entry_table_[256] = {};
  reg::DC_LUT_PWL_DATA gamma_ramp_pwl_rgb_[128][3] = {};
  uint32_t gamma_ramp_rw_component_ = 0;

  XE_NOINLINE XE_COLD void LogKickoffInitator(uint32_t value);
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_COMMAND_PROCESSOR_H_
