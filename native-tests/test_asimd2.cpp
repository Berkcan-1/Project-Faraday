#include "generated/test_asimd2.h"
#include "test_util.h"

using namespace ncv;

namespace {
std::string h128(uint64_t lo, uint64_t hi) { return "0x" + nt::hex(hi) + "_" + nt::hex(lo); }
}  // namespace

// 2c-2b / 2c-3 (HAFİF set): FP vektörleri, doyurucu, uzun/geniş/dar, by-element, skaler-SIMD.
NT_TEST(asimd2_vectors_against_oracle) {
  Machine m;
  size_t bad = 0;
  for (size_t i = 0; i < ncvasimd2::CASE_COUNT; i++) {
    const ncvasimd2::Case& c = ncvasimd2::CASES[i];
    const ncvasimd2::Prog& p = ncvasimd2::PROGS[c.prog];
    m.load(p.data, p.len);
    CpuState& st = m.cpu.mutableState();
    st.n = false; st.z = true; st.c = false; st.v = true;
    st.fpcr = c.fpcr; st.fpsr = 0;
    for (int k = 0; k < 6; k++) { st.vr[k + 1].lo = c.v[k][0]; st.vr[k + 1].hi = c.v[k][1]; }
    st.vr[0].lo = c.v0[0]; st.vr[0].hi = c.v0[1];
    const StopReason r = m.run();
    const CpuState& o = m.cpu.state();
    bool ok = r == StopReason::Halted && !o.n && o.z && !o.c && o.v && o.fpsr == c.fpsr && o.vr[0].lo == c.out[0] && o.vr[0].hi == c.out[1];
    for (int k = 0; k < 6; k++) ok = ok && o.vr[k + 1].lo == c.v[k][0] && o.vr[k + 1].hi == c.v[k][1];
    if (!ok && bad++ < 12)
      nt::fail(__FILE__, __LINE__, std::string(p.name) + " | fpcr=0x" + nt::hex(c.fpcr) + " v1=" + h128(c.v[0][0], c.v[0][1]) + " v2=" + h128(c.v[1][0], c.v[1][1]) +
               " v0in=" + h128(c.v0[0], c.v0[1]) + " | gercek=" + h128(o.vr[0].lo, o.vr[0].hi) + " fpsr=0x" + nt::hex(o.fpsr) +
               " | beklenen=" + h128(c.out[0], c.out[1]) + " fpsr=0x" + nt::hex(c.fpsr) + " stop=" + stopReasonName(r));
  }
  CHECK_EQ(bad, 0ull);
  CHECK(ncvasimd2::CASE_COUNT > 1500);
}
