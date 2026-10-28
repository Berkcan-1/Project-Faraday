// NCV - AArch64 yorumlayıcı çekirdeği.
// Komut desteği docs/compatibility-matrix.md'de sınırlandırılmıştır. Aşama 4A,
// EL0/EL1 + temel SVC/ERET davranışını ekler; MMU, interrupt/GIC ve tam
// mimari sistem-register/exception kapsamı henüz yoktur.
// Uygulanmayan komutlar sessizce taklit edilmez; emülatör hatası olarak dururlar.
#pragma once
#include <array>
#include <cstdint>

#include "../core/ncv_types.h"
#include "../memory/ncv_bus.h"
#include "../devices/ncv_gicv2.h"
#include <chrono>

namespace ncv {

enum class StopReason : int32_t {
  None = 0,                 // step() başarılı
  InstructionLimit,         // run() limiti doldu (hata değil)
  Halted,                   // HLT: NCV test sözleşmesi (mimari davranış DEĞİL)
  Breakpoint,               // BRK
  UnsupportedInstruction,   // decode edilemedi / uygulanmadı
  BusFault,                 // veri erişimi başarısız
  FetchFault,               // komut getirme başarısız
  MisalignedPc,             // PC 4 baytlık hizalı değil
  AlignmentFault,           // exclusive/acquire-release erişimi hizasız (data-abort vektörlemesi henüz yok)
  StoppedByHost,            // Vm::stop()
  ExceptionTaken,            // iç yürütme sonucu; step() bunu devam eden guest yürütmesine çevirir
};
const char* stopReasonName(StopReason r);

struct StopInfo {
  StopReason reason = StopReason::None;
  uint64_t pc = 0;          // olayın olduğu komutun adresi
  uint32_t insn = 0;
  Status busStatus = Status::Ok;
  uint64_t faultAddr = 0;
  uint16_t imm = 0;         // HLT/BRK imm16
};

struct VReg {
  uint64_t lo = 0, hi = 0;   // 128-bit SIMD&FP register, little-endian
};

struct CpuState {
  uint64_t x[31] = {};
  VReg vr[32];              // V0..V31
  uint32_t fpcr = 0;         // yazılabilir: RMode[23:22], FZ[24], DN[25]
  uint32_t fpsr = 0;         // birikimli bayraklar: IOC DZC OFC UFC IXC IDC (QC: SIMD, 2c-2)
  // Exception-level / PSTATE model (Aşama 4A). `sp` is a compatibility mirror
  // of the currently selected SP; architectural code should use sp_el0/sp_el1.
  uint8_t current_el = 0;                  // 0 or 1; EL2/EL3 are not implemented
  bool spsel = false;                      // EL1: false = SP_EL0 (EL1t), true = SP_EL1 (EL1h)
  uint8_t daif = 0xF;                      // D, A, I, F mask bits in bits 3:0
  uint64_t sp_el0 = 0, sp_el1 = 0;
  uint64_t sp = 0;
  uint64_t pc = 0;
  bool n = false, z = false, c = false, v = false;

  // Implemented system-register storage. This is not yet an MMU implementation.
  uint64_t sctlr_el1 = 0, tcr_el1 = 0, ttbr0_el1 = 0, ttbr1_el1 = 0, mair_el1 = 0;
  uint64_t vbar_el1 = 0, esr_el1 = 0, far_el1 = 0, elr_el1 = 0, spsr_el1 = 0;
  uint64_t tpidr_el0 = 0, tpidrro_el0 = 0, tpidr_el1 = 0;
  uint64_t actlr_el1 = 0, cpacr_el1 = 0, contextidr_el1 = 0, cntkctl_el1 = 0;
  uint64_t afsr0_el1 = 0, afsr1_el1 = 0, amair_el1 = 0, par_el1 = 0;
  uint64_t csselr_el1 = 0;
  // Minimal architected physical timer state. Counter is host-monotonic time scaled to CNTFRQ.
  uint64_t cntfrq_el0 = 1000000;
  uint64_t cntp_cval_el0 = 0;
  uint32_t cntp_ctl_el0 = 0;
  uint64_t cntv_cval_el0 = 0;
  uint32_t cntv_ctl_el0 = 0;
  // Last guest memory abort, captured before exception entry. These fields are
  // diagnostics only and do not affect guest execution.
  uint64_t last_abort_pc = 0, last_abort_va = 0, last_abort_pa = 0;
  uint32_t last_abort_insn = 0;
  int32_t last_abort_bus_status = static_cast<int32_t>(Status::Ok);
  uint8_t last_abort_size = 0; // byte width of the failing bus transaction, if known
  uint8_t last_abort_access = 255; // 0=read, 1=write, 2=execute, 255=none
  bool last_abort_table_walk = false;
  // Fault-time MMU context: lets diagnostics distinguish a bad PTE from an unmapped bus target.
  uint64_t last_abort_tcr_el1 = 0, last_abort_ttbr0_el1 = 0, last_abort_ttbr1_el1 = 0;
  uint64_t last_abort_x8 = 0, last_abort_walk_root = 0, last_abort_walk_va_page = 0;
  uint64_t last_abort_l1_desc = 0, last_abort_l2_desc = 0, last_abort_l3_desc = 0;
  uint64_t last_abort_l3_entry_pa = 0;
  bool last_abort_pte_writer_found = false;
  uint64_t last_abort_pte_writer_pc = 0, last_abort_pte_writer_va = 0;
  uint64_t last_abort_pte_writer_pa = 0, last_abort_pte_writer_value = 0;
  uint32_t last_abort_pte_writer_insn = 0;
  uint8_t last_abort_pte_writer_size = 0;
  bool last_abort_walk_ttbr1 = false, last_abort_translation_cache_hit = false;
  // On a bus abort that used a TLB entry, independently re-read the current
  // guest page tables once. This distinguishes a stale cached translation from
  // a PTE that currently maps the same VA to the failing physical address.
  bool last_abort_fresh_walk_attempted = false, last_abort_fresh_walk_ok = false;
  uint64_t last_abort_fresh_pa = 0, last_abort_fresh_root = 0;
  uint64_t last_abort_fresh_l1_desc = 0, last_abort_fresh_l2_desc = 0, last_abort_fresh_l3_desc = 0;
  uint8_t last_abort_fresh_fsc = 0;
  bool last_abort_fresh_table_walk = false;
  uint64_t last_abort_fresh_fault_pa = 0;
  int32_t last_abort_fresh_bus_status = static_cast<int32_t>(Status::Ok);
};

class Cpu {
 public:
  explicit Cpu(Bus* bus, GicV2* gic = nullptr) : bus_(bus), gic_(gic) {}

  // initialEL defaults to EL0 for backward-compatible test payloads. Real kernel
  // payloads should explicitly start at EL1 until an EL3/EL2 boot path exists.
  void reset(uint64_t entryPc, uint64_t initialSp, uint8_t initialEL = 0);
  StopReason step();                       // tek komut
  StopReason run(uint64_t maxInstructions);

  const CpuState& state() const { return s_; }
  CpuState& mutableState() { return s_; }
  const StopInfo& lastStop() const { return stop_; }
  uint64_t instructionsRetired() const { return retired_; }
  uint32_t lastInstruction() const { return lastInstruction_; }

 private:
  enum class AccessKind : uint8_t { Read, Write, Execute };
  struct MmuFault {
    uint8_t fsc = 0;          // ESR_EL1.DFSC/IFSC
    uint8_t level = 0;
    bool tableWalk = false;
    uint64_t faultPhysicalAddress = 0;
    Status busStatus = Status::Ok;
  };
  struct TlbEntry {
    bool valid = false;
    bool ttbr1 = false;
    uint64_t vaPage = 0;
    uint64_t paPage = 0;
    uint64_t leaf = 0;
    uint64_t descriptors[4] = {};
    uint8_t level = 0;
    bool tablePxn = false, tableUxn = false, tableNoEl0 = false, tableReadOnly = false;
  };
  struct ZeroPaPteWriteTrace {
    bool valid = false;
    uint64_t pc = 0, va = 0, pa = 0, value = 0;
    uint32_t insn = 0;
    uint8_t size = 0;
  };
  static constexpr size_t kTlbEntries = 256;
  // Diagnostic-only ring: remember guest stores that look like valid 4 KiB
  // page descriptors mapping to PA 0, then match them against the faulting PTE.
  static constexpr size_t kZeroPaPteWriteTraceEntries = 1024;
  std::array<ZeroPaPteWriteTrace, kZeroPaPteWriteTraceEntries> zeroPaPteWrites_{};
  size_t zeroPaPteWriteNext_ = 0, zeroPaPteWriteCount_ = 0;
  void recordZeroPaPteWrite(uint64_t pc, uint32_t insn, uint64_t va, uint64_t pa,
                            unsigned size, uint64_t value);
  std::array<TlbEntry, kTlbEntries> tlb_{};
  // Most recent address-translation walk. Captured into CpuState if that access aborts.
  uint64_t walkRoot_ = 0, walkVaPage_ = 0;
  uint64_t walkDescriptors_[4] = {};
  bool walkTtbr1_ = false, walkCacheHit_ = false;
  void flushTlb();
  bool translateAddress(uint64_t va, AccessKind kind, uint64_t* pa, MmuFault* fault);
  StopReason loadMemory(uint64_t va, unsigned size, uint64_t* out, uint64_t pc,
                        uint32_t insn, AccessKind kind = AccessKind::Read);
  StopReason storeMemory(uint64_t va, unsigned size, uint64_t value, uint64_t pc,
                         uint32_t insn);
  StopReason raiseAbort(uint64_t pc, uint32_t insn, uint64_t va, AccessKind kind,
                        uint8_t fsc, bool tableWalk = false,
                        uint64_t faultPhysicalAddress = 0,
                        Status busStatus = Status::Ok, unsigned accessSize = 0);
  StopReason execute(uint32_t insn, uint64_t pc);
  StopReason execFp(uint32_t insn, uint64_t pc);   // SIMD&FP yükleme/saklama + skaler FP (ncv_fpsimd.cpp)
  StopReason execSys(uint32_t insn, uint64_t pc, uint64_t* nextPc);  // MRS/MSR (register)
  uint64_t takeException(uint64_t returnPc, uint8_t ec, uint32_t iss = 0, uint64_t far = 0);
  uint64_t takeIrq(uint64_t returnPc);
  uint64_t counterValue() const;
  void updateTimerLine();
  uint64_t activeSp() const;
  void setActiveSp(uint64_t value);
  StopReason execAsimd(uint32_t insn, uint64_t pc);
  StopReason execAsimd2(uint32_t insn, uint64_t pc);  // ASIMD FP, doyurucu, uzun/geniş/dar, by-element, skaler-SIMD (ncv_asimd2.cpp)
  bool scalarOp_ = false;
  bool exceptionTaken_ = false;                              // skaler-SIMD: vektör çekirdeği yalnızca lane 0'ı işler // ASIMD tamsayı vektör + yapı yükleme/saklama (ncv_asimd.cpp)
  // opc: 0 store, 1 load (sıfır genişletme), 2 işaretli yükleme -> X, 3 işaretli yükleme -> W
  StopReason memAccess(uint32_t insn, uint64_t pc, unsigned size, unsigned opc, unsigned rt, uint64_t addr);
  StopReason stopWith(StopReason r, uint64_t pc, uint32_t insn, Status bs = Status::Ok,
                      uint64_t fa = 0, uint16_t imm = 0);
  uint64_t getX(unsigned r) const { return r == 31 ? 0 : s_.x[r]; }      // 31 = XZR
  void setX(unsigned r, uint64_t v) { if (r != 31) s_.x[r] = v; }
  uint64_t getXSp(unsigned r) const { return r == 31 ? activeSp() : s_.x[r]; }  // 31 = active SP
  void setXSp(unsigned r, uint64_t v) { if (r == 31) setActiveSp(v); else s_.x[r] = v; }

  Bus* bus_;
  GicV2* gic_;
  std::chrono::steady_clock::time_point counterEpoch_{};
  CpuState s_;
  StopInfo stop_;
  uint64_t retired_ = 0;
  uint32_t lastInstruction_ = 0;  // last fetched guest opcode, published at slice boundaries
  // Yerel exclusive monitor (tek çekirdek)
  bool exclValid_ = false;
  uint64_t exclAddr_ = 0;
  unsigned exclSize_ = 0;
};

}  // namespace ncv
