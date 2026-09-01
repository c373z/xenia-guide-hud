#pragma once

#if !defined(NDEBUG)
#define XE_ENABLE_PM4_DISASM 1
#endif

using namespace xe::gpu::xenos;
void COMMAND_PROCESSOR::ExecuteIndirectBuffer(uint32_t ptr,
                                              uint32_t count) XE_RESTRICT {
  SCOPE_profile_cpu_f("gpu");

  trace_writer_.WriteIndirectBufferStart(ptr, count * sizeof(uint32_t));
  if (count != 0) {
    RingBuffer old_reader = reader_;

    // Execute commands!
    new (&reader_)
        RingBuffer(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
    reader_.set_write_offset(count * sizeof(uint32_t));
    // prefetch the wraparound range
    // it likely is already in L3 cache, but in a zen system it may be another
    // chiplets l3
    reader_.BeginPrefetchedRead<swcache::PrefetchTag::Level2>(
        COMMAND_PROCESSOR::GetCurrentRingReadCount());
    // Phase 635: the worker blocks inside an indirect buffer dispatched from
    // xam's ring. Log each packet header for buffers outside the title's
    // regions, so the last line printed is the packet that did not return.
    const bool guide_ib_watch =
        cvars::guide_cp_probe && (ptr & 0xFF000000u) != 0x1F000000u;
    uint32_t guide_ib_pkt = 0;
    do {
      if (guide_ib_watch && guide_ib_pkt < 32) {
        ++guide_ib_pkt;
        uint32_t off = uint32_t(reader_.read_offset());
        uint32_t hdr = xe::load_and_swap<uint32_t>(
            memory_->TranslatePhysical(ptr + off));
        XELOGI("CPIBPkt {:08X} #{}: off={:X} hdr={:08X} type={} op={:02X}",
               ptr, guide_ib_pkt, off, hdr, hdr >> 30,
               (hdr >> 30) == 3 ? ((hdr >> 8) & 0x7F) : 0);
      }
      if (COMMAND_PROCESSOR::ExecutePacket()) {
        continue;
      } else {
        // Return up a level if we encounter a bad packet.
        XELOGE("**** INDIRECT RINGBUFFER: Failed to execute packet.");
        assert_always();
        break;
      }
    } while (reader_.read_count());

    trace_writer_.WriteIndirectBufferEnd();
    reader_ = old_reader;
  } else {
    // rare, but i've seen it happen! (and then a division by 0 occurs)
    return;
  }
}
XE_NOINLINE
static void LOGU32s(logging::LoggerBatch<LogLevel::Debug>& logger,
                    const std::vector<uint32_t>& values) {
  bool first = true;
#if 0
  XELOGD("[ ");
  for (auto&& val : values) {
    if (first) {
      XELOGD("0x{:08X}", val);
      first = false;
    } else {
      XELOGD(", 0x{:08X}", val);
    }
  }
  XELOGD(" ]");
#else

  for (auto&& val : values) {
    if (first) {
      logger("0x{:08X}", val);
      first = false;
    } else {
      logger(", 0x{:08X}", val);
    }
  }
#endif
}

std::string GenerateRegnameForPm4Print(uint32_t reg) {
  auto reg_info = RegisterFile::GetRegisterInfo(reg);

  if (reg_info) {
    return reg_info->name;
  } else {
    return fmt::format("Unknown_Reg_{:04X}", reg);
  }
}

XE_NOINLINE
void COMMAND_PROCESSOR::DisassembleCurrentPacket() XE_RESTRICT {
  xe::gpu::PacketInfo packet_info;

  logging::LoggerBatch<LogLevel::Debug> logger{};

  if (PacketDisassembler::DisasmPacket(reader_.buffer() + reader_.read_offset(),
                                       &packet_info)) {
    logger("CP - {}, count {}, predicated = {}\n", packet_info.type_info->name,
           packet_info.count, packet_info.predicated);

#define LOG_ACTION_FIELD(__type, name) \
  logger("\t" #name " = {:08X}\n", static_cast<uint32_t>(action.__type.name))
#define LOG_ACTION_FIELD_DEC(__type, name) \
  logger("\t" #name " = {}\n", static_cast<size_t>(action.__type.name))

#define LOG_ENDIANNESS(__type, name) \
  logger("\t" #name " = {}\n",       \
         xenos::GetEndianEnglishDescription(action.__type.name))
#define LOG_PRIMTYPE(__type, name) \
  logger("\t" #name " = {}\n",     \
         xenos::GetPrimitiveTypeEnglishDescription(action.__type.name))
    for (auto&& action : packet_info.actions) {
      using PType = PacketAction::Type;
      switch (action.type) {
        case PType::kRegisterWrite:
          break;
        case PType::kSetBinMask:
          logger("\tSetBinMask {}\n", action.set_bin_mask.value);
          break;
        case PType::kSetBinSelect:
          logger("\tSetBinSelect {}\n", action.set_bin_select.value);
          break;
        case PType::kMeInit:
          logger("\tMeInit - ");
          LOGU32s(logger, action.words);
          logger("\n");
          break;
        case PType::kGenInterrupt:
          logger("\tGenInterrupt for cpu mask {:04X}\n",
                 action.gen_interrupt.cpu_mask);
          break;
        case PType::kSetBinMaskHi:
        case PType::kSetBinMaskLo:
        case PType::kSetBinSelectHi:
        case PType::kSetBinSelectLo:
          LOG_ACTION_FIELD(lohi_op, value);
          break;
        case PType::kWaitRegMem:
          LOG_ACTION_FIELD(wait_reg_mem, wait_info);
          LOG_ACTION_FIELD(wait_reg_mem, poll_reg_addr);
          LOG_ACTION_FIELD(wait_reg_mem, ref);
          LOG_ACTION_FIELD(wait_reg_mem, mask);
          LOG_ACTION_FIELD(wait_reg_mem, wait);
          break;
        case PType::kRegRmw: {
          uint32_t rmw_info = action.reg_rmw.rmw_info;
          uint32_t and_mask = action.reg_rmw.and_mask;
          uint32_t or_mask = action.reg_rmw.or_mask;

          uint32_t and_mask_is_reg = (rmw_info >> 31) & 0x1;
          uint32_t or_mask_is_reg = (rmw_info >> 30) & 0x1;

          std::string and_mask_str;

          if (and_mask_is_reg) {
            and_mask_str = GenerateRegnameForPm4Print(and_mask & 0x1FFF);
          } else {
            and_mask_str = fmt::format("0x{:08X}", and_mask);
          }
          std::string or_mask_str;

          if (or_mask_is_reg) {
            or_mask_str = GenerateRegnameForPm4Print(or_mask & 0x1FFF);
          } else {
            or_mask_str = fmt::format("0x{:08X}", or_mask);
          }

          std::string dest = GenerateRegnameForPm4Print(rmw_info & 0x1FFF);

          logger("\t{} = ({} & {}) | {}\n", dest, dest, and_mask_str,
                 or_mask_str);
          LOG_ACTION_FIELD(reg_rmw, rmw_info);
          LOG_ACTION_FIELD(reg_rmw, and_mask);
          LOG_ACTION_FIELD(reg_rmw, or_mask);

          break;
        }
        case PType::kCondWrite:
          LOG_ACTION_FIELD(cond_write, wait_info);
          LOG_ACTION_FIELD(cond_write, poll_reg_addr);
          LOG_ACTION_FIELD(cond_write, ref);
          LOG_ACTION_FIELD(cond_write, mask);
          LOG_ACTION_FIELD(cond_write, write_reg_addr);
          LOG_ACTION_FIELD(cond_write, write_data);
          break;

        case PType::kEventWrite:
          LOG_ACTION_FIELD(event_write, initiator);
          break;
        case PType::kEventWriteSHD:
          LOG_ACTION_FIELD(event_write_shd, initiator);
          LOG_ACTION_FIELD(event_write_shd, address);
          LOG_ACTION_FIELD(event_write_shd, value);
          break;
        case PType::kEventWriteExt:
          LOG_ACTION_FIELD(event_write_ext, unk0);
          LOG_ACTION_FIELD(event_write_ext, unk1);
          break;
        case PType::kDrawIndx:
          LOG_ACTION_FIELD(draw_indx, dword0);
          LOG_ACTION_FIELD(draw_indx, dword1);
          LOG_ACTION_FIELD_DEC(draw_indx, index_count);
          LOG_PRIMTYPE(draw_indx, prim_type);
          LOG_ACTION_FIELD(draw_indx, src_sel);
          LOG_ACTION_FIELD(draw_indx, guest_base);
          LOG_ACTION_FIELD_DEC(draw_indx, index_size);
          LOG_ENDIANNESS(draw_indx, endianness);
          break;
        case PType::kDrawIndx2:
          LOG_ACTION_FIELD(draw_indx2, dword0);
          LOG_ACTION_FIELD_DEC(draw_indx2, index_count);
          LOG_PRIMTYPE(draw_indx2, prim_type);
          LOG_ACTION_FIELD(draw_indx2, src_sel);
          LOG_ACTION_FIELD_DEC(draw_indx2, indices_size);
          logger("Indices = ");
          LOGU32s(logger, action.words);
          logger("\n");
          break;
        case PType::kInvalidateState:
          LOG_ACTION_FIELD(invalidate_state, state_mask);
          break;
        case PType::kImLoad:
          LOG_ACTION_FIELD(im_load, shader_type);
          LOG_ACTION_FIELD(im_load, addr);
          LOG_ACTION_FIELD(im_load, start);
          LOG_ACTION_FIELD_DEC(im_load, size_dwords);
          break;
        case PType::kImLoadImmediate:
          LOG_ACTION_FIELD_DEC(im_load_imm, shader_type);
          LOG_ACTION_FIELD(im_load_imm, start);
          LOG_ACTION_FIELD_DEC(im_load_imm, size_dwords);
          logger("Shader instruction words = ");
          LOGU32s(logger, action.words);
          logger("\n");
          break;
        case PType::kWaitForIdle:
          LOG_ACTION_FIELD(wait_for_idle, probably_unused);
          break;
        case PType::kContextUpdate:
          LOG_ACTION_FIELD(context_update, maybe_unused);
          break;
        case PType::kVizQuery:
          LOG_ACTION_FIELD_DEC(vizquery, id);
          LOG_ACTION_FIELD_DEC(vizquery, end);
          LOG_ACTION_FIELD(vizquery, dword0);
          break;
        case PType::kEventWriteZPD:
          LOG_ACTION_FIELD(event_write_zpd, initiator);

          break;
        case PType::kMemWrite:
          LOG_ACTION_FIELD(mem_write, addr);
          LOG_ENDIANNESS(mem_write, endianness);
          logger("Values to write (with GpuSwap pre-applied) = ");
          LOGU32s(logger, action.words);
          logger("\n");
          break;
        case PType::kRegToMem:
          logger("{}\n", GenerateRegnameForPm4Print(action.reg2mem.reg_addr));
          LOG_ACTION_FIELD(reg2mem, mem_addr);
          LOG_ENDIANNESS(reg2mem, endianness);
          break;
        case PType::kIndirBuffer:
          LOG_ACTION_FIELD(indir_buffer, list_ptr);
          LOG_ACTION_FIELD_DEC(indir_buffer, list_length);
          break;
        case PType::kXeSwap:
          LOG_ACTION_FIELD(xe_swap, frontbuffer_ptr);
          break;
      }
    }
  } else {
    logger("Unknown packet! Failed to disassemble.\n");
  }
  logger.submit('d');
}
bool COMMAND_PROCESSOR::ExecutePacket() {
#if XE_ENABLE_PM4_DISASM == 1
  if (cvars::disassemble_pm4 && logging::ShouldLog(LogLevel::Debug)) {
    COMMAND_PROCESSOR::DisassembleCurrentPacket();
  }
#endif
  const uint32_t packet = reader_.ReadAndSwap<uint32_t>();
  const uint32_t packet_type = packet >> 30;

  XE_LIKELY_IF(packet && packet != 0x0BADF00D) {
    XE_LIKELY_IF((packet != 0xCDCDCDCD)) {
    actually_execute_packet:
      // chrispy: reorder checks by probability
      XE_LIKELY_IF(packet_type == 3) {
        return COMMAND_PROCESSOR::ExecutePacketType3(packet);
      }
      else {
        if (packet_type ==
            0) {  // dont know whether 0 or 1 are the next most frequent
          return COMMAND_PROCESSOR::ExecutePacketType0(packet);
        } else {
          if (packet_type == 1) {
            return COMMAND_PROCESSOR::ExecutePacketType1(packet);
          } else {
            // originally there was a default case that msvc couldn't optimize
            // away because it doesnt have value range analysis but in reality
            // there is no default, a uint32_t >> 30 only has 4 possible values
            // and all are covered here
            // return COMMAND_PROCESSOR::ExecutePacketType2(packet);
            // executepackettype2 is identical
            goto handle_bad_packet;
          }
        }
      }
    }
    else {
      XELOGW("GPU packet is CDCDCDCD - probably read uninitialized memory!");
      goto actually_execute_packet;
    }
  }
  else {
  handle_bad_packet:
    trace_writer_.WritePacketStart(uint32_t(reader_.read_ptr() - 4), 1);
    trace_writer_.WritePacketEnd();
    return true;
  }
}
XE_NOINLINE
XE_COLD
bool COMMAND_PROCESSOR::ExecutePacketType0_CountOverflow(uint32_t count) {
  XELOGE("ExecutePacketType0 overflow (read count {:08X}, packet count {:08X})",
         COMMAND_PROCESSOR::GetCurrentRingReadCount(),
         count * sizeof(uint32_t));
  return false;
}
/*
    Todo: optimize this function this one along with execute packet type III are
   the most frequently called functions for PM4
*/
XE_NOINLINE
bool COMMAND_PROCESSOR::ExecutePacketType0(uint32_t packet) XE_RESTRICT {
  // Type-0 packet.
  // Write count registers in sequence to the registers starting at
  // (base_index << 2).

  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;

  if (COMMAND_PROCESSOR::GetCurrentRingReadCount() >=
      count * sizeof(uint32_t)) {
    trace_writer_.WritePacketStart(uint32_t(reader_.read_ptr() - 4), 1 + count);

    uint32_t base_index = (packet & 0x7FFF);
    uint32_t write_one_reg = (packet >> 15) & 0x1;
    // Phase 936: the injected stream writes DC_LUT gamma ramp entries
    // (0x1921-0x1927, 0x1930-0x1936). Xenia applies those to the display, so
    // replaying them blacks the screen - the whole fault chased from phase 913
    // to 935. Skip them while executing the Guide's stream; the ramp belongs
    // to the title.
    if (guide_overlay_exec_ && cvars::guide_overlay_skip_lut) {
      uint32_t lo = base_index;
      uint32_t hi = base_index + (write_one_reg ? 1 : count);
      if ((hi > 0x1920u && lo < 0x1928u) || (hi > 0x1930u && lo < 0x1937u)) {
        ++guide_ov_lutskip_;
        reader_.AdvanceRead(count * sizeof(uint32_t));
        trace_writer_.WritePacketEnd();
        return true;
      }
    }
    // Phase 543: the Guide writes no SET_CONSTANT packets of any type, and its
    // stream is largely type-0 (phase 514) - direct register writes. Fetch
    // constants live at 0x4800+, so this is where they would be set. Report any
    // type-0 write that covers that range while the Guide is drawing.
    if (guide_in_draw_scope_ && !guide_replaying_) {
      uint32_t lo = base_index, hi = base_index + (write_one_reg ? 1 : count);
      if (hi > 0x4800u && lo < 0x48C0u) {
        static uint32_t t0log = 0;
        if (t0log++ < 12) {
          // Phase 544: print the values, not just the range. Slot 2 is written
          // with a null address (phase 543); seeing all three slots shows
          // whether the others are sane - i.e. whether one allocation failed or
          // the whole setup is empty.
          RegisterFile& trf = *register_file_;
          XELOGI("GuideType0Fetch: base={:04X} count={} | s0={:08X}/{:08X} "
                 "s1={:08X}/{:08X} s2={:08X}/{:08X}",
                 base_index, count, trf[0x4800], trf[0x4801], trf[0x4802],
                 trf[0x4803], trf[0x4804], trf[0x4805]);
        }
      } else if (base_index >= 0x4000u && base_index < 0x4800u) {
        // Phase 548: the draws are kAutoIndex with no vertex fetch (547), so the
        // geometry comes from these ALU constants. Print them as floats - if the
        // positions are degenerate or off-screen that is visible directly, and
        // it is the last untested stage.
        // Phase 548: the crux is whether the Guide ever writes VERTEX shader
        // constants (vec4 0-255, 0x4000-0x43FF) as opposed to pixel ones. A
        // capped log cannot answer that, so count both ranges over the whole
        // run and report periodically.
        {
          static uint32_t n_vs = 0, n_ps = 0, rep = 0;
          if (base_index < 0x4400u) ++n_vs; else ++n_ps;
          if ((++rep % 200u) == 0u) {
            XELOGI("GuideALURange: {} vertex-range writes (0x4000-0x43FF), "
                   "{} pixel-range writes (0x4400+)",
                   n_vs, n_ps);
          }
        }
        // Log the rare vertex-range writes in full - 13 in a run, against 2187
        // pixel-range - since those carry the transform the geometry depends on.
        static uint32_t vslog = 0;
        if (base_index < 0x4400u && vslog++ < 8) {
          RegisterFile& vrf = *register_file_;
          std::string vv;
          for (uint32_t k = 0; k < 16 && k < count; ++k) {
            uint32_t raw = vrf[base_index + k];
            float fv;
            std::memcpy(&fv, &raw, 4);
            vv += fmt::format("{} ", fv);
          }
          XELOGI("GuideVS: base={:04X} (vec4 {}) count={} | {}", base_index,
                 (base_index - 0x4000u) / 4u, count, vv);
          // Only the first 16 of 1024 registers were visible above, so the quad
          // rect could sit anywhere in the bank. Report every non-zero vec4 -
          // if the bank is empty apart from vec4 0, the geometry has nothing to
          // be built from.
          std::string nz;
          uint32_t nz_count = 0;
          for (uint32_t v = 0; v < 256u && base_index + v * 4 + 3 < 0x4400u;
               ++v) {
            uint32_t a0 = vrf[0x4000 + v * 4], a1 = vrf[0x4001 + v * 4];
            uint32_t a2 = vrf[0x4002 + v * 4], a3 = vrf[0x4003 + v * 4];
            if (a0 | a1 | a2 | a3) {
              ++nz_count;
              if (nz_count <= 8) {
                float g[4];
                uint32_t rr[4] = {a0, a1, a2, a3};
                for (uint32_t q = 0; q < 4; ++q) std::memcpy(&g[q], &rr[q], 4);
                nz += fmt::format("[{}]={} {} {} {} ", v, g[0], g[1], g[2],
                                  g[3]);
              }
            }
          }
          XELOGI("GuideVSNonZero: {} of 256 vec4s non-zero | {}", nz_count, nz);
        }
        static uint32_t alulog = 0;
        if (alulog++ < 6) {
          RegisterFile& arf = *register_file_;
          std::string vals;
          for (uint32_t k = 0; k < 12 && k < count; ++k) {
            uint32_t raw = arf[base_index + k];
            float fv;
            std::memcpy(&fv, &raw, 4);
            vals += fmt::format("{} ", fv);
          }
          XELOGI("GuideALU: base={:04X} (vec4 {}) count={} | {}", base_index,
                 (base_index - 0x4000u) / 4u, count, vals);
        }
      } else {
        static uint32_t t0other = 0;
        if (t0other++ < 12) {
          XELOGI("GuideType0: base={:04X} count={}", base_index, count);
        }
      }
    }

    if (!write_one_reg) {
      COMMAND_PROCESSOR::WriteRegisterRangeFromRing(&reader_, base_index,
                                                    count);

    } else {
      COMMAND_PROCESSOR::WriteOneRegisterFromRing(base_index, count);
    }

    trace_writer_.WritePacketEnd();
    return true;
  } else {
    return COMMAND_PROCESSOR::ExecutePacketType0_CountOverflow(count);
  }
}
XE_NOINLINE
bool COMMAND_PROCESSOR::ExecutePacketType1(uint32_t packet) XE_RESTRICT {
  // Type-1 packet.
  // Contains two registers of data. Type-0 should be more common.
  trace_writer_.WritePacketStart(uint32_t(reader_.read_ptr() - 4), 3);
  uint32_t reg_index_1 = packet & 0x7FF;
  uint32_t reg_index_2 = (packet >> 11) & 0x7FF;
  uint32_t reg_data_1 = reader_.ReadAndSwap<uint32_t>();
  uint32_t reg_data_2 = reader_.ReadAndSwap<uint32_t>();
  COMMAND_PROCESSOR::WriteRegister(reg_index_1, reg_data_1);
  COMMAND_PROCESSOR::WriteRegister(reg_index_2, reg_data_2);
  trace_writer_.WritePacketEnd();
  return true;
}

bool COMMAND_PROCESSOR::ExecutePacketType2(uint32_t packet) XE_RESTRICT {
  // Type-2 packet.
  // No-op. Do nothing.
  trace_writer_.WritePacketStart(uint32_t(reader_.read_ptr() - 4), 1);
  trace_writer_.WritePacketEnd();
  return true;
}
XE_FORCEINLINE
XE_NOALIAS
uint32_t COMMAND_PROCESSOR::GetCurrentRingReadCount() {
  return reader_.read_count();
}
XE_NOINLINE
XE_COLD
bool COMMAND_PROCESSOR::ExecutePacketType3_CountOverflow(uint32_t count) {
  XELOGE("ExecutePacketType3 overflow (read count {:08X}, packet count {:08X})",
         COMMAND_PROCESSOR::GetCurrentRingReadCount(),
         count * sizeof(uint32_t));
  return false;
}
XE_NOINLINE
bool COMMAND_PROCESSOR::ExecutePacketType3(uint32_t packet) XE_RESTRICT {
  // Type-3 packet.
  uint32_t opcode = (packet >> 8) & 0x7F;
  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  auto data_start_offset = reader_.read_offset();

  if (COMMAND_PROCESSOR::GetCurrentRingReadCount() >=
      count * sizeof(uint32_t)) {
    // To handle nesting behavior when tracing we special case indirect buffers.
    if (opcode == PM4_INDIRECT_BUFFER) {
      trace_writer_.WritePacketStart(uint32_t(reader_.read_ptr() - 4), 2);
    } else {
      trace_writer_.WritePacketStart(uint32_t(reader_.read_ptr() - 4),
                                     1 + count);
    }

    // & 1 == predicate - when set, we do bin check to see if we should execute
    // the packet. Only type 3 packets are affected.
    // We also skip predicated swaps, as they are never valid (probably?).
    if (packet & 1) {
      bool any_pass = (bin_select_ & bin_mask_) != 0;
      if (!any_pass || opcode == PM4_XE_SWAP) {
        reader_.AdvanceRead(count * sizeof(uint32_t));
        trace_writer_.WritePacketEnd();
        return true;
      }
    }

    bool result = false;
    switch (opcode) {
      case PM4_ME_INIT:
        result = COMMAND_PROCESSOR::ExecutePacketType3_ME_INIT(packet, count);
        break;
      case PM4_NOP:
        result = COMMAND_PROCESSOR::ExecutePacketType3_NOP(packet, count);
        break;
      case PM4_INTERRUPT:
        result = COMMAND_PROCESSOR::ExecutePacketType3_INTERRUPT(packet, count);
        break;
      case PM4_XE_SWAP:
        result = COMMAND_PROCESSOR::ExecutePacketType3_XE_SWAP(packet, count);
        break;
      case PM4_INDIRECT_BUFFER:
      case PM4_INDIRECT_BUFFER_PFD:
        result = COMMAND_PROCESSOR::ExecutePacketType3_INDIRECT_BUFFER(packet,
                                                                       count);
        break;
      case PM4_WAIT_REG_MEM:
        result =
            COMMAND_PROCESSOR::ExecutePacketType3_WAIT_REG_MEM(packet, count);
        break;
      case PM4_REG_RMW:
        result = COMMAND_PROCESSOR::ExecutePacketType3_REG_RMW(packet, count);
        break;
      case PM4_REG_TO_MEM:
        result =
            COMMAND_PROCESSOR::ExecutePacketType3_REG_TO_MEM(packet, count);
        break;
      case PM4_MEM_WRITE:
        result = COMMAND_PROCESSOR::ExecutePacketType3_MEM_WRITE(packet, count);
        break;
      case PM4_COND_WRITE:
        result =
            COMMAND_PROCESSOR::ExecutePacketType3_COND_WRITE(packet, count);
        break;
      case PM4_EVENT_WRITE:
        result =
            COMMAND_PROCESSOR::ExecutePacketType3_EVENT_WRITE(packet, count);
        break;
      case PM4_EVENT_WRITE_SHD:
        result = COMMAND_PROCESSOR::ExecutePacketType3_EVENT_WRITE_SHD(packet,
                                                                       count);
        break;
      case PM4_EVENT_WRITE_EXT:
        result = COMMAND_PROCESSOR::ExecutePacketType3_EVENT_WRITE_EXT(packet,
                                                                       count);
        break;
      case PM4_EVENT_WRITE_ZPD:
        result = COMMAND_PROCESSOR::ExecutePacketType3_EVENT_WRITE_ZPD(packet,
                                                                       count);
        break;
      case PM4_DRAW_INDX:
      case PM4_DRAW_INDX_2: {
        ++guide_draw_count_;
        // Phase 538: snapshot the fetch constants as they stand for a real
        // Guide draw, so the replay can be given the same geometry bindings.
        // Taken per draw so the last one captured is a state that actually
        // rasterised.
        if (guide_in_draw_scope_ && !guide_replaying_) {
          RegisterFile& srf = *register_file_;
          for (uint32_t fi = 0; fi < 0xC0u; ++fi) {
            guide_fetch_[fi] = srf[0x4800 + fi];
          }
          guide_fetch_saved_ = true;
        }
        // Phase 534: mark that this indirect buffer contained a Guide draw;
        // the enclosing IB handler turns that into the replay range.
        if (!guide_replaying_ && guide_in_draw_scope_) {
          guide_ib_had_draw_ = true;
        }
        // Phase 554: replay at the first title draw of the frame, inside the
        // render pass, rather than inside the resolve.
        if (cvars::guide_replay_at_draw && guide_frame_needs_replay_ &&
            !guide_in_draw_scope_ && !guide_replaying_ && guide_replay_armed_ &&
            guide_replay_words_) {
          guide_frame_needs_replay_ = false;
          guide_replaying_ = true;
          xe::gpu::g_guide_replaying = true;
          uint32_t before_rd = guide_draw_count_;
          COMMAND_PROCESSOR::ExecuteGuestBufferVirtualUnsafe(
              guide_replay_addr_, guide_replay_words_);
          xe::gpu::g_guide_replaying = false;
          guide_replaying_ = false;
          static uint32_t rdlog2 = 0;
          if (rdlog2++ < 6) {
            XELOGI("GuideReplayAtDraw: {} draws replayed inside the render pass",
                   guide_draw_count_ - before_rd);
          }
        }
        // Phase 530: the Guide's draws arrive as a burst. Resolve at the first
        // draw that is NOT the Guide's after one, which is the last moment its
        // pixels are still in EDRAM untouched by the title.
        if (!guide_in_draw_scope_ && guide_burst_pending_) {
          guide_burst_pending_ = false;
          COMMAND_PROCESSOR::GuideExtraResolve();
        }
        if (guide_in_draw_scope_) guide_burst_pending_ = true;
        // Phase 525: the Guide's blend state is src=kSrcAlpha, dst=
        // kOneMinusSrcAlpha (RB_BLENDCONTROL0 = 01010706). With a source alpha
        // of zero that computes exactly the destination, which is what phase
        // 524 measured - a resolve producing byte-identical output. Force the
        // Guide's own draws opaque to find out whether the geometry covers
        // anything; if the image changes, coverage is fine and alpha is the
        // problem.
        uint32_t saved_blend = 0;
        bool forced = false;
        if (guide_in_draw_scope_) ++guide_scoped_draws_;
        // Phase 552: apply during the replay too. The replay's A/B has always
        // read 0/2900, which was attributed to the replay being ineffective -
        // but geometry drawn with alpha 0 through a kSrcAlpha blend produces
        // exactly that, so the two explanations are indistinguishable until the
        // override covers the replayed draws.
        if ((guide_in_draw_scope_ || guide_replaying_ ||
             guide_overlay_exec_) &&
            cvars::guide_force_opaque) {
          RegisterFile& brf = *register_file_;
          saved_blend = brf[0x2201];
          brf[0x2201] = 0x00000001u;  // src=kOne, dst=kZero, add
          forced = true;
          // Phase 965: several conclusions rest on this override applying to
          // the Guide's draws. Count it rather than assuming.
          if (guide_overlay_exec_) {
            static uint32_t fo = 0;
            if (++fo <= 2 || (fo % 200) == 0) {
              XELOGI("GuideForceOpaque: applied to {} of the Guide's draws "
                     "(blend was {:08X})",
                     fo, saved_blend);
            }
          }
        }
        result = (opcode == PM4_DRAW_INDX)
                     ? COMMAND_PROCESSOR::ExecutePacketType3_DRAW_INDX(packet,
                                                                       count)
                     : COMMAND_PROCESSOR::ExecutePacketType3_DRAW_INDX_2(
                           packet, count);
        if (forced) (*register_file_)[0x2201] = saved_blend;
      } break;
      case PM4_SET_CONSTANT:
        result =
            COMMAND_PROCESSOR::ExecutePacketType3_SET_CONSTANT(packet, count);
        break;
      case PM4_SET_CONSTANT2:
        result =
            COMMAND_PROCESSOR::ExecutePacketType3_SET_CONSTANT2(packet, count);
        break;
      case PM4_LOAD_ALU_CONSTANT:
        result = COMMAND_PROCESSOR::ExecutePacketType3_LOAD_ALU_CONSTANT(packet,
                                                                         count);
        break;
      case PM4_SET_SHADER_CONSTANTS:
        result = COMMAND_PROCESSOR::ExecutePacketType3_SET_SHADER_CONSTANTS(
            packet, count);
        break;
      case PM4_IM_LOAD:
        // Phase 553: shaders load through these packets, not through registers -
        // phase 552's guess at 0x0578 was scratch registers. If the Guide's
        // indirect buffer contains no IM_LOAD, the replayed draws run whatever
        // program the title last bound, with the Guide's vertices fed to it.
        // Counted rather than sampled: a shared cap made phase 538's replay
        // entries invisible and read as "the replay is not drawing".
        if (guide_in_draw_scope_ || guide_replaying_) {
          static uint32_t im_b = 0, im_r = 0, im_rep = 0;
          if (guide_replaying_) ++im_r; else ++im_b;
          if ((++im_rep % 300u) == 0u) {
            XELOGI("GuideShaderLoads: IM_LOAD burst={} replay={}", im_b, im_r);
          }
        }

        result = COMMAND_PROCESSOR::ExecutePacketType3_IM_LOAD(packet, count);
        break;
      case PM4_IM_LOAD_IMMEDIATE:
        if (guide_in_draw_scope_ || guide_replaying_) {
          static uint32_t imi_b = 0, imi_r = 0, imi_rep = 0;
          if (guide_replaying_) ++imi_r; else ++imi_b;
          if ((++imi_rep % 300u) == 0u) {
            XELOGI("GuideShaderLoadsImm: IM_LOAD_IMMEDIATE burst={} replay={}",
                   imi_b, imi_r);
          }
        }
        result = COMMAND_PROCESSOR::ExecutePacketType3_IM_LOAD_IMMEDIATE(packet,
                                                                         count);
        break;
      case PM4_INVALIDATE_STATE:
        result = COMMAND_PROCESSOR::ExecutePacketType3_INVALIDATE_STATE(packet,
                                                                        count);
        break;
      case PM4_VIZ_QUERY:
        result = COMMAND_PROCESSOR::ExecutePacketType3_VIZ_QUERY(packet, count);
        break;

      case PM4_SET_BIN_MASK_LO: {
        uint32_t value = reader_.ReadAndSwap<uint32_t>();
        bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | value;
        result = true;
      } break;
      case PM4_SET_BIN_MASK_HI: {
        uint32_t value = reader_.ReadAndSwap<uint32_t>();
        bin_mask_ =
            (bin_mask_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
        result = true;
      } break;
      case PM4_SET_BIN_SELECT_LO: {
        uint32_t value = reader_.ReadAndSwap<uint32_t>();
        bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | value;
        result = true;
      } break;
      case PM4_SET_BIN_SELECT_HI: {
        uint32_t value = reader_.ReadAndSwap<uint32_t>();
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) |
                      (static_cast<uint64_t>(value) << 32);
        result = true;
      } break;
      case PM4_SET_BIN_MASK: {
        assert_true(count == 2);
        uint64_t val_hi = reader_.ReadAndSwap<uint32_t>();
        uint64_t val_lo = reader_.ReadAndSwap<uint32_t>();
        bin_mask_ = (val_hi << 32) | val_lo;
        result = true;
      } break;
      case PM4_SET_BIN_SELECT: {
        assert_true(count == 2);
        uint64_t val_hi = reader_.ReadAndSwap<uint32_t>();
        uint64_t val_lo = reader_.ReadAndSwap<uint32_t>();
        bin_select_ = (val_hi << 32) | val_lo;
        result = true;
      } break;
      case PM4_CONTEXT_UPDATE: {
        assert_true(count == 1);
        uint32_t value = reader_.ReadAndSwap<uint32_t>();
        XELOGGPU("GPU context update = {:08X}", value);
        assert_true(value == 0);
        result = true;
        break;
      }
      case PM4_WAIT_FOR_IDLE: {
        // This opcode is used by 5454084E while going / being ingame.
        assert_true(count == 1);
        uint32_t value = reader_.ReadAndSwap<uint32_t>();
        XELOGGPU("GPU wait for idle = {:08X}", value);
        result = true;
        break;
      }

      default:
        return COMMAND_PROCESSOR::HitUnimplementedOpcode(opcode, count);
    }

    trace_writer_.WritePacketEnd();
#if XE_ENABLE_TRACE_WRITER_INSTRUMENTATION == 1

    if (opcode == PM4_XE_SWAP) {
      // End the trace writer frame.
      if (trace_writer_.is_open()) {
        trace_writer_.WriteEvent(EventCommand::Type::kSwap);
        trace_writer_.Flush();
        if (trace_state_ == TraceState::kSingleFrame) {
          trace_state_ = TraceState::kDisabled;
          trace_writer_.Close();
        }
      } else if (trace_state_ == TraceState::kSingleFrame) {
        // New trace request - we only start tracing at the beginning of a
        // frame.
        uint32_t title_id = kernel_state_->GetExecutableModule()->title_id();
        auto file_name = fmt::format("{:08X}_{}.xtr", title_id, counter_ - 1);
        auto path = trace_frame_path_ / file_name;
        trace_writer_.Open(path, title_id);
        InitializeTrace();
      }
    }
#endif

    assert_true(reader_.read_offset() ==
                (data_start_offset + (count * sizeof(uint32_t))) %
                    reader_.capacity());
    return result;
  } else {
    return COMMAND_PROCESSOR::ExecutePacketType3_CountOverflow(count);
  }
}

XE_NOINLINE
XE_COLD
bool COMMAND_PROCESSOR::HitUnimplementedOpcode(uint32_t opcode,
                                               uint32_t count) XE_RESTRICT {
  XELOGGPU("Unimplemented GPU OPCODE: 0x{:02X}\t\tCOUNT: {}\n", opcode, count);
  assert_always();
  reader_.AdvanceRead(count * sizeof(uint32_t));
  trace_writer_.WritePacketEnd();
  return false;
}
XE_NOINLINE
bool COMMAND_PROCESSOR::ExecutePacketType3_ME_INIT(uint32_t packet,
                                                   uint32_t count) XE_RESTRICT {
  // initialize CP's micro-engine
  me_bin_.resize(count);
  for (uint32_t i = 0; i < count; i++) {
    me_bin_[i] = reader_.ReadAndSwap<uint32_t>();
  }
  return true;
}

bool COMMAND_PROCESSOR::ExecutePacketType3_NOP(uint32_t packet,
                                               uint32_t count) XE_RESTRICT {
  // skip N 32-bit words to get to the next packet
  // No-op, ignore some data.
  reader_.AdvanceRead(count * sizeof(uint32_t));
  return true;
}
XE_NOINLINE
bool COMMAND_PROCESSOR::ExecutePacketType3_INTERRUPT(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  SCOPE_profile_cpu_f("gpu");

  // generate interrupt from the command stream
  uint32_t cpu_mask = reader_.ReadAndSwap<uint32_t>();
  for (int n = 0; n < 6; n++) {
    if (cpu_mask & (1 << n)) {
      graphics_system_->DispatchInterruptCallback(1, n);
    }
  }
  return true;
}
XE_NOINLINE
bool COMMAND_PROCESSOR::ExecutePacketType3_XE_SWAP(uint32_t packet,
                                                   uint32_t count) XE_RESTRICT {
  SCOPE_profile_cpu_f("gpu");

  Profiler::Flip();

  // Xenia-specific VdSwap hook.
  // VdSwap will post this to tell us we need to swap the screen/fire an
  // interrupt.
  // 63 words here, but only the first has any data.
  uint32_t magic = reader_.ReadAndSwap<fourcc_t>();
  assert_true(magic == kSwapSignature);

  // TODO(benvanik): only swap frontbuffer ptr.
  uint32_t frontbuffer_ptr = reader_.ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_width = reader_.ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_height = reader_.ReadAndSwap<uint32_t>();
  reader_.AdvanceRead((count - 4) * sizeof(uint32_t));

  // Draw the Guide over the finished frame, before it is presented.
  {
    // Report what the swap handler sees. The publish side logs a clean range
    // and this side logs nothing, so one of them is not running or they are
    // not looking at the same command processor instance.
    static uint32_t swap_seen = 0;
    {
      // Phase 880: time each swap so a rate change is visible rather than
      // inferred from how many 600-swap milestones a run reaches.
      static std::chrono::steady_clock::time_point t0{};
      static uint32_t nlog = 0;
      auto now = std::chrono::steady_clock::now();
      if (t0.time_since_epoch().count() == 0) t0 = now;
      uint32_t every = std::max(1u, uint32_t(cvars::guide_swap_log_every));
      if ((swap_seen % every) == 0 && nlog++ < 4000) {
        // Phase 880: sample what is actually IN the buffer the swap names.
        // Every signal so far has been indirect - PNG size, swap rate,
        // frontbuffer address - and each has produced a wrong conclusion. If
        // these pixels are non-zero while the capture is black, the pixels
        // are fine and the presentation is not; if they are zero, whatever
        // wrote them is the problem.
        uint32_t nz = 0, sum = 0;
        if (frontbuffer_ptr) {
          uint8_t* fbp = memory_->TranslatePhysical(frontbuffer_ptr);
          if (fbp) {
            for (uint32_t i = 0; i < 4096; ++i) {
              uint32_t off = (i * 1451) % (1280u * 720u);
              uint32_t px = *reinterpret_cast<uint32_t*>(fbp + off * 4u);
              if (px & 0x00FFFFFFu) ++nz;
              sum += px & 0xFFu;
            }
          }
        }
        // Phase 943: the swap packet carries 1E69E000 every frame while the
        // texture cache reports its source alternating between 1E69E000 and
        // 1E306000. They are not the same buffer. Sample the one the display
        // actually loads from - texture fetch 0's base page - beside it.
        uint32_t tf_base = 0, tf_nz = 0;
        {
          xenos::xe_gpu_texture_fetch_t tf =
              register_file_->GetTextureFetch(0);
          tf_base = tf.base_address << 12;
          const uint8_t* tp =
              tf_base ? memory_->TranslatePhysical(tf_base) : nullptr;
          if (tp) {
            for (uint32_t i = 0; i < 4096; ++i) {
              uint32_t off = (i * 1451u) % (1280u * 720u);
              uint32_t px = *reinterpret_cast<const uint32_t*>(tp + off * 4u);
              if (px & 0x00FFFFFFu) ++tf_nz;
            }
          }
        }
        XELOGI("SwapSourceCmp: packet fb={:08X} nonzero={} | texfetch0 "
               "{:08X} nonzero={}/4096",
               frontbuffer_ptr, nz, tf_base, tf_nz);
        XELOGI("SwapTick: #{} at {}ms fb={:08X} nonzero={}/4096 sum={}",
               swap_seen,
               std::chrono::duration_cast<std::chrono::milliseconds>(now - t0)
                   .count(),
               frontbuffer_ptr, nz, sum);
      }
    }
    if ((swap_seen++ % 600u) == 0) {
      // Phase 728: paint a marker directly into the buffer the swap names.
      // This is the one thing never tested - whether that memory is what the
      // display shows.
      if (cvars::guide_paint_marker && frontbuffer_ptr) {
        uint8_t* fb = memory_->TranslatePhysical(frontbuffer_ptr);
        if (fb) {
          for (uint32_t row = 0; row < 128; ++row) {
            std::memset(fb + size_t(row) * 1280 * 4, 0xFF, 256 * 4);
          }
          static uint32_t mk = 0;
          if (++mk <= 2) {
            XELOGI("GuidePaintMarker: filled 256x128 at {:08X}",
                   frontbuffer_ptr);
          }
        }
      }
      // Phase 724: the Guide resolves to 1E69E000. Print what the swap
      // actually presents from - if they differ, the pixels are landing in a
      // buffer this present does not read.
      XELOGI("GuideSwapFront: frontbuffer={:08X} {}x{}", frontbuffer_ptr,
             frontbuffer_width, frontbuffer_height);
      XELOGI("GuideOverlay: swap #{} sees ptr={:08X} words={}", swap_seen,
             guide_overlay_ptr_, guide_overlay_words_);
    }
  }
  // Phase 946: when the stream is to be run just before the title's resolve,
  // leave it published here - this handler would otherwise consume and clear
  // it, which is why the injection never fired.
  if (guide_overlay_ptr_ && guide_overlay_words_ &&
      !cvars::guide_overlay_before_resolve) {
    uint32_t gptr = guide_overlay_ptr_;
    uint32_t gwords = guide_overlay_words_;
    if (!cvars::guide_overlay_repeat) {
      guide_overlay_ptr_ = 0;
    }
    XELOGI("GuideOverlay: executing {} words at {:08X} before swap", gwords,
           gptr);
    uint32_t draws_before = guide_draw_count_;
    // Phase 892: give the geometry a surface to draw into. The stream's
    // state preamble is not in the executed range and does not parse from its
    // own start, but the title's surface registers are already captured at its
    // resolve (and, until now, never used). Put them back before the draws.
    uint32_t keep_surf[3] = {0, 0, 0};
    bool surf_restored = false;
    if (cvars::guide_overlay_restore_surface && guide_resolve_saved_) {
      RegisterFile& orf = *register_file_;
      for (uint32_t i = 0; i < 2; ++i) {
        keep_surf[i] = orf[0x2000 + i];
        orf[0x2000 + i] = guide_saved_surface_[i];
      }
      surf_restored = true;
      XELOGI("GuideOverlaySurf: set SURFACE_INFO={:08X} COLOR_INFO={:08X} "
             "(was {:08X} {:08X})",
             guide_saved_surface_[0], guide_saved_surface_[1], keep_surf[0],
             keep_surf[1]);
    }
    // Phase 894: restore the title's whole RB/PA block before the stream
    // runs. Where the stream has an opinion its own type-0 packets overwrite
    // this during execution; where it has none - which is most of the block,
    // since it was recorded in a context that already had these set - the
    // title's values stand in for the missing context.
    std::vector<uint32_t> keep_ctx;
    if (cvars::guide_overlay_restore_context && guide_title_regs_valid_) {
      RegisterFile& crf = *register_file_;
      keep_ctx.resize(kGuideCtxHi - kGuideCtxLo);
      for (uint32_t r = kGuideCtxLo; r < kGuideCtxHi; ++r) {
        keep_ctx[r - kGuideCtxLo] = crf[r];
        crf[r] = guide_title_regs_[r - kGuideCtxLo];
      }
    }
    size_t dcl_before = COMMAND_PROCESSOR::GuideCommandListBytes();
    // Phase 948: reset the host binding trackers BEFORE the burst, not after.
    // Phase 931 reset them afterwards to protect the title and it changed
    // nothing; resetting here forces the Guide's first draw to re-bind render
    // targets rather than assume the title's bindings are still current in the
    // command list.
    if (cvars::guide_overlay_reset_state) {
      COMMAND_PROCESSOR::GuideResetHostState();
    }
    guide_overlay_exec_ = true;
    COMMAND_PROCESSOR::GuideOcclusionBegin();
    COMMAND_PROCESSOR::ExecuteGuestBufferVirtualUnsafe(gptr, gwords);
    if (cvars::guide_clear_rt) {
      COMMAND_PROCESSOR::GuideClearRenderTarget();
    }
    COMMAND_PROCESSOR::GuideOcclusionEnd();
    {
      size_t dcl_after = COMMAND_PROCESSOR::GuideCommandListBytes();
      guide_seen_burst_ = true;
      guide_burst_list_bytes_ = dcl_after;
      static uint32_t dl = 0;
      if (dl++ < 3) {
        XELOGI("GuideCmdList: {} bytes before the burst, {} after (+{})",
               dcl_before, dcl_after, dcl_after - dcl_before);
      }
    }
    if (!keep_ctx.empty()) {
      RegisterFile& crf = *register_file_;
      for (uint32_t r = kGuideCtxLo; r < kGuideCtxHi; ++r) {
        crf[r] = keep_ctx[r - kGuideCtxLo];
      }
    }
    if (surf_restored) {
      RegisterFile& orf = *register_file_;
      for (uint32_t i = 0; i < 2; ++i) orf[0x2000 + i] = keep_surf[i];
    }
    guide_overlay_exec_ = false;
    // Phase 931: put the host binding trackers back the way BeginSubmission
    // leaves them, so the title's next frame re-binds instead of trusting a
    // cache the Guide's draws moved.
    if (cvars::guide_overlay_reset_state) {
      COMMAND_PROCESSOR::GuideResetHostState();
    }
    if (guide_title_state_valid_) {
      XELOGI("TitleDrawState (last before overlay): mode={} SURFACE={:08X} "
             "COLOR={:08X} DEPTH={:08X} | MASK={:08X} COLORCTL={:08X} "
             "DEPTHCTL={:08X} BLEND0={:08X} | scissor {:08X} {:08X} VTE={:08X} "
             "SU_SC={:08X}",
             guide_title_state_[0] & 0x7u, guide_title_state_[1],
             guide_title_state_[2], guide_title_state_[3],
             guide_title_state_[4], guide_title_state_[5],
             guide_title_state_[6], guide_title_state_[7],
             guide_title_state_[8], guide_title_state_[9],
             guide_title_state_[10], guide_title_state_[11]);
    }
    XELOGI("GuideDrawFate: seen={} pre-dropped={} viz-dropped={} issued={} "
           "backend-failed={} surface-patched={} mask-patched={} "
           "viewport-patched={}",
           guide_ov_seen_, guide_ov_predrop_, guide_ov_vizdrop_,
           guide_ov_issued_, guide_ov_failed_, guide_ov_surfpatch_,
           guide_ov_maskpatch_, guide_ov_vportpatch_);
    XELOGI("GuideVTE: passthru patches={} clip patches={} marker patches={}",
           guide_ov_vtepatch_, guide_ov_clippatch_, guide_ov_markerpatch_);
    if (cvars::guide_quad_census) {
      XELOGI("GuideQuadCensus: measured={} thin(<1px tall)={} "
             "narrow(<1px wide)={} degenerate={} non-finite={} skipped={} | "
             "max {}x{} | union x[{}..{}] y[{}..{}]",
             guide_ov_qn_, guide_ov_qthin_, guide_ov_qnarrow_,
             guide_ov_qdegen_, guide_ov_qnonfin_, guide_ov_qskip_,
             guide_ov_qmaxw_, guide_ov_qmaxh_, guide_ov_qbb_[0],
             guide_ov_qbb_[2], guide_ov_qbb_[1], guide_ov_qbb_[3]);
    }
    // Phase 888: the draws execute against the right surface and write no
    // pixels, and blending is ruled out - so they are rejected before the
    // output merger. Read the state that can do that straight out of the
    // register file at the end of the burst.
    {
      static uint32_t rst = 0;
      if (rst++ < 3) {
        RegisterFile& srf = *register_file_;
        auto asf = [&](uint32_t r) {
          float f;
          uint32_t v = srf[r];
          std::memcpy(&f, &v, 4);
          return f;
        };
        uint32_t tl = srf[0x2081], br = srf[0x2082];
        XELOGI("GuideRaster: scissor TL=({},{}) BR=({},{}) | vport xscale={} "
               "yscale={} | VTE_CNTL={:08X} SU_SC_MODE={:08X} "
               "MODECONTROL={:08X}",
               tl & 0x7FFFu, (tl >> 16) & 0x7FFFu, br & 0x7FFFu,
               (br >> 16) & 0x7FFFu, asf(0x210F), asf(0x2111), srf[0x2206],
               srf[0x2205], srf[0x2208]);
      }
    }
    XELOGI("GuideOverlay: {} GPU draws dispatched",
           guide_draw_count_ - draws_before);
    // Phase 883: GuideExtraResolve is triggered from the draw path, at the
    // first non-Guide draw after a Guide burst. That trigger cannot fire for
    // these draws: they run here, in the swap handler, after every title draw
    // in the frame, so the next non-Guide draw belongs to the NEXT frame and
    // the title has overwritten EDRAM by then. Resolve them here instead,
    // while the pixels are still in EDRAM.
    if (guide_draw_count_ != draws_before) {
      COMMAND_PROCESSOR::GuideExtraResolve();
    }
  }

  // Phase 523: the Guide draws between this frame's resolve and its swap
  // (phase 522: resolves == swaps + 1 at every Guide draw), so its geometry sits
  // in EDRAM after the displayed pixels were already copied out. Resolve again
  // here, with the copy registers the title's own resolve just used, so the
  // Guide's pixels reach the same destination before the swap.
  // Phase 532: re-apply the Guide's contribution on top of the finished frame.
  // The title's resolve has overwritten the buffer since the diff was taken, so
  // write back only the dwords the Guide changed.
  if (cvars::guide_composite_frontbuffer && guide_fb_valid_ &&
      guide_fb_dest_ == (frontbuffer_ptr & ~0xFFFu)) {
    uint8_t* fbp = memory_->TranslatePhysical(guide_fb_dest_);
    if (fbp) {
      uint32_t* dst = reinterpret_cast<uint32_t*>(fbp);
      uint32_t n = uint32_t(guide_fb_before_.size()), applied = 0;
      for (uint32_t i = 0; i < n; ++i) {
        if (guide_fb_before_[i] != guide_fb_after_[i]) {
          dst[i] = guide_fb_after_[i];
          ++applied;
        }
      }
      static uint32_t aplog = 0;
      if (aplog++ < 8) {
        XELOGI("GuideComposite: re-applied {} of {} dwords onto {:08X}", applied,
               n, guide_fb_dest_);
      }
    }
    guide_fb_valid_ = false;
  }
  // Phase 531: the extra resolve writes to whichever buffer the title last
  // resolved (guide_saved_copy_[1]). With double buffering that need not be the
  // buffer about to be presented, in which case the Guide's pixels are put into
  // the back one and never shown - a simpler failure than the ordering one, and
  // worth ruling out before moving the draw hook.
  {
    static uint32_t sblog = 0;
    if (sblog++ < 8) {
      XELOGI("GuideSwapBuf: presenting {:08X} | extra resolve wrote {:08X} | {}",
             frontbuffer_ptr, guide_saved_copy_[1] & ~0xFFFu,
             ((guide_saved_copy_[1] & ~0xFFFu) == (frontbuffer_ptr & ~0xFFFu))
                 ? "SAME"
                 : "DIFFERENT");
    }
  }
  guide_frame_needs_replay_ = true;  // phase 554: arm for the next frame
  guide_draws_at_last_swap_ = guide_draw_count_;
  ++guide_swap_count_;  // phase 522
  // Phase 613: the worker is blocked inside a single packet after the ring
  // handover. A swap that never returns is the likeliest candidate, since it
  // hands off to the presenter.
  {
    static uint32_t sw = 0;
    ++sw;
    if (cvars::guide_cp_probe && (sw < 6 || (sw % 60) == 0)) {
      XELOGI("CPSwap in #{} fb={:08X}", sw, frontbuffer_ptr);
    }
  }
  COMMAND_PROCESSOR::IssueSwap(frontbuffer_ptr, frontbuffer_width,
                               frontbuffer_height);
  {
    static uint32_t swo = 0;
    ++swo;
    if (cvars::guide_cp_probe && (swo < 6 || (swo % 60) == 0)) {
      XELOGI("CPSwap out #{}", swo);
    }
  }

  ++counter_;
  return true;
}

bool COMMAND_PROCESSOR::ExecutePacketType3_INDIRECT_BUFFER(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  // indirect buffer dispatch
  uint32_t list_ptr = CpuToGpu(reader_.ReadAndSwap<uint32_t>());
  uint32_t list_length = reader_.ReadAndSwap<uint32_t>();
  assert_zero(list_length & ~0xFFFFF);
  list_length &= 0xFFFFF;
  // Phase 857: a device command buffer reaches the GPU by being chained into
  // the ring with this packet. The Guide emits twelve DRAW_INDX into its own
  // buffer (853) and nothing renders; log every buffer that IS chained, so
  // whether the Guide's is among them stops being a guess.
  {
    // Phase 862: the first 24 chains were all in 1F4Fxxxx-1F52xxxx, but 24 of
    // ~2200 is not a survey. Bucket every IB target by 1MB so the full range
    // of what gets submitted is visible - if xam ever chains a buffer of its
    // own, it shows up here.
    {
      static std::map<uint32_t, uint32_t> ib_buckets;
      static uint32_t ib_total = 0;
      ++ib_total;
      ++ib_buckets[list_ptr >> 20];
      if ((ib_total % 2000u) == 0u) {
        std::string h;
        for (auto& kv : ib_buckets) {
          h += fmt::format("{:03X}xxxxx:{} ", kv.first, kv.second);
        }
        XELOGI("IBBuckets after {}: {}", ib_total, h);
      }
    }
    static uint32_t ib_log = 0;
    if (ib_log < 24) {
      ++ib_log;
      XELOGI("IBChain #{}: ptr={:08X} len={} words | primary={:08X} size={}",
             ib_log, list_ptr, list_length, primary_buffer_ptr_,
             primary_buffer_size_);
    }
  }
  // Phase 534: if this IB turns out to contain the Guide's draws, its address
  // and length are the replayable range. Save and restore the marker so nested
  // buffers attribute to the innermost one that actually drew.
  // Phase 592: the Guide emits ~11.6KB of packets per frame into a buffer we
  // bound, and gpu_draws never moves. Either that buffer is never handed to
  // the CP or it is handed over and contains nothing that draws. Log each
  // distinct 1MB region an IB comes from - compact enough to leave on, and
  // it answers which of the two is true.
  {
    static uint32_t seen[24] = {};
    static uint32_t seen_n = 0;
    uint32_t region = GpuToCpu(list_ptr) & 0xFFF00000u;
    bool known = false;
    for (uint32_t i = 0; i < seen_n; ++i) {
      if (seen[i] == region) {
        known = true;
        break;
      }
    }
    if (!known && seen_n < 24) {
      seen[seen_n++] = region;
      XELOGI("GuideIBRegion #{}: {:08X} (first ptr {:08X}, len {})", seen_n,
             region, GpuToCpu(list_ptr), list_length);
      // Phase 622: dump the buffer itself for a region we have not seen. An
      // IB that is dispatched but contains only state, or is empty, looks
      // identical from the outside to one that draws.
      {
        uint32_t ib = GpuToCpu(list_ptr);
        uint32_t n = std::min<uint32_t>(list_length, 24u);
        std::string body;
        for (uint32_t w = 0; w < n; ++w) {
          uint32_t dw =
              xe::load_and_swap<uint32_t>(memory_->TranslatePhysical(ib + w * 4));
          body += fmt::format("{:08X} ", dw);
        }
        XELOGI("GuideIBBody {:08X} [{}]: {}", ib, list_length, body);
      }
    }
  }
  // Phase 623: the region probe reports only the FIRST buffer from each 1MB
  // region, so "xam dispatched an IB" says nothing about the hundreds that
  // follow. Tally every IB per region, and how many of them contained a draw,
  // so "does the Guide ever submit geometry" is answerable outright.
  const uint32_t guide_tally_region = GpuToCpu(list_ptr) & 0xFFF00000u;
  const uint32_t guide_tally_draws0 = guide_draw_count_;
  bool saved_had = guide_ib_had_draw_;
  guide_ib_had_draw_ = false;
  // Phase 746: remember which indirect buffer is executing, so a shader load
  // can name the stream it arrived in.
  g_guide_current_ib = GpuToCpu(list_ptr);
  g_guide_current_ib_words = list_length;
  // Phase 741: checksum the title's indirect buffers so the range we submit
  // can be compared against them. If ours matches one, we have been
  // resubmitting the title's own commands.
  {
    static uint32_t ibn = 0;
    if (ibn < 6 && list_length && list_length < 0x40000u) {
      const uint32_t* ibp =
          memory_->TranslatePhysical<const uint32_t*>(GpuToCpu(list_ptr));
      if (ibp) {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < list_length; ++i) {
          sum = sum * 31u + ibp[i];
        }
        ++ibn;
        XELOGI("TitleIBSum #{}: addr={:08X} words={} sum={:08X}", ibn,
               GpuToCpu(list_ptr), list_length, sum);
      }
    }
  }
  COMMAND_PROCESSOR::ExecuteIndirectBuffer(GpuToCpu(list_ptr), list_length);
  if (guide_ib_had_draw_ && !guide_replaying_) {
    uint32_t src = GpuToCpu(list_ptr);
    uint32_t bytes = list_length * 4u;
    guide_replay_armed_ = false;
    if (list_length > 0 && bytes <= 0x40000u) {
      if (!guide_replay_scratch_) {
        guide_replay_scratch_ = memory_->SystemHeapAlloc(0x40000u, 256);
        guide_replay_scratch_size_ = guide_replay_scratch_ ? 0x40000u : 0u;
      }
      const uint8_t* sp = memory_->TranslatePhysical(src);
      uint8_t* dp = guide_replay_scratch_
                        ? memory_->TranslateVirtual(guide_replay_scratch_)
                        : nullptr;
      if (sp && dp && bytes <= guide_replay_scratch_size_) {
        std::memcpy(dp, sp, bytes);
        guide_replay_addr_ = guide_replay_scratch_;
        guide_replay_words_ = list_length;
        guide_replay_armed_ = true;
      }
    }
    static uint32_t iblog = 0;
    if (iblog++ < 8) {
      XELOGI("GuideIB: draws in IB {:08X} len={} -> copied to {:08X} armed={}",
             src, list_length, guide_replay_addr_, guide_replay_armed_);
    }
  }
  {
    static uint32_t t_region[24] = {};
    static uint32_t t_ibs[24] = {};
    static uint32_t t_draws[24] = {};
    static uint32_t t_n = 0;
    static uint32_t t_total = 0;
    uint32_t slot = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < t_n; ++i) {
      if (t_region[i] == guide_tally_region) {
        slot = i;
        break;
      }
    }
    if (slot == 0xFFFFFFFFu && t_n < 24) {
      slot = t_n++;
      t_region[slot] = guide_tally_region;
    }
    if (slot != 0xFFFFFFFFu) {
      ++t_ibs[slot];
      // guide_ib_had_draw_ is gated on guide_in_draw_scope_, a Guide-specific
      // flag that is not set in every configuration - using it reported "0
      // draws" for the title, which plainly renders. Diff the actual draw
      // counter across the buffer instead.
      t_draws[slot] += guide_draw_count_ - guide_tally_draws0;
    }
    bool fresh_region = (slot != 0xFFFFFFFFu && t_ibs[slot] == 1);
    if (cvars::guide_cp_probe &&
        ((++t_total % 600u) == 0u || fresh_region)) {
      std::string tally;
      for (uint32_t i = 0; i < t_n; ++i) {
        tally += fmt::format("{:08X}:{}ib/{}draw ", t_region[i], t_ibs[i],
                             t_draws[i]);
      }
      XELOGI("GuideIBTally: {}", tally);
    }
  }
  guide_ib_had_draw_ = saved_had || guide_ib_had_draw_;
  return true;
}

/*
        chrispy: this is fine to inline, as a noinline function it compiled down
   to 54 bytes
*/
static bool MatchValueAndRef(uint32_t value, uint32_t ref, uint32_t wait_info) {
  // smaller code is generated than the #else path, although whether it is
  // faster i do not know. i don't think games do an enormous number of
  // cond_write though, so we have picked the path with the smaller codegen. we
  // do technically have more instructions executed vs the switch case method,
  // but we have no mispredicts and most of our instructions are 0.25/0.3
  // throughput
  return ((((value < ref) << 1) | ((value <= ref) << 2) |
           ((value == ref) << 3) | ((value != ref) << 4) |
           ((value >= ref) << 5) | ((value > ref) << 6) | (1 << 7)) >>
          (wait_info & 7)) &
         1;
}
XE_NOINLINE
bool COMMAND_PROCESSOR::ExecutePacketType3_WAIT_REG_MEM(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  SCOPE_profile_cpu_f("gpu");

  // wait until a register or memory location is a specific value
  uint32_t wait_info = reader_.ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader_.ReadAndSwap<uint32_t>();
  uint32_t ref = reader_.ReadAndSwap<uint32_t>();
  uint32_t mask = reader_.ReadAndSwap<uint32_t>();
  uint32_t wait = reader_.ReadAndSwap<uint32_t>();

  bool is_memory = (wait_info & 0x10) != 0;
  assert_true(is_memory || poll_reg_addr < RegisterFile::kRegisterCount);
  const volatile uint32_t& value_ref =
      is_memory ? *reinterpret_cast<uint32_t*>(memory_->TranslatePhysical(
                      poll_reg_addr & ~uint32_t(0x3)))
                : register_file_->values[poll_reg_addr];

  uint32_t guide_wait_spins = 0;
  const uint32_t guide_wait_ring = primary_buffer_ptr_;
  bool matched = false;

  do {
    uint32_t value = value_ref;
    if (is_memory) {
      trace_writer_.WriteMemoryRead(CpuToGpu(poll_reg_addr & ~uint32_t(0x3)),
                                    sizeof(uint32_t));
      value = xenos::GpuSwap(value,
                             static_cast<xenos::Endian>(poll_reg_addr & 0x3));
    } else {
      if (poll_reg_addr == XE_GPU_REG_COHER_STATUS_HOST) {
        MakeCoherent();
        value = value_ref;
      }
    }
    matched = MatchValueAndRef(value & mask, ref, wait_info);

    if (!matched) {
      // Wait.
      if (wait >= 0x100) {
        PrepareForWait();
        if (!cvars::vsync) {
          // User wants it fast and dangerous.
          // do nothing
        } else {
          xe::threading::Sleep(std::chrono::milliseconds(wait / 0x100));
          ReturnFromWait();
        }

        if (!worker_running_) {
          // Short-circuited exit.
          return false;
        }
      } else {
      }
    }
  // Phase 613: this loop waits forever if the polled value never changes,
  // which is the shape of the post-handover hang. Report a wait that has
  // clearly stopped progressing, with what it is waiting on.
      if (cvars::guide_cp_probe && ++guide_wait_spins == 400u) {
        XELOGW("CPWaitMem: still waiting after 400 polls - is_memory={} "
               "addr={:08X} ref={:08X} mask={:08X}",
               is_memory ? 1 : 0, poll_reg_addr, ref, mask);
      }
      // Phase 613: a WAIT_REG_MEM polls a fence the GPU is expected to
      // write. If the ring it came from has been replaced - which is what
      // the Guide's handover does - that fence can never be written and this
      // loop never ends, taking the whole command processor with it. Abandon
      // the wait when the ring underneath us changes; the packet's ring is
      // gone, so its fence is moot.
      if (primary_buffer_ptr_ != guide_wait_ring) {
        XELOGW("CPWaitMem: ring changed {:08X} -> {:08X} while waiting on "
               "{:08X}; abandoning the wait",
               guide_wait_ring, primary_buffer_ptr_, poll_reg_addr);
        break;
      }
    } while (!matched);

  return true;
}
XE_NOINLINE
bool COMMAND_PROCESSOR::ExecutePacketType3_REG_RMW(uint32_t packet,
                                                   uint32_t count) XE_RESTRICT {
  // register read/modify/write
  // ? (used during shader upload and edram setup)
  uint32_t rmw_info = reader_.ReadAndSwap<uint32_t>();
  uint32_t and_mask = reader_.ReadAndSwap<uint32_t>();
  uint32_t or_mask = reader_.ReadAndSwap<uint32_t>();
  uint32_t value = register_file_->values[rmw_info & 0x1FFF];
  if ((rmw_info >> 31) & 0x1) {
    // & reg
    value &= register_file_->values[and_mask & 0x1FFF];
  } else {
    // & imm
    value &= and_mask;
  }
  if ((rmw_info >> 30) & 0x1) {
    // | reg
    value |= register_file_->values[or_mask & 0x1FFF];
  } else {
    // | imm
    value |= or_mask;
  }
  COMMAND_PROCESSOR::WriteRegister(rmw_info & 0x1FFF, value);
  return true;
}

bool COMMAND_PROCESSOR::ExecutePacketType3_REG_TO_MEM(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  // Copy Register to Memory (?)
  // Count is 2, assuming a Register Addr and a Memory Addr.

  uint32_t reg_addr = reader_.ReadAndSwap<uint32_t>();
  uint32_t mem_addr = reader_.ReadAndSwap<uint32_t>();

  uint32_t reg_val;

  assert_true(reg_addr < RegisterFile::kRegisterCount);
  reg_val = register_file_->values[reg_addr];

  auto endianness = static_cast<xenos::Endian>(mem_addr & 0x3);
  mem_addr &= ~0x3;
  reg_val = GpuSwap(reg_val, endianness);
  xe::store(memory_->TranslatePhysical(mem_addr), reg_val);
  trace_writer_.WriteMemoryWrite(CpuToGpu(mem_addr), 4);

  return true;
}
XE_NOINLINE
bool COMMAND_PROCESSOR::ExecutePacketType3_MEM_WRITE(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  uint32_t write_addr = reader_.ReadAndSwap<uint32_t>();
  for (uint32_t i = 0; i < count - 1; i++) {
    uint32_t write_data = reader_.ReadAndSwap<uint32_t>();

    auto endianness = static_cast<xenos::Endian>(write_addr & 0x3);
    auto addr = write_addr & ~0x3;
    write_data = GpuSwap(write_data, endianness);
    xe::store(memory_->TranslatePhysical(addr), write_data);
    trace_writer_.WriteMemoryWrite(CpuToGpu(addr), 4);
    write_addr += 4;
  }

  return true;
}
XE_NOINLINE
bool COMMAND_PROCESSOR::ExecutePacketType3_COND_WRITE(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  // conditional write to memory or register
  uint32_t wait_info = reader_.ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader_.ReadAndSwap<uint32_t>();
  uint32_t ref = reader_.ReadAndSwap<uint32_t>();
  uint32_t mask = reader_.ReadAndSwap<uint32_t>();
  uint32_t write_reg_addr = reader_.ReadAndSwap<uint32_t>();
  uint32_t write_data = reader_.ReadAndSwap<uint32_t>();
  uint32_t value;
  if (wait_info & 0x10) {
    // Memory.
    auto endianness = static_cast<xenos::Endian>(poll_reg_addr & 0x3);
    poll_reg_addr &= ~0x3;
    trace_writer_.WriteMemoryRead(CpuToGpu(poll_reg_addr), 4);
    value = xe::load<uint32_t>(memory_->TranslatePhysical(poll_reg_addr));
    value = GpuSwap(value, endianness);
  } else {
    // Register.
    assert_true(poll_reg_addr < RegisterFile::kRegisterCount);
    value = register_file_->values[poll_reg_addr];
  }
  bool matched = MatchValueAndRef(value & mask, ref, wait_info);

  if (matched) {
    // Write.
    if (wait_info & 0x100) {
      // Memory.
      auto endianness = static_cast<xenos::Endian>(write_reg_addr & 0x3);
      write_reg_addr &= ~0x3;
      write_data = GpuSwap(write_data, endianness);
      xe::store(memory_->TranslatePhysical(write_reg_addr), write_data);
      trace_writer_.WriteMemoryWrite(CpuToGpu(write_reg_addr), 4);
    } else {
      // Register.
      COMMAND_PROCESSOR::WriteRegister(write_reg_addr, write_data);
    }
  }
  return true;
}
XE_FORCEINLINE
void COMMAND_PROCESSOR::WriteEventInitiator(uint32_t value) XE_RESTRICT {
  register_file_->values[XE_GPU_REG_VGT_EVENT_INITIATOR] = value;
}
bool COMMAND_PROCESSOR::ExecutePacketType3_EVENT_WRITE(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  // generate an event that creates a write to memory when completed
  uint32_t initiator = reader_.ReadAndSwap<uint32_t>();
  // Writeback initiator.

  COMMAND_PROCESSOR::WriteEventInitiator(initiator & 0x3f);
  if (count == 1) {
    // Just an event flag? Where does this write?
  } else {
    // Write to an address.
    assert_always();
    reader_.AdvanceRead((count - 1) * sizeof(uint32_t));
  }
  return true;
}
XE_NOINLINE
bool COMMAND_PROCESSOR::ExecutePacketType3_EVENT_WRITE_SHD(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  // generate a VS|PS_done event
  uint32_t initiator = reader_.ReadAndSwap<uint32_t>();
  uint32_t address = reader_.ReadAndSwap<uint32_t>();
  uint32_t value = reader_.ReadAndSwap<uint32_t>();
  // Writeback initiator.
  COMMAND_PROCESSOR::WriteEventInitiator(initiator & 0x3F);
  uint32_t data_value;
  if ((initiator >> 31) & 0x1) {
    // Write counter (GPU vblank counter?).
    data_value = counter_;
  } else {
    // Write value.
    data_value = value;
  }
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;
  data_value = GpuSwap(data_value, endianness);
  uint8_t* write_destination = memory_->TranslatePhysical(address);
  if (address > 0x1FFFFFFF) {
    uint32_t writeback_base =
        register_file_->values[XE_GPU_REG_WRITEBACK_START];
    uint32_t writeback_size = register_file_->values[XE_GPU_REG_WRITEBACK_SIZE];
    uint32_t writeback_offset = address - writeback_base;
    // check whether the guest has written writeback base. if they haven't, skip
    // the offset check
    if (writeback_base != 0 && writeback_offset < writeback_size) {
      write_destination =
          memory_->TranslateVirtual(0x7F000000 + writeback_offset);
    }
  }
  xe::store(write_destination, data_value);
  trace_writer_.WriteMemoryWrite(CpuToGpu(address), 4);
  return true;
}

bool COMMAND_PROCESSOR::ExecutePacketType3_EVENT_WRITE_EXT(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  // generate a screen extent event
  uint32_t initiator = reader_.ReadAndSwap<uint32_t>();
  uint32_t address = reader_.ReadAndSwap<uint32_t>();
  // Writeback initiator.
  COMMAND_PROCESSOR::WriteEventInitiator(initiator & 0x3F);
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;

  // Let us hope we can fake this.
  // This callback tells the driver the xy coordinates affected by a previous
  // drawcall.
  // https://www.google.com/patents/US20060055701
  uint16_t extents[] = {
      byte_swap<unsigned short>(0 >> 3),  // min x
      byte_swap<unsigned short>(xenos::kTexture2DCubeMaxWidthHeight >>
                                3),       // max x
      byte_swap<unsigned short>(0 >> 3),  // min y
      byte_swap<unsigned short>(xenos::kTexture2DCubeMaxWidthHeight >>
                                3),  // max y
      byte_swap<unsigned short>(0),  // min z
      byte_swap<unsigned short>(1),  // max z
  };
  assert_true(endianness == xenos::Endian::k8in16);

  uint16_t* destination = (uint16_t*)memory_->TranslatePhysical(address);

  for (unsigned i = 0; i < 6; ++i) {
    destination[i] = extents[i];
  }

  trace_writer_.WriteMemoryWrite(CpuToGpu(address), sizeof(extents));
  return true;
}

XE_NOINLINE
bool COMMAND_PROCESSOR::ExecutePacketType3_EVENT_WRITE_ZPD(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  assert_true(count == 1);
  uint32_t initiator = reader_.ReadAndSwap<uint32_t>();
  // Writeback initiator.
  COMMAND_PROCESSOR::WriteEventInitiator(initiator & 0x3F);

  uint32_t report_address =
      register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
  uint32_t report_record_base = XenosZPDReport::GetRecordBase(report_address);
  bool is_begin_record = XenosZPDReport::IsBeginRecord(report_address);
  bool is_end_record = XenosZPDReport::IsEndRecord(report_address);

  xe_gpu_depth_sample_counts* report =
      report_record_base
          ? memory_->TranslatePhysical<xe_gpu_depth_sample_counts*>(
                report_record_base)
          : nullptr;

  // True if the record has the pending D3D sentinel.
  // Useful as a hint, but not authoritative for report boundaries.
  // QueryBatch titles can have multiple pending sentinels in a row and don't
  // necessarily update in an order we currently observe.
  bool guest_marks_end = report && XenosZPDReport::HasPendingSentinel(report);
  bool logical_active = zpd_active_segment_.logical_active;

  // QueryBatch fake fallback, which ignores record layout and just returns an
  // incrementing sample count on each event.
  if (cvars::occlusion_query_querybatch_range > 0) {
    uint32_t sample_count =
        XenosZPDReport::QueryBatchFakeSamples(querybatch_zpd_sample_count_);
    if (report) {
      // Both QueryBatch and conventional fake samples skip elective saturation.
      XenosZPDReport::WriteSampleCount(report, sample_count, false);
    }
    return true;
  }

  if (GetZPDMode() != ZPDMode::kFake && !zpd_force_fake_fallback_) {
    if (logical_active && is_end_record) {
      COMMAND_PROCESSOR::EndZPDReport(report_address, false);
      return true;
    }
    if (is_begin_record) {
      // Clear the record so the game knows the BEGIN was processed and
      // stale sentinel data from a prior query lifetime doesn't persist.
      if (report) {
        std::memset(report, 0, sizeof(xe_gpu_depth_sample_counts));
      }
      COMMAND_PROCESSOR::BeginZPDReport(report_address);
      return true;
    }
    if (!logical_active && is_end_record) {
      // No logical report is active for this slot, so this is likely an
      // orphaned END. In fast mode, replay the last cached delta so polling
      // code does not sit on the sentinel forever.
      if (GetZPDMode() == ZPDMode::kFast || GetZPDMode() == ZPDMode::kFastAlt) {
        uint32_t cached_delta = 1;
        auto cache_it = fast_zpd_report_cached_values_.find(report_record_base);
        if (cache_it != fast_zpd_report_cached_values_.end()) {
          cached_delta = cache_it->second;
        }
        COMMAND_PROCESSOR::WriteZPDReport(0, report_record_base, 0,
                                          cached_delta, false);
      } else {
        // In strict mode, just pump in case a previous report has resolved.
        COMMAND_PROCESSOR::PumpQueryResolves();
      }
      return true;
    }
    // Address is neither BEGIN nor END (non-standard layout). Fall through
    // to the fake path so the guest at least gets a result written rather
    // than leaving the sentinel in place forever.
  }

  // Conventional fake fallback, which only touches records marked as pending.
  if (cvars::occlusion_query_fake_lower_threshold < 0 || !report_record_base ||
      !guest_marks_end) {
    return true;
  }

  fake_zpd_sample_count_ =
      (fake_zpd_sample_count_ <=
       static_cast<uint32_t>(cvars::occlusion_query_fake_lower_threshold))
          ? static_cast<uint32_t>(cvars::occlusion_query_fake_upper_threshold)
          : fake_zpd_sample_count_ - 1;

  XenosZPDReport::WriteSampleCount(report, fake_zpd_sample_count_, false);
  return true;
}

XE_NOINLINE
void COMMAND_PROCESSOR::GuideExtraResolve() {
  // Phase 530: resolve the Guide's geometry immediately after its burst of
  // draws. This previously ran just before IssueSwap, which phase 529 showed is
  // BEFORE the Guide draws - so it resolved a frame that did not contain them
  // and reported "no change" for 3400 frames. Here the pixels are still in
  // EDRAM and the title has not drawn over them yet.
  //
  // The resolve rectangle lives in vertex-fetch slot 0 and the destination in
  // RB_COPY_*, both of which the Guide's own draws overwrite, so restore the
  // state captured at the title's resolve and put the Guide's back afterwards.
  if (!cvars::guide_resolve_after_draw || !guide_resolve_saved_) return;
  RegisterFile& rf = *register_file_;
  uint32_t keep_copy[4], keep_vf0[2];
  for (uint32_t i = 0; i < 4; ++i) {
    keep_copy[i] = rf[0x2318 + i];
    rf[0x2318 + i] = guide_saved_copy_[i];
  }
  keep_vf0[0] = rf[0x4800];
  keep_vf0[1] = rf[0x4801];
  rf[0x4800] = guide_saved_vf0_[0];
  rf[0x4801] = guide_saved_vf0_[1];
  // Phase 884: GetResolveInfo rejects this with "Unsupported resolve vertex
  // buffer format", which is `fetch.type != kVertex || fetch.size != 6`.
  // Print what was saved and what is being restored, because "the title's own
  // resolve saved it" is an assumption about which IssueCopy ran last.
  {
    static uint32_t vlog = 0;
    if (vlog++ < 4) {
      XELOGI("GuideVF0: live {:08X} {:08X} | restoring {:08X} {:08X} "
             "(type={} size={})",
             keep_vf0[0], keep_vf0[1], guide_saved_vf0_[0], guide_saved_vf0_[1],
             guide_saved_vf0_[0] & 0x3u, (guide_saved_vf0_[1] >> 2) & 0x3FFFFFu);
      // Phase 887: guide_saved_surface_ is captured and never restored, so
      // this resolve reads whichever surface the Guide's draws left bound.
      // If that is still the title's, the resolve is reading the wrong EDRAM
      // and "copied nothing new" is explained without the geometry being
      // empty.
      XELOGI("GuideSurf: live SURFACE_INFO={:08X} COLOR_INFO={:08X} | title's "
             "were {:08X} {:08X} | copy live {:08X} {:08X} restoring {:08X} "
             "{:08X}",
             rf[0x2000], rf[0x2001], guide_saved_surface_[0],
             guide_saved_surface_[1], keep_copy[0], keep_copy[1],
             guide_saved_copy_[0], guide_saved_copy_[1]);
    }
  }

  uint32_t dest = guide_saved_copy_[1] & ~0xFFFu;
  auto sum_dest = [&]() -> uint32_t {
    if (!cvars::guide_verify_resolve || !dest) return 0;
    const uint8_t* pp = memory_->TranslatePhysical(dest);
    if (!pp) return 0;
    uint32_t h = 2166136261u;
    // Phase 533: 0x180000 covered under half a 1280x720 frame, so both the
    // checksum and the snapshot below missed most of the image. A full frame is
    // 1280*720*4 = 0x384000.
    for (uint32_t off = 0; off < 0x384000u; off += 0x400u) {
      h = (h ^ *reinterpret_cast<const uint32_t*>(pp + off)) * 16777619u;
    }
    return h;
  };
  uint32_t sum_before = sum_dest();

  // Phase 532: snapshot the destination either side of the copy. Every dword
  // that changes is the Guide's contribution to this frame.
  const uint32_t kFbWords = 0x384000u / 4u;
  bool snap = cvars::guide_composite_frontbuffer && dest;
  const uint8_t* fbp = snap ? memory_->TranslatePhysical(dest) : nullptr;
  if (fbp) {
    guide_fb_before_.resize(kFbWords);
    std::memcpy(guide_fb_before_.data(), fbp, kFbWords * 4u);
  }

  guide_resolve_replay_ = true;
  bool ok = COMMAND_PROCESSOR::IssueCopy();
  guide_resolve_replay_ = false;

  if (fbp) {
    guide_fb_after_.resize(kFbWords);
    std::memcpy(guide_fb_after_.data(), fbp, kFbWords * 4u);
    guide_fb_dest_ = dest;
    guide_fb_valid_ = true;
  }

  uint32_t sum_after = sum_dest();

  // Phase 541: phase 530 showed this resolve changes the destination 1600/1600
  // times, so it contains the Guide's rendered pixels. Dump it once and look at
  // it - that answers "what does the Guide actually draw" directly, where every
  // checksum so far could only say "something changed".
  if (cvars::guide_verify_resolve && dest) {
    static bool dumped = false;
    if (!dumped) {
      dumped = true;
      const uint8_t* pp = memory_->TranslatePhysical(dest);
      if (pp) {
        auto dpath = xe::filesystem::GetExecutableFolder() / "guide_resolve.raw";
        FILE* df = xe::filesystem::OpenFile(dpath, "wb");
        if (df) {
          uint32_t hdr[3] = {1280u, 720u, 1280u * 4u};
          fwrite(hdr, sizeof(hdr), 1, df);
          fwrite(pp, 1, 1280u * 720u * 4u, df);
          fclose(df);
          XELOGI("GuideResolveDump: wrote the post-Guide resolve at {:08X}",
                 dest);
        }
      }
    }
  }

  for (uint32_t i = 0; i < 4; ++i) rf[0x2318 + i] = keep_copy[i];
  rf[0x4800] = keep_vf0[0];
  rf[0x4801] = keep_vf0[1];

  static uint32_t reslog = 0, changed = 0, total = 0;
  ++total;
  if (sum_before != sum_after) ++changed;
  if (reslog++ < 8 || (total % 200u) == 0u) {
    XELOGI("GuideResolve: after burst, IssueCopy {} dest={:08X} sum {:08X} -> "
           "{:08X} {} | {}/{} resolves changed the image",
           ok ? "ok" : "FAILED", dest, sum_before, sum_after,
           (sum_before != sum_after) ? "CHANGED" : "same", changed, total);
  }
}


bool COMMAND_PROCESSOR::ExecutePacketType3Draw(
    uint32_t packet, const char* opcode_name, uint32_t viz_query_condition,
    uint32_t count_remaining) XE_RESTRICT {
  // if viz_query_condition != 0, this is a conditional draw based on viz query.
  // This ID matches the one issued in PM4_VIZ_QUERY
  // uint32_t viz_id = viz_query_condition & 0x3F;
  // when true, render conditionally based on query result
  // uint32_t viz_use = viz_query_condition & 0x100;

  assert_not_zero(count_remaining);
  if (!count_remaining) {
    XELOGE("{}: Packet too small, can't read VGT_DRAW_INITIATOR", opcode_name);
    return false;
  }
  reg::VGT_DRAW_INITIATOR vgt_draw_initiator;
  vgt_draw_initiator.value = reader_.ReadAndSwap<uint32_t>();
  --count_remaining;

  register_file_->values[XE_GPU_REG_VGT_DRAW_INITIATOR] =
      vgt_draw_initiator.value;
  // Phase 526: the Guide's draws produce no fragments and every piece of state
  // around them is permissive (phase 525), so read the draw itself. A zero index
  // count or a degenerate primitive type rasterises to nothing and would explain
  // it exactly.
  if (guide_in_draw_scope_ || guide_replaying_) {
    // Phase 538: separate counters for burst and replay. A shared one was
    // exhausted by the burst's 15 draws per frame before a single replay
    // entry printed, which reads as "the replay is not drawing" when it is
    // only "the log was full". The phase-537 attempt to widen it silently
    // did nothing - the patch text had the wrong indentation.
    static uint32_t dilog_burst = 0, dilog_replay = 0;
    if (guide_replaying_ ? (dilog_replay++ < 6) : (dilog_burst++ < 6)) {
      // IssueDraw is skipped entirely - without setting draw_succeeded false
      // and without logging - when viz_query_ena && kill_pix_post_hi_z. That is
      // a silent skip that looks exactly like what is being seen: the packet is
      // counted, no backend failure appears, and no pixels are written.
      auto vq = register_file_->Get<reg::PA_SC_VIZ_QUERY>();
      // Phase 537: auto-indexed draws read geometry through the vertex fetch
      // constants at 0x4800+. If those are set outside the captured IB, the
      // replay runs with whatever the title last left there - valid draws,
      // correct target, geometry pointing at the wrong memory.
      {
        RegisterFile& frf = *register_file_;
        // Phase 550: vertex fetch slots are 2 dwords apart - 96 of them
        // (kVertexFetchConstantCount = 3 * 32), which the dumped shader
        // confirms by fetching vf95. The Guide writes base=4800 (vf0/1/2) and
        // base=48BA (vf93/94/95), and the two dumped vertex shaders fetch vf0
        // and vf95 respectively. Print both so the Guide's actual source is
        // identifiable rather than guessed.
        XELOGI("GuideFetch[{}]: vf0={:08X}/{:08X} vf1={:08X}/{:08X} "
               "vf2={:08X}/{:08X} | vf93={:08X}/{:08X} vf94={:08X}/{:08X} "
               "vf95={:08X}/{:08X}",
               guide_replaying_ ? "replay" : "burst", frf[0x4800], frf[0x4801],
               frf[0x4802], frf[0x4803], frf[0x4804], frf[0x4805],
               frf[0x48BA], frf[0x48BB], frf[0x48BC], frf[0x48BD], frf[0x48BE],
               frf[0x48BF]);
        // Phase 542: the question never asked - WHERE is this geometry? The
        // fetch constant carries the vertex buffer address in its upper bits;
        // read the first vertices and print them as floats. Off-screen or
        // degenerate positions would explain draws that are well-formed,
        // correctly targeted, and cover nothing.
        // Phase 547: fetch slots are SIX dwords apart, not two -
        // SHADER_CONSTANT_FETCH_00_0 .. _31_5 is 32 slots x 6. Indexing by 2
        // was decoding dwords 2-5 of a texture descriptor as separate vertex
        // constants, which is where phases 542/544's "slot 2, kVertex,
        // address 0" came from. Walk all 32 slots at the right stride and
        // report only the ones that really are vertex fetches.
        // Phase 550: vf95 is the Guide's real vertex binding - valid kVertex,
        // 32 dwords, address advancing 0x80 per draw. Its shader
        // (1E6883FCCDE1F688) reads position straight from the fetch with no
        // transform, so the floats at that address ARE the screen coordinates.
        // Read them; this is the last unexamined value in the chain.
        for (uint32_t slot = 95; slot < 96; ++slot) {
          uint32_t w0 = frf[0x4800 + slot * 2], w1 = frf[0x4801 + slot * 2];
          uint32_t type = w0 & 3u;
          if (type != 3u) continue;              // kVertex only
          // xe_gpu_vertex_fetch_t: address is a 30-bit field at bit 2 holding
          // an address in DWORDS, so the byte address is (w0 >> 2) * 4, i.e.
          // w0 & ~3. The first version shifted left instead and produced
          // out-of-range addresses that read as zeros - which looked like null
          // geometry and was purely the arithmetic.
          uint32_t addr = w0 & ~3u;
          uint32_t size_dw = (w1 >> 2) & 0xFFFFFFu;  // size:24 at bit 2
          const uint8_t* vp = memory_->TranslatePhysical(addr);
          if (!vp || size_dw < 4) continue;
          float f[7];
          for (uint32_t k = 0; k < 7; ++k) {
            uint32_t raw = xe::load_and_swap<uint32_t>(vp + k * 4);
            std::memcpy(&f[k], &raw, 4);
          }
          // Phase 716: seven floats out of a 32-dword buffer is not enough
          // to tell a NaN vertex from a misread stride. Print the whole
          // buffer, capped, so the layout is visible rather than inferred.
          std::string vdump;
          for (uint32_t vi = 0; vi < size_dw && vi < 32u; ++vi) {
            vdump += fmt::format("{} ", f[vi]);
          }
          XELOGI("GuideVerts[vf{}]: addr={:08X} dwords={} | {}", slot, addr,
                 size_dw, vdump);
        }
      }
      // Phase 527: phase 522 concluded "the Guide draws after the frame's
      // resolve" from counters read on the TITLE thread while they are updated
      // here on the GPU thread. This log runs on the GPU thread, so printing the
      // same counters here settles the ordering without a cross-thread read.
      XELOGI("GuideDrawIndx: [resolves={} swaps={} clears={}] prim_type={} "
             "source_select={} num_indices={} | viz_query_ena={} "
             "kill_pix_post_hi_z={} -> {}",
             guide_resolve_count_, guide_swap_count_, guide_clear_count_,
             uint32_t(vgt_draw_initiator.prim_type),
             uint32_t(vgt_draw_initiator.source_select),
             uint32_t(vgt_draw_initiator.num_indices),
             uint32_t(vq.viz_query_ena), uint32_t(vq.kill_pix_post_hi_z),
             (vq.viz_query_ena && vq.kill_pix_post_hi_z) ? "SKIPPED"
                                                        : "issued");
    }
  }
  bool draw_succeeded = true;
  // TODO(Triang3l): Remove IndexBufferInfo and replace handling of all this
  // with PrimitiveProcessor when the old Vulkan renderer is removed.
  bool is_indexed = false;
  IndexBufferInfo index_buffer_info;
  switch (vgt_draw_initiator.source_select) {
    case xenos::SourceSelect::kDMA: {
      // Indexed draw.
      is_indexed = true;

      // Two separate bounds checks so if there's only one missing register
      // value out of two, one uint32_t will be skipped in the command buffer,
      // not two.
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        XELOGE("{}: Packet too small, can't read VGT_DMA_BASE", opcode_name);
        return false;
      }
      uint32_t vgt_dma_base = reader_.ReadAndSwap<uint32_t>();
      --count_remaining;
      register_file_->values[XE_GPU_REG_VGT_DMA_BASE] = vgt_dma_base;
      reg::VGT_DMA_SIZE vgt_dma_size;
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        XELOGE("{}: Packet too small, can't read VGT_DMA_SIZE", opcode_name);
        return false;
      }
      vgt_dma_size.value = reader_.ReadAndSwap<uint32_t>();
      --count_remaining;
      register_file_->values[XE_GPU_REG_VGT_DMA_SIZE] = vgt_dma_size.value;

      uint32_t index_size_bytes =
          vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16
              ? sizeof(uint16_t)
              : sizeof(uint32_t);
      // The base address must already be word-aligned according to the R6xx
      // documentation, but for safety.
      index_buffer_info.guest_base = vgt_dma_base & ~(index_size_bytes - 1);
      index_buffer_info.endianness = vgt_dma_size.swap_mode;
      index_buffer_info.format = vgt_draw_initiator.index_size;
      index_buffer_info.length = vgt_dma_size.num_words * index_size_bytes;
      index_buffer_info.count = vgt_draw_initiator.num_indices;
    } break;
    case xenos::SourceSelect::kImmediate: {
      // TODO(Triang3l): VGT_IMMED_DATA.
      XELOGE(
          "{}: Using immediate vertex indices, which are not supported yet. "
          "Report the game to Xenia developers!",
          opcode_name, uint32_t(vgt_draw_initiator.source_select));
      draw_succeeded = false;
      assert_always();
    } break;
    case xenos::SourceSelect::kAutoIndex: {
      // Auto draw.
      index_buffer_info.guest_base = 0;
      index_buffer_info.length = 0;
    } break;
    default: {
      // Invalid source selection.
      draw_succeeded = false;
      assert_unhandled_case(vgt_draw_initiator.source_select);
    } break;
  }

  // Skip to the next command, for example, if there are immediate indexes that
  // we don't support yet.
  reader_.AdvanceRead(count_remaining * sizeof(uint32_t));

  // Phase 890: guide_draw_count_ increments when the PACKET is seen, several
  // steps before anything reaches the backend, so "541 draws dispatched" is
  // not evidence that 541 draws were issued. Count what actually happens to
  // the Guide's draws: dropped before the call, called and refused, or issued.
  // Phase 893: log the same state at a TITLE draw. Fixing the Guide's
  // registers one at a time has found two defects and neither was enough;
  // the differences against a draw that demonstrably renders are the whole
  // remaining list, in one reading.
  // Phase 926: ndc_scale = guest_viewport_scale * 2/extent, so it converts
  // PIXELS to NDC. The title's 0.000244 implies its vertices span 0..8192,
  // not 0..1280. Read the title's own vertex data and settle what space it
  // works in - the Guide's coordinates only make sense relative to it.
  if (!guide_overlay_exec_) {
    static uint32_t tvd = 0;
    if (tvd < 2) {
      Shader* tvs = active_vertex_shader();
      if (tvs && !tvs->vertex_bindings().empty()) {
        RegisterFile& trf2 = *register_file_;
        for (auto& vb : tvs->vertex_bindings()) {
          uint32_t fc = vb.fetch_constant;
          uint32_t s0 = trf2[0x4800 + fc * 2];
          uint32_t addr = s0 & 0xFFFFFFFCu;
          const uint8_t* vp = addr ? memory_->TranslatePhysical(addr) : nullptr;
          if (!vp) continue;
          std::string fl;
          for (uint32_t k = 0; k < 8; ++k) {
            uint32_t raw = xe::load_and_swap<uint32_t>(vp + k * 4);
            float f;
            std::memcpy(&f, &raw, 4);
            fl += fmt::format("{} ", f);
          }
          XELOGI("TitleVertexData: slot{} @{:08X} stride={} : {}", fc, addr,
                 vb.stride_words, fl);
          ++tvd;
          break;
        }
      }
    }
    // Sampling the FIRST title draws reads all zeros - they happen before the
    // title has set any state. Keep the most recent one instead, which is the
    // state of a draw that demonstrably rendered, and report it next to the
    // Guide's.
    RegisterFile& trf = *register_file_;
    static const uint32_t kIdx[12] = {0x2208, 0x2000, 0x2001, 0x2002,
                                      0x2104, 0x2202, 0x2200, 0x2201,
                                      0x2081, 0x2082, 0x2206, 0x2205};
    for (uint32_t i = 0; i < 12; ++i) guide_title_state_[i] = trf[kIdx[i]];
    guide_title_state_valid_ = true;
    for (uint32_t r = kGuideCtxLo; r < kGuideCtxHi; ++r) {
      guide_title_regs_[r - kGuideCtxLo] = trf[r];
    }
    guide_title_regs_valid_ = true;
  }
  // Phase 974: the same marker, on the title's own draws. If the screen does
  // not turn magenta this patch never reaches the GPU, and every negative
  // taken with guide_marker_color measures the instrument rather than the
  // Guide.
  if (cvars::guide_clear_rt_title && !guide_overlay_exec_) {
    COMMAND_PROCESSOR::GuideClearRenderTarget();
  }
  if (cvars::guide_marker_title && !guide_overlay_exec_) {
    RegisterFile& mrf = *register_file_;
    auto mf = [&](uint32_t r, float f) {
      uint32_t v;
      std::memcpy(&v, &f, 4);
      mrf[r] = v;
    };
    mf(0x4400, 1.0f); mf(0x4401, 0.0f); mf(0x4402, 1.0f); mf(0x4403, 1.0f);
    mf(0x4404, 1.0f); mf(0x4405, 1.0f); mf(0x4406, 1.0f); mf(0x4407, 1.0f);
    mrf[0x2201] = 0x00010001u;
    mrf[0x2202] = (mrf[0x2202] & ~0x1Fu) | 0x7u;
    COMMAND_PROCESSOR::GuideInvalidateFloatConstants();
  }
  if (guide_overlay_exec_) {
    ++guide_ov_seen_;
    if (!draw_succeeded) ++guide_ov_predrop_;
    // Phase 916: each packet adds ~3000 scattered pixels where its geometry is
    // a 323x1 quad. Read what the draw actually declares - primitive type and
    // index count - since a mis-read primitive turns one strip into speckle.
    {
      static uint32_t pdl = 0;
      if (pdl++ < 6) {
        XELOGI("GuideDrawPrim: #{} prim_type={} num_indices={} indexed={} "
               "src_sel={}",
               guide_ov_seen_, uint32_t(vgt_draw_initiator.prim_type),
               uint32_t(vgt_draw_initiator.num_indices), is_indexed ? 1 : 0,
               uint32_t(vgt_draw_initiator.source_select));
      }
    }
    // Phase 891: RB_COLOR_INFO has only ever been sampled after the burst,
    // where it holds whatever the resolve section left. Read it at the draws
    // themselves - the first, and the first one after the mode flips to
    // kCopy - so the geometry's own EDRAM base is on the record.
    static uint32_t fate_log = 0;
    RegisterFile& drf = *register_file_;
    // Phase 892: restoring the surface before the stream runs is useless - the
    // stream writes SURFACE_INFO=0 itself, between the restore and the first
    // draw. Patch it at the draw instead, which is the only point where the
    // value has to be right.
    if (cvars::guide_overlay_restore_surface && guide_resolve_saved_ &&
        (drf[0x2000] & 0x3FFFu) == 0u) {
      drf[0x2000] = guide_saved_surface_[0];
      drf[0x2001] = guide_saved_surface_[1];
      ++guide_ov_surfpatch_;
    }
    // RB_COLOR_MASK is 0 at the Guide's draws - every channel of every render
    // target masked off, so nothing can be written no matter what else is
    // right. Enable RT0's four channels.
    if (cvars::guide_overlay_mask_off) {
      // Execute the draws with every colour channel masked off.
      drf[0x2104] = 0u;
    } else if (cvars::guide_overlay_restore_surface && drf[0x2104] == 0u) {
      drf[0x2104] = 0x0000000Fu;
      ++guide_ov_maskpatch_;
    }
    // Phase 911: the vertices are pixel-sized, so the alternative to supplying
    // a viewport is switching the transform off, which is what the title does
    // for its own pre-transformed geometry.
    // Phase 927: PA_CL_CLIP_CNTL.clip_disable is the register that differs
    // between the title (1) and the Guide (0), and it selects which viewport
    // path Xenia takes.
    if (cvars::guide_overlay_clip_disable &&
        !((drf[0x2204] >> 16) & 1u)) {
      drf[0x2204] |= (1u << 16);
      ++guide_ov_clippatch_;
    }
    if (cvars::guide_overlay_vte_passthru && (drf[0x2206] & 0x3Fu)) {
      drf[0x2206] = 0x00000300u;
      ++guide_ov_vtepatch_;
    }
    // VTE_CNTL enables the viewport scale/offset transform and every viewport
    // register is zero, so each vertex maps to a single point. Supply the
    // transform for the surface the draws are being pointed at.
    // Phase 923: these were mutually exclusive and each is half of one fix.
    // With VTE off, Xenia derives the viewport from PA_CL_VPORT_*, which are
    // zero - giving a 1x1 viewport and a black frame. With VTE on, Xenia
    // assumes the guest already transformed and passes vertices through as
    // NDC, but they are pixel coordinates. The pair needed is the viewport
    // values AND the transform disabled, so allow both.
    if (cvars::guide_overlay_restore_surface &&
        (cvars::guide_overlay_vte_passthru || (drf[0x2206] & 0x3Fu)) &&
        drf[0x210F] == 0u && drf[0x2111] == 0u) {
      auto setf = [&](uint32_t r, float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        drf[r] = v;
      };
      if (cvars::guide_overlay_vport_identity) {
        setf(0x210F, 1.0f);   // x scale
        setf(0x2110, 0.0f);   // x offset
        setf(0x2111, 1.0f);   // y scale
        setf(0x2112, 0.0f);   // y offset
      } else {
        setf(0x210F, 640.0f);   // x scale
        setf(0x2110, 640.0f);   // x offset
        setf(0x2111, -360.0f);  // y scale
        setf(0x2112, 360.0f);   // y offset
      }
      setf(0x2113, 1.0f);     // z scale
      setf(0x2114, 0.0f);     // z offset
      ++guide_ov_vportpatch_;
    }
    // Phase 953: c4.x and c5.y are 2/w and -2/h computed with w = h = 0, so
    // they are infinite and every transformed vertex is non-finite. Supply the
    // values a 1280x720 target implies.
    if (cvars::guide_fix_projection) {
      float c4x;
      uint32_t r4 = drf[0x4010];
      std::memcpy(&c4x, &r4, 4);
      if (!std::isfinite(c4x)) {
        auto setf = [&](uint32_t r, float f) {
          uint32_t v;
          std::memcpy(&v, &f, 4);
          drf[r] = v;
        };
        // Phase 962: the Guide's device reports 852x480 (961), not the
        // title's 1280x720. Every patch so far has supplied the title's
        // numbers to geometry authored against the Guide's own target.
        setf(0x4010, 2.0f / 852.0f);   // c4.x =  2/w
        setf(0x4015, -2.0f / 480.0f);  // c5.y = -2/h
        // The register file is not what the shader reads - invalidate the
        // uploaded constant buffer so these values are sent to the GPU.
        COMMAND_PROCESSOR::GuideInvalidateFloatConstants();
        ++guide_ov_projpatch_;
      }
    }
    // Phase 969: every earlier reading covers what the draw declares - state,
    // shader, constants, bindings - and none covers how large the resulting
    // primitive is. Draw #1's quad is 323x1 pixels after the model transform
    // (964): one pixel tall. A sub-pixel primitive rasterises to nothing
    // however correct everything feeding it is, and the extent has only ever
    // been computed for that one draw. Measure all of them.
    if (cvars::guide_quad_census) {
      Shader* qvs = active_vertex_shader();
      uint32_t qfc = 0xFFFFFFFFu;
      if (qvs && !qvs->vertex_bindings().empty()) {
        qfc = qvs->vertex_bindings()[0].fetch_constant;
      }
      const uint8_t* qvd = nullptr;
      uint32_t qaddr = 0, qwords = 0;
      if (qfc != 0xFFFFFFFFu) {
        uint32_t s0 = drf[0x4800 + qfc * 2];
        uint32_t s1 = drf[0x4801 + qfc * 2];
        qaddr = s0 & 0xFFFFFFFCu;
        qwords = (s1 >> 2) & 0xFFFFFFu;
        if (qaddr) qvd = memory_->TranslatePhysical(qaddr);
      }
      auto qcf = [&](uint32_t i) {
        float f;
        uint32_t v = drf[0x4000 + i];
        std::memcpy(&f, &v, 4);
        return f;
      };
      if (cvars::guide_invalidate_vertex && qaddr && qwords) {
        COMMAND_PROCESSOR::GuideInvalidateGuestRange(qaddr, qwords * 4u);
      }
      uint32_t qnv = uint32_t(vgt_draw_initiator.num_indices);
      if (qvd && qnv >= 3 && qnv * 2 <= qwords) {
        // 964: r1.z carries the X axis and r1.x the Y axis, so
        //   X = vx*c0.x + vy*c0.y + c0.w
        //   Y = vx*c1.x + vy*c1.y + c1.w
        float c0x = qcf(0), c0y = qcf(1), c0w = qcf(3);
        float c1x = qcf(4), c1y = qcf(5), c1w = qcf(7);
        float lox = 1e30f, hix = -1e30f, loy = 1e30f, hiy = -1e30f;
        bool fin = true;
        for (uint32_t k = 0; k < qnv && k * 2 + 1 < qwords; ++k) {
          uint32_t ax = xe::load_and_swap<uint32_t>(qvd + (k * 2) * 4);
          uint32_t ay = xe::load_and_swap<uint32_t>(qvd + (k * 2 + 1) * 4);
          float vx, vy;
          std::memcpy(&vx, &ax, 4);
          std::memcpy(&vy, &ay, 4);
          float X = vx * c0x + vy * c0y + c0w;
          float Y = vx * c1x + vy * c1y + c1w;
          if (!std::isfinite(X) || !std::isfinite(Y)) fin = false;
          lox = std::min(lox, X);
          hix = std::max(hix, X);
          loy = std::min(loy, Y);
          hiy = std::max(hiy, Y);
        }
        if (fin) {
          float qw = hix - lox, qh = hiy - loy;
          ++guide_ov_qn_;
          if (qh < 1.0f) ++guide_ov_qthin_;
          if (qw < 1.0f) ++guide_ov_qnarrow_;
          if (qw < 0.01f && qh < 0.01f) ++guide_ov_qdegen_;
          guide_ov_qmaxw_ = std::max(guide_ov_qmaxw_, qw);
          guide_ov_qmaxh_ = std::max(guide_ov_qmaxh_, qh);
          guide_ov_qbb_[0] = std::min(guide_ov_qbb_[0], lox);
          guide_ov_qbb_[1] = std::min(guide_ov_qbb_[1], loy);
          guide_ov_qbb_[2] = std::max(guide_ov_qbb_[2], hix);
          guide_ov_qbb_[3] = std::max(guide_ov_qbb_[3], hiy);
          if (guide_ov_qn_ <= 10) {
            XELOGI("GuideQuad: #{} nv={} @{:08X} x[{}..{}] y[{}..{}] = {}x{}",
                   guide_ov_qn_, qnv, qaddr, lox, hix, loy, hiy, qw, qh);
          }
        } else {
          ++guide_ov_qnonfin_;
        }
      } else {
        ++guide_ov_qskip_;
      }
    }
    // Phase 972: every diff-based instrument in this log is blind where it
    // matters. The blade lands at window x 365..959, y 139..540 (971), and
    // capdiff's animation mask covers 320,195,960,567 - so the masked reading
    // that reports "0 differing pixels" excludes most of the region the Guide
    // draws in, and unmasked the menu animation floods it at ~9%. An absolute
    // test needs no reference frame: paint the fragments a colour the title
    // cannot produce, and count exact matches in a single capture.
    if (cvars::guide_marker_color) {
      auto mf = [&](uint32_t r, float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        drf[r] = v;
      };
      // The whole pixel shader is "mul oC0, c0, c1" (955), and the pixel half
      // of the constant file starts at 0x4400, not 0x4000 (970).
      mf(0x4400, 1.0f); mf(0x4401, 0.0f); mf(0x4402, 1.0f); mf(0x4403, 1.0f);
      mf(0x4404, 1.0f); mf(0x4405, 1.0f); mf(0x4406, 1.0f); mf(0x4407, 1.0f);
      drf[0x2201] = 0x00010001u;                    // src One, dst Zero, ADD
      drf[0x2202] = (drf[0x2202] & ~0x1Fu) | 0x7u;  // alpha test/to-mask off
      COMMAND_PROCESSOR::GuideInvalidateFloatConstants();
      ++guide_ov_markerpatch_;
    }
    uint32_t mode_now = drf[0x2208] & 0x7u;
    // Phase 918: the vertex data was read once, for draw #1, and generalised
    // to all 416 (917). Sample across the burst instead - if the quads grow to
    // screen size the speckle is the Guide's own interface, not corruption.
    bool spread = (guide_ov_seen_ == 1 || guide_ov_seen_ == 5 ||
                   guide_ov_seen_ == 20 || guide_ov_seen_ == 60 ||
                   guide_ov_seen_ == 150 || guide_ov_seen_ == 300);
    if (spread) {
      if (true) {
        ++fate_log;
        XELOGI("GuideDrawSurf: draw #{} mode={} SURFACE_INFO={:08X} "
               "COLOR_INFO={:08X} DEPTH_INFO={:08X} | COLOR_MASK={:08X} "
               "COLORCONTROL={:08X} DEPTHCONTROL={:08X} BLEND0={:08X} | "
               "scissor {:08X} {:08X} VTE={:08X} SU_SC={:08X} WINOFF={:08X}",
               guide_ov_seen_, mode_now, drf[0x2000], drf[0x2001], drf[0x2002],
               drf[0x2104], drf[0x2202], drf[0x2200], drf[0x2201], drf[0x2081],
               drf[0x2082], drf[0x2206], drf[0x2205], drf[0x2080]);
        // VTE=0x43F enables the viewport scale/offset transform, which the
        // title (0x300) does not use. With those registers zero every vertex
        // collapses to a point, which looks exactly like geometry that covers
        // nothing.
        auto vf = [&](uint32_t r) {
          float f;
          uint32_t v = drf[r];
          std::memcpy(&f, &v, 4);
          return f;
        };
        // Phase 971: PA_CL_CLIP_CNTL reads 0008000F for the Guide's draws and
        // 00090000 for the title's. The low four bits are ucp_ena_0..3, so
        // the Guide enables four USER CLIP PLANES and does not set
        // clip_disable, while the title enables none and disables clipping
        // outright. Xenia implements these (pipeline_cache.cc:633) by emitting
        // SV_ClipDistance from PA_CL_UCP_n - registers the Guide's stream
        // never writes. Read what they actually hold.
        {
          uint32_t cc = drf[0x2204];
          std::string up;
          for (uint32_t pl = 0; pl < 6; ++pl) {
            if (!((cc >> pl) & 1u)) continue;
            up += fmt::format("ucp{}=(", pl);
            for (uint32_t c = 0; c < 4; ++c) {
              up += fmt::format("{}{}", c ? "," : "", vf(0x2388 + pl * 4 + c));
            }
            up += ") ";
          }
          XELOGI("GuideUCP: CLIP_CNTL={:08X} ucp_ena={:X} clip_disable={} | {}",
                 cc, cc & 0x3Fu, (cc >> 16) & 1u,
                 up.empty() ? "<none enabled>" : up);
        }
        XELOGI("GuideViewport: xscale={} xoffset={} yscale={} yoffset={} "
               "zscale={} zoffset={}",
               vf(0x210F), vf(0x2110), vf(0x2111), vf(0x2112), vf(0x2113),
               vf(0x2114));
        // Phase 895: the state is now the title's own and nothing appears, so
        // look at the geometry. Vertex buffers come from the fetch constants
        // at 0x4800 + 2*slot: dword_0 is type:2 | address:30 with the address
        // in dwords, dword_1 is endian:2 | size:24. Dump the slots that hold
        // a vertex fetch and the first floats each one points at.
        // Phase 909: phases 895-907 assumed the null in fetch slot 2 is what
        // starves these draws, without checking that the shader reads slot 2
        // at all. The vertex shader lists the constants it fetches from, so
        // ask it rather than assuming.
        {
          Shader* vs = active_vertex_shader();
          std::string need;
          if (vs) {
            for (auto& vb : vs->vertex_bindings()) {
              uint32_t fc = vb.fetch_constant;
              uint32_t s0 = drf[0x4800 + fc * 2];
              uint32_t s1 = drf[0x4801 + fc * 2];
              uint32_t vaddr = s0 & 0xFFFFFFFCu;
              uint32_t vwords = (s1 >> 2) & 0xFFFFFFu;
              // Phase 939: replace the geometry with a full-screen NDC quad,
              // as a triangle fan of four vertices matching the declared
              // k_32_32_FLOAT stride-2 format. If this does not appear, the
              // injection path cannot draw at all.
              if (cvars::guide_overlay_test_quad && vaddr) {
                uint8_t* wp = memory_->TranslatePhysical(vaddr);
                if (wp) {
                  // Phase 950: the phase-939 quad was written in NDC, but the
                  // shader applies the model transform in c0/c1 first - a
                  // translate of (285,163) for the first draw - which put it
                  // far off screen. That control was invalid. Compensate with
                  // this draw's own translate so the result lands at -1..1.
                  float tx, ty;
                  uint32_t rx = drf[0x4003], ry = drf[0x4007];
                  std::memcpy(&tx, &rx, 4);
                  std::memcpy(&ty, &ry, 4);
                  const float kQuad[8] = {
                      -1.f - tx, -1.f - ty, 1.f - tx, -1.f - ty,
                      1.f - tx,  1.f - ty,  -1.f - tx, 1.f - ty};
                  for (uint32_t k = 0; k < 8; ++k) {
                    uint32_t raw;
                    std::memcpy(&raw, &kQuad[k], 4);
                    xe::store_and_swap<uint32_t>(wp + k * 4, raw);
                  }
                  ++guide_ov_quadpatch_;
                }
              }
              // Phase 969: the phase-950 compensation subtracted the model
              // translate from an NDC quad, mixing two coordinate spaces -
              // the shader's whole chain up to c4..c7 is in the Guide's pixel
              // space. Express the quad there and invert the model transform
              // properly, so the vertices land on a chosen pixel rectangle
              // whatever c0/c1 hold.
              if (cvars::guide_overlay_quad_px && vaddr) {
                uint8_t* wp = memory_->TranslatePhysical(vaddr);
                auto pcf = [&](uint32_t i) {
                  float f;
                  uint32_t v = drf[0x4000 + i];
                  std::memcpy(&f, &v, 4);
                  return f;
                };
                float c0x = pcf(0), c0y = pcf(1), c0w = pcf(3);
                float c1x = pcf(4), c1y = pcf(5), c1w = pcf(7);
                float det = c0x * c1y - c0y * c1x;
                if (wp && std::isfinite(det) && (det > 1e-6f || det < -1e-6f)) {
                  // Same winding as the Guide's own fan (964):
                  // (maxX,maxY) (minX,maxY) (minX,minY) (maxX,minY).
                  const float kX[4] = {700.f, 120.f, 120.f, 700.f};
                  const float kY[4] = {380.f, 380.f, 100.f, 100.f};
                  for (uint32_t k = 0; k < 4; ++k) {
                    float tX = kX[k] - c0w, tY = kY[k] - c1w;
                    float vx = (tX * c1y - tY * c0y) / det;
                    float vy = (c0x * tY - c1x * tX) / det;
                    uint32_t rw;
                    std::memcpy(&rw, &vx, 4);
                    xe::store_and_swap<uint32_t>(wp + (k * 2) * 4, rw);
                    std::memcpy(&rw, &vy, 4);
                    xe::store_and_swap<uint32_t>(wp + (k * 2 + 1) * 4, rw);
                  }
                  // Written from host code, which does not trip the write
                  // watch, so SharedMemory would keep serving the old copy.
                  COMMAND_PROCESSOR::GuideInvalidateGuestRange(vaddr, 8 * 4u);
                  if (guide_ov_quadpatch_ < 3) {
                    XELOGI("GuidePxQuad: patched @{:08X} det={} c0=({},{},{}) "
                           "c1=({},{},{})",
                           vaddr, det, c0x, c0y, c0w, c1x, c1y, c1w);
                  }
                  ++guide_ov_quadpatch_;
                }
              }
              need += fmt::format("slot{}(type={} addr={:08X} words={} stride={}) ",
                                  fc, s0 & 0x3u, vaddr, vwords,
                                  vb.stride_words);
              // Phase 925: everything since phase 910 assumes these are
              // float32 because they decode as clean floats. The fetch
              // instruction declares the real format - read it.
              for (auto& at : vb.attributes) {
                const auto& op = at.fetch_instr.attributes;
                XELOGI("GuideVFmt: slot{} data_format={} offset={} stride={} "
                       "exp_adjust={} signed={} norm={}",
                       fc, uint32_t(op.data_format), op.offset, op.stride,
                       op.exp_adjust, op.is_signed ? 1 : 0,
                       uint32_t(op.signed_rf_mode) );
              }
              // Phase 910: the binding is valid, so read what it points at.
              // These are post-conversion physical addresses.
              const uint8_t* vdat =
                  vaddr ? memory_->TranslatePhysical(vaddr) : nullptr;
              if (vdat && vwords) {
                std::string fl;
                for (uint32_t k = 0; k < 12 && k < vwords; ++k) {
                  uint32_t raw = xe::load_and_swap<uint32_t>(vdat + k * 4);
                  float f;
                  std::memcpy(&f, &raw, 4);
                  fl += fmt::format("{} ", f);
                }
                XELOGI("GuideVertexData: slot{} @{:08X} [{} words]: {}", fc,
                       vaddr, vwords, fl);
              }
            }
          }
          // Phase 918: every quad is small and starts at the origin, so their
        // position has to come from somewhere else - a transform in the ALU
        // constants. Phase 897 found no SET_CONSTANT of any type in the arena,
        // so read the first float constants and see whether they look like a
        // matrix or like whatever the title left behind.
        {
          auto cf = [&](uint32_t i) {
            float f;
            uint32_t v = drf[0x4000 + i];
            std::memcpy(&f, &v, 4);
            return f;
          };
          std::string c;
          for (uint32_t i = 0; i < 16; ++i) {
            c += fmt::format("{}{} ", (i % 4 == 0) ? "| " : "", cf(i));
          }
          XELOGI("GuideALUConst: c0..c3 = {}", c);
          // Phase 951: the shader computes oPos with dp3 against c4..c7 -
          // a projection matrix that has never been read. c0..c3 are only the
          // model transform feeding it.
          std::string pm;
          for (uint32_t i = 16; i < 32; ++i) {
            pm += fmt::format("{}{} ", (i % 4 == 0) ? "| " : "", cf(i));
          }
          XELOGI("GuideProjConst: c4..c7 = {}", pm);
          // Phase 970: every constant dump in this log has been base 0x4000,
          // which is the VERTEX half of the file. Xenos has 512 float4s and
          // the pixel shader reads the second 256 - XE_GPU_REG_SHADER_
          // CONSTANT_256_X at 0x4400 - so the two constants the Guide's whole
          // pixel shader consists of ("mul oC0, c0, c1") have never been read.
          // If either is zero the fragments are produced and blended away by
          // SrcAlpha/InvSrcAlpha, which every instrument here would record as
          // an unchanged frame.
          std::string ps;
          for (uint32_t i = 0; i < 16; ++i) {
            float f;
            uint32_t v = drf[0x4400 + i];
            std::memcpy(&f, &v, 4);
            ps += fmt::format("{}{} ", (i % 4 == 0) ? "| " : "", f);
          }
          XELOGI("GuidePSConst: pc0..pc3 = {}", ps);
          // Phase 924: a pixel-to-NDC conversion would carry 2/1280 =
          // 0.0015625 and -2/720 = -0.0027778. Scan the constant file for
          // anything of that magnitude rather than assuming where it sits.
          std::string hits;
          uint32_t nh = 0;
          // Phase 926: this scanned 256 floats, which is c0..c63. Xenos has
          // 256 float constants - 1024 floats - so three quarters of the file
          // were never looked at, including wherever a projection matrix would
          // sit.
          for (uint32_t i = 0; i < 1024; ++i) {
            float f = cf(i);
            float af = f < 0 ? -f : f;
            if (af > 0.0005f && af < 0.006f) {
              ++nh;
              if (nh <= 10) {
                hits += fmt::format("c{}.{}={} ", i / 4, i % 4, f);
              }
            }
          }
          XELOGI("GuideALUScan: {} constants in 0.0005..0.006 | {}", nh,
                 hits.empty() ? "NONE" : hits);
          // A shader could hold the screen size and divide instead of holding
          // its reciprocal, which the small-magnitude scan would miss.
          std::string big;
          uint32_t nb = 0;
          for (uint32_t i = 0; i < 1024; ++i) {
            float f = cf(i);
            float af = f < 0 ? -f : f;
            if ((af > 300.0f && af < 400.0f) || (af > 600.0f && af < 700.0f) ||
                (af > 700.0f && af < 740.0f) || (af > 1200.0f && af < 1300.0f)) {
              ++nb;
              if (nb <= 10) big += fmt::format("c{}.{}={} ", i / 4, i % 4, f);
            }
          }
          XELOGI("GuideALUDims: {} constants near 320/360/640/720/1280 | {}",
                 nb, big.empty() ? "NONE" : big);
        }
        XELOGI("GuideVSBindings: shader={} bindings={} | {}",
                 vs ? "present" : "NONE",
                 vs ? vs->vertex_bindings().size() : 0,
                 need.empty() ? "<none>" : need);
        }
        for (uint32_t slot = 0; slot < 6; ++slot) {
          uint32_t d0 = drf[0x4800 + slot * 2];
          uint32_t d1 = drf[0x4801 + slot * 2];
          if ((d0 & 0x3u) != uint32_t(xenos::FetchConstantType::kVertex)) {
            continue;
          }
          uint32_t addr = d0 & 0xFFFFFFFCu;
          uint32_t words = (d1 >> 2) & 0xFFFFFFu;
          std::string vals;
          const uint8_t* vp = memory_->TranslatePhysical(addr);
          if (vp && words) {
            for (uint32_t k = 0; k < 8 && k < words; ++k) {
              uint32_t raw = xe::load_and_swap<uint32_t>(vp + k * 4);
              float f;
              std::memcpy(&f, &raw, 4);
              vals += fmt::format("{} ", f);
            }
          }
          XELOGI("GuideVertexBuf: slot {} addr={:08X} words={} | {}", slot,
                 addr, words, vals.empty() ? "<unreadable>" : vals);
        }
      }
    }
  }
  if (draw_succeeded) {
    auto viz_query = register_file_->Get<reg::PA_SC_VIZ_QUERY>();
    if (guide_overlay_exec_ &&
        (viz_query.viz_query_ena && viz_query.kill_pix_post_hi_z)) {
      ++guide_ov_vizdrop_;
    }
    if (!(viz_query.viz_query_ena && viz_query.kill_pix_post_hi_z)) {
      // TODO(Triang3l): Don't drop the draw call completely if the vertex
      // shader has memexport.
      // TODO(Triang3l || JoelLinn): Handle this properly in the render
      // backends.
      draw_succeeded = COMMAND_PROCESSOR::IssueDraw(
          vgt_draw_initiator.prim_type, vgt_draw_initiator.num_indices,
          is_indexed ? &index_buffer_info : nullptr,
          xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode,
                                     vgt_draw_initiator.prim_type));
      if (guide_overlay_exec_) {
        if (draw_succeeded) ++guide_ov_issued_; else ++guide_ov_failed_;
      }
      if (!draw_succeeded) {
        XELOGE("{}({}, {}, {}): Failed in backend", opcode_name,
               vgt_draw_initiator.num_indices,
               uint32_t(vgt_draw_initiator.prim_type),
               uint32_t(vgt_draw_initiator.source_select));
      }
    }
  }

  // If read the packed correctly, but merely couldn't execute it (because of,
  // for instance, features not supported by the host), don't terminate command
  // buffer processing as that would leave rendering in a way more inconsistent
  // state than just a single dropped draw command.
  return true;
}

bool COMMAND_PROCESSOR::ExecutePacketType3_DRAW_INDX(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  // "initiate fetch of index buffer and draw"
  // Generally used by Xbox 360 Direct3D 9 for kDMA and kAutoIndex sources.
  // With a viz query token as the first one.
  uint32_t count_remaining = count;
  assert_not_zero(count_remaining);
  if (!count_remaining) {
    XELOGE("PM4_DRAW_INDX: Packet too small, can't read the viz query token");
    return false;
  }
  uint32_t viz_query_condition = reader_.ReadAndSwap<uint32_t>();
  --count_remaining;
  return COMMAND_PROCESSOR::ExecutePacketType3Draw(
      packet, "PM4_DRAW_INDX", viz_query_condition, count_remaining);
}

bool COMMAND_PROCESSOR::ExecutePacketType3_DRAW_INDX_2(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  // "draw using supplied indices in packet"
  // Generally used by Xbox 360 Direct3D 9 for kAutoIndex source.
  // No viz query token.
  return COMMAND_PROCESSOR::ExecutePacketType3Draw(packet, "PM4_DRAW_INDX_2", 0,
                                                   count);
}
XE_FORCEINLINE
bool COMMAND_PROCESSOR::ExecutePacketType3_SET_CONSTANT(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  // load constant into chip and to memory
  // PM4_REG(reg) ((0x4 << 16) | (GSL_HAL_SUBBLOCK_OFFSET(reg)))
  //                                     reg - 0x2000
  uint32_t offset_type = reader_.ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t countm1 = count - 1;
  // Phase 543: the Guide writes no FETCH constants at all, so phase 542's
  // "slot 2 is null" may be about a slot its shaders never read. Log every
  // constant type it does write - 0 ALU, 1 FETCH, 2 BOOL, 3 LOOP - before
  // concluding anything about where its geometry comes from.
  if (guide_in_draw_scope_ && !guide_replaying_) {
    static uint32_t sclog = 0;
    if (sclog++ < 16) {
      XELOGI("GuideSetConst: type={} index={} count={}", type, index,
             countm1 + 1);
    }
  }
  switch (type) {
    case 0:  // ALU
      // index += 0x4000;
      // COMMAND_PROCESSOR::WriteRegisterRangeFromRing( index, countm1);
      COMMAND_PROCESSOR::WriteALURangeFromRing(&reader_, index, countm1);
      break;
    case 1:  // FETCH

      COMMAND_PROCESSOR::WriteFetchRangeFromRing(&reader_, index, countm1);

      // Phase 543: the Guide's draws fetch vertices from address 0 (phase 542).
      // Either its stream never writes that slot, or it writes a null address.
      // Log what it actually writes - index is in fetch dwords, so slot N
      // occupies index 2N and 2N+1.
      if (guide_in_draw_scope_ && !guide_replaying_) {
        static uint32_t fclog = 0;
        if (fclog++ < 12) {
          RegisterFile& wrf = *register_file_;
          XELOGI("GuideSetFetch: index={} ({} dwords) -> slot {} now "
                 "{:08X}/{:08X}",
                 index, countm1 + 1, index / 2, wrf[0x4800 + (index & ~1u)],
                 wrf[0x4801 + (index & ~1u)]);
        }
      }
      break;
    case 2:  // BOOL
      COMMAND_PROCESSOR::WriteBoolRangeFromRing(&reader_, index, countm1);

      break;
    case 3:  // LOOP

      COMMAND_PROCESSOR::WriteLoopRangeFromRing(&reader_, index, countm1);

      break;
    case 4:  // REGISTERS

      COMMAND_PROCESSOR::WriteREGISTERSRangeFromRing(&reader_, index, countm1);

      break;
    default:
      assert_always();
      reader_.AdvanceRead((count - 1) * sizeof(uint32_t));
      return true;
  }

  return true;
}
XE_NOINLINE
bool COMMAND_PROCESSOR::ExecutePacketType3_SET_CONSTANT2(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  uint32_t offset_type = reader_.ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  uint32_t countm1 = count - 1;

  COMMAND_PROCESSOR::WriteRegisterRangeFromRing(&reader_, index, countm1);

  return true;
}
XE_FORCEINLINE
bool COMMAND_PROCESSOR::ExecutePacketType3_LOAD_ALU_CONSTANT(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  // load constants from memory
  uint32_t address = reader_.ReadAndSwap<uint32_t>();
  address &= 0x3FFFFFFF;
  uint32_t offset_type = reader_.ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t size_dwords = reader_.ReadAndSwap<uint32_t>();
  size_dwords &= 0xFFF;
  uint32_t type = (offset_type >> 16) & 0xFF;

  auto xlat_address = (uint32_t*)memory_->TranslatePhysical(address);

  switch (type) {
    case 0:  // ALU
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      COMMAND_PROCESSOR::WriteALURangeFromMem(index, xlat_address, size_dwords);

      break;
    case 1:  // FETCH
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);
      COMMAND_PROCESSOR::WriteFetchRangeFromMem(index, xlat_address,
                                                size_dwords);
      break;
    case 2:  // BOOL
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);

      COMMAND_PROCESSOR::WriteBoolRangeFromMem(index, xlat_address,
                                               size_dwords);
      break;
    case 3:  // LOOP
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);

      COMMAND_PROCESSOR::WriteLoopRangeFromMem(index, xlat_address,
                                               size_dwords);

      break;
    case 4:  // REGISTERS
      // chrispy: todo, REGISTERS cannot write any special regs, so optimize for
      // that
      trace_writer_.WriteMemoryRead(CpuToGpu(address), size_dwords * 4);

      COMMAND_PROCESSOR::WriteREGISTERSRangeFromMem(index, xlat_address,
                                                    size_dwords);
      break;
    default:
      assert_always();
      return true;
  }

  return true;
}

bool COMMAND_PROCESSOR::ExecutePacketType3_SET_SHADER_CONSTANTS(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  uint32_t offset_type = reader_.ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  uint32_t countm1 = count - 1;
  COMMAND_PROCESSOR::WriteRegisterRangeFromRing(&reader_, index, countm1);

  return true;
}

bool COMMAND_PROCESSOR::ExecutePacketType3_IM_LOAD(uint32_t packet,
                                                   uint32_t count) XE_RESTRICT {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (pointer-based)
  uint32_t addr_type = reader_.ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(addr_type & 0x3);
  uint32_t addr = addr_type & ~0x3;
  uint32_t start_size = reader_.ReadAndSwap<uint32_t>();
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);
  trace_writer_.WriteMemoryRead(CpuToGpu(addr), size_dwords * 4);
  auto shader = COMMAND_PROCESSOR::LoadShader(
      shader_type, addr, memory_->TranslatePhysical<uint32_t*>(addr),
      size_dwords);
  // Phase 739: the active shaders are set here, by IM_LOAD - not by
  // SQ_PROGRAM_CNTL. The Guide's stream carries 7 of these per buffer, so this
  // is where its shaders either become active or do not.
  {
    // Phase 739: log the title's loads too - if both reference the same
    // addresses, the Guide is not loading its own shaders at all.
    bool g = guide_in_draw_scope_ || guide_replaying_;
    // Phase 740: shared index, the check that resolved the identical ambiguity
    // in phase 734. Same indices means one set of events logged twice.
    static uint32_t im_index = 0;
    ++im_index;
    // Phase 742: every "title" sample so far came from indices 1-6, the first
    // events in the run - which may be xam's own initialisation, not the
    // game's. Sample the non-guide side LATE, during gameplay, so the label
    // means what it claims.
    // Phase 744: census every shader address loaded in the run. If xam's own
    // UI shaders are ever created, they appear here as addresses the title
    // does not use.
    {
      static std::map<uint32_t, std::pair<uint64_t, uint32_t>> addrs;
      auto& e = addrs[addr];
      e.first = shader ? shader->ucode_data_hash() : 0ull;
      ++e.second;
      if (im_index % 4000u == 0u) {
        std::string list;
        for (auto& kv : addrs) {
          list += fmt::format("{:08X}/{:04X}x{} ", kv.first,
                              uint32_t(kv.second.first >> 48), kv.second.second);
        }
        XELOGI("IMLoadCensus: {} distinct addresses | {}", addrs.size(), list);
      }
    }
    static uint32_t imlg = 0, imlt = 0;
    bool want = g ? (imlg < 6) : (im_index > 6000u && imlt < 6);
    if (want && (g ? ++imlg : ++imlt)) {
      XELOGI("IMLoad[{}] #{}: type={} addr={:08X} size_dw={} -> hash={:016X}",
             g ? "guide" : "title", im_index,
             shader_type == xenos::ShaderType::kVertex ? "VS" : "PS", addr,
             size_dwords, shader ? shader->ucode_data_hash() : 0ull);
    }
  }
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  return true;
}

bool COMMAND_PROCESSOR::ExecutePacketType3_IM_LOAD_IMMEDIATE(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (code embedded in packet)
  uint32_t dword0 = reader_.ReadAndSwap<uint32_t>();
  uint32_t dword1 = reader_.ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(dword0);
  uint32_t start_size = dword1;
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);
  assert_true(reader_.read_count() >= size_dwords * 4);
  assert_true(count - 2 >= size_dwords);
  auto shader = COMMAND_PROCESSOR::LoadShader(
      shader_type, uint32_t(reader_.read_ptr()),
      reinterpret_cast<uint32_t*>(reader_.read_ptr()), size_dwords);
  // Phase 745: the other half of the census. Phase 744 covered only the
  // pointer-based loads, which is why the hashes the draw tally shows being
  // bound were missing from it.
  {
    static std::map<uint64_t, std::pair<uint32_t, uint32_t>> imm;
    uint64_t h = shader ? shader->ucode_data_hash() : 0ull;
    auto& e = imm[h];
    e.first += (guide_in_draw_scope_ || guide_replaying_) ? 1u : 0u;
    ++e.second;
    // Phase 751: do the Guide's shaders come from the skin? huduiskin's 'skin'
    // section sits at 90F90000, 75851 bytes (phase 749). If the ucode being
    // loaded lives there, the skin is the source; if not, it is not.
    // Phase 756: log both scopes. If the title's inline ucode for a hash is
    // byte-identical to the Guide's, they are one shader from one source.
    if (shader) {
      bool gsc = guide_in_draw_scope_ || guide_replaying_;
      static uint32_t skg = 0, skt = 0;
      if (gsc ? (skg < 3) : (skt < 3)) {
        gsc ? ++skg : ++skt;
        uint32_t sk = gsc ? skg : skt;
        const uint32_t* uc =
            reinterpret_cast<const uint32_t*>(reader_.read_ptr());
        // Phase 754: phase 751 scanned only the 'skin' section. huduiskin also
        // exports a 'xam' section at 90FA2880, 152215 bytes, which was never
        // searched - so "not in the skin" was a statement about half the module.
        bool found = false;
        uint32_t at = 0;
        struct { uint32_t base, size; } secs[2] = {{0x90F90000u, 75851u},
                                                   {0x90FA2880u, 152215u}};
        for (auto& sec : secs) {
          // TranslateVirtual is base+offset arithmetic: it returns a non-null
          // pointer whether or not the range is mapped, so the old `!p2` guard
          // never fired and this swept up to 152KB of unmapped memory. When
          // huduiskin has not loaded yet that is a HOST FAULT at 90F90000,
          // which kills the run before the Guide composites (draws 7 -> 0) and
          // made every measurement taken after it a false negative.
          auto* hp = memory_->LookupHeap(sec.base);
          if (!hp || hp->QueryRangeAccess(sec.base, sec.base + sec.size) ==
                         xe::memory::PageAccess::kNoAccess) {
            continue;
          }
          const uint8_t* p2 = memory_->TranslateVirtual(sec.base);
          if (!p2 || size_dwords < 4) continue;
          for (uint32_t o = 0; o + 16 <= sec.size && !found; o += 4) {
            if (std::memcmp(p2 + o, uc, 16) == 0) {
              found = true;
              at = sec.base + o;
            }
          }
          if (found) break;
        }
        // Phase 755: print eight dwords so the ucode can be searched for in
        // xam's own image offline. One dword was not enough to look for.
        std::string head;
        for (uint32_t k = 0; k < 8u && k < size_dwords; ++k) {
          head += fmt::format("{:08X} ", uc[k]);
        }
        XELOGI("ShaderSrc[{}] #{}: hash={:016X} dw={} | in huduiskin: {} "
               "{:08X} | ucode: {}",
               gsc ? "guide" : "title", sk, shader->ucode_data_hash(),
               size_dwords, found ? "YES" : "no", at, head);
      }
    }
    // Phase 746: E915 never loads in guide scope but loads 325 times a run.
    // Name the stream it arrives in.
    if (uint32_t(h >> 48) == 0xE915u) {
      static uint32_t e915 = 0;
      if (++e915 <= 4) {
        XELOGI("E915Load #{}: ib={:08X} words={} | guide_scope={} replaying={}",
               e915, g_guide_current_ib, g_guide_current_ib_words,
               guide_in_draw_scope_ ? 1 : 0, guide_replaying_ ? 1 : 0);
      }
    }
    static uint32_t immn = 0;
    if (++immn % 4000u == 0u) {
      std::string list;
      for (auto& kv : imm) {
        list += fmt::format("{:04X}:{}g/{} ", uint32_t(kv.first >> 48),
                            kv.second.first, kv.second.second);
      }
      XELOGI("IMImmCensus: {} distinct shaders (guide/total) | {}", imm.size(),
             list);
    }
  }
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  reader_.AdvanceRead(size_dwords * sizeof(uint32_t));
  return true;
}

/*
        todo: shouldn't this do something?
*/

bool COMMAND_PROCESSOR::ExecutePacketType3_INVALIDATE_STATE(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  // selective invalidation of state pointers
  /*uint32_t mask =*/reader_.ReadAndSwap<uint32_t>();
  // driver_->InvalidateState(mask);
  return true;
}

bool COMMAND_PROCESSOR::ExecutePacketType3_VIZ_QUERY(
    uint32_t packet, uint32_t count) XE_RESTRICT {
  // begin/end initiator for viz query extent processing
  // https://www.google.com/patents/US20050195186
  assert_true(count == 1);

  uint32_t dword0 = reader_.ReadAndSwap<uint32_t>();

  uint32_t id = dword0 & 0x3F;
  uint32_t end = dword0 & 0x100;
  if (!end) {
    // begin a new viz query @ id
    // On hardware this clears the internal state of the scan converter (which
    // is different to the register)
    COMMAND_PROCESSOR::WriteEventInitiator(VIZQUERY_START);
    // XELOGGPU("Begin viz query ID {:02X}", id);
  } else {
    // end the viz query
    COMMAND_PROCESSOR::WriteEventInitiator(VIZQUERY_END);
    // XELOGGPU("End viz query ID {:02X}", id);
    // The scan converter writes the internal result back to the register here.
    // We just fake it and say it was visible in case it is read back.
    if (id < 32) {
      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0] |= uint32_t(1)
                                                                     << id;
    } else {
      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1] |=
          uint32_t(1) << (id - 32);
    }
  }

  return true;
}

uint32_t COMMAND_PROCESSOR::ExecutePrimaryBuffer(uint32_t read_index,
                                                 uint32_t write_index) {
  SCOPE_profile_cpu_f("gpu");
#if XE_ENABLE_TRACE_WRITER_INSTRUMENTATION == 1
  // If we have a pending trace stream open it now. That way we ensure we get
  // all commands.
  if (!trace_writer_.is_open() && trace_state_ == TraceState::kStreaming) {
    uint32_t title_id = kernel_state_->GetExecutableModule()
                            ? kernel_state_->GetExecutableModule()->title_id()
                            : 0;
    auto file_name = fmt::format("{:08X}_stream.xtr", title_id);
    auto path = trace_stream_path_ / file_name;
    trace_writer_.Open(path, title_id);
    InitializeTrace();
  }
#endif
  // Adjust pointer base.
  uint32_t start_ptr = primary_buffer_ptr_ + read_index * sizeof(uint32_t);
  start_ptr = (primary_buffer_ptr_ & ~0x1FFFFFFF) | (start_ptr & 0x1FFFFFFF);
  uint32_t end_ptr = primary_buffer_ptr_ + write_index * sizeof(uint32_t);
  end_ptr = (primary_buffer_ptr_ & ~0x1FFFFFFF) | (end_ptr & 0x1FFFFFFF);

  trace_writer_.WritePrimaryBufferStart(start_ptr, write_index - read_index);

  // Execute commands!

  RingBuffer old_reader = reader_;
  new (&reader_) RingBuffer(memory_->TranslatePhysical(primary_buffer_ptr_),
                            primary_buffer_size_);

  reader_.set_read_offset(read_index * sizeof(uint32_t));
  reader_.set_write_offset(write_index * sizeof(uint32_t));
  // prefetch the wraparound range
  // it likely is already in L3 cache, but in a zen system it may be another
  // chiplets l3
  reader_.BeginPrefetchedRead<swcache::PrefetchTag::Level2>(
      GetCurrentRingReadCount());
  // Phase 612: after the Guide's ring handover the worker never returns here.
  // A guard distinguishes a spin in this loop from a block inside a single
  // packet - the two have different causes and different fixes.
  uint32_t guide_spin = 0;
  // Phase 614: if the ring is replaced while we are executing out of it, the
  // reader and primary_buffer_ptr_ no longer describe the same buffer and
  // everything after that point is stale. Abandon the call so the worker
  // re-enters against the new ring.
  const uint32_t guide_entry_ring = primary_buffer_ptr_;
  bool guide_ring_changed = false;
  do {
    // Phase 621: name the packet the worker sticks on. Log the header and
    // its offset for the first packets executed out of a ring we have not
    // logged before, so the last line printed is the one that did not return.
    if (cvars::guide_cp_probe) {
      static uint32_t pk_ring = 0;
      static uint32_t pk_n = 0;
      if (pk_ring != primary_buffer_ptr_) {
        pk_ring = primary_buffer_ptr_;
        pk_n = 0;
      }
      if (pk_n < 48) {
        ++pk_n;
        uint32_t off = uint32_t(reader_.read_offset());
        uint32_t hdr = xe::load_and_swap<uint32_t>(
            memory_->TranslatePhysical(primary_buffer_ptr_ + off));
        XELOGI("CPPkt {}: off={:X} idx={} hdr={:08X} type={} op={:02X}", pk_n,
               off, off / 4, hdr, hdr >> 30,
               (hdr >> 30) == 3 ? ((hdr >> 8) & 0x7F) : 0);
      }
    }
    if (cvars::guide_cp_probe && ++guide_spin == 100000u) {
      XELOGW("CPExec: primary loop still running after 100000 packets "
             "ring={:08X} read_count={} rptr_off={}",
             primary_buffer_ptr_, reader_.read_count(), reader_.read_offset());
    }
    if (!COMMAND_PROCESSOR::ExecutePacket()) {
      // This probably should be fatal - but we're going to continue anyways.
      XELOGE("**** PRIMARY RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
    if (primary_buffer_ptr_ != guide_entry_ring) {
      XELOGW("CPExec: ring replaced mid-execution {:08X} -> {:08X}; "
             "abandoning this buffer",
             guide_entry_ring, primary_buffer_ptr_);
      guide_ring_changed = true;
      break;
    }
  } while (reader_.read_count());

  COMMAND_PROCESSOR::OnPrimaryBufferEnd();

  trace_writer_.WritePrimaryBufferEnd();

  reader_ = old_reader;
  // On a mid-flight ring change, write_index indexes the buffer that is gone.
  // read_ptr_index_ was reset to 0 by InitializeRingBuffer; hand that back so
  // the worker starts at the beginning of the new ring.
  return guide_ring_changed ? read_ptr_index_ : write_index;
}

void COMMAND_PROCESSOR::ExecutePacket(uint32_t ptr, uint32_t count) {
  // Execute commands!
  RingBuffer old_reader = reader_;

  new (&reader_)
      RingBuffer{memory_->TranslatePhysical(ptr), count * sizeof(uint32_t)};

  reader_.set_write_offset(count * sizeof(uint32_t));

  do {
    if (!COMMAND_PROCESSOR::ExecutePacket()) {
      XELOGE("**** ExecutePacket: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader_.read_count());
  reader_ = old_reader;
}
