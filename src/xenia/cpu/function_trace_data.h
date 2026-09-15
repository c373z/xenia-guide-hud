/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_FUNCTION_TRACE_DATA_H_
#define XENIA_CPU_FUNCTION_TRACE_DATA_H_

#include <cstdint>
#include <cstring>

#include "xenia/base/memory.h"

namespace xe {
namespace cpu {

class FunctionTraceData {
 public:
  static const int kFunctionCallerHistoryCount = 4;

  struct Header {
    // Format is used by tooling, changes must be made across all targets.
    // + 0   4b  (data size)
    // + 4   4b  start_address
    // + 8   4b  end_address
    // +12   4b  type (user, external, etc)
    // +16   8b  function_thread_use  // bitmask of thread id
    // +24   8b  function_call_count
    // +32   4b+ function_caller_history[4]
    // +48   8b+ instruction_execute_count[instruction count]
    uint32_t data_size;
    uint32_t start_address;
    uint32_t end_address;
    uint32_t type;
    uint64_t function_thread_use;
    uint64_t function_call_count;
    uint32_t function_caller_history[kFunctionCallerHistoryCount];
    // Phase 1096cx: the low 32 bits of guest r3 at each entry, same slot
    // indexing as function_caller_history. Added to answer "which message id
    // does the pump 81780460 actually receive", which needs an ARGUMENT and
    // not a caller. Everything sizes the payload with sizeof(Header), so this
    // is internally safe; external readers of a written xtrace.bin would need
    // the same change.
    uint32_t function_arg_history[kFunctionCallerHistoryCount];
    // Phase 1096dr: r4 as well. 8177F588 (xam's sys-app invoke) runs 13 times
    // and the id that distinguishes "open the Guide" from the other twelve is
    // its SECOND argument, built at 8177F808 and passed to 817610E8.
    uint32_t function_arg2_history[kFunctionCallerHistoryCount];
    // Phase 1096ej: value-at-instruction capture. Phase 1096 needed "what
    // value does THIS instruction see" three separate times (1096dw, 1096ea,
    // 1096ei) and had no probe for it, so each time it reasoned from execution
    // counts instead - and twice that reasoning was later contradicted. These
    // hold the last 8 values of a chosen register at a chosen instruction.
    uint32_t insn_value_history[8];
    uint32_t insn_value_count;
    // Phase 1097: the eight-slot ring above is a SAMPLE, and reading it as a
    // complete list is a mistake this project has now been positioned to make
    // twice (a 606-call census printed its last eight values and said nothing
    // about whether message 0x24 was ever sent). These answer the exact
    // question "did this register ever hold THIS value here", with no cap:
    // a count, and the last four link registers at a match, which name the
    // caller that produced it.
    uint32_t insn_match_count;
    uint32_t insn_match_lr[4];
    // Phase 1097: and the whole small-value census in one run. Answering
    // "which message ids arrive here" one id per run is what made the 0x24 /
    // 0x2A / 0x0F walk cost four runs; every id of interest in XUI's space is
    // below 256, so a bucket per value settles the question in one. Values
    // >= 256 are simply not counted - insn_value_history still samples those.
    uint32_t insn_value_hist[256];
    // uint64_t instruction_execute_count[];
  };

  FunctionTraceData() : header_(nullptr) {}

  void Reset(uint8_t* trace_data, size_t trace_data_size,
             uint32_t start_address, uint32_t end_address) {
    header_ = reinterpret_cast<Header*>(trace_data);
    header_->data_size = uint32_t(trace_data_size);
    header_->start_address = start_address;
    header_->end_address = end_address;
    header_->type = 0;
    header_->function_thread_use = 0;
    header_->function_call_count = 0;
    for (int i = 0; i < kFunctionCallerHistoryCount; ++i) {
      header_->function_caller_history[i] = 0;
      header_->function_arg_history[i] = 0;
      header_->function_arg2_history[i] = 0;
    }
    for (int i = 0; i < 8; ++i) {
      header_->insn_value_history[i] = 0;
    }
    header_->insn_value_count = 0;
    header_->insn_match_count = 0;
    for (int i = 0; i < 4; ++i) {
      header_->insn_match_lr[i] = 0;
    }
    for (int i = 0; i < 256; ++i) {
      header_->insn_value_hist[i] = 0;
    }
    // Clear any remaining.
    std::memset(trace_data + sizeof(Header), 0,
                trace_data_size - sizeof(Header));
  }

  bool is_valid() const { return header_ != nullptr; }

  uint32_t start_address() const { return header_->start_address; }
  uint32_t end_address() const { return header_->end_address; }
  uint32_t instruction_count() const {
    return (header_->end_address - header_->start_address) / 4 + 1;
  }

  Header* header() const { return header_; }

  uint8_t* instruction_execute_counts() const {
    return reinterpret_cast<uint8_t*>(header_) + sizeof(Header);
  }

  static size_t SizeOfHeader() { return sizeof(Header); }

  static size_t SizeOfInstructionCounts(uint32_t start_address,
                                        uint32_t end_address) {
    uint32_t instruction_count = (end_address - start_address) / 4 + 1;
    return instruction_count * 8;
  }

 private:
  Header* header_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_FUNCTION_TRACE_DATA_H_
