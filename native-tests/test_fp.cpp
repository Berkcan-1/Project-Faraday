#include "generated/test_fpvectors.h"
#include "generated/test_programs.h"
#include "test_util.h"

using namespace ncv;
using namespace ncvgen;

namespace {
void poisonV(Machine& m) {
  for (int k = 0; k < 32; k++) { m.cpu.mutableState().vr[k].lo = ncvfp::POISON_LO; m.cpu.mutableState().vr[k].hi = ncvfp::POISON_HI; }
}
void setFlags(Machine& m, unsigned nzcv) {
  CpuState& s = m.cpu.mutableState();
  s.n = (nzcv >> 3) & 1; s.z = (nzcv >> 2) & 1; s.c = (nzcv >> 1) & 1; s.v = nzcv & 1;
}
unsigned getFlags(const CpuState& s) { return (s.n << 3) | (s.z << 2) | (s.c << 1) | s.v; }
}  // namespace

// Tüm skaler FP: assembler kodlaması + Fraction tabanlı ARM FP oracle (tools/fpo.py).
NT_TEST(fp_scalar_vectors_against_oracle) {
  Machine m;
  size_t bad = 0;
  for (size_t i = 0; i < ncvfp::CASE_COUNT; i++) {
    const ncvfp::Case& c = ncvfp::CASES[i];
    const ncvfp::Prog& p = ncvfp::PROGS[c.prog];
    m.load(p.data, p.len);
    poisonV(m);
    CpuState& st = m.cpu.mutableState();
    st.fpcr = c.fpcr;
    st.fpsr = 0;
    setFlags(m, c.nzcvIn);
    for (int k = 0; k < 3; k++) { m.setX(k + 1, c.x[k]); st.vr[k + 1].lo = c.v[k]; }
    const StopReason r = m.run();
    const CpuState& o = m.cpu.state();
    bool ok = r == StopReason::Halted && getFlags(o) == c.nzcvOut && o.fpsr == c.fpsr && o.fpcr == c.fpcr;
    switch (c.kind) {
      case 0: ok = ok && o.vr[0].lo == c.out && o.vr[0].hi == 0; break;
      case 1: ok = ok && o.x[0] == c.out; break;
      case 3: ok = ok && o.vr[0].lo == ncvfp::POISON_LO && o.vr[0].hi == c.out; break;
      default: break;
    }
    for (int k = 0; k < 3; k++) ok = ok && o.vr[k + 1].lo == c.v[k] && o.x[k + 1] == c.x[k];   // kaynaklar bozulmamalı
    if (!ok && bad++ < 8)
      nt::fail(__FILE__, __LINE__, std::string(p.name) + " | fpcr=0x" + nt::hex(c.fpcr) + " v1=0x" + nt::hex(c.v[0]) + " v2=0x" +
               nt::hex(c.v[1]) + " v3=0x" + nt::hex(c.v[2]) + " x1=0x" + nt::hex(c.x[0]) + " | gercek v0=0x" + nt::hex(o.vr[0].lo) +
               " x0=0x" + nt::hex(o.x[0]) + " nzcv=" + std::to_string(getFlags(o)) + " fpsr=0x" + nt::hex(o.fpsr) +
               " | beklenen out=0x" + nt::hex(c.out) + " nzcv=" + std::to_string(c.nzcvOut) + " fpsr=0x" + nt::hex(c.fpsr) +
               " stop=" + stopReasonName(r));
  }
  CHECK_EQ(bad, 0ull);
  CHECK(ncvfp::CASE_COUNT > 10000);
}

NT_TEST(fp_simd_load_store_modes) {
  Machine m; m.load(prog_fp_ldst, prog_fp_ldst_len);
  const uint64_t B = Machine::kBase + 0x1000, X1 = 0x4444333322221111ull;
  poisonV(m);
  m.setX(10, B);
  CHECK(m.run() == StopReason::Halted);
  const CpuState& s = m.cpu.state();
  CHECK_EQ(s.vr[1].lo, X1); CHECK_EQ(s.vr[1].hi, X1);                    // ldr q
  CHECK_EQ(s.vr[2].lo, X1); CHECK_EQ(s.vr[2].hi, 0ull);                  // ldr d: üst yarı sıfırlanır
  CHECK_EQ(s.vr[3].lo, 0x22221111ull); CHECK_EQ(s.vr[3].hi, 0ull);       // ldr s
  CHECK_EQ(s.vr[4].lo, 0x1111ull); CHECK_EQ(s.vr[4].hi, 0ull);           // ldr h
  CHECK_EQ(s.vr[5].lo, 0x11ull); CHECK_EQ(s.vr[5].hi, 0ull);             // ldr b
  CHECK_EQ(s.vr[6].lo, X1); CHECK_EQ(s.vr[6].hi, X1);                    // ldp q (signed offset)
  CHECK_EQ(s.vr[7].lo, X1); CHECK_EQ(s.vr[7].hi, X1);
  CHECK_EQ(s.vr[8].lo, X1); CHECK_EQ(s.vr[9].lo, X1);                    // stp d pre-index / ldp d post-index
  CHECK_EQ(s.vr[8].hi, 0ull); CHECK_EQ(s.vr[9].hi, 0ull);
  CHECK_EQ(m.x(10), B + 112);
  CHECK_EQ(s.vr[10].lo, X1); CHECK_EQ(s.vr[10].hi, X1);                  // ldr q, negatif ofset
  CHECK_EQ(s.vr[11].lo, X1);                                             // ldur d
  CHECK_EQ(s.vr[13].lo, 0x22221111ull); CHECK_EQ(s.vr[13].hi, 0ull);    // register offset lsl #2
  CHECK_EQ(s.vr[14].lo, 0x1122334455667788ull); CHECK_EQ(s.vr[14].hi, 0x99AABBCCDDEEFF00ull);   // ldr q literal
  CHECK_EQ(s.vr[15].lo, 0x22221111ull); CHECK_EQ(s.vr[16].lo, 0x22221111ull);                   // ldnp/stnp s
  CHECK_EQ(m.rd(B, 8), X1); CHECK_EQ(m.rd(B + 8, 8), X1);                // q yazma: little-endian lo, sonra hi
  CHECK_EQ(m.rd(B + 30, 1), 0x11ull);
}

NT_TEST(fp_system_registers_el0_view) {
  Machine m; m.load(prog_sys_fp, prog_sys_fp_len);
  CpuState& st = m.cpu.mutableState();
  st.fpcr = 0x00400000;
  setFlags(m, 5);                          // 0101: Z ve V
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.x(1), 0x00400000ull);
  CHECK_EQ(m.x(3), 0x03800000ull);         // yazılabilir bitler: RMode, FZ, DN
  CHECK_EQ(m.x(5), 0x03C00000ull);         // AHP/tuzak bitleri desteklenmez -> okunamaz/yazılamaz
  CHECK_EQ(m.x(6), 0x0800009Full);         // FPSR yazılabilir maskesi
  CHECK_EQ(m.x(7), 0x50000000ull);
  CHECK_EQ(m.x(9), 0xA0000000ull);
  const CpuState& s = m.cpu.state();
  CHECK(s.n && !s.z && s.c && !s.v);
  CHECK_EQ(m.x(11), 0x1234ull); CHECK_EQ(m.x(12), 0ull);
  // TPIDRRO_EL0 write from EL0 is now a guest exception (covered in test_arch.cpp).
}
