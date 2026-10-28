#include "ncv_cpu.h"

namespace ncv {

const char* stopReasonName(StopReason r) {
  switch (r) {
    case StopReason::None: return "None";
    case StopReason::InstructionLimit: return "InstructionLimit";
    case StopReason::Halted: return "Halted";
    case StopReason::Breakpoint: return "Breakpoint";
    case StopReason::UnsupportedInstruction: return "UnsupportedInstruction";
    case StopReason::BusFault: return "BusFault";
    case StopReason::FetchFault: return "FetchFault";
    case StopReason::MisalignedPc: return "MisalignedPc";
    case StopReason::AlignmentFault: return "AlignmentFault";
    case StopReason::StoppedByHost: return "StoppedByHost";
    case StopReason::ExceptionTaken: return "ExceptionTaken";
  }
  return "Unknown";
}

namespace {

inline uint64_t signExtend(uint64_t v, unsigned bits) {
  const uint64_t m = 1ull << (bits - 1);
  return (v ^ m) - m;  // iki'nin tümleyeni, uint64 sarmalı iyi tanımlı
}
inline int64_t sext(uint64_t v, bool is64) {
  return is64 ? static_cast<int64_t>(v) : static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(v)));
}
inline uint64_t rorBits(uint64_t v, unsigned r, unsigned bits) {
  const uint64_t m = bits == 64 ? ~0ull : 0xFFFFFFFFull;
  v &= m;
  return r == 0 ? v : (((v >> r) | (v << (bits - r))) & m);
}

struct AddResult {
  uint64_t value;
  bool n, z, c, v;
};

// ARM ARM: AddWithCarry(x, y, carry_in); is64=false -> 32-bit işlem
AddResult addWithCarry(uint64_t x, uint64_t y, bool carryIn, bool is64) {
  AddResult res;
  if (is64) {
    const unsigned __int128 usum = static_cast<unsigned __int128>(x) + y + (carryIn ? 1 : 0);
    const uint64_t r = static_cast<uint64_t>(usum);
    res.value = r;
    res.n = (r >> 63) != 0;
    res.z = (r == 0);
    res.c = (usum >> 64) != 0;
    res.v = (((x ^ r) & (y ^ r)) >> 63) != 0;
  } else {
    const uint64_t xs = x & 0xFFFFFFFFull, ys = y & 0xFFFFFFFFull;
    const uint64_t sum = xs + ys + (carryIn ? 1 : 0);
    const uint64_t r = sum & 0xFFFFFFFFull;
    res.value = r;
    res.n = (r >> 31) != 0;
    res.z = (r == 0);
    res.c = (sum >> 32) != 0;
    res.v = ((((xs ^ r) & (ys ^ r)) >> 31) & 1) != 0;
  }
  return res;
}

// type: 0 LSL, 1 LSR, 2 ASR, 3 ROR. amt: 0..datasize-1
uint64_t applyShift(unsigned type, uint64_t v, unsigned amt, bool is64) {
  const unsigned bits = is64 ? 64 : 32;
  const uint64_t m = is64 ? ~0ull : 0xFFFFFFFFull;
  v &= m;
  if (amt == 0) return v;
  switch (type) {
    case 0: return (v << amt) & m;
    case 1: return v >> amt;
    case 2: return static_cast<uint64_t>(sext(v, is64) >> amt) & m;
    default: return rorBits(v, amt, bits);
  }
}

// ARM ARM: DecodeBitMasks (immN, imms, immr, immediate, M=datasize). false -> ayrılmış/UNDEFINED kodlama.
bool decodeBitMasks(unsigned immN, unsigned imms, unsigned immr, bool immediate, unsigned M,
                    uint64_t* wmask, uint64_t* tmask) {
  const unsigned x = (immN << 6) | ((~imms) & 0x3Fu);
  if (x == 0) return false;
  const unsigned len = 31u - static_cast<unsigned>(__builtin_clz(x));
  if (len < 1) return false;
  if (M < (1u << len)) return false;  // eleman, veri genişliğinden büyük olamaz
  const unsigned esize = 1u << len, levels = esize - 1;
  if (immediate && (imms & levels) == levels) return false;
  const unsigned S = imms & levels, R = immr & levels;
  const unsigned d = (S - R) & levels;
  const uint64_t emask = (esize == 64) ? ~0ull : ((1ull << esize) - 1);
  auto ones = [](unsigned n) -> uint64_t { return n >= 64 ? ~0ull : ((1ull << n) - 1); };
  const uint64_t welem = ones(S + 1) & emask;
  const uint64_t telem = ones(d + 1) & emask;
  const uint64_t w = R ? (((welem >> R) | (welem << (esize - R))) & emask) : welem;
  uint64_t wm = 0, tm = 0;
  for (unsigned i = 0; i < M; i += esize) {
    wm |= w << i;
    tm |= telem << i;
  }
  *wmask = wm;
  *tmask = tm;
  return true;
}

bool conditionHolds(unsigned cond, const CpuState& s) {
  bool r;
  switch (cond >> 1) {
    case 0: r = s.z; break;                         // EQ / NE
    case 1: r = s.c; break;                         // CS / CC
    case 2: r = s.n; break;                         // MI / PL
    case 3: r = s.v; break;                         // VS / VC
    case 4: r = s.c && !s.z; break;                 // HI / LS
    case 5: r = (s.n == s.v); break;                // GE / LT
    case 6: r = (s.n == s.v) && !s.z; break;        // GT / LE
    default: r = true; break;                       // AL / NV
  }
  if ((cond & 1) && cond != 0xF) r = !r;
  return r;
}

// ADD/SUB (extended register): option 0..7 = UXTB,UXTH,UXTW,UXTX,SXTB,SXTH,SXTW,SXTX
uint64_t extendReg(uint64_t v, unsigned option, unsigned shift, bool is64) {
  static const unsigned kBits[4] = {8, 16, 32, 64};
  const unsigned bits = kBits[option & 3];
  uint64_t x = bits == 64 ? v : (v & ((1ull << bits) - 1));
  if ((option & 4) && bits < 64) x = signExtend(x, bits);
  x <<= shift;
  return is64 ? x : (x & 0xFFFFFFFFull);
}

inline uint64_t bitReverse(uint64_t v, unsigned bits) {
  uint64_t r = 0;
  for (unsigned i = 0; i < bits; i++) r |= ((v >> i) & 1ull) << (bits - 1 - i);
  return r;
}

// Tek yazmaçlı yükleme/saklama için geçerli (size, opc) çifti mi? PRFM ve ayrılmış kodlamalar false.
inline bool validLoadStore(unsigned size, unsigned opc) {
  if (opc <= 1) return true;
  if (size == 3) return false;
  if (size == 2 && opc == 3) return false;
  return true;
}

}  // namespace

void Cpu::reset(uint64_t entryPc, uint64_t initialSp, uint8_t initialEL) {
  s_ = CpuState();
  if (initialEL > 1) initialEL = 0;  // NCV does not implement EL2/EL3
  s_.current_el = initialEL;
  s_.spsel = initialEL == 1;
  s_.pc = entryPc;
  s_.sp = initialSp;
  if (initialEL == 0) s_.sp_el0 = initialSp;
  else s_.sp_el1 = initialSp;
  stop_ = StopInfo();
  retired_ = 0;
  lastInstruction_ = 0;
  exclValid_ = false;
  zeroPaPteWrites_ = {};
  zeroPaPteWriteNext_ = 0;
  zeroPaPteWriteCount_ = 0;
  counterEpoch_ = std::chrono::steady_clock::now();
  flushTlb();
}

uint64_t Cpu::counterValue() const {
  if (s_.cntfrq_el0 == 0) return 0;
  const auto elapsed = std::chrono::steady_clock::now() - counterEpoch_;
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  if (nanos <= 0) return 0;
  const unsigned __int128 ticks = static_cast<unsigned __int128>(nanos) * s_.cntfrq_el0 / 1000000000ull;
  return static_cast<uint64_t>(ticks);
}

void Cpu::updateTimerLine() {
  if (gic_ == nullptr) return;
  const uint64_t now = counterValue();
  const bool phys = (s_.cntp_ctl_el0 & 1u) && !(s_.cntp_ctl_el0 & 2u) && static_cast<int64_t>(now - s_.cntp_cval_el0) >= 0;
  const bool virt = (s_.cntv_ctl_el0 & 1u) && !(s_.cntv_ctl_el0 & 2u) && static_cast<int64_t>(now - s_.cntv_cval_el0) >= 0;
  gic_->setLevel(GicV2::kPhysicalTimerPpi, phys);
  gic_->setLevel(27, virt);
}

uint64_t Cpu::activeSp() const {
  if (s_.current_el == 0) return s_.sp_el0;
  return s_.spsel ? s_.sp_el1 : s_.sp_el0;
}

void Cpu::setActiveSp(uint64_t value) {
  if (s_.current_el == 0) s_.sp_el0 = value;
  else if (s_.spsel) s_.sp_el1 = value;
  else s_.sp_el0 = value;
  s_.sp = value;  // compatibility mirror for existing tests/API
}

uint64_t Cpu::takeException(uint64_t returnPc, uint8_t ec, uint32_t iss, uint64_t far) {
  const uint8_t oldEl = s_.current_el;
  const bool oldSpSel = s_.spsel;
  const uint64_t mode = oldEl == 0 ? 0ull : (oldSpSel ? 5ull : 4ull);
  s_.spsr_el1 = (static_cast<uint64_t>(s_.n) << 31) |
                (static_cast<uint64_t>(s_.z) << 30) |
                (static_cast<uint64_t>(s_.c) << 29) |
                (static_cast<uint64_t>(s_.v) << 28) |
                (static_cast<uint64_t>(s_.daif & 0xF) << 6) | mode;
  s_.elr_el1 = returnPc;
  if (ec >= 0x20 && ec <= 0x25) s_.far_el1 = far;
  // IL=1 for a 32-bit A64 instruction. FAR_EL1 is not changed for these
  // instruction exceptions; the later data-abort implementation will set it.
  s_.esr_el1 = (static_cast<uint64_t>(ec & 0x3F) << 26) | (1ull << 25) |
               (static_cast<uint64_t>(iss) & 0x1FFFFFFull);

  // VBAR_EL1 is 2 KiB aligned. A synchronous exception from EL0 uses the
  // lower-EL-AArch64 slot; same-EL exceptions use SP0/SPx slots.
  uint64_t vectorOffset = 0;
  if (oldEl == 0) vectorOffset = 0x400;
  else vectorOffset = oldSpSel ? 0x200 : 0x000;

  s_.current_el = 1;
  s_.spsel = true;                 // exception entry selects SP_EL1 (EL1h)
  s_.daif = 0xF;                    // mask debug, SError, IRQ, FIQ on entry
  s_.sp = s_.sp_el1;
  return (s_.vbar_el1 & ~0x7FFull) + vectorOffset;
}

uint64_t Cpu::takeIrq(uint64_t returnPc) {
  const uint8_t oldEl = s_.current_el;
  const bool oldSpSel = s_.spsel;
  const uint64_t mode = oldEl == 0 ? 0ull : (oldSpSel ? 5ull : 4ull);
  s_.spsr_el1 = (static_cast<uint64_t>(s_.n) << 31) |
                (static_cast<uint64_t>(s_.z) << 30) |
                (static_cast<uint64_t>(s_.c) << 29) |
                (static_cast<uint64_t>(s_.v) << 28) |
                (static_cast<uint64_t>(s_.daif & 0xF) << 6) | mode;
  s_.elr_el1 = returnPc;
  s_.esr_el1 = 0;
  s_.current_el = 1;
  s_.spsel = true;
  s_.daif = static_cast<uint8_t>(s_.daif | 0x2u); // mask IRQ on entry
  s_.sp = s_.sp_el1;
  uint64_t vectorOffset = 0;
  if (oldEl == 0) vectorOffset = 0x480;
  else if (oldSpSel) vectorOffset = 0x280;
  else vectorOffset = 0x080;
  return (s_.vbar_el1 & ~0x7FFull) + vectorOffset;
}

void Cpu::flushTlb() {
  for (auto& entry : tlb_) entry.valid = false;
}

// Aşama 4B: AArch64 EL1 stage-1 translation, 4 KiB granule, 48-bit'e kadar VA.
// TLB bilerek yok: ilk doğruluk diliminde her erişim page-table walk yapar.
// Bu yavaş bir referans uygulamasıdır; performans için daha sonra TLB/TLBI eklenecek.
bool Cpu::translateAddress(uint64_t va, AccessKind kind, uint64_t* pa, MmuFault* fault) {
  if (pa == nullptr || fault == nullptr) return false;
  *fault = MmuFault();
  walkRoot_ = 0; walkVaPage_ = 0; walkTtbr1_ = false; walkCacheHit_ = false;
  for (uint64_t& descriptor : walkDescriptors_) descriptor = 0;
  if ((s_.sctlr_el1 & 1ull) == 0) { *pa = va; return true; }  // SCTLR_EL1.M=0: identity mapping

  const uint64_t tcr = s_.tcr_el1;
  const unsigned t0sz = static_cast<unsigned>(tcr & 0x3Fu);
  const unsigned t1sz = static_cast<unsigned>((tcr >> 16) & 0x3Fu);
  const unsigned tg0 = static_cast<unsigned>((tcr >> 14) & 3u);
  const unsigned tg1 = static_cast<unsigned>((tcr >> 30) & 3u);
  // Initial implementation deliberately supports only the common 4 KiB granule.
  if (tg0 != 0 || tg1 != 2 || t0sz < 16 || t0sz > 39 || t1sz < 16 || t1sz > 39) {
    fault->fsc = 0x00;  // address-size fault: unsupported/invalid translation regime
    return false;
  }
  const bool tbi0 = ((tcr >> 37) & 1u) != 0;
  const bool tbi1 = ((tcr >> 38) & 1u) != 0;
  const unsigned vaBits0 = 64 - t0sz;
  const unsigned vaBits1 = 64 - t1sz;
  const uint64_t topByteMask = 0x00FFFFFFFFFFFFFFull;
  bool useTtbr1 = false;
  uint64_t effectiveVa = va;
  bool regionMatched = false;

  // TBI ignores bits[63:56] separately for each translation region.
  uint64_t va0 = tbi0 ? (va & topByteMask) : va;
  if ((va0 >> vaBits0) == 0) {
    effectiveVa = va0;
    useTtbr1 = false;
    regionMatched = true;
  }
  if (!regionMatched) {
    uint64_t va1 = tbi1 ? (va & topByteMask) : va;
    const uint64_t upperMask = tbi1 ? ((1ull << (56 - vaBits1)) - 1ull) : (~0ull >> vaBits1);
    if ((va1 >> vaBits1) == upperMask) {
      effectiveVa = va1;
      useTtbr1 = true;
      regionMatched = true;
    }
  }
  if (!regionMatched) { fault->fsc = 0x00; return false; }  // address-size fault
  if ((useTtbr1 && ((tcr >> 23) & 1u)) || (!useTtbr1 && ((tcr >> 7) & 1u))) {
    const unsigned bits = useTtbr1 ? vaBits1 : vaBits0;
    const unsigned levels = (bits - 12 + 8) / 9;
    fault->level = static_cast<uint8_t>(4 - levels);
    fault->fsc = static_cast<uint8_t>(0x04 + fault->level);  // walk disabled
    return false;
  }

  const unsigned vaBits = useTtbr1 ? vaBits1 : vaBits0;
  const uint64_t vaPage = effectiveVa & ~0xFFFull;
  walkVaPage_ = vaPage;
  walkTtbr1_ = useTtbr1;
  const size_t tlbIndex = static_cast<size_t>((vaPage >> 12) & (kTlbEntries - 1));
  TlbEntry& cached = tlb_[tlbIndex];
  uint64_t leaf = 0;
  unsigned leafLevel = 3;
  bool tablePxn = false, tableUxn = false, tableNoEl0 = false, tableReadOnly = false;
  const bool cacheHit = cached.valid && cached.vaPage == vaPage && cached.ttbr1 == useTtbr1;
  walkCacheHit_ = cacheHit;
  walkRoot_ = (useTtbr1 ? s_.ttbr1_el1 : s_.ttbr0_el1) & 0x0000FFFFFFFFF000ull;
  if (cacheHit) {
    leaf = cached.leaf;
    for (unsigned level = 0; level < 4; ++level) walkDescriptors_[level] = cached.descriptors[level];
    leafLevel = cached.level;
    tablePxn = cached.tablePxn;
    tableUxn = cached.tableUxn;
    tableNoEl0 = cached.tableNoEl0;
    tableReadOnly = cached.tableReadOnly;
    *pa = cached.paPage | (effectiveVa & 0xFFFull);
  } else {
    const unsigned levels = (vaBits - 12 + 8) / 9;
    const unsigned startLevel = 4 - levels;
    uint64_t table = (useTtbr1 ? s_.ttbr1_el1 : s_.ttbr0_el1) & 0x0000FFFFFFFFF000ull;
    bool foundLeaf = false;

    for (unsigned level = startLevel; level <= 3; ++level) {
      const unsigned shift = 12 + 9 * (3 - level);
      const unsigned indexBits = (level == startLevel) ? (vaBits - shift) : 9;
      const uint64_t indexMask = (1ull << indexBits) - 1ull;
      const uint64_t index = (effectiveVa >> shift) & indexMask;
      uint64_t desc = 0;
      const Status bst = bus_->read(GuestAddr(table + index * 8), 8, &desc);
      if (bst != Status::Ok) {
        // DFSC 0x14..0x17 describes a synchronous external abort during a
        // stage-1 translation-table walk at levels 0..3. Preserve the exact
        // guest-physical descriptor address and host bus result for diagnosis.
        fault->fsc = static_cast<uint8_t>(0x14u + level);
        fault->level = static_cast<uint8_t>(level);
        fault->tableWalk = true;
        fault->faultPhysicalAddress = table + index * 8;
        fault->busStatus = bst;
        return false;
      }
      walkDescriptors_[level] = desc;
      const unsigned type = static_cast<unsigned>(desc & 3u);
      if ((type & 1u) == 0) {
        fault->fsc = static_cast<uint8_t>(0x04 + level);  // translation fault
        fault->level = static_cast<uint8_t>(level);
        return false;
      }
      if (level == 3) {
        if (type != 3) {
          fault->fsc = static_cast<uint8_t>(0x04 + level);
          fault->level = static_cast<uint8_t>(level);
          return false;
        }
        leaf = desc; leafLevel = level; foundLeaf = true;
        break;
      }
      if (type == 3) {
        // Table descriptor restrictions are inherited by every descendant.
        tablePxn = tablePxn || ((desc >> 59) & 1u) != 0;   // PXNTable
        tableUxn = tableUxn || ((desc >> 60) & 1u) != 0;   // UXNTable
        tableNoEl0 = tableNoEl0 || ((desc >> 62) & 1u) != 0; // APTable[1]
        tableReadOnly = tableReadOnly || ((desc >> 61) & 1u) != 0; // APTable[0]
        table = desc & 0x0000FFFFFFFFF000ull;
        continue;
      }
      if (type == 1 && (level == 1 || level == 2)) {
        leaf = desc; leafLevel = level; foundLeaf = true; break;  // block descriptor
      }
      fault->fsc = static_cast<uint8_t>(0x04 + level);
      fault->level = static_cast<uint8_t>(level);
      return false;
    }
    if (!foundLeaf) { fault->fsc = static_cast<uint8_t>(0x04 + leafLevel); fault->level = static_cast<uint8_t>(leafLevel); return false; }

    const unsigned offsetBits = 12 + 9 * (3 - leafLevel);
    const uint64_t offsetMask = (1ull << offsetBits) - 1ull;
    const uint64_t outputBase = (leaf & 0x0000FFFFFFFFF000ull) & ~offsetMask;
    *pa = outputBase | (effectiveVa & offsetMask);
    // Cache the translation without permissions being pre-authorized. EL0/EL1,
    // read/write/execute permission checks are re-evaluated on every access.
    cached.valid = true;
    cached.ttbr1 = useTtbr1;
    cached.vaPage = vaPage;
    cached.paPage = *pa & ~0xFFFull;
    cached.leaf = leaf;
    for (unsigned level = 0; level < 4; ++level) cached.descriptors[level] = walkDescriptors_[level];
    cached.level = static_cast<uint8_t>(leafLevel);
    cached.tablePxn = tablePxn;
    cached.tableUxn = tableUxn;
    cached.tableNoEl0 = tableNoEl0;
    cached.tableReadOnly = tableReadOnly;
  }

  if (((leaf >> 10) & 1u) == 0) {  // AF: hardware AF update is not yet implemented
    fault->fsc = static_cast<uint8_t>(0x08 + leafLevel);
    fault->level = static_cast<uint8_t>(leafLevel);
    return false;
  }
  const unsigned ap = static_cast<unsigned>((leaf >> 6) & 3u);
  const bool atEl0 = s_.current_el == 0;
  const bool write = kind == AccessKind::Write;
  const bool execute = kind == AccessKind::Execute;
  bool permissionDenied = false;
  if (atEl0 && (((ap & 1u) == 0) || tableNoEl0)) permissionDenied = true;
  if (write && (((ap & 2u) != 0) || tableReadOnly)) permissionDenied = true;
  if (execute) {
    if (atEl0) permissionDenied = permissionDenied || ((leaf >> 54) & 1u) != 0 || tableUxn;
    else permissionDenied = permissionDenied || ((leaf >> 53) & 1u) != 0 || tablePxn;
    // SCTLR_EL1.WXN makes writable privileged mappings execute-never.
    if ((s_.sctlr_el1 & (1ull << 19)) && ((ap & 2u) == 0) && !atEl0) permissionDenied = true;
  }
  if (permissionDenied) {
    fault->fsc = static_cast<uint8_t>(0x0C + leafLevel);
    fault->level = static_cast<uint8_t>(leafLevel);
    return false;
  }

  const unsigned offsetBits = 12 + 9 * (3 - leafLevel);
  const uint64_t offsetMask = (1ull << offsetBits) - 1ull;
  const uint64_t outputBase = (leaf & 0x0000FFFFFFFFF000ull) & ~offsetMask;
  *pa = outputBase | (effectiveVa & offsetMask);
  return true;
}

void Cpu::recordZeroPaPteWrite(uint64_t pc, uint32_t insn, uint64_t va, uint64_t pa,
                               unsigned size, uint64_t value) {
  // A valid 4 KiB page descriptor with output address bits [47:12] all zero.
  // High bits 54/53/51 are attributes (UXN/PXN/DBM in common Linux layouts),
  // not part of the 48-bit output address.
  constexpr uint64_t kOutputAddressMask = 0x0000FFFFFFFFF000ull;
  if (size != 8 || (value & 3ull) != 3ull || (value & (1ull << 10)) == 0 ||
      (value & kOutputAddressMask) != 0)
    return;
  ZeroPaPteWriteTrace& event = zeroPaPteWrites_[zeroPaPteWriteNext_];
  event.valid = true;
  event.pc = pc;
  event.insn = insn;
  event.va = va;
  event.pa = pa;
  event.value = value;
  event.size = static_cast<uint8_t>(size);
  zeroPaPteWriteNext_ = (zeroPaPteWriteNext_ + 1) % kZeroPaPteWriteTraceEntries;
  if (zeroPaPteWriteCount_ < kZeroPaPteWriteTraceEntries) ++zeroPaPteWriteCount_;
}

StopReason Cpu::raiseAbort(uint64_t pc, uint32_t insn, uint64_t va, AccessKind kind,
                           uint8_t fsc, bool tableWalk, uint64_t faultPhysicalAddress,
                           Status busStatus, unsigned accessSize) {
  const bool instruction = kind == AccessKind::Execute;
  const bool write = kind == AccessKind::Write;
  const uint8_t ec = instruction ? (s_.current_el == 0 ? 0x20 : 0x21)
                                 : (s_.current_el == 0 ? 0x24 : 0x25);
  uint32_t iss = static_cast<uint32_t>(fsc & 0x3Fu);
  if (write && !instruction) iss |= 1u << 6;  // WnR
  // Capture the original access before exception entry. The guest's abort
  // handler may park forever, so ELR/ESR/FAR alone are not enough to identify
  // which emulator bus transaction failed.
  // If the failing translation came through the software TLB, perform one
  // diagnostic-only page-table walk with that cache entry temporarily bypassed.
  // The original TLB entry and walk diagnostics are restored immediately; this
  // probe never changes the translation used by the guest instruction.
  bool freshWalkAttempted = false;
  bool freshWalkOk = false;
  uint64_t freshPa = 0, freshRoot = 0, freshDescriptors[4] = {};
  uint8_t freshFsc = 0;
  bool freshTableWalk = false;
  uint64_t freshFaultPa = 0;
  Status freshBusStatus = Status::Ok;
  const uint64_t savedWalkRoot = walkRoot_;
  const uint64_t savedWalkVaPage = walkVaPage_;
  uint64_t savedWalkDescriptors[4] = {};
  for (unsigned level = 0; level < 4; ++level) savedWalkDescriptors[level] = walkDescriptors_[level];
  const bool savedWalkTtbr1 = walkTtbr1_;
  const bool savedWalkCacheHit = walkCacheHit_;
  if (walkCacheHit_ && (s_.sctlr_el1 & 1ull) != 0) {
    const size_t idx = static_cast<size_t>((walkVaPage_ >> 12) & (kTlbEntries - 1));
    const TlbEntry savedEntry = tlb_[idx];
    tlb_[idx].valid = false;
    MmuFault freshFault;
    freshWalkAttempted = true;
    freshWalkOk = translateAddress(va, kind, &freshPa, &freshFault);
    freshFsc = freshFault.fsc;
    freshTableWalk = freshFault.tableWalk;
    freshFaultPa = freshFault.faultPhysicalAddress;
    freshBusStatus = freshFault.busStatus;
    freshRoot = walkRoot_;
    for (unsigned level = 0; level < 4; ++level) freshDescriptors[level] = walkDescriptors_[level];
    tlb_[idx] = savedEntry;
    walkRoot_ = savedWalkRoot;
    walkVaPage_ = savedWalkVaPage;
    for (unsigned level = 0; level < 4; ++level) walkDescriptors_[level] = savedWalkDescriptors[level];
    walkTtbr1_ = savedWalkTtbr1;
    walkCacheHit_ = savedWalkCacheHit;
  }
  s_.last_abort_fresh_walk_attempted = freshWalkAttempted;
  s_.last_abort_fresh_walk_ok = freshWalkOk;
  s_.last_abort_fresh_pa = freshPa;
  s_.last_abort_fresh_root = freshRoot;
  s_.last_abort_fresh_l1_desc = freshDescriptors[1];
  s_.last_abort_fresh_l2_desc = freshDescriptors[2];
  s_.last_abort_fresh_l3_desc = freshDescriptors[3];
  s_.last_abort_fresh_fsc = freshFsc;
  s_.last_abort_fresh_table_walk = freshTableWalk;
  s_.last_abort_fresh_fault_pa = freshFaultPa;
  s_.last_abort_fresh_bus_status = static_cast<int32_t>(freshBusStatus);
  s_.last_abort_pc = pc;
  s_.last_abort_insn = insn;
  s_.last_abort_va = va;
  s_.last_abort_pa = faultPhysicalAddress;
  s_.last_abort_bus_status = static_cast<int32_t>(busStatus);
  s_.last_abort_size = static_cast<uint8_t>(accessSize > 255 ? 255 : accessSize);
  s_.last_abort_access = instruction ? 2u : (write ? 1u : 0u);
  s_.last_abort_table_walk = tableWalk;
  s_.last_abort_tcr_el1 = s_.tcr_el1;
  s_.last_abort_ttbr0_el1 = s_.ttbr0_el1;
  s_.last_abort_ttbr1_el1 = s_.ttbr1_el1;
  s_.last_abort_x8 = s_.x[8];
  s_.last_abort_walk_root = walkRoot_;
  s_.last_abort_walk_va_page = walkVaPage_;
  s_.last_abort_l1_desc = walkDescriptors_[1];
  s_.last_abort_l2_desc = walkDescriptors_[2];
  s_.last_abort_l3_desc = walkDescriptors_[3];
  s_.last_abort_walk_ttbr1 = walkTtbr1_;
  s_.last_abort_translation_cache_hit = walkCacheHit_;
  s_.last_abort_l3_entry_pa = 0;
  s_.last_abort_pte_writer_found = false;
  s_.last_abort_pte_writer_pc = 0;
  s_.last_abort_pte_writer_insn = 0;
  s_.last_abort_pte_writer_va = 0;
  s_.last_abort_pte_writer_pa = 0;
  s_.last_abort_pte_writer_value = 0;
  s_.last_abort_pte_writer_size = 0;
  // Level-2 table descriptor points to the level-3 table; TCR.T1SZ=25 in
  // the reported case means 39-bit VA and a 9-bit L3 index.
  if ((walkDescriptors_[2] & 3ull) == 3ull) {
    constexpr uint64_t kOutputAddressMask = 0x0000FFFFFFFFF000ull;
    const uint64_t l3Base = walkDescriptors_[2] & kOutputAddressMask;
    const uint64_t l3Index = (walkVaPage_ >> 12) & 0x1FFull;
    if (l3Base <= UINT64_MAX - l3Index * 8ull) {
      const uint64_t entryPa = l3Base + l3Index * 8ull;
      s_.last_abort_l3_entry_pa = entryPa;
      // Search newest-first for a guest 64-bit store that installed the same
      // zero-PA descriptor at precisely this table entry.
      for (size_t n = 0; n < zeroPaPteWriteCount_; ++n) {
        const size_t idx = (zeroPaPteWriteNext_ + kZeroPaPteWriteTraceEntries - 1 - n) %
                           kZeroPaPteWriteTraceEntries;
        const ZeroPaPteWriteTrace& event = zeroPaPteWrites_[idx];
        if (event.valid && event.pa == entryPa && event.value == walkDescriptors_[3]) {
          s_.last_abort_pte_writer_found = true;
          s_.last_abort_pte_writer_pc = event.pc;
          s_.last_abort_pte_writer_insn = event.insn;
          s_.last_abort_pte_writer_va = event.va;
          s_.last_abort_pte_writer_pa = event.pa;
          s_.last_abort_pte_writer_value = event.value;
          s_.last_abort_pte_writer_size = event.size;
          break;
        }
      }
    }
  }
  // NCV does not implement stage-2 translation, so S1PTW is not asserted.
  s_.far_el1 = va;
  s_.pc = takeException(pc, ec, iss, va);
  return StopReason::ExceptionTaken;
}

StopReason Cpu::loadMemory(uint64_t va, unsigned size, uint64_t* out, uint64_t pc,
                           uint32_t insn, AccessKind kind) {
  if (out == nullptr || !(size == 1 || size == 2 || size == 4 || size == 8))
    return stopWith(kind == AccessKind::Execute ? StopReason::FetchFault : StopReason::BusFault,
                    pc, insn, Status::InvalidArgument, va);
  if (size > 1 && (va & (size - 1)) != 0 && (s_.sctlr_el1 & (1ull << 1)))
    return raiseAbort(pc, insn, va, kind, 0x21);  // alignment fault, when SCTLR_EL1.A is set
  const bool mmu = (s_.sctlr_el1 & 1ull) != 0;
  if (!mmu) {
    uint64_t value = 0;
    const Status st = bus_->read(GuestAddr(va), size, &value);
    if (st != Status::Ok)
      return stopWith(kind == AccessKind::Execute ? StopReason::FetchFault : StopReason::BusFault,
                      pc, insn, st, va);
    *out = value;
    return StopReason::None;
  }
  if (va > UINT64_MAX - (size - 1)) return raiseAbort(pc, insn, va, kind, 0x00);
  uint64_t physical[8] = {};
  for (unsigned i = 0; i < size; ++i) {
    MmuFault fault;
    if (!translateAddress(va + i, kind, &physical[i], &fault))
      return raiseAbort(pc, insn, va + i, kind, fault.fsc, fault.tableWalk,
                        fault.faultPhysicalAddress, fault.busStatus, size);
  }
  uint64_t value = 0;
  bool contiguous = true;
  for (unsigned i = 1; i < size; ++i) if (physical[i] != physical[0] + i) contiguous = false;
  if (contiguous) {
    const Status st = bus_->read(GuestAddr(physical[0]), size, &value);
    if (st != Status::Ok) return raiseAbort(pc, insn, va, kind, 0x10, false, physical[0], st, size);
  } else {
    for (unsigned i = 0; i < size; ++i) {
      uint64_t byte = 0;
      const Status st = bus_->read(GuestAddr(physical[i]), 1, &byte);
      if (st != Status::Ok) return raiseAbort(pc, insn, va + i, kind, 0x10, false, physical[i], st, 1);
      value |= (byte & 0xFFu) << (8 * i);
    }
  }
  *out = value;
  return StopReason::None;
}

StopReason Cpu::storeMemory(uint64_t va, unsigned size, uint64_t value, uint64_t pc,
                            uint32_t insn) {
  if (!(size == 1 || size == 2 || size == 4 || size == 8))
    return stopWith(StopReason::BusFault, pc, insn, Status::InvalidArgument, va);
  if (size > 1 && (va & (size - 1)) != 0 && (s_.sctlr_el1 & (1ull << 1)))
    return raiseAbort(pc, insn, va, AccessKind::Write, 0x21);  // alignment fault
  const bool mmu = (s_.sctlr_el1 & 1ull) != 0;
  if (!mmu) {
    const Status st = bus_->write(GuestAddr(va), size, value);
    return st == Status::Ok ? StopReason::None : stopWith(StopReason::BusFault, pc, insn, st, va);
  }
  if (va > UINT64_MAX - (size - 1)) return raiseAbort(pc, insn, va, AccessKind::Write, 0x00);
  uint64_t physical[8] = {};
  // Translate the entire access before modifying guest memory.
  for (unsigned i = 0; i < size; ++i) {
    MmuFault fault;
    if (!translateAddress(va + i, AccessKind::Write, &physical[i], &fault))
      return raiseAbort(pc, insn, va + i, AccessKind::Write, fault.fsc, fault.tableWalk,
                        fault.faultPhysicalAddress, fault.busStatus, size);
  }
  bool contiguous = true;
  for (unsigned i = 1; i < size; ++i) if (physical[i] != physical[0] + i) contiguous = false;
  if (contiguous) {
    const Status st = bus_->write(GuestAddr(physical[0]), size, value);
    if (st != Status::Ok) return raiseAbort(pc, insn, va, AccessKind::Write, 0x10, false, physical[0], st, size);
    recordZeroPaPteWrite(pc, insn, va, physical[0], size, value);
  } else {
    for (unsigned i = 0; i < size; ++i) {
      const Status st = bus_->write(GuestAddr(physical[i]), 1, (value >> (8 * i)) & 0xFFu);
      if (st != Status::Ok) return raiseAbort(pc, insn, va + i, AccessKind::Write, 0x10, false, physical[i], st, 1);
    }
  }
  return StopReason::None;
}

StopReason Cpu::stopWith(StopReason r, uint64_t pc, uint32_t insn, Status bs, uint64_t fa,
                         uint16_t imm) {
  stop_.reason = r;
  stop_.pc = pc;
  stop_.insn = insn;
  stop_.busStatus = bs;
  stop_.faultAddr = fa;
  stop_.imm = imm;
  return r;
}

StopReason Cpu::run(uint64_t maxInstructions) {
  for (uint64_t i = 0; i < maxInstructions; i++) {
    const StopReason r = step();
    if (r != StopReason::None) return r;
  }
  return stopWith(StopReason::InstructionLimit, s_.pc, 0);
}

StopReason Cpu::step() {
  updateTimerLine();
  if (gic_ != nullptr && (s_.daif & 0x2u) == 0 && gic_->hasDeliverableInterrupt()) {
    s_.pc = takeIrq(s_.pc);
    return StopReason::None;
  }
  const uint64_t pc = s_.pc;
  if ((pc & 3) != 0) {
    if (s_.sctlr_el1 & 1ull) {
      const StopReason abort = raiseAbort(pc, 0, pc, AccessKind::Execute, 0x21);
      return abort == StopReason::ExceptionTaken ? StopReason::None : abort;
    }
    return stopWith(StopReason::MisalignedPc, pc, 0, Status::Ok, pc);
  }
  uint64_t raw = 0;
  const StopReason fetch = loadMemory(pc, 4, &raw, pc, 0, AccessKind::Execute);
  if (fetch == StopReason::ExceptionTaken) return StopReason::None;
  if (fetch != StopReason::None) return fetch;
  lastInstruction_ = static_cast<uint32_t>(raw);
  const StopReason result = execute(lastInstruction_, pc);
  // Memory helpers report guest aborts as ExceptionTaken so the current
  // instruction unwinds without committing its remaining side effects.
  if (result == StopReason::ExceptionTaken) return StopReason::None;
  return result;
}

StopReason Cpu::memAccess(uint32_t insn, uint64_t pc, unsigned size, unsigned opc, unsigned rt,
                          uint64_t addr) {
  const unsigned bytes = 1u << size;
  if (opc == 0) return storeMemory(addr, bytes, getX(rt), pc, insn);
  uint64_t v = 0;
  const StopReason access = loadMemory(addr, bytes, &v, pc, insn);
  if (access != StopReason::None) return access;
  if (opc >= 2) {
    v = signExtend(v, bytes * 8);
    if (opc == 3) v &= 0xFFFFFFFFull;
  }
  setX(rt, v);  // başarısız yükleme hedef register'ı bozmaz
  return StopReason::None;
}

namespace {
constexpr uint32_t sysKey(unsigned o0, unsigned op1, unsigned crn, unsigned crm, unsigned op2) {
  return (o0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2;
}
constexpr uint32_t kNZCV = sysKey(1, 3, 4, 2, 0);
constexpr uint32_t kFPCR = sysKey(1, 3, 4, 4, 0);
constexpr uint32_t kFPSR = sysKey(1, 3, 4, 4, 1);
constexpr uint32_t kTPIDR_EL0 = sysKey(1, 3, 13, 0, 2);
constexpr uint32_t kTPIDRRO_EL0 = sysKey(1, 3, 13, 0, 3);
constexpr uint32_t kCurrentEL = sysKey(1, 0, 4, 2, 2);
constexpr uint32_t kDAIF = sysKey(1, 3, 4, 2, 1);
constexpr uint32_t kSCTLR_EL1 = sysKey(1, 0, 1, 0, 0);
constexpr uint32_t kACTLR_EL1 = sysKey(1, 0, 1, 0, 1);
constexpr uint32_t kCPACR_EL1 = sysKey(1, 0, 1, 0, 2);
constexpr uint32_t kTTBR0_EL1 = sysKey(1, 0, 2, 0, 0);
constexpr uint32_t kTTBR1_EL1 = sysKey(1, 0, 2, 0, 1);
constexpr uint32_t kTCR_EL1 = sysKey(1, 0, 2, 0, 2);
constexpr uint32_t kESR_EL1 = sysKey(1, 0, 5, 2, 0);
constexpr uint32_t kFAR_EL1 = sysKey(1, 0, 6, 0, 0);
constexpr uint32_t kMAIR_EL1 = sysKey(1, 0, 10, 2, 0);
constexpr uint32_t kVBAR_EL1 = sysKey(1, 0, 12, 0, 0);
constexpr uint32_t kSPSR_EL1 = sysKey(1, 0, 4, 0, 0);
constexpr uint32_t kELR_EL1 = sysKey(1, 0, 4, 0, 1);
constexpr uint32_t kSP_EL0 = sysKey(1, 0, 4, 1, 0);
constexpr uint32_t kTPIDR_EL1 = sysKey(1, 0, 13, 0, 4);
constexpr uint32_t kCONTEXTIDR_EL1 = sysKey(1, 0, 13, 0, 1);
constexpr uint32_t kCNTKCTL_EL1 = sysKey(1, 0, 14, 1, 0);
constexpr uint32_t kAFSR0_EL1 = sysKey(1, 0, 5, 1, 0);
constexpr uint32_t kAFSR1_EL1 = sysKey(1, 0, 5, 1, 1);
constexpr uint32_t kAMAIR_EL1 = sysKey(1, 0, 10, 3, 0);
constexpr uint32_t kPAR_EL1 = sysKey(1, 0, 7, 4, 0);
constexpr uint32_t kMIDR_EL1 = sysKey(1, 0, 0, 0, 0);
// REVIDR_EL1 is S3_0_C0_C0_6 (MRS X8, REVIDR_EL1 = 0xD53800C8).
// It is implementation-defined revision data. The generic NCV CPU has no
// silicon-specific errata identity, so report zero consistently.
constexpr uint32_t kREVIDR_EL1 = sysKey(1, 0, 0, 0, 6);
constexpr uint32_t kMPIDR_EL1 = sysKey(1, 0, 0, 0, 5);
constexpr uint32_t kID_AA64PFR0_EL1 = sysKey(1, 0, 0, 4, 0);
constexpr uint32_t kID_AA64PFR1_EL1 = sysKey(1, 0, 0, 4, 1);
constexpr uint32_t kID_AA64DFR0_EL1 = sysKey(1, 0, 0, 5, 0);
constexpr uint32_t kID_AA64ISAR0_EL1 = sysKey(1, 0, 0, 6, 0);
constexpr uint32_t kID_AA64MMFR0_EL1 = sysKey(1, 0, 0, 7, 0);
constexpr uint32_t kID_AA64MMFR4_EL1 = sysKey(1, 0, 0, 7, 4);
constexpr uint32_t kID_AA64PFR2_EL1 = sysKey(1, 0, 0, 4, 2);
constexpr uint32_t kID_AA64ZFR0_EL1 = sysKey(1, 0, 0, 4, 4);
constexpr uint32_t kID_AA64DFR1_EL1 = sysKey(1, 0, 0, 5, 1);
constexpr uint32_t kID_AA64AFR0_EL1 = sysKey(1, 0, 0, 5, 4);
constexpr uint32_t kID_AA64AFR1_EL1 = sysKey(1, 0, 0, 5, 5);
constexpr uint32_t kID_AA64ISAR1_EL1 = sysKey(1, 0, 0, 6, 1);
constexpr uint32_t kID_AA64ISAR2_EL1 = sysKey(1, 0, 0, 6, 2);
// ID_AA64ISAR3_EL1 (S3_0_C0_C6_3), introduced in newer Arm architecture revisions.
// NCV advertises no optional features from this register, so MRS returns zero.
constexpr uint32_t kID_AA64ISAR3_EL1 = sysKey(1, 0, 0, 6, 3);
constexpr uint32_t kID_AA64MMFR1_EL1 = sysKey(1, 0, 0, 7, 1);
constexpr uint32_t kID_AA64MMFR2_EL1 = sysKey(1, 0, 0, 7, 2);
constexpr uint32_t kID_AA64MMFR3_EL1 = sysKey(1, 0, 0, 7, 3);
constexpr uint32_t kID_AA64SMFR0_EL1 = sysKey(1, 0, 0, 4, 5);
constexpr uint32_t kCLIDR_EL1 = sysKey(1, 1, 0, 0, 1);
constexpr uint32_t kAIDR_EL1 = sysKey(1, 1, 0, 0, 7);
constexpr uint32_t kCSSELR_EL1 = sysKey(1, 2, 0, 0, 0);
constexpr uint32_t kCCSIDR_EL1 = sysKey(1, 1, 0, 0, 0);
constexpr uint32_t kCTR_EL0 = sysKey(1, 3, 0, 0, 1);
constexpr uint32_t kDCZID_EL0 = sysKey(1, 3, 0, 0, 7);
constexpr uint32_t kCNTFRQ_EL0 = sysKey(1, 3, 14, 0, 0);
constexpr uint32_t kCNTPCT_EL0 = sysKey(1, 3, 14, 0, 1);
constexpr uint32_t kCNTVCT_EL0 = sysKey(1, 3, 14, 0, 2);
constexpr uint32_t kCNTP_TVAL_EL0 = sysKey(1, 3, 14, 2, 0);
constexpr uint32_t kCNTP_CTL_EL0 = sysKey(1, 3, 14, 2, 1);
constexpr uint32_t kCNTP_CVAL_EL0 = sysKey(1, 3, 14, 2, 2);
constexpr uint32_t kCNTV_TVAL_EL0 = sysKey(1, 3, 14, 3, 0);
constexpr uint32_t kCNTV_CTL_EL0 = sysKey(1, 3, 14, 3, 1);
constexpr uint32_t kCNTV_CVAL_EL0 = sysKey(1, 3, 14, 3, 2);
// Optional architectural debug registers. The virtual CPU advertises no debug
// feature (ID_AA64DFR0_EL1.DebugVer == 0), so these are modeled as RAZ/WI: reads
// return zero and writes have no effect. This lets generic kernels clear MDSCR
// during early boot without pretending to provide breakpoints or single-step.
constexpr uint32_t kOSDTRRX_EL1 = sysKey(0, 0, 0, 0, 2);
constexpr uint32_t kMDCCINT_EL1 = sysKey(0, 0, 0, 2, 0);
constexpr uint32_t kMDSCR_EL1 = sysKey(0, 0, 0, 2, 2);
constexpr uint32_t kOSDTRTX_EL1 = sysKey(0, 0, 0, 3, 2);
constexpr uint32_t kMDSELR_EL1 = sysKey(0, 0, 0, 4, 2);
constexpr uint32_t kMDSTEPOP_EL1 = sysKey(0, 0, 0, 5, 2);
constexpr uint32_t kOSECCR_EL1 = sysKey(0, 0, 0, 6, 2);
constexpr uint32_t kMDRAR_EL1 = sysKey(0, 0, 1, 0, 0);
constexpr uint32_t kOSLAR_EL1 = sysKey(0, 0, 1, 0, 4);
constexpr uint32_t kOSLSR_EL1 = sysKey(0, 0, 1, 1, 4);
constexpr uint32_t kOSDLR_EL1 = sysKey(0, 0, 1, 3, 4);
constexpr uint32_t kFpcrMask = 0x03C00000u;  // RMode, FZ, DN (AHP and traps are not implemented)
constexpr uint32_t kFpsrMask = 0x0800009Fu;  // IOC DZC OFC UFC IXC IDC QC

bool isDebugSysReg(uint32_t key) {
  const unsigned op0 = 2u + ((key >> 14) & 1u);
  const unsigned op1 = (key >> 11) & 7u;
  const unsigned crn = (key >> 7) & 15u;
  const unsigned crm = (key >> 3) & 15u;
  const unsigned op2 = key & 7u;
  if (op0 != 2u || op1 != 0u) return false;
  // Breakpoint/watchpoint value and control registers: DBGBVR/BCR, DBGWVR/WCR.
  if (crn == 0u && op2 >= 4u) return true;
  // Named debug registers used by common AArch64 kernel debug setup.
  if (key == kOSDTRRX_EL1 || key == kMDCCINT_EL1 || key == kMDSCR_EL1 ||
      key == kOSDTRTX_EL1 || key == kMDSELR_EL1 || key == kMDSTEPOP_EL1 ||
      key == kOSECCR_EL1 || key == kMDRAR_EL1 || key == kOSLAR_EL1 ||
      key == kOSLSR_EL1 || key == kOSDLR_EL1) return true;
  (void)crm;
  return false;
}

bool isIdSysReg(uint32_t key) {
  const unsigned op0 = 2u + ((key >> 14) & 1u);
  const unsigned op1 = (key >> 11) & 7u;
  const unsigned crn = (key >> 7) & 15u;
  const unsigned crm = (key >> 3) & 15u;
  return op0 == 3u && op1 == 0u && crn == 0u && crm <= 7u;
}

bool requiresEl1(uint32_t key) {
  if (isDebugSysReg(key) || isIdSysReg(key)) return true;
  return key == kSCTLR_EL1 || key == kACTLR_EL1 || key == kCPACR_EL1 ||
         key == kTTBR0_EL1 || key == kTTBR1_EL1 || key == kTCR_EL1 ||
         key == kESR_EL1 || key == kFAR_EL1 || key == kMAIR_EL1 || key == kVBAR_EL1 ||
         key == kSPSR_EL1 || key == kELR_EL1 || key == kSP_EL0 || key == kTPIDR_EL1 ||
         key == kCONTEXTIDR_EL1 || key == kCNTKCTL_EL1 || key == kAFSR0_EL1 ||
         key == kAFSR1_EL1 || key == kAMAIR_EL1 || key == kPAR_EL1 ||
         key == kMIDR_EL1 || key == kREVIDR_EL1 || key == kMPIDR_EL1 || key == kID_AA64PFR0_EL1 ||
         key == kID_AA64PFR1_EL1 || key == kID_AA64DFR0_EL1 || key == kID_AA64ISAR0_EL1 ||
         key == kID_AA64MMFR0_EL1 || key == kID_AA64PFR2_EL1 || key == kID_AA64ZFR0_EL1 ||
         key == kID_AA64DFR1_EL1 || key == kID_AA64AFR0_EL1 || key == kID_AA64AFR1_EL1 ||
         key == kID_AA64ISAR1_EL1 || key == kID_AA64ISAR2_EL1 || key == kID_AA64ISAR3_EL1 || key == kID_AA64MMFR1_EL1 ||
         key == kID_AA64MMFR2_EL1 || key == kID_AA64MMFR3_EL1 || key == kID_AA64MMFR4_EL1 || key == kID_AA64SMFR0_EL1 ||
         key == kCLIDR_EL1 || key == kAIDR_EL1 || key == kCSSELR_EL1 || key == kCCSIDR_EL1 ||
         key == kCNTP_TVAL_EL0 || key == kCNTP_CTL_EL0 ||
         key == kCNTP_CVAL_EL0 || key == kCNTV_TVAL_EL0 || key == kCNTV_CTL_EL0 || key == kCNTV_CVAL_EL0;
}
bool isKnownSysReg(uint32_t key) {
  if (isDebugSysReg(key) || isIdSysReg(key)) return true;
  return key == kNZCV || key == kDAIF || key == kFPCR || key == kFPSR || key == kTPIDR_EL0 ||
         key == kTPIDRRO_EL0 || key == kCurrentEL || key == kMIDR_EL1 || key == kREVIDR_EL1 || key == kMPIDR_EL1 ||
         key == kID_AA64PFR0_EL1 || key == kID_AA64PFR1_EL1 || key == kID_AA64DFR0_EL1 ||
         key == kID_AA64ISAR0_EL1 || key == kID_AA64ISAR3_EL1 || key == kID_AA64MMFR0_EL1 || key == kID_AA64PFR2_EL1 ||
         key == kID_AA64ZFR0_EL1 || key == kID_AA64DFR1_EL1 || key == kID_AA64AFR0_EL1 ||
         key == kID_AA64AFR1_EL1 || key == kID_AA64ISAR1_EL1 || key == kID_AA64ISAR2_EL1 ||
         key == kID_AA64MMFR1_EL1 || key == kID_AA64MMFR2_EL1 || key == kID_AA64MMFR3_EL1 ||
         key == kID_AA64MMFR4_EL1 ||
         key == kID_AA64SMFR0_EL1 || key == kCLIDR_EL1 || key == kAIDR_EL1 ||
         key == kCSSELR_EL1 || key == kCCSIDR_EL1 || key == kCTR_EL0 ||
         key == kDCZID_EL0 || key == kCNTFRQ_EL0 || key == kCNTPCT_EL0 || key == kCNTVCT_EL0 ||
         key == kCNTP_TVAL_EL0 || key == kCNTP_CTL_EL0 || key == kCNTP_CVAL_EL0 ||
         key == kCNTV_TVAL_EL0 || key == kCNTV_CTL_EL0 || key == kCNTV_CVAL_EL0 ||
         isDebugSysReg(key) || requiresEl1(key);
}
}  // namespace

// Implemented MRS/MSR subset. Privileged accesses from EL0 take an undefined
// instruction exception to EL1; unknown encodings remain an explicit emulator stop.
StopReason Cpu::execSys(uint32_t insn, uint64_t pc, uint64_t* nextPc) {
  const bool read = ((insn >> 21) & 1) != 0;
  const unsigned rt = insn & 31;
  const uint32_t key = (insn >> 5) & 0x7FFF;
  auto undefined = [&]() {
    *nextPc = takeException(pc, 0x00, 0);
    return StopReason::None;
  };

  if (!isKnownSysReg(key)) return stopWith(StopReason::UnsupportedInstruction, pc, insn);
  if ((requiresEl1(key) && s_.current_el < 1) ||
      (key == kTPIDRRO_EL0 && !read && s_.current_el == 0) ||
      (key == kDAIF && !read && s_.current_el < 1))
    return undefined();

  if (read) {
    uint64_t v = 0;
    if (isDebugSysReg(key)) v = 0;  // RAZ: debug architecture intentionally not exposed
    else if (key == kNZCV) v = (static_cast<uint64_t>(s_.n) << 31) | (static_cast<uint64_t>(s_.z) << 30) |
                          (static_cast<uint64_t>(s_.c) << 29) | (static_cast<uint64_t>(s_.v) << 28);
    else if (key == kDAIF) v = static_cast<uint64_t>(s_.daif & 0xFu) << 6;
    else if (key == kFPCR) v = s_.fpcr;
    else if (key == kFPSR) v = s_.fpsr;
    else if (key == kTPIDR_EL0) v = s_.tpidr_el0;
    else if (key == kTPIDRRO_EL0) v = s_.tpidrro_el0;
    else if (key == kCurrentEL) v = static_cast<uint64_t>(s_.current_el) << 2;
    else if (key == kSCTLR_EL1) v = s_.sctlr_el1;
    else if (key == kACTLR_EL1) v = s_.actlr_el1;
    else if (key == kCPACR_EL1) v = s_.cpacr_el1;
    else if (key == kTTBR0_EL1) v = s_.ttbr0_el1;
    else if (key == kTTBR1_EL1) v = s_.ttbr1_el1;
    else if (key == kTCR_EL1) v = s_.tcr_el1;
    else if (key == kESR_EL1) v = s_.esr_el1;
    else if (key == kFAR_EL1) v = s_.far_el1;
    else if (key == kMAIR_EL1) v = s_.mair_el1;
    else if (key == kVBAR_EL1) v = s_.vbar_el1;
    else if (key == kSPSR_EL1) v = s_.spsr_el1;
    else if (key == kELR_EL1) v = s_.elr_el1;
    else if (key == kSP_EL0) v = s_.sp_el0;
    else if (key == kTPIDR_EL1) v = s_.tpidr_el1;
    else if (key == kCONTEXTIDR_EL1) v = s_.contextidr_el1;
    else if (key == kCNTKCTL_EL1) v = s_.cntkctl_el1;
    else if (key == kAFSR0_EL1) v = s_.afsr0_el1;
    else if (key == kAFSR1_EL1) v = s_.afsr1_el1;
    else if (key == kAMAIR_EL1) v = s_.amair_el1;
    else if (key == kPAR_EL1) v = s_.par_el1;
    else if (key == kMIDR_EL1) v = 0x410FD034ull; // synthetic generic ARMv8-A profile; not a silicon identity
    else if (key == kREVIDR_EL1) v = 0; // no implementation-specific revision identity in the virtual CPU
    else if (key == kMPIDR_EL1) v = 0x80000000ull; // Aff0=0, single vCPU
    else if (key == kID_AA64PFR0_EL1) v = 0x0000000000000011ull; // EL0/EL1; baseline FP/AdvSIMD, no optional FP16/EL2/EL3
    else if (key == kID_AA64PFR1_EL1 || key == kID_AA64DFR0_EL1 || key == kID_AA64ISAR0_EL1 || key == kID_AA64ISAR3_EL1) v = 0;
    else if (key == kID_AA64MMFR0_EL1) v = 0x5ull; // 48-bit PA, 4 KiB granule profile
    else if (key == kID_AA64PFR2_EL1 || key == kID_AA64ZFR0_EL1 || key == kID_AA64DFR1_EL1 ||
             key == kID_AA64AFR0_EL1 || key == kID_AA64AFR1_EL1 || key == kID_AA64ISAR1_EL1 ||
             key == kID_AA64ISAR2_EL1 || key == kID_AA64MMFR1_EL1 || key == kID_AA64MMFR2_EL1 ||
             key == kID_AA64MMFR3_EL1 || key == kID_AA64MMFR4_EL1 || key == kID_AA64SMFR0_EL1 || // <-- BURAYA
             key == kCLIDR_EL1 || key == kAIDR_EL1 || key == kCCSIDR_EL1) v = 0;
    else if (key == kCSSELR_EL1) v = s_.csselr_el1;
    else if (key == kCTR_EL0) v = 0x0000000000444004ull;
    else if (key == kDCZID_EL0) v = 0x14ull; // DZP=1: DC ZVA unavailable in this emulator
    else if (key == kCNTFRQ_EL0) v = s_.cntfrq_el0;
    else if (key == kCNTPCT_EL0 || key == kCNTVCT_EL0) v = counterValue();
    else if (key == kCNTP_CVAL_EL0) v = s_.cntp_cval_el0;
    else if (key == kCNTV_CVAL_EL0) v = s_.cntv_cval_el0;
    else if (key == kCNTP_CTL_EL0) {
      const bool expired = static_cast<int64_t>(counterValue() - s_.cntp_cval_el0) >= 0;
      v = (s_.cntp_ctl_el0 & 3u) | (expired ? 4u : 0u);
    } else if (key == kCNTV_CTL_EL0) {
      const bool expired = static_cast<int64_t>(counterValue() - s_.cntv_cval_el0) >= 0;
      v = (s_.cntv_ctl_el0 & 3u) | (expired ? 4u : 0u);
    } else if (key == kCNTP_TVAL_EL0) v = static_cast<uint64_t>(static_cast<int32_t>(s_.cntp_cval_el0 - counterValue()));
    else if (key == kCNTV_TVAL_EL0) v = static_cast<uint64_t>(static_cast<int32_t>(s_.cntv_cval_el0 - counterValue()));
    else if (isIdSysReg(key)) v = 0;
    else return stopWith(StopReason::UnsupportedInstruction, pc, insn);
    setX(rt, v);
  } else {
    const uint64_t v = getX(rt);
    if (isDebugSysReg(key)) { /* WI: no debug registers, breakpoints, or stepping in this VM */ }
    else if (isIdSysReg(key)) return undefined();
    else if (key == kNZCV) { s_.n = (v >> 31) & 1; s_.z = (v >> 30) & 1; s_.c = (v >> 29) & 1; s_.v = (v >> 28) & 1; }
    else if (key == kDAIF) s_.daif = static_cast<uint8_t>((v >> 6) & 0xFu);
    else if (key == kFPCR) s_.fpcr = static_cast<uint32_t>(v) & kFpcrMask;
    else if (key == kFPSR) s_.fpsr = static_cast<uint32_t>(v) & kFpsrMask;
    else if (key == kTPIDR_EL0) s_.tpidr_el0 = v;
    else if (key == kTPIDRRO_EL0) s_.tpidrro_el0 = v; // writable by EL1 only
    else if (key == kCurrentEL || key == kESR_EL1) return undefined();
    else if (key == kSCTLR_EL1) { s_.sctlr_el1 = v; flushTlb(); }
    else if (key == kACTLR_EL1) s_.actlr_el1 = v;
    else if (key == kCPACR_EL1) s_.cpacr_el1 = v;
    else if (key == kTTBR0_EL1) { s_.ttbr0_el1 = v; flushTlb(); }
    else if (key == kTTBR1_EL1) { s_.ttbr1_el1 = v; flushTlb(); }
    else if (key == kTCR_EL1) { s_.tcr_el1 = v; flushTlb(); }
    else if (key == kFAR_EL1) s_.far_el1 = v;
    else if (key == kMAIR_EL1) s_.mair_el1 = v;
    else if (key == kVBAR_EL1) s_.vbar_el1 = v & ~0x7FFull;
    else if (key == kSPSR_EL1) s_.spsr_el1 = v;
    else if (key == kELR_EL1) s_.elr_el1 = v;
    else if (key == kSP_EL0) {
      s_.sp_el0 = v;
      if (s_.current_el == 0 || !s_.spsel) s_.sp = v;
    } else if (key == kTPIDR_EL1) s_.tpidr_el1 = v;
    else if (key == kCONTEXTIDR_EL1) s_.contextidr_el1 = v;
    else if (key == kCNTKCTL_EL1) s_.cntkctl_el1 = v;
    else if (key == kAFSR0_EL1) s_.afsr0_el1 = v;
    else if (key == kAFSR1_EL1) s_.afsr1_el1 = v;
    else if (key == kAMAIR_EL1) s_.amair_el1 = v;
    else if (key == kPAR_EL1) s_.par_el1 = v;
    else if (key == kMIDR_EL1 || key == kREVIDR_EL1 || key == kMPIDR_EL1 || key == kID_AA64PFR0_EL1 ||
             key == kID_AA64PFR1_EL1 || key == kID_AA64DFR0_EL1 || key == kID_AA64ISAR0_EL1 ||
             key == kID_AA64MMFR0_EL1 || key == kID_AA64PFR2_EL1 || key == kID_AA64ZFR0_EL1 ||
             key == kID_AA64DFR1_EL1 || key == kID_AA64AFR0_EL1 || key == kID_AA64AFR1_EL1 ||
             key == kID_AA64ISAR1_EL1 || key == kID_AA64ISAR2_EL1 || key == kID_AA64ISAR3_EL1 || key == kID_AA64MMFR1_EL1 ||
             key == kID_AA64MMFR2_EL1 || key == kID_AA64MMFR3_EL1 || key == kID_AA64MMFR4_EL1 || key == kID_AA64SMFR0_EL1 ||
             key == kCLIDR_EL1 || key == kAIDR_EL1 || key == kCCSIDR_EL1 || key == kCTR_EL0 ||
             key == kDCZID_EL0 || key == kCNTPCT_EL0 || key == kCNTVCT_EL0) return undefined();
    else if (key == kCSSELR_EL1) s_.csselr_el1 = v & 0xFu;
    else if (key == kCNTFRQ_EL0) { if (s_.current_el != 1 || v == 0) return undefined(); s_.cntfrq_el0 = v; counterEpoch_ = std::chrono::steady_clock::now(); updateTimerLine(); }
    else if (key == kCNTP_CVAL_EL0) { s_.cntp_cval_el0 = v; updateTimerLine(); }
    else if (key == kCNTV_CVAL_EL0) { s_.cntv_cval_el0 = v; updateTimerLine(); }
    else if (key == kCNTP_TVAL_EL0) { s_.cntp_cval_el0 = counterValue() + static_cast<int32_t>(v); updateTimerLine(); }
    else if (key == kCNTV_TVAL_EL0) { s_.cntv_cval_el0 = counterValue() + static_cast<int32_t>(v); updateTimerLine(); }
    else if (key == kCNTP_CTL_EL0) { s_.cntp_ctl_el0 = static_cast<uint32_t>(v) & 3u; updateTimerLine(); }
    else if (key == kCNTV_CTL_EL0) { s_.cntv_ctl_el0 = static_cast<uint32_t>(v) & 3u; updateTimerLine(); }
    else return stopWith(StopReason::UnsupportedInstruction, pc, insn);
  }
  return StopReason::None;
}

StopReason Cpu::execute(uint32_t insn, uint64_t pc) {
  uint64_t next = pc + 4;
  const unsigned rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
  const bool sf = (insn >> 31) != 0;
  const uint64_t mask = sf ? ~0ull : 0xFFFFFFFFull;
  const unsigned dsize = sf ? 64 : 32;
  auto unsupported = [&]() { return stopWith(StopReason::UnsupportedInstruction, pc, insn); };
  auto setFlags = [&](const AddResult& r) { s_.n = r.n; s_.z = r.z; s_.c = r.c; s_.v = r.v; };

  if (insn == 0xD69F03E0u) {  // ERET
    if (s_.current_el != 1) {
      next = takeException(pc, 0x00, 0);
    } else {
      const uint64_t saved = s_.spsr_el1;
      const unsigned mode = static_cast<unsigned>(saved & 0x1Fu);
      if (mode != 0 && mode != 4 && mode != 5) {
        next = takeException(pc, 0x0E, 0);  // illegal exception return
      } else {
        s_.n = ((saved >> 31) & 1) != 0;
        s_.z = ((saved >> 30) & 1) != 0;
        s_.c = ((saved >> 29) & 1) != 0;
        s_.v = ((saved >> 28) & 1) != 0;
        s_.daif = static_cast<uint8_t>((saved >> 6) & 0xF);
        s_.current_el = mode == 0 ? 0 : 1;
        s_.spsel = mode == 5;
        s_.sp = activeSp();
        next = s_.elr_el1;
      }
    }
  } else if ((insn & 0xFFE0001Fu) == 0xD4000001u) {  // SVC #imm16
    const uint32_t imm16 = (insn >> 5) & 0xFFFFu;
    next = takeException(pc + 4, 0x15, imm16);
  } else if ((insn & 0xFFE0001Fu) == 0xD4000002u ||
             (insn & 0xFFE0001Fu) == 0xD4000003u) {  // HVC/SMC: minimal single-vCPU PSCI conduit
    if (s_.current_el != 1) {
      next = takeException(pc, 0x00, 0);
    } else {
      const uint64_t function = getX(0);
      int64_t result = -1; // PSCI_NOT_SUPPORTED
      switch (function) {
        case 0x84000000ull: result = 0x00010001; break; // PSCI_VERSION 1.1
        case 0x84000006ull: result = 2; break;          // MIGRATE_INFO_TYPE: not required for this board
        case 0x8400000Aull: {                         // PSCI_FEATURES
          const uint64_t queried = getX(1);
          switch (queried) {
            case 0x84000000ull: case 0x8400000Aull: case 0x84000004ull:
              result = 0; break;
            case 0x84000003ull: case 0xC4000003ull: // CPU_ON unavailable: one vCPU only
            case 0x84000008ull: case 0x84000009ull: // no host power control
              result = -1; break;
            default: result = -1; break;
          }
          break;
        }
        case 0x84000004ull: case 0xC4000004ull: // AFFINITY_INFO for the sole vCPU
          result = (getX(1) == 0) ? 0 : -2; break;  // ON / INVALID_PARAMETERS
        case 0x84000003ull: case 0xC4000003ull: // CPU_ON: reject unsupported/nonexistent target
          result = (getX(1) == 0) ? -4 : -2; break; // ALREADY_ON / INVALID_PARAMETERS
        default: result = -1; break;
      }
      setX(0, static_cast<uint64_t>(result));
    }
  } else if ((insn & 0xFFFFFE1Fu) == 0xD500401Fu) {  // MSR SPSel, #imm
    if (s_.current_el != 1) {
      next = takeException(pc, 0x00, 0);
    } else {
      s_.spsel = ((insn >> 8) & 1u) != 0;
      s_.sp = activeSp();
    }
  } else if ((insn & 0xFFFFF0FFu) == 0xD50340DFu) {  // MSR DAIFSet, #imm4
    if (s_.current_el != 1) next = takeException(pc, 0x00, 0);
    else s_.daif = static_cast<uint8_t>((s_.daif | ((insn >> 8) & 0xFu)) & 0xFu);
  } else if ((insn & 0xFFFFF0FFu) == 0xD50340FFu) {  // MSR DAIFClr, #imm4
    if (s_.current_el != 1) next = takeException(pc, 0x00, 0);
    else s_.daif = static_cast<uint8_t>((s_.daif & ~((insn >> 8) & 0xFu)) & 0xFu);
  } else if ((insn & 0xFFFFF01Fu) == 0xD503201Fu) {  // HINT
    // WFE/WFI are treated as scheduling hints in this single-vCPU interpreter.
    // Interrupts are checked at each instruction boundary; execution is not blocked here.
    // This preserves guest forward progress but is not power/performance-accurate.
  } else if ((insn & 0xFFFFF01Fu) == 0xD503301Fu) {  // CLREX / DSB / DMB / ISB
    const unsigned op2 = (insn >> 5) & 7;
    if (op2 == 2) exclValid_ = false;                       // CLREX
    else if (op2 != 4 && op2 != 5 && op2 != 6 && op2 != 7) return unsupported();
    // tek çekirdekli, sıralı yorumlayıcıda bariyerlerin gözlenebilir etkisi yok
  } else if ((insn & 0xFFE0001Fu) == 0xD4400000u) {  // HLT #imm16
    return stopWith(StopReason::Halted, pc, insn, Status::Ok, 0, (insn >> 5) & 0xFFFF);
  } else if ((insn & 0xFFE0001Fu) == 0xD4200000u) {  // BRK #imm16
    return stopWith(StopReason::Breakpoint, pc, insn, Status::Ok, 0, (insn >> 5) & 0xFFFF);
  } else if ((insn & 0x7C000000u) == 0x14000000u) {  // B / BL
    const uint64_t off = signExtend(insn & 0x3FFFFFFu, 26) << 2;
    if (sf) setX(30, pc + 4);
    next = pc + off;
  } else if ((insn & 0xFF000010u) == 0x54000000u) {  // B.cond
    if (conditionHolds(insn & 0xF, s_)) next = pc + (signExtend((insn >> 5) & 0x7FFFFu, 19) << 2);
  } else if ((insn & 0x7E000000u) == 0x34000000u) {  // CBZ / CBNZ
    const uint64_t v = getX(insn & 31) & mask;
    const bool nz = ((insn >> 24) & 1) != 0;
    if ((v == 0) != nz) next = pc + (signExtend((insn >> 5) & 0x7FFFFu, 19) << 2);
  } else if ((insn & 0x7E000000u) == 0x36000000u) {  // TBZ / TBNZ
    const unsigned bit = ((insn >> 31) << 5) | ((insn >> 19) & 31);
    const bool set = ((getX(insn & 31) >> bit) & 1) != 0;
    if (set == (((insn >> 24) & 1) != 0)) next = pc + (signExtend((insn >> 5) & 0x3FFFu, 14) << 2);
  } else if ((insn & 0xFFFFFC1Fu) == 0xD61F0000u) {  // BR
    next = getX(rn);
  } else if ((insn & 0xFFFFFC1Fu) == 0xD63F0000u) {  // BLR
    const uint64_t target = getX(rn);
    setX(30, pc + 4);
    next = target;
  } else if ((insn & 0xFFFFFC1Fu) == 0xD65F0000u) {  // RET
    next = getX(rn);
  } else if ((insn & 0x1F800000u) == 0x12800000u) {  // MOVN / MOVZ / MOVK
    const unsigned opc = (insn >> 29) & 3, hw = (insn >> 21) & 3;
    if (!sf && hw > 1) return unsupported();
    const unsigned shift = hw * 16;
    const uint64_t imm = static_cast<uint64_t>((insn >> 5) & 0xFFFF) << shift;
    if (opc == 0) setX(rd, ~imm & mask);
    else if (opc == 2) setX(rd, imm);
    else if (opc == 3) setX(rd, ((getX(rd) & ~(0xFFFFull << shift)) | imm) & mask);
    else return unsupported();
  } else if ((insn & 0x1F800000u) == 0x11000000u) {  // ADD/SUB/ADDS/SUBS (immediate)
    const bool sub = ((insn >> 30) & 1) != 0, flags = ((insn >> 29) & 1) != 0;
    uint64_t imm = (insn >> 10) & 0xFFF;
    if ((insn >> 22) & 1) imm <<= 12;
    const uint64_t a = getXSp(rn);
    const AddResult r = sub ? addWithCarry(a, ~imm, true, sf) : addWithCarry(a, imm, false, sf);
    if (flags) { setFlags(r); setX(rd, r.value); }   // Rd=31 -> XZR (CMP/CMN)
    else setXSp(rd, r.value);                        // Rd=31 -> SP
  } else if ((insn & 0x1F000000u) == 0x10000000u) {  // ADR / ADRP
    const uint64_t immlo = (insn >> 29) & 3, immhi = (insn >> 5) & 0x7FFFFu;
    const uint64_t imm = signExtend((immhi << 2) | immlo, 21);
    if ((insn >> 31) != 0) setX(rd, (pc & ~0xFFFull) + (imm << 12));
    else setX(rd, pc + imm);
  } else if ((insn & 0x1F200000u) == 0x0B000000u) {  // ADD/SUB/ADDS/SUBS (shifted register)
    const unsigned type = (insn >> 22) & 3, imm6 = (insn >> 10) & 63;
    if (type == 3 || (!sf && imm6 >= 32)) return unsupported();
    const bool sub = ((insn >> 30) & 1) != 0, flags = ((insn >> 29) & 1) != 0;
    const uint64_t op2 = applyShift(type, getX(rm), imm6, sf);
    const uint64_t a = getX(rn);
    const AddResult r = sub ? addWithCarry(a, ~op2, true, sf) : addWithCarry(a, op2, false, sf);
    if (flags) setFlags(r);
    setX(rd, r.value);
  } else if ((insn & 0x1F000000u) == 0x0A000000u) {  // AND/ORR/EOR/ANDS (+N: BIC/ORN/EON/BICS), shifted reg
    const unsigned opc = (insn >> 29) & 3, type = (insn >> 22) & 3, imm6 = (insn >> 10) & 63;
    if (!sf && imm6 >= 32) return unsupported();
    uint64_t op2 = applyShift(type, getX(rm), imm6, sf);
    if ((insn >> 21) & 1) op2 = ~op2 & mask;
    const uint64_t a = getX(rn) & mask;
    uint64_t res;
    switch (opc) {
      case 0: case 3: res = a & op2; break;
      case 1: res = a | op2; break;
      default: res = a ^ op2; break;
    }
    if (opc == 3) { s_.n = ((res >> (dsize - 1)) & 1) != 0; s_.z = (res == 0); s_.c = false; s_.v = false; }
    setX(rd, res);
  } else if ((insn & 0x1F800000u) == 0x12000000u) {  // AND/ORR/EOR/ANDS (immediate)
    const unsigned opc = (insn >> 29) & 3, N = (insn >> 22) & 1, immr = (insn >> 16) & 63, imms = (insn >> 10) & 63;
    uint64_t wmask, tmask;
    if ((!sf && N) || !decodeBitMasks(N, imms, immr, true, dsize, &wmask, &tmask)) return unsupported();
    const uint64_t a = getX(rn) & mask;
    uint64_t res;
    switch (opc) {
      case 0: case 3: res = a & wmask; break;
      case 1: res = a | wmask; break;
      default: res = a ^ wmask; break;
    }
    if (opc == 3) {
      s_.n = ((res >> (dsize - 1)) & 1) != 0; s_.z = (res == 0); s_.c = false; s_.v = false;
      setX(rd, res);       // ANDS: Rd=31 -> XZR (TST)
    } else {
      setXSp(rd, res);     // AND/ORR/EOR: Rd=31 -> SP
    }
  } else if ((insn & 0x1F800000u) == 0x13000000u) {  // SBFM / BFM / UBFM
    const unsigned opc = (insn >> 29) & 3, N = (insn >> 22) & 1, immr = (insn >> 16) & 63, imms = (insn >> 10) & 63;
    if (opc == 3 || N != static_cast<unsigned>(sf) || (!sf && ((immr | imms) & 32))) return unsupported();
    uint64_t wmask, tmask;
    if (!decodeBitMasks(N, imms, immr, false, dsize, &wmask, &tmask)) return unsupported();
    const uint64_t src = getX(rn) & mask;
    const uint64_t dst = (opc == 1) ? (getX(rd) & mask) : 0;
    const uint64_t bot = (dst & ~wmask) | (rorBits(src, immr, dsize) & wmask);
    uint64_t res;
    if (opc == 0) {
      const uint64_t top = ((src >> imms) & 1) ? mask : 0;
      res = (top & ~tmask) | (bot & tmask);
    } else if (opc == 2) {
      res = bot & tmask;
    } else {
      res = (dst & ~tmask) | (bot & tmask);
    }
    setX(rd, res & mask);
  } else if ((insn & 0x1FE00800u) == 0x1A800000u && ((insn >> 29) & 1) == 0) {  // CSEL/CSINC/CSINV/CSNEG
    const bool op = ((insn >> 30) & 1) != 0, o2 = ((insn >> 10) & 1) != 0;
    uint64_t v;
    if (conditionHolds((insn >> 12) & 0xF, s_)) {
      v = getX(rn);
    } else {
      const uint64_t m = getX(rm);
      v = !op ? (o2 ? m + 1 : m) : (o2 ? (~m + 1) : ~m);
    }
    setX(rd, v & mask);
  } else if ((insn & 0x1F000000u) == 0x1B000000u) {  // MADD/MSUB, SMADDL.., SMULH/UMULH
    if (((insn >> 29) & 3) != 0) return unsupported();
    const unsigned op31 = (insn >> 21) & 7, ra = (insn >> 10) & 31;
    const bool o0 = ((insn >> 15) & 1) != 0;
    const uint64_t a = getX(ra), n = getX(rn), m = getX(rm);
    switch (op31) {
      case 0: {  // MADD / MSUB
        const uint64_t prod = (n & mask) * (m & mask);
        setX(rd, (o0 ? a - prod : a + prod) & mask);
        break;
      }
      case 1: {  // SMADDL / SMSUBL
        if (!sf) return unsupported();
        const uint64_t prod = static_cast<uint64_t>(sext(n, false) * sext(m, false));
        setX(rd, o0 ? a - prod : a + prod);
        break;
      }
      case 5: {  // UMADDL / UMSUBL
        if (!sf) return unsupported();
        const uint64_t prod = (n & 0xFFFFFFFFull) * (m & 0xFFFFFFFFull);
        setX(rd, o0 ? a - prod : a + prod);
        break;
      }
      case 2: {  // SMULH
        if (!sf || o0) return unsupported();
        const __int128 p = static_cast<__int128>(static_cast<int64_t>(n)) * static_cast<__int128>(static_cast<int64_t>(m));
        setX(rd, static_cast<uint64_t>(p >> 64));
        break;
      }
      case 6: {  // UMULH
        if (!sf || o0) return unsupported();
        const unsigned __int128 p = static_cast<unsigned __int128>(n) * m;
        setX(rd, static_cast<uint64_t>(p >> 64));
        break;
      }
      default: return unsupported();
    }
  } else if ((insn & 0x5FE00000u) == 0x1AC00000u && ((insn >> 29) & 1) == 0) {  // UDIV/SDIV/LSLV/LSRV/ASRV/RORV
    const unsigned opcode = (insn >> 10) & 0x3F;
    const uint64_t a = getX(rn) & mask, b = getX(rm) & mask;
    uint64_t res;
    if (opcode == 2) {
      res = b ? a / b : 0;
    } else if (opcode == 3) {
      const int64_t sa = sext(a, sf), sb = sext(b, sf);
      if (sb == 0) res = 0;
      else if (sb == -1) res = 0 - static_cast<uint64_t>(sa);   // INT_MIN / -1 taşmasında sarar
      else res = static_cast<uint64_t>(sa / sb);
    } else if (opcode >= 8 && opcode <= 11) {
      res = applyShift(opcode - 8, a, static_cast<unsigned>(b % dsize), sf);
    } else {
      return unsupported();  // CRC32 vb.
    }
    setX(rd, res & mask);
  } else if ((insn & 0x1FE0FC00u) == 0x1A000000u) {  // ADC/ADCS/SBC/SBCS
    const bool sub = ((insn >> 30) & 1) != 0, flags = ((insn >> 29) & 1) != 0;
    const uint64_t a = getX(rn), b = getX(rm);
    const AddResult r = addWithCarry(a, sub ? ~b : b, s_.c, sf);
    if (flags) setFlags(r);
    setX(rd, r.value);
  } else if ((insn & 0x1FE00000u) == 0x0B200000u) {  // ADD/SUB/ADDS/SUBS (extended register)
    const unsigned option = (insn >> 13) & 7, imm3 = (insn >> 10) & 7;
    if (imm3 > 4) return unsupported();
    const bool sub = ((insn >> 30) & 1) != 0, flags = ((insn >> 29) & 1) != 0;
    const uint64_t op2 = extendReg(getX(rm), option, imm3, sf);
    const uint64_t a = getXSp(rn);
    const AddResult r = sub ? addWithCarry(a, ~op2, true, sf) : addWithCarry(a, op2, false, sf);
    if (flags) { setFlags(r); setX(rd, r.value); }
    else setXSp(rd, r.value);
  } else if ((insn & 0x3FE00410u) == 0x3A400000u) {  // CCMN / CCMP (register, immediate)
    const bool isCcmp = ((insn >> 30) & 1) != 0, immForm = ((insn >> 11) & 1) != 0;
    if (conditionHolds((insn >> 12) & 0xF, s_)) {
      const uint64_t b = immForm ? rm : getX(rm);   // imm5 aynı bit alanında
      setFlags(isCcmp ? addWithCarry(getX(rn), ~b, true, sf) : addWithCarry(getX(rn), b, false, sf));
    } else {
      s_.n = (insn >> 3) & 1; s_.z = (insn >> 2) & 1; s_.c = (insn >> 1) & 1; s_.v = insn & 1;
    }
  } else if ((insn & 0x7FE00000u) == 0x5AC00000u && ((insn >> 16) & 31) == 0) {  // RBIT/REV16/REV32/REV/CLZ/CLS
    const unsigned opcode = (insn >> 10) & 0x3F;
    const uint64_t v = getX(rn) & mask;
    uint64_t res;
    switch (opcode) {
      case 0: res = bitReverse(v, dsize); break;
      case 1: res = (((v & 0x00FF00FF00FF00FFull) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFull)) & mask; break;
      case 2:
        if (sf) res = (static_cast<uint64_t>(__builtin_bswap32(static_cast<uint32_t>(v >> 32))) << 32) | __builtin_bswap32(static_cast<uint32_t>(v));
        else res = __builtin_bswap32(static_cast<uint32_t>(v));
        break;
      case 3:
        if (!sf) return unsupported();
        res = __builtin_bswap64(v);
        break;
      case 4: res = v == 0 ? dsize : static_cast<uint64_t>(__builtin_clzll(v)) - (64 - dsize); break;
      case 5: {
        const uint64_t t = (v ^ (v >> 1)) & (mask >> 1);   // komşu bit farkları, dsize-1 bit
        res = t == 0 ? dsize - 1 : (dsize - 2) - (63u - static_cast<unsigned>(__builtin_clzll(t)));
        break;
      }
      default: return unsupported();   // PAC vb.
    }
    setX(rd, res & mask);
  } else if ((insn & 0x1F800000u) == 0x13800000u) {  // EXTR (ROR immediate)
    const unsigned imms = (insn >> 10) & 63, N = (insn >> 22) & 1;
    if (((insn >> 29) & 3) != 0 || ((insn >> 21) & 1) != 0 || N != static_cast<unsigned>(sf) || (!sf && imms >= 32)) return unsupported();
    const uint64_t hi = getX(rn) & mask, lo = getX(rm) & mask;
    setX(rd, (imms == 0 ? lo : ((lo >> imms) | (hi << (dsize - imms)))) & mask);
  } else if ((insn & 0x3F000000u) == 0x39000000u) {  // LDR/STR (immediate, unsigned offset), PRFM
    const unsigned size = (insn >> 30) & 3, opc = (insn >> 22) & 3;
    if (!(size == 3 && opc == 2)) {  // PRFM: ipucu, bellek erişimi yok
      if (!validLoadStore(size, opc)) return unsupported();
      const uint64_t addr = getXSp(rn) + (static_cast<uint64_t>((insn >> 10) & 0xFFF) << size);
      const StopReason r = memAccess(insn, pc, size, opc, rd, addr);
      if (r != StopReason::None) return r;
    }
  } else if ((insn & 0x3F200000u) == 0x38000000u) {  // LDUR/STUR/PRFUM, LDTR/STTR, pre/post-index (imm9)
    const unsigned size = (insn >> 30) & 3, opc = (insn >> 22) & 3, idx = (insn >> 10) & 3;
    if (size == 3 && opc == 2) {
      if (idx != 0) return unsupported();  // PRFUM dışında ayrılmış
    } else {
      if (!validLoadStore(size, opc)) return unsupported();
      const uint64_t imm9 = signExtend((insn >> 12) & 0x1FF, 9);
      const uint64_t base = getXSp(rn);
      const bool wback = (idx == 1 || idx == 3);
      if (wback && rn == rd && rn != 31) return unsupported();  // UNPREDICTABLE kodlama
      // idx 0: LDUR/STUR, idx 2: LDTR/STTR. NOT: LDTR/STTR gerçekte EL0 yetkisiyle erişir; MMU/EL modeli (Aşama 4)
      // gelene kadar normal erişimden farkı yoktur. O zaman burada EL0 izin denetimi eklenmeli.
      const uint64_t addr = (idx == 1) ? base : base + imm9;
      const StopReason r = memAccess(insn, pc, size, opc, rd, addr);
      if (r != StopReason::None) return r;
      if (wback) setXSp(rn, base + imm9);
    }
  } else if ((insn & 0x3F200C00u) == 0x38200800u) {  // LDR/STR/PRFM (register offset)
    const unsigned size = (insn >> 30) & 3, opc = (insn >> 22) & 3, option = (insn >> 13) & 7;
    if (option != 2 && option != 3 && option != 6 && option != 7) return unsupported();
    if (!(size == 3 && opc == 2)) {
      if (!validLoadStore(size, opc)) return unsupported();
      const uint64_t rmv = getX(rm);
      uint64_t off = (option == 2) ? (rmv & 0xFFFFFFFFull)
                   : (option == 6) ? signExtend(rmv & 0xFFFFFFFFull, 32) : rmv;
      if ((insn >> 12) & 1) off <<= size;
      const StopReason r = memAccess(insn, pc, size, opc, rd, getXSp(rn) + off);
      if (r != StopReason::None) return r;
    }
  } else if ((insn & 0x3F000000u) == 0x18000000u) {  // LDR (literal), LDRSW (literal), PRFM (literal)
    const unsigned opc = (insn >> 30) & 3;
    if (opc != 3) {
      const uint64_t addr = pc + (signExtend((insn >> 5) & 0x7FFFFu, 19) << 2);
      const StopReason r = memAccess(insn, pc, opc == 1 ? 3 : 2, opc == 2 ? 2 : 1, rd, addr);
      if (r != StopReason::None) return r;
    }
  } else if ((insn & 0x3E000000u) == 0x28000000u) {  // LDP/STP/LDPSW, LDNP/STNP
    const unsigned opc = (insn >> 30) & 3, idx = (insn >> 23) & 3, rt2 = (insn >> 10) & 31;
    const bool load = ((insn >> 22) & 1) != 0;
    if (opc == 3 || (opc == 1 && (!load || idx == 0))) return unsupported();  // STPSW / LDNPSW yok
    const unsigned scale = (opc == 2) ? 3 : 2, bytes = 1u << scale;
    const uint64_t off = signExtend((insn >> 15) & 0x7F, 7) << scale;
    const uint64_t base = getXSp(rn);
    const bool wback = (idx == 1 || idx == 3);   // idx 0: LDNP/STNP (non-temporal ipucu, sonuç aynı)
    if (wback && (rd == rn || rt2 == rn) && rn != 31) return unsupported();  // UNPREDICTABLE
    if (load && rd == rt2) return unsupported();                            // UNPREDICTABLE
    const uint64_t addr = (idx == 1) ? base : base + off;
    if (load) {
      uint64_t v1 = 0, v2 = 0;
      StopReason ar = loadMemory(addr, bytes, &v1, pc, insn);
      if (ar != StopReason::None) return ar;
      ar = loadMemory(addr + bytes, bytes, &v2, pc, insn);
      if (ar != StopReason::None) return ar;
      if (opc == 1) { v1 = signExtend(v1, 32); v2 = signExtend(v2, 32); }  // LDPSW
      setX(rd, v1);
      setX(rt2, v2);
    } else {
      StopReason ar = storeMemory(addr, bytes, getX(rd), pc, insn);
      if (ar != StopReason::None) return ar;
      ar = storeMemory(addr + bytes, bytes, getX(rt2), pc, insn);
      if (ar != StopReason::None) return ar;
    }
    if (wback) setXSp(rn, base + off);
  } else if ((insn & 0x3F000000u) == 0x08000000u) {  // exclusive / acquire-release (LDXR, STXR, LDAR, STLR, LDXP, STXP)
    const unsigned size = (insn >> 30) & 3, rt2 = (insn >> 10) & 31, rs = rm;
    const bool o2 = ((insn >> 23) & 1) != 0, load = ((insn >> 22) & 1) != 0, o1 = ((insn >> 21) & 1) != 0;
    const bool o0 = ((insn >> 15) & 1) != 0;
    const uint64_t addr = getXSp(rn);
    if (o2 && o1) return unsupported();                 // CAS/CASP (LSE)
    if (o2) {                                           // LDAR/STLR; LDLAR/STLLR (o0==0) yok
      if (!o0) return unsupported();
      if (addr & ((1ull << size) - 1)) return stopWith(StopReason::AlignmentFault, pc, insn, Status::Ok, addr);
      const StopReason r = memAccess(insn, pc, size, load ? 1 : 0, rd, addr);   // sıralı yorumlayıcıda acquire/release = normal erişim
      if (r != StopReason::None) return r;
    } else if (!o1) {                                   // LDXR/LDAXR/STXR/STLXR
      const unsigned bytes = 1u << size;
      if (addr & (bytes - 1)) return stopWith(StopReason::AlignmentFault, pc, insn, Status::Ok, addr);
      if (load) {
        const StopReason r = memAccess(insn, pc, size, 1, rd, addr);
        if (r != StopReason::None) return r;
        exclValid_ = true; exclAddr_ = addr; exclSize_ = bytes;
      } else {
        if (rs == rd || (rs == rn && rn != 31)) return unsupported();  // UNPREDICTABLE
        const bool ok = exclValid_ && exclAddr_ == addr && exclSize_ == bytes;
        if (ok) {
          const StopReason r = memAccess(insn, pc, size, 0, rd, addr);
          if (r != StopReason::None) return r;
        }
        setX(rs, ok ? 0 : 1);
        exclValid_ = false;
      }
    } else {                                            // LDXP/LDAXP/STXP/STLXP
      if (size < 2) return unsupported();
      const unsigned bytes = 1u << size;
      if (addr & (2 * bytes - 1)) return stopWith(StopReason::AlignmentFault, pc, insn, Status::Ok, addr);
      if (load) {
        if (rd == rt2) return unsupported();            // UNPREDICTABLE
        uint64_t v1 = 0, v2 = 0;
        StopReason ar = loadMemory(addr, bytes, &v1, pc, insn);
        if (ar != StopReason::None) return ar;
        ar = loadMemory(addr + bytes, bytes, &v2, pc, insn);
        if (ar != StopReason::None) return ar;
        setX(rd, v1);
        setX(rt2, v2);
        exclValid_ = true; exclAddr_ = addr; exclSize_ = 2 * bytes;
      } else {
        if (rs == rd || rs == rt2 || (rs == rn && rn != 31)) return unsupported();  // UNPREDICTABLE
        const bool ok = exclValid_ && exclAddr_ == addr && exclSize_ == 2 * bytes;
        if (ok) {
          StopReason ar = storeMemory(addr, bytes, getX(rd), pc, insn);
          if (ar != StopReason::None) return ar;
          ar = storeMemory(addr + bytes, bytes, getX(rt2), pc, insn);
          if (ar != StopReason::None) return ar;
        }
        setX(rs, ok ? 0 : 1);
        exclValid_ = false;
      }
    }
  } else if ((insn & 0xFFF00000u) == 0xD5000000u &&
             ((((insn >> 12) & 0xFu) == 7u) || (((insn >> 12) & 0xFu) == 8u))) {
    // SYS cache-maintenance (CRn=7) is a no-op in this unified, uncached memory
    // model. TLBI (CRn=8) invalidates the software TLB; flushing more entries
    // than requested is safe and avoids stale guest translations in this MVP.
    if (s_.current_el != 1) next = takeException(pc, 0x00, 0);
    else if (((insn >> 12) & 0xFu) == 8u) flushTlb();
  } else if ((insn & 0xFFD00000u) == 0xD5100000u) {  // MSR/MRS (register)
    const StopReason r = execSys(insn, pc, &next);
    if (r != StopReason::None) return r;
  } else {
    // SIMD&FP yükleme/saklama ve skaler FP; tanınmazsa UnsupportedInstruction döner
    const StopReason r = execFp(insn, pc);
    if (r != StopReason::None) return r;
  }

  s_.pc = next;
  retired_++;
  return StopReason::None;
}

}  // namespace ncv
