#include "generated/test_programs.h"
#include "generated/test_vectors.h"
#include "test_util.h"

using namespace ncv;
using namespace ncvgen;

#define LOAD(m, name) (m).load(prog_##name, prog_##name##_len)

NT_TEST(cpu_movewide) {
  Machine m; LOAD(m, movewide);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.x(0), 0xDEF09ABC56781234ull);
  CHECK_EQ(m.x(1), ~0ull);
  CHECK_EQ(m.x(2), 1ull << 48);
  CHECK_EQ(m.x(3), 0x7FFFFFFFFFFFFFFFull);
  CHECK_EQ(m.cpu.instructionsRetired(), 7ull);        // HLT sayılmaz
  CHECK_EQ(m.cpu.state().pc, Machine::kBase + 28);    // PC HLT'de kalır
}

NT_TEST(cpu_addsub_imm_and_flags) {
  Machine m; LOAD(m, addsub_imm);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.x(0), 10ull); CHECK_EQ(m.x(1), 15ull); CHECK_EQ(m.x(2), 7ull);
  CHECK_EQ(m.x(3), 4106ull); CHECK_EQ(m.x(4), 0ull);
  const CpuState& s = m.cpu.state();
  CHECK(s.z && s.c && !s.n && !s.v);                   // 10-10: sıfır, borç yok
}

NT_TEST(cpu_flags_carry) {
  Machine m; LOAD(m, flags_carry_add);
  CHECK(m.run() == StopReason::Halted);
  const CpuState& s = m.cpu.state();
  CHECK_EQ(m.x(1), 0ull);
  CHECK(s.z && s.c && !s.n && !s.v);
}

NT_TEST(cpu_flags_signed_overflow) {
  Machine m; LOAD(m, flags_overflow_add);
  CHECK(m.run() == StopReason::Halted);
  const CpuState& s = m.cpu.state();
  CHECK_EQ(m.x(1), 0x8000000000000000ull);
  CHECK(s.n && !s.z && !s.c && s.v);
}

NT_TEST(cpu_flags_borrow) {
  Machine m; LOAD(m, flags_borrow);
  CHECK(m.run() == StopReason::Halted);
  const CpuState& s = m.cpu.state();
  CHECK_EQ(m.x(1), 0xFFFFFFFFFFFFFFFEull);
  CHECK(s.n && !s.z && !s.c && !s.v);
}

NT_TEST(cpu_sp_vs_xzr_encoding) {
  Machine m; LOAD(m, sp_ops);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.cpu.state().sp, Machine::kBase + Machine::kRam - 16);  // ADD/SUB imm: Rd=31 -> SP
  CHECK_EQ(m.x(5), Machine::kBase + Machine::kRam - 16);            // Rn=31 -> SP

  Machine m2; LOAD(m2, cmp_sp_xzr);
  CHECK(m2.run() == StopReason::Halted);
  const CpuState& s = m2.cpu.state();
  CHECK_EQ(s.sp, Machine::kBase + Machine::kRam);                   // SUBS Rd=31 -> XZR, SP değişmez
  CHECK(!s.n && !s.z && s.c && !s.v);
}

NT_TEST(cpu_xzr_reads_zero_writes_discarded) {
  Machine m; LOAD(m, xzr_write);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.x(0), 0ull);
  CHECK_EQ(m.x(2), 9ull);
}

NT_TEST(cpu_logic_and_shifted_operands) {
  Machine m; LOAD(m, logic_shift);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.x(2), 0x30ull); CHECK_EQ(m.x(3), 0xFCull); CHECK_EQ(m.x(4), 0xCCull);
  CHECK_EQ(m.x(5), 0x4B0ull); CHECK_EQ(m.x(6), 0xC0ull); CHECK_EQ(m.x(7), 0x3Cull);
  CHECK_EQ(m.x(8), 0x30ull); CHECK_EQ(m.x(10), 0xEFull); CHECK_EQ(m.x(11), 0xF1ull);
  CHECK_EQ(m.x(12), 0xC000000000000003ull);
  const CpuState& s = m.cpu.state();
  CHECK(!s.n && !s.z && !s.c && !s.v);                              // ANDS: C=V=0
}

NT_TEST(cpu_loop_and_conditional_branch) {
  Machine m; LOAD(m, branches_loop);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.x(1), 15ull); CHECK_EQ(m.x(0), 0ull);
}

NT_TEST(cpu_call_ret_and_indirect) {
  Machine m; LOAD(m, call_ret);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.x(0), 7ull);
  CHECK_EQ(m.x(30), Machine::kBase + 8);
  CHECK_EQ(m.cpu.state().pc, Machine::kBase + 8);

  Machine m2; LOAD(m2, indirect);
  CHECK(m2.run() == StopReason::Halted);
  CHECK_EQ(m2.x(2), Machine::kBase + 12);
  CHECK_EQ(m2.x(3), 0x55ull);
  CHECK_EQ(m2.x(30), Machine::kBase + 8);
}

NT_TEST(cpu_cbz_cbnz) {
  Machine m; LOAD(m, cbz_cbnz);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.x(1), 0ull); CHECK_EQ(m.x(2), 2ull); CHECK_EQ(m.x(3), 3ull);
}

NT_TEST(cpu_loads_and_stores) {
  Machine m; LOAD(m, loadstore);
  const uint64_t data = Machine::kBase + 0x1000;
  m.setX(1, data);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.x(0), 0xDEADBEEFull);
  CHECK_EQ(m.x(2), 0xDEADBEEFull);
  CHECK_EQ(m.x(3), 0xEFull);                 // little-endian: düşük bayt önce
  CHECK_EQ(m.x(4), 0xBEull);
  CHECK_EQ(m.x(5), 0ull);                    // yüksek yarı sıfır
  CHECK_EQ(m.x(6), 0xDEADBEEFull);
  CHECK_EQ(m.x(7), 0xDEADBEEFull);
  CHECK_EQ(m.x(8), 0xBEEFull);
  CHECK_EQ(m.x(9), 0xDEADBEEFull);
  CHECK_EQ(m.rd(data + 8, 1), 0xBEull);      // STRB yalnızca 1 bayt yazar
  CHECK_EQ(m.rd(data + 9, 1), 0ull);
  CHECK_EQ(m.rd(data + 32, 2), 0xBEEFull);
  CHECK_EQ(m.rd(data + 34, 2), 0ull);
  CHECK_EQ(m.rd(data + 0x40, 8), 0xDEADBEEFull);
}

NT_TEST(cpu_bus_faults_leave_state_intact) {
  Machine m; LOAD(m, fault_load);
  CHECK(m.run() == StopReason::BusFault);
  const StopInfo& s = m.cpu.lastStop();
  CHECK_ST(s.busStatus, Status::Unmapped);
  CHECK_EQ(s.faultAddr, 0x1000ull);
  CHECK_EQ(s.pc, Machine::kBase + 8);
  CHECK_EQ(m.x(0), 7ull);                    // başarısız yükleme hedef register'ı bozmaz
  CHECK_EQ(m.cpu.state().pc, Machine::kBase + 8);
  CHECK_EQ(m.cpu.instructionsRetired(), 2ull);

  Machine m2; LOAD(m2, fault_store);
  CHECK(m2.run() == StopReason::BusFault);
  CHECK_ST(m2.cpu.lastStop().busStatus, Status::Unmapped);

  Machine m3; LOAD(m3, fault_mmio_unimpl);
  CHECK(m3.run() == StopReason::BusFault);
  CHECK_ST(m3.cpu.lastStop().busStatus, Status::DeviceError);
  CHECK_EQ(m3.cpu.lastStop().faultAddr, Machine::kUart + 0x100);
}

NT_TEST(cpu_pc_faults) {
  Machine m; LOAD(m, br_x0);
  m.setX(0, Machine::kBase + 6);
  CHECK(m.run() == StopReason::MisalignedPc);
  CHECK_EQ(m.cpu.lastStop().pc, Machine::kBase + 6);

  Machine m2; LOAD(m2, br_x0);
  m2.setX(0, 0x1000);
  CHECK(m2.run() == StopReason::FetchFault);
  CHECK_ST(m2.cpu.lastStop().busStatus, Status::Unmapped);
  CHECK_EQ(m2.cpu.lastStop().pc, 0x1000ull);
}

NT_TEST(cpu_hlt_brk_immediates) {
  Machine m; LOAD(m, hlt_imm);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.cpu.lastStop().imm, 0x42u);
  CHECK_EQ(m.cpu.state().pc, Machine::kBase);
  Machine m2; LOAD(m2, brk_imm);
  CHECK(m2.run() == StopReason::Breakpoint);
  CHECK_EQ(m2.cpu.lastStop().imm, 0x123u);
}

NT_TEST(cpu_unsupported_instructions_stop_cleanly) {
  struct P { const char* n; const uint8_t* d; size_t l; };
#define U(x) {#x, prog_unsup_##x, prog_unsup_##x##_len}
  const P ps[] = {U(udf), U(ldadd), U(cas), U(stxr_unpred)};
#undef U
  for (const P& p : ps) {
    Machine m; m.load(p.d, p.l);
    const StopReason r = m.run();
    if (r != StopReason::UnsupportedInstruction) nt::fail(__FILE__, __LINE__, std::string(p.n) + ": beklenen UnsupportedInstruction");
    const uint32_t word = p.d[0] | (p.d[1] << 8) | (p.d[2] << 16) | ((uint32_t)p.d[3] << 24);
    CHECK_EQ(m.cpu.lastStop().insn, word);
    CHECK_EQ(m.cpu.lastStop().pc, Machine::kBase);
    CHECK_EQ(m.cpu.instructionsRetired(), 0ull);
  }
  Machine z;                                   // sıfırlanmış bellek = UDF #0
  z.cpu.reset(Machine::kBase, Machine::kBase + Machine::kRam);
  CHECK(z.run() == StopReason::UnsupportedInstruction);
}

NT_TEST(cpu_load_store_addressing_modes) {
  Machine m; LOAD(m, ls_modes);
  const uint64_t B = Machine::kBase + 0x1000, S = Machine::kBase + Machine::kRam;
  const uint64_t X1 = 0x4444333322221111ull, X2 = ~0ull;
  m.setX(10, B);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.x(3), X1); CHECK_EQ(m.x(4), X2);                 // ldp (signed offset)
  CHECK_EQ(m.x(5), X2); CHECK_EQ(m.x(6), X1);                 // stp pre-index sonrası ldp
  CHECK_EQ(m.x(7), X2); CHECK_EQ(m.x(8), X1);                 // ldp post-index
  CHECK_EQ(m.x(11), 0x22221111ull); CHECK_EQ(m.x(12), 0xFFFFFFFFull);   // W çifti sıfır genişletir
  CHECK_EQ(m.x(13), 0x22221111ull); CHECK_EQ(m.x(14), ~0ull);          // ldpsw işaret genişletir
  CHECK_EQ(m.x(15), X1);                                      // stur/ldur negatif ofset
  CHECK_EQ(m.x(16), X2);                                      // pre-index str + post-index ldr
  CHECK_EQ(m.x(10), B + 32);                                  // iki writeback sonrası taban
  CHECK_EQ(m.x(18), 0x11ull);                                 // register offset (X)
  CHECK_EQ(m.x(20), 0x22221111ull);                           // register offset, lsl #2
  CHECK_EQ(m.x(22), 0x22221111ull);                           // sxtw #2 (pozitif)
  CHECK_EQ(m.x(23), 0x11ull);
  CHECK_EQ(m.x(24), ~0ull);                                   // ldrsb -> X
  CHECK_EQ(m.x(25), 0xFFFFFFFFull);                           // ldrsb -> W: üst 32 bit sıfır
  CHECK_EQ(m.x(26), ~0ull); CHECK_EQ(m.x(27), ~0ull);         // ldrsh / ldrsw
  CHECK_EQ(m.x(29), 0x29ull); CHECK_EQ(m.x(30), 0x30ull);     // prolog/epilog çifti
  CHECK_EQ(m.cpu.state().sp, S);                              // stp !/ldp, #16 SP'yi geri getirir
  CHECK_EQ(m.rd(B + 0, 8), X1); CHECK_EQ(m.rd(B + 8, 8), X2);
  CHECK_EQ(m.rd(B + 16, 8), X2); CHECK_EQ(m.rd(B + 24, 8), X1);
  CHECK_EQ(m.rd(B + 34, 1), 0x11ull);
  CHECK_EQ(m.rd(B + 36, 4), 0x22221111ull);
  CHECK_EQ(m.rd(S - 16, 8), 0x29ull); CHECK_EQ(m.rd(S - 8, 8), 0x30ull);
}

NT_TEST(cpu_load_literal) {
  Machine m; LOAD(m, ls_literal);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.x(1), 0x1122334455667788ull);
  CHECK_EQ(m.x(2), 0x99AABBCCull);
  CHECK_EQ(m.x(3), 0xFFFFFFFF80000001ull);
}

// Python'da komutun matematiksel anlamından üretilen vakalar (bkz. tools/gen_vectors.py).
NT_TEST(cpu_vectors_against_python_oracle) {
  Machine m;
  size_t bad = 0;
  for (size_t i = 0; i < ncvvec::CASE_COUNT; i++) {
    const ncvvec::Case& c = ncvvec::CASES[i];
    const ncvvec::Prog& p = ncvvec::PROGS[c.prog];
    m.load(p.data, p.len);
    for (int k = 0; k < 5; k++) m.setX(k, c.in[k]);
    CpuState& st = m.cpu.mutableState();
    st.n = (c.nzcvIn >> 3) & 1; st.z = (c.nzcvIn >> 2) & 1; st.c = (c.nzcvIn >> 1) & 1; st.v = c.nzcvIn & 1;
    const StopReason r = m.run();
    const CpuState& o = m.cpu.state();
    const unsigned nz = (o.n << 3) | (o.z << 2) | (o.c << 1) | o.v;
    bool ok = (r == StopReason::Halted) && o.x[0] == c.outX0 && nz == c.nzcvOut;
    for (int k = 1; k < 5; k++) ok = ok && o.x[k] == c.in[k];     // kaynaklar bozulmamalı
    if (!ok && bad++ < 8)
      nt::fail(__FILE__, __LINE__, std::string(p.name) + " | x1=0x" + nt::hex(c.in[1]) + " x2=0x" + nt::hex(c.in[2]) +
               " nzcv=" + std::to_string(c.nzcvIn) + " | x0 gercek=0x" + nt::hex(o.x[0]) + " beklenen=0x" + nt::hex(c.outX0) +
               " nzcv gercek=" + std::to_string(nz) + " beklenen=" + std::to_string(c.nzcvOut) +
               " stop=" + stopReasonName(r));
  }
  CHECK_EQ(bad, 0ull);
  CHECK(ncvvec::CASE_COUNT > 15000);
}

NT_TEST(cpu_hints_and_barriers_are_nops) {
  Machine m; LOAD(m, hints);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.cpu.instructionsRetired(), 12ull);
  CHECK_EQ(m.x(1), 1ull);
}

NT_TEST(cpu_prfm_never_touches_memory) {
  Machine m; LOAD(m, prfm_nop);
  m.setX(0, 0x1000);                 // eşlenmemiş adres: gerçek erişim olsaydı BusFault olurdu
  m.setX(2, 0x2000);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.x(1), 1ull);
  CHECK_EQ(m.cpu.instructionsRetired(), 6ull);
}

NT_TEST(cpu_nontemporal_and_unprivileged_pairs) {
  Machine m; LOAD(m, ls_extra);
  const uint64_t B = Machine::kBase + 0x1000;
  m.setX(10, B);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.x(3), 0x22221111ull); CHECK_EQ(m.x(4), ~0ull);              // ldnp/stnp X
  CHECK_EQ(m.x(5), 0x22221111ull); CHECK_EQ(m.x(6), 0xFFFFFFFFull);      // ldnp/stnp W
  CHECK_EQ(m.x(7), 0x22221111ull);                                       // sttr/ldtr
  CHECK_EQ(m.x(8), ~0ull); CHECK_EQ(m.x(9), 0xFFFFFFFFull);              // ldtrsb X / W
  CHECK_EQ(m.x(11), 0x1111ull); CHECK_EQ(m.x(12), 0x1111ull);
  CHECK_EQ(m.x(13), ~0ull);                                              // ldtrsw
  CHECK_EQ(m.rd(B + 56, 1), 0xFFull);
}

NT_TEST(cpu_exclusives_and_acquire_release) {
  Machine m; LOAD(m, excl);
  const uint64_t B = Machine::kBase + 0x1000;
  m.setX(10, B);
  CHECK(m.run() == StopReason::Halted);
  CHECK_EQ(m.x(2), 1ull); CHECK_EQ(m.x(3), 0ull);        // ldxr -> +1 -> stxr başarılı
  CHECK_EQ(m.x(26), 1ull);                               // başarılı stxr monitörü temizler: ikinci stxr başarısız
  CHECK_EQ(m.x(4), 1ull);
  CHECK_EQ(m.x(5), 1ull);                                // clrex sonrası stxr BAŞARISIZ (durum=1)
  CHECK_EQ(m.x(6), 1ull); CHECK_EQ(m.x(7), 0ull);        // ldaxr/stlxr
  CHECK_EQ(m.x(8), 0x55ull);                             // ldar, başarılı stlxr'ın yazdığını görür
  CHECK_EQ(m.x(12), 0x77ull);                            // stlr/ldar
  CHECK_EQ(m.x(13), 0x55ull); CHECK_EQ(m.x(14), 0ull);   // ldxrb / stxrb
  CHECK_EQ(m.x(15), 0x55ull); CHECK_EQ(m.x(16), 0ull);   // ldaxrh / stlxrh
  CHECK_EQ(m.x(17), 0x77ull); CHECK_EQ(m.x(18), 0x77ull);// ldarb / ldarh
  CHECK_EQ(m.x(20), 0x55ull); CHECK_EQ(m.x(21), 0x77ull);// ldxp
  CHECK_EQ(m.x(22), 0ull);                               // stxp başarılı
  CHECK_EQ(m.x(23), 0x77ull); CHECK_EQ(m.x(24), 0x55ull);// çift yer değiştirdi
  CHECK_EQ(m.x(25), 1ull);                               // monitor stxp ile temizlenmişti -> stxr başarısız
  CHECK_EQ(m.rd(B, 8), 0x77ull);
  CHECK_EQ(m.rd(B + 8, 8), 0x77ull);
}

NT_TEST(cpu_unaligned_exclusive_and_acquire_fault) {
  Machine m; LOAD(m, excl_unaligned);
  m.setX(1, Machine::kBase + 0x1004);
  CHECK(m.run() == StopReason::AlignmentFault);
  CHECK_EQ(m.cpu.lastStop().faultAddr, Machine::kBase + 0x1004);
  CHECK_EQ(m.cpu.instructionsRetired(), 0ull);
  Machine m2; LOAD(m2, ldar_unaligned);
  m2.setX(1, Machine::kBase + 0x1002);
  CHECK(m2.run() == StopReason::AlignmentFault);
}

NT_TEST(cpu_instruction_limit) {
  Machine m; LOAD(m, loop_forever);
  CHECK(m.run(1000) == StopReason::InstructionLimit);
  CHECK_EQ(m.cpu.instructionsRetired(), 1000ull);
  CHECK(m.run(500) == StopReason::InstructionLimit);   // kaldığı yerden devam
  CHECK_EQ(m.cpu.instructionsRetired(), 1500ull);
  CHECK(m.run(0) == StopReason::InstructionLimit);
}

// 15 koşul kodu x 121 (a,b) çifti: beklenen sonuçlar Python'da matematiksel tanımdan üretildi (bayrak mantığından değil).
NT_TEST(cpu_condition_codes_against_independent_oracle) {
  Machine m;
  size_t bad = 0;
  for (size_t i = 0; i < COND_VECS_LEN; i++) {
    const CondVec& v = COND_VECS[i];
    m.load(COND_PROGS[v.cc].data, COND_PROGS[v.cc].len);
    m.setX(0, v.a); m.setX(1, v.b);
    const StopReason r = m.run();
    if (r != StopReason::Halted || m.x(2) != v.taken) {
      if (bad++ < 5)
        nt::fail(__FILE__, __LINE__, std::string("b.") + COND_NAMES[v.cc] + " a=0x" + nt::hex(v.a) + " b=0x" + nt::hex(v.b) +
                 " beklenen=" + std::to_string(v.taken) + " gercek=" + std::to_string(m.x(2)));
    }
  }
  CHECK_EQ(bad, 0ull);
  CHECK_EQ(COND_VECS_LEN, 1815ull);
}
