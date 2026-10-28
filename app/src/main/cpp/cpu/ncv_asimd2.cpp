// NCV - ASIMD 2c-2b/2c-3: FP vektörleri, doyurucu (SQ*/UQ*), uzun/geniş/dar aritmetik, by-element, skaler-SIMD.
// Skaler-SIMD formları ayrı kodlanmaz: vektör çekirdeği 'scalarOp_' modunda yalnızca lane 0'ı işler.
// DOĞRULAMA DÜZEYİ: bu parça kısa testlerle (az vaka, mutasyon bataryası YOK) doğrulandı; ARM ARM ile satır satır karşılaştırılmadı.
#include "ncv_cpu.h"
#include "ncv_softfloat.h"

namespace ncv {
namespace {

constexpr uint32_t kQC = 1u << 27;

inline uint64_t mk(unsigned esz) { return esz >= 64 ? ~0ull : ((1ull << esz) - 1); }
inline uint64_t getLane(const VReg& r, unsigned esz, unsigned i) {
  const unsigned bit = i * esz;
  const uint64_t w = bit >= 64 ? r.hi : r.lo;
  return (w >> (bit & 63)) & mk(esz);
}
inline void setLane(VReg& r, unsigned esz, unsigned i, uint64_t v) {
  const unsigned bit = i * esz;
  uint64_t& w = bit >= 64 ? r.hi : r.lo;
  const unsigned off = bit & 63;
  if (esz == 64) { w = v; return; }
  const uint64_t m = mk(esz) << off;
  w = (w & ~m) | ((v << off) & m);
}
inline int64_t sx(uint64_t v, unsigned esz) {
  v &= mk(esz);
  if (esz == 64) return static_cast<int64_t>(v);
  const uint64_t m = 1ull << (esz - 1);
  return static_cast<int64_t>((v ^ m) - m);
}
inline unsigned highestBit(unsigned x) { return 31u - static_cast<unsigned>(__builtin_clz(x)); }

typedef __int128 i128;
uint64_t satS(i128 v, unsigned esz, bool* q) {
  const i128 hi = (static_cast<i128>(1) << (esz - 1)) - 1, lo = -(static_cast<i128>(1) << (esz - 1));
  if (v > hi) { *q = true; v = hi; } else if (v < lo) { *q = true; v = lo; }
  return static_cast<uint64_t>(v) & mk(esz);
}
uint64_t satU(i128 v, unsigned esz, bool* q) {
  const i128 hi = (static_cast<i128>(1) << esz) - 1;
  if (v > hi) { *q = true; v = hi; } else if (v < 0) { *q = true; v = 0; }
  return static_cast<uint64_t>(v);
}

sf::Env makeEnv(uint32_t fpcr) {
  sf::Env e;
  e.rmode = static_cast<sf::Rnd>((fpcr >> 22) & 3);
  e.fz = ((fpcr >> 24) & 1) != 0;
  e.dn = ((fpcr >> 25) & 1) != 0;
  return e;
}

// doyurucu üç-aynı çekirdeği (SQADD/SQSUB/SQSHL/SQRSHL/SQDMULH ve U karşılıkları)
uint64_t sat3(unsigned opcode, bool U, unsigned esz, uint64_t a, uint64_t b, bool* q) {
  const i128 as = sx(a, esz), bs = sx(b, esz);
  switch (opcode) {
    case 0x01: return U ? satU(static_cast<i128>(a) + b, esz, q) : satS(as + bs, esz, q);
    case 0x05: return U ? satU(static_cast<i128>(a) - b, esz, q) : satS(as - bs, esz, q);
    case 0x09: case 0x0B: {
      const int sh = static_cast<int8_t>(b & 0xFF);
      const i128 v = U ? static_cast<i128>(a) : as;
      i128 res;
      if (sh >= 0) {
        if (v == 0) res = 0;
        else if (sh >= static_cast<int>(esz)) res = v < 0 ? -(static_cast<i128>(1) << 100) : (static_cast<i128>(1) << 100);
        else res = v * (static_cast<i128>(1) << sh);
      } else {
        int rs = -sh;
        if (rs > 100) rs = 100;
        const i128 rnd = opcode == 0x0B ? (static_cast<i128>(1) << (rs - 1)) : 0;
        res = (v + rnd) >> rs;
      }
      return U ? satU(res, esz, q) : satS(res, esz, q);
    }
    default: {  // 0x16: SQDMULH (U=0) / SQRDMULH (U=1)
      i128 p = static_cast<i128>(2) * as * bs;
      if (U) p += static_cast<i128>(1) << (esz - 1);
      return satS(p >> esz, esz, q);
    }
  }
}

enum FOp { O_ADD, O_SUB, O_MUL, O_DIV, O_MAX, O_MIN, O_MAXNM, O_MINNM, O_ABD, O_MULX, O_RECPS, O_RSQRTS, O_FMLA, O_FMLS, O_EQ, O_GE, O_GT, O_AGE, O_AGT };

uint64_t fpBin(FOp op, sf::Fmt f, uint64_t a, uint64_t b, uint64_t d, sf::Env& env) {
  const uint64_t sign = f == sf::S ? 0x80000000ull : 0x8000000000000000ull;
  const uint64_t all = f == sf::S ? 0xFFFFFFFFull : ~0ull;
  switch (op) {
    case O_ADD: return sf::add(f, a, b, env);
    case O_SUB: return sf::sub(f, a, b, env);
    case O_MUL: return sf::mul(f, a, b, env);
    case O_DIV: return sf::div(f, a, b, env);
    case O_MAX: return sf::minMax(f, a, b, env, false, false);
    case O_MIN: return sf::minMax(f, a, b, env, true, false);
    case O_MAXNM: return sf::minMax(f, a, b, env, false, true);
    case O_MINNM: return sf::minMax(f, a, b, env, true, true);
    case O_ABD: return sf::sub(f, a, b, env) & ~sign;
    case O_MULX: return sf::mulX(f, a, b, env);
    case O_RECPS: return sf::recipStep(f, a, b, env);
    case O_RSQRTS: return sf::rsqrtStep(f, a, b, env);
    case O_FMLA: return sf::mulAdd(f, a, b, d, env);
    case O_FMLS: return sf::mulAdd(f, a ^ sign, b, d, env);
    case O_EQ: return sf::compare(f, a, b, env, false) == 6 ? all : 0;
    case O_GE: { const unsigned z = sf::compare(f, a, b, env, true); return (z == 2 || z == 6) ? all : 0; }
    case O_GT: return sf::compare(f, a, b, env, true) == 2 ? all : 0;
    case O_AGE: { const unsigned z = sf::compare(f, a & ~sign, b & ~sign, env, true); return (z == 2 || z == 6) ? all : 0; }
    default: return sf::compare(f, a & ~sign, b & ~sign, env, true) == 2 ? all : 0;   // O_AGT
  }
}

uint64_t vfpExpand(unsigned imm8, bool dbl) {
  const uint64_t b6 = (imm8 >> 6) & 1, sign = (imm8 >> 7) & 1;
  if (!dbl) return (sign << 31) | (((b6 ^ 1) << 7 | (b6 ? 0x1Fu : 0u) << 2 | ((imm8 >> 4) & 3)) << 23) | ((imm8 & 0xFu) << 19);
  return (sign << 63) | (((b6 ^ 1) << 10 | (b6 ? 0xFFu : 0u) << 2 | ((imm8 >> 4) & 3)) << 52) | (static_cast<uint64_t>(imm8 & 0xFu) << 48);
}

}  // namespace

StopReason Cpu::execAsimd2(uint32_t insn, uint64_t pc) {
  auto unsupported = [&]() { return stopWith(StopReason::UnsupportedInstruction, pc, insn); };
  const unsigned rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
  const bool Q = ((insn >> 30) & 1) != 0, U = ((insn >> 29) & 1) != 0;
  const unsigned size = (insn >> 22) & 3;

  // ---------- Skaler-SIMD: vektör çekirdeğine lane-0 modunda yönlendir ----------
  if (!scalarOp_) {
    const bool sThree = (insn & 0xDF200400u) == 0x5E200400u, sTwo = (insn & 0xDF3E0C00u) == 0x5E200800u;
    const bool sShift = (insn & 0xDF800400u) == 0x5F000400u, sElem = (insn & 0xDF000400u) == 0x5F000000u;
    const bool sDiff = (insn & 0xDF200C00u) == 0x5E200000u, sPair = (insn & 0xDF3E0C00u) == 0x5E300800u;
    if (sPair) {  // FADDP/FMAXP/FMINP/FMAXNMP/FMINNMP (skaler, 2 eleman)
      const unsigned opcode = (insn >> 12) & 0x1F, sz = (insn >> 22) & 1, b23 = (insn >> 23) & 1;
      if (!U) return unsupported();
      FOp op;
      if (opcode == 0x0C) op = b23 ? O_MINNM : O_MAXNM;
      else if (opcode == 0x0D && !b23) op = O_ADD;
      else if (opcode == 0x0F) op = b23 ? O_MIN : O_MAX;
      else return unsupported();
      sf::Env env = makeEnv(s_.fpcr);
      const sf::Fmt f = sz ? sf::D : sf::S;
      const unsigned esz = sz ? 64 : 32;
      VReg R;
      R.lo = fpBin(op, f, getLane(s_.vr[rn], esz, 0), getLane(s_.vr[rn], esz, 1), 0, env);
      s_.vr[rd] = R;
      s_.fpsr |= env.flags;
      return StopReason::None;
    }
    if (sThree || sTwo || sShift || sElem || sDiff) {
      uint32_t v = insn & ~0x10000000u;
      bool narrow = sDiff;
      if (sTwo) { const unsigned o = (insn >> 12) & 0x1F; narrow = o == 0x12 || o == 0x14 || o == 0x16; }
      if (sShift) { const unsigned o = (insn >> 11) & 0x1F; narrow = o >= 0x10 && o <= 0x13; }
      if (narrow) v &= ~0x40000000u;       // skaler kodlamada Q=1; dar/uzun komutlarda vektör karşılığı Q=0 ('2' olmayan) biçimdir
      scalarOp_ = true;
      const StopReason r = execAsimd(v, pc);
      scalarOp_ = false;
      if (r != StopReason::None) stop_.insn = insn;
      return r;
    }
  }

  const VReg A = s_.vr[rn], B = s_.vr[rm], D = s_.vr[rd];
  VReg R;
  bool qc = false;
  sf::Env env = makeEnv(s_.fpcr);
  auto commit = [&]() -> StopReason { s_.vr[rd] = R; s_.fpsr |= env.flags | (qc ? kQC : 0); return StopReason::None; };
  auto nl = [&](unsigned esz, bool q) -> unsigned { return scalarOp_ ? 1u : (q ? 128u : 64u) / esz; };

  // ---------- Üç aynı: FP ve doyurucu ----------
  if ((insn & 0x9F200400u) == 0x0E200400u) {
    const unsigned opcode = (insn >> 11) & 0x1F;
    if (opcode >= 0x18) {
      const unsigned sz = (insn >> 22) & 1, b23 = (insn >> 23) & 1;
      if (sz && !Q) return unsupported();
      FOp op;
      bool pw = false;
      switch (opcode) {
        case 0x18: op = b23 ? O_MINNM : O_MAXNM; pw = U; break;
        case 0x19: if (U) return unsupported(); op = b23 ? O_FMLS : O_FMLA; break;
        case 0x1A: if (!U) op = b23 ? O_SUB : O_ADD; else if (!b23) { op = O_ADD; pw = true; } else op = O_ABD; break;
        case 0x1B: if (b23) return unsupported(); op = U ? O_MUL : O_MULX; break;
        case 0x1C: if (!U) { if (b23) return unsupported(); op = O_EQ; } else op = b23 ? O_GT : O_GE; break;
        case 0x1D: if (!U) return unsupported(); op = b23 ? O_AGT : O_AGE; break;
        case 0x1E: op = b23 ? O_MIN : O_MAX; pw = U; break;
        default: if (!U) op = b23 ? O_RSQRTS : O_RECPS; else { if (b23) return unsupported(); op = O_DIV; } break;
      }
      if (pw && scalarOp_) return unsupported();
      const sf::Fmt f = sz ? sf::D : sf::S;
      const unsigned esz = sz ? 64 : 32, n = nl(esz, Q);
      for (unsigned i = 0; i < n; i++) {
        uint64_t r;
        if (pw) {
          const VReg& src = i < n / 2 ? A : B;
          const unsigned j = (i % (n / 2)) * 2;
          r = fpBin(op, f, getLane(src, esz, j), getLane(src, esz, j + 1), 0, env);
        } else {
          r = fpBin(op, f, getLane(A, esz, i), getLane(B, esz, i), getLane(D, esz, i), env);
        }
        setLane(R, esz, i, r);
      }
      return commit();
    }
    if (opcode == 0x01 || opcode == 0x05 || opcode == 0x09 || opcode == 0x0B || opcode == 0x16) {
      const unsigned esz = 8u << size, n = nl(esz, Q);
      if (size == 3 && !Q) return unsupported();
      if (opcode == 0x16 && (size == 0 || size == 3)) return unsupported();
      for (unsigned i = 0; i < n; i++) setLane(R, esz, i, sat3(opcode, U, esz, getLane(A, esz, i), getLane(B, esz, i), &qc));
      return commit();
    }
    return unsupported();
  }

  // ---------- İki register diğer: FP ve doyurucu ----------
  if ((insn & 0x9F3E0C00u) == 0x0E200800u) {
    const unsigned opcode = (insn >> 12) & 0x1F, sz = (insn >> 22) & 1, b23 = (insn >> 23) & 1;
    if (opcode == 0x03 || opcode == 0x07) {      // SUQADD/USQADD, SQABS/SQNEG
      const unsigned esz = 8u << size, n = nl(esz, Q);
      if (size == 3 && !Q) return unsupported();
      for (unsigned i = 0; i < n; i++) {
        const uint64_t a = getLane(A, esz, i), d = getLane(D, esz, i);
        uint64_t r;
        if (opcode == 0x07) r = satS(U ? -static_cast<i128>(sx(a, esz)) : (sx(a, esz) < 0 ? -static_cast<i128>(sx(a, esz)) : static_cast<i128>(sx(a, esz))), esz, &qc);
        else r = U ? satU(static_cast<i128>(d) + sx(a, esz), esz, &qc) : satS(static_cast<i128>(sx(d, esz)) + a, esz, &qc);
        setLane(R, esz, i, r);
      }
      return commit();
    }
    if (opcode == 0x14 || (U && opcode == 0x12)) {   // SQXTN/UQXTN/SQXTUN (+2)
      if (size == 3) return unsupported();
      const unsigned esz = 8u << size, half = scalarOp_ ? 1u : 64 / esz;
      R = Q ? D : VReg();
      for (unsigned i = 0; i < half; i++) {
        const uint64_t src = getLane(A, esz * 2, i);
        uint64_t r;
        if (opcode == 0x14) r = U ? satU(static_cast<i128>(src), esz, &qc) : satS(sx(src, esz * 2), esz, &qc);
        else r = satU(sx(src, esz * 2), esz, &qc);
        setLane(R, esz, i + (Q ? half : 0), r);
      }
      return commit();
    }
    // FP işlemleri
    enum { K_ABS, K_NEG, K_SQRT, K_RINT, K_CVTI, K_CVTF, K_RECPE, K_RSQRTE, K_CMP, K_FCVTN, K_FCVTXN, K_FCVTL };
    int kind = -1, cmpk = 0;
    sf::Rnd rm_ = sf::RN;
    bool sg = false, exact = false;
    switch (opcode) {
      case 0x0C: if (!b23) return unsupported(); kind = K_CMP; cmpk = U ? 1 : 0; break;                    // FCMGE / FCMGT zero
      case 0x0D: if (!b23) return unsupported(); kind = K_CMP; cmpk = U ? 4 : 2; break;                    // FCMLE / FCMEQ zero
      case 0x0E: if (!b23 || U) return unsupported(); kind = K_CMP; cmpk = 3; break;                       // FCMLT zero
      case 0x0F: if (!b23) return unsupported(); kind = U ? K_NEG : K_ABS; break;
      case 0x16: if (b23) return unsupported(); kind = U ? K_FCVTXN : K_FCVTN; break;
      case 0x17: if (U || b23) return unsupported(); kind = K_FCVTL; break;
      case 0x18: kind = K_RINT; if (!U) rm_ = b23 ? sf::RP : sf::RN; else { if (b23) return unsupported(); rm_ = sf::RA; } break;
      case 0x19: kind = K_RINT; if (!U) rm_ = b23 ? sf::RZ : sf::RM; else { rm_ = env.rmode; exact = !b23; } break;
      case 0x1A: kind = K_CVTI; sg = !U; rm_ = b23 ? sf::RP : sf::RN; break;
      case 0x1B: kind = K_CVTI; sg = !U; rm_ = b23 ? sf::RZ : sf::RM; break;
      case 0x1C: if (b23) return unsupported(); kind = K_CVTI; sg = !U; rm_ = sf::RA; break;
      case 0x1D: if (!b23) { kind = K_CVTF; sg = !U; } else kind = U ? K_RSQRTE : K_RECPE; break;
      case 0x1F: if (!U || !b23) return unsupported(); kind = K_SQRT; break;
      default: return unsupported();
    }
    if (kind == K_FCVTN || kind == K_FCVTXN || kind == K_FCVTL) {
      if (kind == K_FCVTXN && !sz) return unsupported();
      const sf::Fmt big = sz ? sf::D : sf::S, small = sz ? sf::S : sf::H;
      const unsigned besz = sz ? 64 : 32, sesz = besz / 2, cnt = scalarOp_ ? 1u : 64 / sesz;
      if (kind == K_FCVTL) {
        for (unsigned i = 0; i < cnt; i++) setLane(R, besz, i, sf::convert(small, big, getLane(A, sesz, i + (Q ? cnt : 0)), env));
      } else {
        R = Q ? D : VReg();
        for (unsigned i = 0; i < cnt; i++) {
          const uint64_t src = getLane(A, besz, i);
          setLane(R, sesz, i + (Q ? cnt : 0), kind == K_FCVTXN ? sf::convertOdd(big, small, src, env) : sf::convert(big, small, src, env));
        }
      }
      return commit();
    }
    if (sz && !Q) return unsupported();
    const sf::Fmt f = sz ? sf::D : sf::S;
    const unsigned esz = sz ? 64 : 32, n = nl(esz, Q);
    const uint64_t sign = sz ? 0x8000000000000000ull : 0x80000000ull, all = mk(esz);
    for (unsigned i = 0; i < n; i++) {
      const uint64_t a = getLane(A, esz, i);
      uint64_t r;
      switch (kind) {
        case K_ABS: r = a & ~sign; break;
        case K_NEG: r = a ^ sign; break;
        case K_SQRT: r = sf::sqrt(f, a, env); break;
        case K_RINT: r = sf::roundInt(f, a, rm_, exact, env); break;
        case K_CVTI: r = sf::toInt(f, a, sg, esz, 0, rm_, env); break;
        case K_CVTF: r = sf::fromInt(f, a, sg, esz, 0, env); break;
        case K_RECPE: r = sf::recipEstimate(f, a, env); break;
        case K_RSQRTE: r = sf::rsqrtEstimate(f, a, env); break;
        default: {  // K_CMP: sıfırla karşılaştır
          const unsigned z = sf::compare(f, a, 0, env, cmpk != 2);
          const bool t = cmpk == 0 ? z == 2 : cmpk == 1 ? (z == 2 || z == 6) : cmpk == 2 ? z == 6 : cmpk == 3 ? z == 8 : (z == 6 || z == 8);
          r = t ? all : 0;
        }
      }
      setLane(R, esz, i, r);
    }
    return commit();
  }

  // ---------- Şeritler arası FP: FMAXNMV/FMAXV/FMINNMV/FMINV (4S) ----------
  if ((insn & 0x9F3E0C00u) == 0x0E300800u) {
    const unsigned opcode = (insn >> 12) & 0x1F, b23 = (insn >> 23) & 1;
    if (!U || (size & 1) || !Q || (opcode != 0x0C && opcode != 0x0F)) return unsupported();
    const FOp op = opcode == 0x0C ? (b23 ? O_MINNM : O_MAXNM) : (b23 ? O_MIN : O_MAX);
    const uint64_t l0 = getLane(A, 32, 0), l1 = getLane(A, 32, 1), l2 = getLane(A, 32, 2), l3 = getLane(A, 32, 3);
    R.lo = fpBin(op, sf::S, fpBin(op, sf::S, l0, l1, 0, env), fpBin(op, sf::S, l2, l3, 0, env), 0, env);
    return commit();
  }

  // ---------- Üç farklı: uzun/geniş/dar ----------
  if ((insn & 0x9F200C00u) == 0x0E200000u) {
    const unsigned opcode = (insn >> 12) & 0xF;
    if (size == 3 || (opcode == 0xE && (U || size != 0)) || opcode == 0xF) return unsupported();
    const unsigned esz = 8u << size, wesz = esz * 2, cnt = scalarOp_ ? 1u : 64 / esz, part = Q ? 64 / esz : 0;
    if (opcode == 4 || opcode == 6) {            // ADDHN/RADDHN, SUBHN/RSUBHN (+2)
      R = Q ? D : VReg();
      const uint64_t rc = U ? (1ull << (esz - 1)) : 0;
      for (unsigned i = 0; i < cnt; i++) {
        const uint64_t a = getLane(A, wesz, i), b = getLane(B, wesz, i);
        const uint64_t s = opcode == 4 ? a + b + rc : a - b + rc;
        setLane(R, esz, i + (Q ? cnt : 0), (s & mk(wesz)) >> esz);
      }
      return commit();
    }
    for (unsigned i = 0; i < cnt; i++) {
      const uint64_t d = getLane(D, wesz, i);
      uint64_t a, b;
      if (opcode == 1 || opcode == 3) { a = getLane(A, wesz, i); b = getLane(B, esz, i + part); }   // geniş: Vn tam genişlikte
      else { a = getLane(A, esz, i + part); b = getLane(B, esz, i + part); }
      const i128 ea = (opcode == 1 || opcode == 3) ? (U ? static_cast<i128>(a) : static_cast<i128>(sx(a, wesz))) : (U ? static_cast<i128>(a) : static_cast<i128>(sx(a, esz)));
      const i128 eb = U ? static_cast<i128>(b) : static_cast<i128>(sx(b, esz));
      uint64_t r = 0;
      switch (opcode) {
        case 0: case 1: r = static_cast<uint64_t>(ea + eb); break;
        case 2: case 3: r = static_cast<uint64_t>(ea - eb); break;
        case 5: r = d + static_cast<uint64_t>(ea > eb ? ea - eb : eb - ea); break;
        case 7: r = static_cast<uint64_t>(ea > eb ? ea - eb : eb - ea); break;
        case 8: r = d + static_cast<uint64_t>(ea * eb); break;
        case 10: r = d - static_cast<uint64_t>(ea * eb); break;
        case 12: r = static_cast<uint64_t>(ea * eb); break;
        case 9: case 11: case 13: {
          if (U) return unsupported();
          const uint64_t p = satS(static_cast<i128>(2) * ea * eb, wesz, &qc);
          if (opcode == 13) r = p;
          else r = satS(static_cast<i128>(sx(d, wesz)) + (opcode == 9 ? 1 : -1) * static_cast<i128>(sx(p, wesz)), wesz, &qc);
          break;
        }
        default:  // 14: PMULL (8b -> 8h)
          for (int k = 0; k < 8; k++) if ((b >> k) & 1) r ^= a << k;
          break;
      }
      setLane(R, wesz, i, r & mk(wesz));
    }
    return commit();
  }

  // ---------- By-element ----------
  if ((insn & 0x9F000400u) == 0x0F000000u) {
    const unsigned opcode = (insn >> 12) & 0xF, L = (insn >> 21) & 1, M = (insn >> 20) & 1, H = (insn >> 11) & 1, rml = (insn >> 16) & 0xF;
    if ((opcode == 1 || opcode == 5 || opcode == 9) && size >= 2) {      // FMLA/FMLS/FMUL/FMULX
      const unsigned sz = size & 1;
      if ((sz && (L || !Q)) || (opcode != 9 && U)) return unsupported();
      const sf::Fmt f = sz ? sf::D : sf::S;
      const unsigned esz = sz ? 64 : 32, n = nl(esz, Q), idx = sz ? H : ((H << 1) | L);
      const uint64_t e = getLane(s_.vr[(M << 4) | rml], esz, idx);
      for (unsigned i = 0; i < n; i++) {
        const uint64_t a = getLane(A, esz, i), d = getLane(D, esz, i);
        const uint64_t sign = sz ? 0x8000000000000000ull : 0x80000000ull;
        uint64_t r;
        if (opcode == 1) r = sf::mulAdd(f, a, e, d, env);
        else if (opcode == 5) r = sf::mulAdd(f, a ^ sign, e, d, env);
        else r = U ? sf::mulX(f, a, e, env) : sf::mul(f, a, e, env);
        setLane(R, esz, i, r);
      }
      return commit();
    }
    if (size == 0 || size == 3) return unsupported();
    const unsigned esz = 8u << size, rmr = size == 1 ? rml : ((M << 4) | rml), idx = size == 1 ? ((H << 2) | (L << 1) | M) : ((H << 1) | L);
    const uint64_t e = getLane(s_.vr[rmr], esz, idx);
    if (opcode == 0 || opcode == 4 || opcode == 8 || opcode == 0xC || opcode == 0xD) {
      if (((opcode == 0 || opcode == 4) && !U) || (opcode >= 8 && U)) return unsupported();
      const unsigned n = nl(esz, Q);
      for (unsigned i = 0; i < n; i++) {
        const uint64_t a = getLane(A, esz, i), d = getLane(D, esz, i);
        uint64_t r;
        switch (opcode) {
          case 0: r = d + a * e; break;
          case 4: r = d - a * e; break;
          case 8: r = a * e; break;
          default: r = sat3(0x16, opcode == 0xD, esz, a, e, &qc); break;
        }
        setLane(R, esz, i, r & mk(esz));
      }
      return commit();
    }
    if (opcode == 2 || opcode == 6 || opcode == 10 || opcode == 3 || opcode == 7 || opcode == 11) {
      if ((opcode >= 3 && opcode != 6 && opcode != 10 && U)) return unsupported();
      const unsigned wesz = esz * 2, cnt = scalarOp_ ? 1u : 64 / esz, part = Q ? 64 / esz : 0;
      const i128 ee = U ? static_cast<i128>(e) : static_cast<i128>(sx(e, esz));
      for (unsigned i = 0; i < cnt; i++) {
        const uint64_t a = getLane(A, esz, i + part), d = getLane(D, wesz, i);
        const i128 ea = U ? static_cast<i128>(a) : static_cast<i128>(sx(a, esz));
        uint64_t r;
        if (opcode == 2) r = d + static_cast<uint64_t>(ea * ee);
        else if (opcode == 6) r = d - static_cast<uint64_t>(ea * ee);
        else if (opcode == 10) r = static_cast<uint64_t>(ea * ee);
        else {
          const uint64_t p = satS(static_cast<i128>(2) * ea * ee, wesz, &qc);
          if (opcode == 11) r = p;
          else r = satS(static_cast<i128>(sx(d, wesz)) + (opcode == 3 ? 1 : -1) * static_cast<i128>(sx(p, wesz)), wesz, &qc);
        }
        setLane(R, wesz, i, r & mk(wesz));
      }
      return commit();
    }
    return unsupported();
  }

  // ---------- Değiştirilmiş immediate: FMOV (vektör) ----------
  if ((insn & 0x9FF80400u) == 0x0F000400u) {
    const unsigned cmode = (insn >> 12) & 0xF;
    if (cmode != 15) return unsupported();
    const unsigned imm8 = (((insn >> 16) & 7) << 5) | ((insn >> 5) & 0x1F);
    if (!U) {
      const uint64_t lane = vfpExpand(imm8, false);
      R.lo = lane | (lane << 32);
      R.hi = Q ? R.lo : 0;
    } else {
      if (!Q) return unsupported();
      R.lo = R.hi = vfpExpand(imm8, true);
    }
    s_.vr[rd] = R;
    return StopReason::None;
  }

  // ---------- Immediate kaydırma: doyurucu ve sabit-nokta dönüşümler ----------
  if ((insn & 0x9F800400u) == 0x0F000400u && ((insn >> 19) & 0xF) != 0) {
    const unsigned immh = (insn >> 19) & 0xF, immhb = (insn >> 16) & 0x7F, opcode = (insn >> 11) & 0x1F, hb = highestBit(immh);
    if (opcode == 0x0C || opcode == 0x0E) {
      if (opcode == 0x0C && !U) return unsupported();
      const unsigned esz = 8u << hb, n = nl(esz, Q), sh = immhb - esz;
      if ((immh & 8) && !Q) return unsupported();
      for (unsigned i = 0; i < n; i++) {
        const uint64_t a = getLane(A, esz, i);
        i128 v = (opcode == 0x0E && U) ? static_cast<i128>(a) : static_cast<i128>(sx(a, esz));
        v = v * (static_cast<i128>(1) << sh);
        setLane(R, esz, i, (opcode == 0x0E && !U) ? satS(v, esz, &qc) : satU(v, esz, &qc));
      }
      return commit();
    }
    if (opcode >= 0x10 && opcode <= 0x13) {
      if ((immh & 8) || (opcode == 0x10 && !U) || (opcode == 0x11 && !U)) return unsupported();
      const unsigned esz = 8u << hb, half = scalarOp_ ? 1u : 64 / esz, sh = esz * 2 - immhb;
      const bool rnd = opcode == 0x11 || opcode == 0x13;
      R = Q ? D : VReg();
      for (unsigned i = 0; i < half; i++) {
        const uint64_t src = getLane(A, esz * 2, i);
        const bool uns = (opcode >= 0x12) ? U : false;          // 0x10/0x11 (U=1): işaretli kaynak, işaretsiz hedef
        i128 v = uns ? static_cast<i128>(src) : static_cast<i128>(sx(src, esz * 2));
        if (rnd) v += static_cast<i128>(1) << (sh - 1);
        v >>= sh;
        const bool unsignedDst = (opcode <= 0x11) || (opcode >= 0x12 && U);
        setLane(R, esz, i + (Q ? half : 0), unsignedDst ? satU(v, esz, &qc) : satS(v, esz, &qc));
      }
      return commit();
    }
    if (opcode == 0x1C || opcode == 0x1F) {                    // SCVTF/UCVTF, FCVTZS/FCVTZU (sabit-nokta, vektör)
      if (immh < 4) return unsupported();
      const bool dbl = (immh & 8) != 0;
      if (dbl && !Q) return unsupported();
      const sf::Fmt f = dbl ? sf::D : sf::S;
      const unsigned esz = dbl ? 64 : 32, n = nl(esz, Q), fbits = esz * 2 - immhb;
      for (unsigned i = 0; i < n; i++) {
        const uint64_t a = getLane(A, esz, i);
        setLane(R, esz, i, opcode == 0x1C ? sf::fromInt(f, a, !U, esz, fbits, env) : sf::toInt(f, a, !U, esz, fbits, sf::RZ, env));
      }
      return commit();
    }
    return unsupported();
  }

  return unsupported();
}

}  // namespace ncv
