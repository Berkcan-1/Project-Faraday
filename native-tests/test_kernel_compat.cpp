#include "test_util.h"
#include <cstdint>
#include <vector>

using namespace ncv;

namespace {
void loadWords(Machine& m, const std::vector<uint32_t>& words, uint8_t el=1) {
  std::vector<uint8_t> bytes(words.size()*4);
  for(size_t i=0;i<words.size();++i) {
    bytes[i*4]=static_cast<uint8_t>(words[i]); bytes[i*4+1]=static_cast<uint8_t>(words[i]>>8);
    bytes[i*4+2]=static_cast<uint8_t>(words[i]>>16); bytes[i*4+3]=static_cast<uint8_t>(words[i]>>24);
  }
  CHECK_ST(m.bus.loadBytes(GuestAddr(Machine::kBase),bytes.data(),bytes.size()),Status::Ok);
  m.cpu.reset(Machine::kBase,Machine::kBase+Machine::kRam,el);
}
}

NT_TEST(kernel_common_isb_daif_and_cpacr_no_longer_stop) {
  Machine m;
  const std::vector<uint32_t> words={
    0xD5033FDFu, // ISB
    0xD50342DFu, // MSR DAIFSet, #2
    0xD50342FFu, // MSR DAIFClr, #2
    0xD53B4220u, // MRS X0, DAIF
    0xD5181041u, // MSR CPACR_EL1, X1
    0xD5381042u, // MRS X2, CPACR_EL1
    0xD4400000u  // HLT #0
  };
  loadWords(m,words);
  m.setX(1,0x00300000ull);
  CHECK(m.run(64)==StopReason::Halted);
  CHECK_EQ(m.x(0),0x340ull);
  CHECK_EQ(m.cpu.state().daif,0xDu);
  CHECK_EQ(m.cpu.state().cpacr_el1,0x00300000ull);
  CHECK_EQ(m.x(2),0x00300000ull);
}

NT_TEST(psci_hvc_version_and_single_cpu_affinity_are_reported) {
  Machine m;
  const uint32_t code[2]={0xD4000002u,0xD4400000u};
  CHECK_ST(m.bus.loadBytes(GuestAddr(Machine::kBase),reinterpret_cast<const uint8_t*>(code),sizeof(code)),Status::Ok);
  m.cpu.reset(Machine::kBase,Machine::kBase+Machine::kRam,1);
  m.setX(0,0x84000000ull);
  CHECK(m.run(8)==StopReason::Halted);
  CHECK_EQ(m.x(0),0x00010001ull);

  Machine m2;
  const uint32_t code2[2]={0xD4000002u,0xD4400000u};
  CHECK_ST(m2.bus.loadBytes(GuestAddr(Machine::kBase),reinterpret_cast<const uint8_t*>(code2),sizeof(code2)),Status::Ok);
  m2.cpu.reset(Machine::kBase,Machine::kBase+Machine::kRam,1);
  m2.setX(0,0x84000004ull); m2.setX(1,0);
  CHECK(m2.run(8)==StopReason::Halted);
  CHECK_EQ(m2.x(0),0ull);
}

NT_TEST(kernel_mdscr_el1_is_nonfatal_when_debug_architecture_is_disabled) {
  // Observed first blocker in the real boot log: 0xD5100240 =
  // MSR S2_0_C0_C2_2, X0 (MDSCR_EL1). Debug architecture is intentionally
  // not exposed by this single-vCPU board, so reads are RAZ and writes WI.
  const std::vector<uint32_t> words={
    0xD5100240u, // MSR MDSCR_EL1, X0
    0xD5300241u, // MRS X1, MDSCR_EL1
    0xD4400000u  // HLT #0
  };
  Machine m;
  loadWords(m, words);
  m.setX(0, 0xDEADBEEF12345678ull);
  CHECK(m.run(16) == StopReason::Halted);
  CHECK_EQ(m.x(1), 0ull);
  CHECK_EQ(m.cpu.state().pc, Machine::kBase + 8);
}


NT_TEST(kernel_id_aa64isar3_el1_mrs_is_supported) {
  // Newer Linux kernels read ID_AA64ISAR3_EL1 (S3_0_C0_C6_3 = 0xD5380661).
  // The generic NCV vCPU reports no optional features in this register.
  const std::vector<uint32_t> words = {
    0xD5380661u, // MRS X1, ID_AA64ISAR3_EL1
    0xD4400000u  // HLT #0
  };
  Machine m;
  loadWords(m, words, 1);
  m.setX(1, 0xDEADBEEFDEADBEEFull);
  CHECK(m.run(8) == StopReason::Halted);
  CHECK_EQ(m.x(1), 0ull);
  CHECK_EQ(m.cpu.state().pc, Machine::kBase + 4);
}

NT_TEST(kernel_revidr_el1_mrs_is_supported) {
  // Exact second blocker observed in the Android boot log:
  // 0xD53800C8 = MRS X8, REVIDR_EL1 (S3_0_C0_C0_6).
  // The generic virtual CPU has no silicon revision identity, so it returns 0.
  const std::vector<uint32_t> words = {
    0xD53800C8u, // MRS X8, REVIDR_EL1
    0xD4400000u  // HLT #0
  };
  Machine m;
  loadWords(m, words, 1);
  m.setX(8, 0xDEADBEEFDEADBEEFull);
  CHECK(m.run(8) == StopReason::Halted);
  CHECK_EQ(m.x(8), 0ull);
  // HLT stops while PC still points at the HLT instruction (second word).
  CHECK_EQ(m.cpu.state().pc, Machine::kBase + 4);
}
