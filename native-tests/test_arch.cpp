#include "core/ncv_vm.h"
#include "test_util.h"

using namespace ncv;

namespace {
constexpr uint32_t kSvc42 = 0xD4000841u;       // SVC #0x42
constexpr uint32_t kEret = 0xD69F03E0u;
constexpr uint32_t kHlt42 = 0xD4400840u;       // HLT #0x42 (NCV test sentinel)
constexpr uint32_t kMrsCurrentElX0 = 0xD5384240u;
constexpr uint32_t kMrsSctlrEl1X0 = 0xD5381000u;
constexpr uint32_t kMsrSpsel0 = 0xD50040BFu;
constexpr uint32_t kMsrSpsel1 = 0xD50041BFu;

void put32(Machine& m, uint64_t addr, uint32_t value) {
  const uint8_t bytes[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                            static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  CHECK_ST(m.bus.loadBytes(GuestAddr(addr), bytes, sizeof(bytes)), Status::Ok);
}
}


namespace {
constexpr uint64_t kL1 = Machine::kBase + 0x1000;
constexpr uint64_t kL2 = Machine::kBase + 0x2000;
constexpr uint64_t kL3 = Machine::kBase + 0x3000;
constexpr uint64_t kPteValidTable = 3ull;
constexpr uint64_t kPteAf = 1ull << 10;
constexpr uint64_t kPteApEl0Rw = 1ull << 6; // AP[2:1] = 01: EL1 RW, EL0 RW
constexpr uint64_t kTcr4k39 = 25ull | (25ull << 16) | (2ull << 30); // T0SZ/T1SZ=25, TG1=4K

void write64(Machine& m, uint64_t addr, uint64_t value) {
  CHECK_ST(m.bus.write(GuestAddr(addr), 8, value), Status::Ok);
}
void setupMmu4k(Machine& m, bool el0DataAccess = true) {
  write64(m, kL1, kL2 | kPteValidTable);
  write64(m, kL2, kL3 | kPteValidTable);
  // VA 0x0000 -> PA 0x40000000 (code and EL1 exception vector page)
  write64(m, kL3, Machine::kBase | 3ull | kPteAf | kPteApEl0Rw);
  // VA 0x4000 -> PA 0x40005000 (data page)
  write64(m, kL3 + 4 * 8, (Machine::kBase + 0x5000) | 3ull | kPteAf |
          (el0DataAccess ? kPteApEl0Rw : 0ull));
}
void enableMmu4k(Machine& m, uint8_t el = 1) {
  m.cpu.reset(0, Machine::kBase + Machine::kRam, el);
  CpuState& s = m.cpu.mutableState();
  s.ttbr0_el1 = kL1;
  s.ttbr1_el1 = kL1;
  s.tcr_el1 = kTcr4k39;
  s.sctlr_el1 |= 1ull; // M=1
  s.vbar_el1 = 0;
  s.sp_el1 = 0x8000;
  s.sp = el == 0 ? s.sp_el0 : s.sp_el1;
}
}


NT_TEST(arch_mmu_4k_level2_block_descriptor) {
  Machine m;
  put32(m, Machine::kBase, 0xD2824680u); // MOVZ X0,#0x1234
  put32(m, Machine::kBase + 4, kHlt42);
  // T0SZ=25 starts at level 1: L1[0] -> L2; L2[0] is a 2 MiB block.
  write64(m, kL1, kL2 | kPteValidTable);
  write64(m, kL2, Machine::kBase | 1ull | kPteAf | kPteApEl0Rw);
  enableMmu4k(m, 1);
  CHECK(m.run(8) == StopReason::Halted);
  CHECK_EQ(m.cpu.state().x[0], 0x1234ull);
}

NT_TEST(arch_mmu_ttbr1_high_kernel_virtual_address) {
  Machine m;
  put32(m, Machine::kBase, 0xD2824680u); // MOVZ X0,#0x1234
  put32(m, Machine::kBase + 4, kHlt42);
  // Map top-of-address-space VA through TTBR1: L1[511] -> L2[511] -> L3[511].
  write64(m, kL1 + 511 * 8, kL2 | kPteValidTable);
  write64(m, kL2 + 511 * 8, kL3 | kPteValidTable);
  write64(m, kL3 + 511 * 8, Machine::kBase | 3ull | kPteAf); // privileged-only executable page
  enableMmu4k(m, 1);
  m.cpu.mutableState().pc = 0xFFFFFFFFFFFFF000ull;
  CHECK(m.run(8) == StopReason::Halted);
  CHECK_EQ(m.cpu.state().x[0], 0x1234ull);
}

NT_TEST(arch_mmu_4k_translates_fetch_load_and_store) {
  Machine m;
  // MOVZ X0,#0x1234; STR X0,[X1]; LDR X2,[X1]; HLT #0x42
  put32(m, Machine::kBase + 0x00, 0xD2824680u);
  put32(m, Machine::kBase + 0x04, 0xF9000020u);
  put32(m, Machine::kBase + 0x08, 0xF9400022u);
  put32(m, Machine::kBase + 0x0C, kHlt42);
  setupMmu4k(m);
  enableMmu4k(m, 1);
  m.cpu.mutableState().x[1] = 0x4000;
  CHECK(m.run(16) == StopReason::Halted);
  CHECK_EQ(m.cpu.state().x[2], 0x1234ull);
  CHECK_EQ(m.rd(Machine::kBase + 0x5000, 8), 0x1234ull);
}

NT_TEST(arch_mmu_fault_diagnostic_rewalks_tlb_hit_against_live_pte) {
  Machine m;
  // LDR W1,[X8]; the target's valid L3 descriptor deliberately maps to PA 0,
  // which is not backed by Machine RAM. The bus abort must capture a fresh walk.
  put32(m, Machine::kBase, 0xB9400101u);
  constexpr uint64_t l1hi = Machine::kBase + 0x4000;
  constexpr uint64_t l2hi = Machine::kBase + 0x5000;
  constexpr uint64_t l3hi = Machine::kBase + 0x6000;
  const uint64_t targetVa = 0xFFFFFF8008025000ull;
  write64(m, kL1, kL2 | kPteValidTable);
  write64(m, kL2, kL3 | kPteValidTable);
  write64(m, kL3, Machine::kBase | 3ull | kPteAf); // VA 0 maps code to RAM
  write64(m, l1hi, l2hi | kPteValidTable);
  write64(m, l2hi + 0x40 * 8, l3hi | kPteValidTable);
  constexpr uint64_t kPteMappingPaZero = 0x006800000000070Full;
  write64(m, l3hi + 0x25 * 8, kPteMappingPaZero);

  m.cpu.reset(0, Machine::kBase + Machine::kRam, 1);
  CpuState& s = m.cpu.mutableState();
  s.ttbr0_el1 = kL1;
  s.ttbr1_el1 = l1hi;
  s.tcr_el1 = kTcr4k39;
  s.sctlr_el1 |= 1ull;
  s.sp_el1 = Machine::kBase + Machine::kRam;
  s.sp = s.sp_el1;
  s.x[8] = targetVa;

  CHECK(m.cpu.run(1) == StopReason::InstructionLimit);
  const CpuState& after = m.cpu.state();
  CHECK_EQ(after.last_abort_va, targetVa);
  CHECK_EQ(after.last_abort_pa, 0ull);
  CHECK(after.last_abort_translation_cache_hit); // later bytes of the same LDR hit the page just cached by byte 0
  CHECK(after.last_abort_fresh_walk_attempted);
  CHECK(after.last_abort_fresh_walk_ok);
  CHECK_EQ(after.last_abort_fresh_pa, 0ull);
  CHECK_EQ(after.last_abort_fresh_l1_desc, l2hi | kPteValidTable);
  CHECK_EQ(after.last_abort_fresh_l2_desc, l3hi | kPteValidTable);
  CHECK_EQ(after.last_abort_fresh_l3_desc, kPteMappingPaZero);
}

NT_TEST(arch_mmu_diagnostic_identifies_guest_store_that_installed_pa_zero_pte) {
  Machine m;
  // STR X2,[X1]; LDR W1,[X8]. The first guest instruction installs the leaf
  // descriptor that sends the second access to physical address zero.
  put32(m, Machine::kBase + 0x00, 0xF9000022u);
  put32(m, Machine::kBase + 0x04, 0xB9400101u);
  constexpr uint64_t l1hi = Machine::kBase + 0x4000;
  constexpr uint64_t l2hi = Machine::kBase + 0x5000;
  constexpr uint64_t l3hi = Machine::kBase + 0x6000;
  constexpr uint64_t targetVa = 0xFFFFFF8008025000ull;
  constexpr uint64_t targetPtePa = l3hi + 0x25 * 8;
  constexpr uint64_t kPteMappingPaZero = 0x006800000000070Full;
  write64(m, kL1, kL2 | kPteValidTable);
  write64(m, kL2, kL3 | kPteValidTable);
  write64(m, kL3, Machine::kBase | 3ull | kPteAf | kPteApEl0Rw);
  // Give guest VA 0x4000 write access to the physical page containing the target PTE.
  write64(m, kL3 + 4 * 8, l3hi | 3ull | kPteAf | kPteApEl0Rw);
  write64(m, l1hi, l2hi | kPteValidTable);
  write64(m, l2hi + 0x40 * 8, l3hi | kPteValidTable);
  write64(m, targetPtePa, (Machine::kBase + 0x8000) | 3ull | kPteAf);

  m.cpu.reset(0, Machine::kBase + Machine::kRam, 1);
  CpuState& s = m.cpu.mutableState();
  s.ttbr0_el1 = kL1;
  s.ttbr1_el1 = l1hi;
  s.tcr_el1 = kTcr4k39;
  s.sctlr_el1 |= 1ull;
  s.sp_el1 = Machine::kBase + Machine::kRam;
  s.sp = s.sp_el1;
  s.x[1] = 0x4128;
  s.x[2] = kPteMappingPaZero;
  s.x[8] = targetVa;

  CHECK(m.cpu.run(1) == StopReason::InstructionLimit); // guest STR installs the PTE
  CHECK_EQ(m.cpu.run(1), StopReason::InstructionLimit); // following LDR takes guest abort
  const CpuState& after = m.cpu.state();
  CHECK_EQ(after.last_abort_va, targetVa);
  CHECK_EQ(after.last_abort_l3_entry_pa, targetPtePa);
  CHECK(after.last_abort_pte_writer_found);
  CHECK_EQ(after.last_abort_pte_writer_pc, 0ull);
  CHECK_EQ(after.last_abort_pte_writer_insn, 0xF9000022u);
  CHECK_EQ(after.last_abort_pte_writer_va, 0x4128ull);
  CHECK_EQ(after.last_abort_pte_writer_pa, targetPtePa);
  CHECK_EQ(after.last_abort_pte_writer_value, kPteMappingPaZero);
  CHECK_EQ(after.last_abort_pte_writer_size, 8u);
}

NT_TEST(arch_mmu_4k_permission_fault_vectors_to_el1) {
  Machine m;
  // Execute an EL0 load from a privileged-only page; vector contains HLT sentinel.
  put32(m, Machine::kBase + 0x00, 0xF9400022u); // LDR X2,[X1]
  put32(m, Machine::kBase + 0x04, kHlt42);
  put32(m, Machine::kBase + 0x400, kHlt42);
  setupMmu4k(m, false);
  enableMmu4k(m, 0);
  m.cpu.mutableState().x[1] = 0x4000;
  CHECK(m.run(8) == StopReason::Halted);
  const CpuState& s = m.cpu.state();
  CHECK_EQ(s.current_el, 1u);
  CHECK_EQ(s.pc, 0x400ull);
  CHECK_EQ(s.elr_el1, 0ull);
  CHECK_EQ(s.far_el1, 0x4000ull);
  CHECK_EQ(s.esr_el1 & 0x3Fu, 0x0Full); // permission fault, level 3
  CHECK_EQ((s.esr_el1 >> 26) & 0x3Fu, 0x24ull); // Data Abort from lower EL
}


NT_TEST(arch_mmu_tlbi_vmalle1_invalidates_stale_guest_translation) {
  Machine m;
  // LDR X2,[X1]; TLBI VMALLE1; LDR X3,[X1]; HLT #0x42
  put32(m, Machine::kBase + 0x00, 0xF9400022u);
  put32(m, Machine::kBase + 0x04, 0xD508871Fu);
  put32(m, Machine::kBase + 0x08, 0xF9400023u);
  put32(m, Machine::kBase + 0x0C, kHlt42);
  setupMmu4k(m);
  CHECK_ST(m.bus.write(GuestAddr(Machine::kBase + 0x5000), 8, 0x111ull), Status::Ok);
  CHECK_ST(m.bus.write(GuestAddr(Machine::kBase + 0x6000), 8, 0x222ull), Status::Ok);
  enableMmu4k(m, 1);
  m.cpu.mutableState().x[1] = 0x4000;
  CHECK(m.cpu.run(1) == StopReason::InstructionLimit);
  CHECK_EQ(m.cpu.state().x[2], 0x111ull);
  // Update the guest page table externally to model a guest PTE store; the cached
  // translation must persist until TLBI, then the next access must see the new PTE.
  write64(m, kL3 + 4 * 8, (Machine::kBase + 0x6000) | 3ull | kPteAf | kPteApEl0Rw);
  CHECK(m.cpu.run(1) == StopReason::InstructionLimit); // TLBI VMALLE1
  CHECK(m.cpu.run(2) == StopReason::Halted);
  CHECK_EQ(m.cpu.state().x[3], 0x222ull);
}


NT_TEST(arch_mmu_access_flag_fault_reports_level_and_far) {
  Machine m;
  put32(m, Machine::kBase, 0xF9400022u); // LDR X2,[X1]
  put32(m, Machine::kBase + 0x400, kHlt42);
  setupMmu4k(m);
  // Clear AF in the leaf descriptor for VA 0x4000.
  write64(m, kL3 + 4 * 8, (Machine::kBase + 0x5000) | 3ull | kPteApEl0Rw);
  enableMmu4k(m, 0);
  m.cpu.mutableState().x[1] = 0x4000;
  CHECK(m.run(8) == StopReason::Halted);
  CHECK_EQ(m.cpu.state().far_el1, 0x4000ull);
  CHECK_EQ(m.cpu.state().esr_el1 & 0x3Fu, 0x0Bull); // access-flag fault, level 3
}

NT_TEST(arch_mmu_uxn_blocks_el0_instruction_fetch_but_allows_el1_vector) {
  Machine m;
  put32(m, Machine::kBase + 0x400, kHlt42);
  setupMmu4k(m);
  // EL0 may read data from page zero but must not execute it.
  write64(m, kL3, Machine::kBase | 3ull | kPteAf | kPteApEl0Rw | (1ull << 54));
  enableMmu4k(m, 0);
  CHECK(m.run(8) == StopReason::Halted);
  CHECK_EQ(m.cpu.state().current_el, 1u);
  CHECK_EQ(m.cpu.state().far_el1, 0ull);
  CHECK_EQ((m.cpu.state().esr_el1 >> 26) & 0x3Fu, 0x20ull); // Instruction Abort from lower EL
  CHECK_EQ(m.cpu.state().esr_el1 & 0x3Fu, 0x0Full); // permission fault, level 3
}

NT_TEST(arch_mmu_4k_translation_fault_reports_far_esr_and_vectors) {
  Machine m;
  put32(m, Machine::kBase, 0xF9400022u); // LDR X2,[X1]
  put32(m, Machine::kBase + 0x400, kHlt42);
  setupMmu4k(m);
  // Remove the VA 0x4000 leaf descriptor, causing a translation fault at level 3.
  write64(m, kL3 + 4 * 8, 0);
  enableMmu4k(m, 0);
  m.cpu.mutableState().x[1] = 0x4000;
  CHECK(m.run(8) == StopReason::Halted);
  const CpuState& s = m.cpu.state();
  CHECK_EQ(s.far_el1, 0x4000ull);
  CHECK_EQ(s.esr_el1 & 0x3Fu, 0x07ull); // translation fault, level 3
  CHECK_EQ((s.esr_el1 >> 26) & 0x3Fu, 0x24ull);
}



NT_TEST(arch_mmu_misaligned_pc_takes_instruction_abort) {
  Machine m;
  put32(m, Machine::kBase + 0x200, kHlt42); // same-EL SPx synchronous vector
  setupMmu4k(m);
  enableMmu4k(m, 1);
  m.cpu.mutableState().pc = 1;
  CHECK(m.run(8) == StopReason::Halted);
  CHECK_EQ(m.cpu.state().far_el1, 1ull);
  CHECK_EQ((m.cpu.state().esr_el1 >> 26) & 0x3Fu, 0x21ull); // Instruction Abort, same EL
  CHECK_EQ(m.cpu.state().esr_el1 & 0x3Fu, 0x21ull); // alignment fault
}

NT_TEST(arch_sctlr_alignment_check_takes_data_abort) {
  Machine m;
  put32(m, Machine::kBase, 0xF9400020u); // LDR X0,[X1]
  put32(m, Machine::kBase + 4, kHlt42);
  put32(m, Machine::kBase + 0x200, kHlt42); // same-EL SPx synchronous vector
  m.cpu.reset(Machine::kBase, Machine::kBase + Machine::kRam, 1);
  CpuState& s = m.cpu.mutableState();
  s.vbar_el1 = Machine::kBase;
  s.sctlr_el1 |= (1ull << 1); // SCTLR_EL1.A
  s.x[1] = Machine::kBase + 0x1001;
  CHECK(m.run(4) == StopReason::Halted);
  CHECK_EQ(m.cpu.state().far_el1, Machine::kBase + 0x1001);
  CHECK_EQ(m.cpu.state().esr_el1 & 0x3Fu, 0x21ull); // alignment fault
  CHECK_EQ((m.cpu.state().esr_el1 >> 26) & 0x3Fu, 0x25ull); // Data Abort, same EL
}

NT_TEST(arch_current_el_mrs_reports_el0_and_el1) {
  Machine m;
  const uint8_t code[] = {0x40, 0x42, 0x38, 0xD5, 0x40, 0x08, 0x40, 0xD4}; // MRS x0,CurrentEL; HLT #0x42
  CHECK_ST(m.bus.loadBytes(GuestAddr(Machine::kBase), code, sizeof(code)), Status::Ok);
  m.cpu.reset(Machine::kBase, Machine::kBase + Machine::kRam, 0);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.x(0), 0ull);

  Machine m1;
  CHECK_ST(m1.bus.loadBytes(GuestAddr(Machine::kBase), code, sizeof(code)), Status::Ok);
  m1.cpu.reset(Machine::kBase, Machine::kBase + Machine::kRam, 1);
  CHECK(m1.run() == StopReason::Halted);
  CHECK_EQ(m1.x(0), 4ull);
  CHECK_EQ(m1.cpu.state().current_el, 1u);
}

NT_TEST(arch_svc_enters_el1_and_eret_returns_to_el0) {
  Machine m;
  const uint8_t code[] = {0x41, 0x08, 0x00, 0xD4, 0x40, 0x08, 0x40, 0xD4}; // SVC #0x42; HLT #0x42
  CHECK_ST(m.bus.loadBytes(GuestAddr(Machine::kBase), code, sizeof(code)), Status::Ok);
  put32(m, Machine::kBase + 0x400, kEret);
  m.cpu.reset(Machine::kBase, Machine::kBase + Machine::kRam, 0);
  // reset initializes the EL0 stack bank; configure the EL1 stack and vector afterwards.
  m.cpu.mutableState().vbar_el1 = Machine::kBase;
  m.cpu.mutableState().sp_el1 = Machine::kBase + 0x8000;
  m.cpu.mutableState().n = true;
  m.cpu.mutableState().c = true;
  m.cpu.mutableState().daif = 0x5;

  CHECK(m.run(3) == StopReason::Halted);
  const CpuState& s = m.cpu.state();
  CHECK_EQ(s.current_el, 0u);
  CHECK_EQ(s.pc, Machine::kBase + 4);
  CHECK_EQ(s.elr_el1, Machine::kBase + 4);
  CHECK_EQ(s.spsr_el1 & 0x1Fu, 0ull);   // saved EL0t mode
  CHECK_EQ(s.esr_el1, 0x56000042ull);   // EC=SVC64, IL=1, ISS=imm16
  CHECK_EQ(s.sp, Machine::kBase + Machine::kRam);
  CHECK_EQ(s.sp_el1, Machine::kBase + 0x8000);
  CHECK(s.n && !s.z && s.c && !s.v);  // NZCV saved/restored through SPSR_EL1
  CHECK_EQ(s.daif, 0x5u);              // DAIF saved/restored through SPSR_EL1
}

NT_TEST(arch_el0_privileged_sysreg_access_takes_undefined_exception) {
  Machine m;
  const uint8_t code[] = {0x00, 0x10, 0x38, 0xD5}; // MRS x0,SCTLR_EL1
  CHECK_ST(m.bus.loadBytes(GuestAddr(Machine::kBase), code, sizeof(code)), Status::Ok);
  put32(m, Machine::kBase + 0x400, kHlt42);
  m.cpu.reset(Machine::kBase, Machine::kBase + Machine::kRam, 0);
  m.cpu.mutableState().vbar_el1 = Machine::kBase;
  m.cpu.mutableState().sp_el1 = Machine::kBase + 0x8000;
  CHECK(m.run(2) == StopReason::Halted);
  const CpuState& s = m.cpu.state();
  CHECK_EQ(s.current_el, 1u);
  CHECK_EQ(s.pc, Machine::kBase + 0x400);
  CHECK_EQ(s.elr_el1, Machine::kBase); // undefined instruction retries at faulting instruction
  CHECK_EQ(s.esr_el1, (1ull << 25));  // EC=0 (unknown/undefined), IL=1
  CHECK(s.spsel);
}

NT_TEST(arch_spsel_switches_banked_stack_pointer) {
  Machine m;
  const uint32_t code[] = {kMsrSpsel0, kMsrSpsel1, kHlt42};
  for (unsigned i = 0; i < 3; ++i) put32(m, Machine::kBase + i * 4, code[i]);
  m.cpu.reset(Machine::kBase, Machine::kBase + Machine::kRam, 1);
  m.cpu.mutableState().sp_el0 = Machine::kBase + 0x7000;
  m.cpu.mutableState().sp_el1 = Machine::kBase + 0x8000;
  m.cpu.mutableState().sp = Machine::kBase + 0x8000;
  CHECK(m.run() == StopReason::Halted);
  CHECK(m.cpu.state().spsel);
  CHECK_EQ(m.cpu.state().sp, Machine::kBase + 0x8000);
  CHECK_EQ(m.cpu.state().sp_el0, Machine::kBase + 0x7000);
}


NT_TEST(arch_svc_from_el1_uses_current_el_spx_vector) {
  Machine m;
  put32(m, Machine::kBase, kSvc42);
  put32(m, Machine::kBase + 0x200, kHlt42);
  m.cpu.reset(Machine::kBase, Machine::kBase + Machine::kRam, 1);
  m.cpu.mutableState().vbar_el1 = Machine::kBase;
  CHECK(m.run(2) == StopReason::Halted);
  CHECK_EQ(m.cpu.state().pc, Machine::kBase + 0x200);
  CHECK_EQ(m.cpu.state().current_el, 1u);
  CHECK_EQ(m.cpu.state().elr_el1, Machine::kBase + 4);
  CHECK_EQ(m.cpu.state().esr_el1, 0x56000042ull);
}


NT_TEST(arch_vm_can_configure_initial_el1) {
  BoardConfig cfg;
  cfg.initialEL = 1;
  Status st = Status::Ok;
  auto vm = Vm::create(cfg, &st);
  CHECK(vm != nullptr);
  CHECK_ST(st, Status::Ok);
  const uint8_t code[] = {0x40, 0x42, 0x38, 0xD5, 0x40, 0x08, 0x40, 0xD4}; // MRS x0,CurrentEL; HLT #0x42
  CHECK_ST(vm->loadPayload(GuestAddr(cfg.ramBase), code, sizeof(code)), Status::Ok);
  CHECK(vm->runSync(8) == StopReason::Halted);
  CpuState state;
  CHECK_ST(vm->cpuState(&state), Status::Ok);
  CHECK_EQ(state.current_el, 1u);
  CHECK_EQ(state.x[0], 4ull);

  BoardConfig invalid;
  invalid.initialEL = 2;
  auto rejected = Vm::create(invalid, &st);
  CHECK(rejected == nullptr);
  CHECK_ST(st, Status::InvalidArgument);
}
