#include "generated/test_asimd.h"
#include "test_util.h"

using namespace ncv;

namespace {
std::string hex128(uint64_t lo, uint64_t hi) { return "0x" + nt::hex(hi) + "_" + nt::hex(lo); }
}  // namespace

// ASIMD tamsayı vektör komutları: assembler kodlaması + lane tabanlı Python oracle (tools/gen_asimd.py).
NT_TEST(asimd_register_vectors_against_oracle) {
  Machine m;
  size_t bad = 0;
  for (size_t i = 0; i < ncvasimd::CASE_COUNT; i++) {
    const ncvasimd::Case& c = ncvasimd::CASES[i];
    const ncvasimd::Prog& p = ncvasimd::PROGS[c.prog];
    m.load(p.data, p.len);
    CpuState& st = m.cpu.mutableState();
    st.n = false; st.z = true; st.c = false; st.v = true;          // NZCV=0101: değişmemeli
    m.setX(1, c.x1);
    for (int k = 0; k < 6; k++) { st.vr[k + 1].lo = c.v[k][0]; st.vr[k + 1].hi = c.v[k][1]; }
    st.vr[0].lo = c.v0[0]; st.vr[0].hi = c.v0[1];
    const StopReason r = m.run();
    const CpuState& o = m.cpu.state();
    bool ok = r == StopReason::Halted && !o.n && o.z && !o.c && o.v && o.fpsr == 0 && o.x[1] == c.x1;
    if (c.kind == 0) ok = ok && o.vr[0].lo == c.out[0] && o.vr[0].hi == c.out[1];
    else ok = ok && o.x[0] == c.out[0];
    for (int k = 0; k < 6; k++) ok = ok && o.vr[k + 1].lo == c.v[k][0] && o.vr[k + 1].hi == c.v[k][1];   // kaynaklar bozulmamalı
    if (!ok && bad++ < 8)
      nt::fail(__FILE__, __LINE__, std::string(p.name) + " | v1=" + hex128(c.v[0][0], c.v[0][1]) + " v2=" + hex128(c.v[1][0], c.v[1][1]) +
               " v0in=" + hex128(c.v0[0], c.v0[1]) + " x1=0x" + nt::hex(c.x1) + " | gercek=" + hex128(o.vr[0].lo, o.vr[0].hi) + " x0=0x" +
               nt::hex(o.x[0]) + " | beklenen=" + hex128(c.out[0], c.out[1]) + " stop=" + stopReasonName(r));
  }
  CHECK_EQ(bad, 0ull);
  CHECK(ncvasimd::CASE_COUNT > 5000);
}

// LD1..LD4 / ST1..ST4 (çoklu, tek-lane, replicate; post-index immediate ve register).
NT_TEST(asimd_structure_load_store_against_oracle) {
  Machine m;
  const uint64_t B = Machine::kBase + 0x1000;
  size_t bad = 0;
  for (size_t i = 0; i < ncvasimd::MCASE_COUNT; i++) {
    const ncvasimd::MCase& c = ncvasimd::MCASES[i];
    const ncvasimd::Prog& p = ncvasimd::PROGS[c.prog];
    m.load(p.data, p.len);
    CHECK_ST(m.bus.loadBytes(GuestAddr(B), c.mem, 64), Status::Ok);
    CpuState& st = m.cpu.mutableState();
    m.setX(10, B);
    m.setX(11, c.x11);
    for (int k = 0; k < 4; k++) { st.vr[k].lo = c.vin[k][0]; st.vr[k].hi = c.vin[k][1]; }
    const StopReason r = m.run();
    const CpuState& o = m.cpu.state();
    bool ok = r == StopReason::Halted && o.x[10] == B + c.delta;
    for (int k = 0; k < 4; k++) ok = ok && o.vr[k].lo == c.vout[k][0] && o.vr[k].hi == c.vout[k][1];
    uint8_t got[64];
    CHECK_ST(m.bus.readBytes(GuestAddr(B), got, 64), Status::Ok);
    for (int k = 0; k < 64; k++) ok = ok && got[k] == c.memout[k];
    if (!ok && bad++ < 8) {
      std::string d;
      for (int k = 0; k < 4; k++) d += " v" + std::to_string(k) + "=" + hex128(o.vr[k].lo, o.vr[k].hi) + "/" + hex128(c.vout[k][0], c.vout[k][1]);
      int firstMem = -1;
      for (int k = 0; k < 64; k++) if (got[k] != c.memout[k]) { firstMem = k; break; }
      nt::fail(__FILE__, __LINE__, std::string(p.name) + " | stop=" + stopReasonName(r) + " x10delta=" + std::to_string(o.x[10] - B) +
               "/" + std::to_string(c.delta) + " ilk-bellek-farki@" + std::to_string(firstMem) + d);
    }
  }
  CHECK_EQ(bad, 0ull);
  CHECK(ncvasimd::MCASE_COUNT > 1000);
}
