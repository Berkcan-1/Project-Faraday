// NCV - ASIMD (vektör) tamsayı komutları ve yapı yükleme/saklama (LD1..LD4/ST1..ST4). FP vektörleri: Aşama 2c-3.
// Kapsam dışı (UnsupportedInstruction): doyurucu/yuvarlamalı (SQ*/UQ*) komutlar, uzun/geniş/dar aritmetik (SADDL, UMULL...),
// by-element komutlar, skaler-SIMD formları (ADDP Dd hariç), polinom (PMULL), kripto, FP vektörleri.
#include "ncv_cpu.h"

namespace ncv {
namespace {

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
inline unsigned clzE(uint64_t v, unsigned esz) { v &= mk(esz); return v == 0 ? esz : static_cast<unsigned>(__builtin_clzll(v)) - (64 - esz); }
inline unsigned clsE(uint64_t v, unsigned esz) {
  const uint64_t t = (v ^ (v >> 1)) & (mk(esz) >> 1);
  return t == 0 ? esz - 1 : (esz - 2) - (63u - static_cast<unsigned>(__builtin_clzll(t)));
}
inline uint64_t rbitByte(uint64_t b) {
  uint64_t r = 0;
  for (int i = 0; i < 8; i++) r |= ((b >> i) & 1) << (7 - i);
  return r;
}
inline unsigned highestBit(unsigned x) { return 31u - static_cast<unsigned>(__builtin_clz(x)); }

}  // namespace

StopReason Cpu::execAsimd(uint32_t insn, uint64_t pc) {
  // Tanınmayan/ayrılmış kodlamalar 2c-2b/2c-3 çözücüsüne devredilir (o da tanımazsa UnsupportedInstruction döner).
  auto unsupported = [&]() { return execAsimd2(insn, pc); };
  const unsigned rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
  const bool Q = ((insn >> 30) & 1) != 0, U = ((insn >> 29) & 1) != 0;
  const unsigned size = (insn >> 22) & 3;
  const VReg A = s_.vr[rn], B = s_.vr[rm], D = s_.vr[rd];
  VReg R;  // sonuç (varsayılan sıfır: Q=0 ise üst yarı sıfırlanır)

  // ---------- Üç aynı (tamsayı) ----------
  if ((insn & 0x9F200400u) == 0x0E200400u) {
    const unsigned opcode = (insn >> 11) & 0x1F;
    if (opcode == 0x03) {  // AND/BIC/ORR/ORN, EOR/BSL/BIT/BIF
      const uint64_t an[2] = {A.lo, A.hi}, bm[2] = {B.lo, B.hi}, dd[2] = {D.lo, D.hi};
      uint64_t r[2];
      for (int k = 0; k < 2; k++) {
        const uint64_t n = an[k], m = bm[k], d = dd[k];
        if (!U) r[k] = size == 0 ? (n & m) : size == 1 ? (n & ~m) : size == 2 ? (n | m) : (n | ~m);
        else r[k] = size == 0 ? (n ^ m) : size == 1 ? ((d & n) | (~d & m)) : size == 2 ? ((d & ~m) | (n & m)) : ((d & m) | (n & ~m));
      }
      R.lo = r[0]; R.hi = Q ? r[1] : 0;
      s_.vr[rd] = R;
      return StopReason::None;
    }
    const unsigned esz = 8u << size, n = scalarOp_ ? 1u : (Q ? 128u : 64u) / esz;
    if (size == 3 && !Q) return unsupported();
    const bool noSize3 = opcode == 0x00 || opcode == 0x02 || opcode == 0x04 || opcode == 0x0C || opcode == 0x0D || opcode == 0x0E ||
                         opcode == 0x0F || opcode == 0x12 || opcode == 0x13 || opcode == 0x14 || opcode == 0x15;
    if (noSize3 && size == 3) return unsupported();
    if (opcode == 0x14 || opcode == 0x15 || (opcode == 0x17 && !U)) {   // SMAXP/UMAXP, SMINP/UMINP, ADDP
      if (opcode == 0x17 && U) return unsupported();
      const unsigned half = n / 2;
      for (unsigned i = 0; i < n; i++) {
        const VReg& src = i < half ? A : B;
        const unsigned j = (i % half) * 2;
        const uint64_t x = getLane(src, esz, j), y = getLane(src, esz, j + 1);
        uint64_t r;
        if (opcode == 0x17) r = x + y;
        else if (opcode == 0x14) r = U ? (x > y ? x : y) : (sx(x, esz) > sx(y, esz) ? x : y);
        else r = U ? (x < y ? x : y) : (sx(x, esz) < sx(y, esz) ? x : y);
        setLane(R, esz, i, r);
      }
      s_.vr[rd] = R;
      return StopReason::None;
    }
    if (opcode == 0x13 && U && size != 0) return unsupported();   // PMUL yalnızca bayt
    switch (opcode) {
      case 0x00: case 0x02: case 0x04: case 0x06: case 0x07: case 0x08: case 0x0A: case 0x0C: case 0x0D: case 0x0E:
      case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: break;
      default: return unsupported();   // SQADD/SQSUB/SQSHL/SQDMULH/FP vb.
    }
    for (unsigned i = 0; i < n; i++) {
      const uint64_t a = getLane(A, esz, i), b = getLane(B, esz, i), d = getLane(D, esz, i);
      const int64_t as = sx(a, esz), bs = sx(b, esz);
      uint64_t r = 0;
      switch (opcode) {
        case 0x00: r = U ? static_cast<uint64_t>((static_cast<__int128>(a) + b) >> 1) : static_cast<uint64_t>((static_cast<__int128>(as) + bs) >> 1); break;
        case 0x02: r = U ? static_cast<uint64_t>((static_cast<__int128>(a) + b + 1) >> 1) : static_cast<uint64_t>((static_cast<__int128>(as) + bs + 1) >> 1); break;
        case 0x04: r = U ? static_cast<uint64_t>((static_cast<__int128>(a) - b) >> 1) : static_cast<uint64_t>((static_cast<__int128>(as) - bs) >> 1); break;
        case 0x06: r = (U ? a > b : as > bs) ? mk(esz) : 0; break;
        case 0x07: r = (U ? a >= b : as >= bs) ? mk(esz) : 0; break;
        case 0x08: case 0x0A: {
          const int sh = static_cast<int8_t>(b & 0xFF);
          const __int128 v = U ? static_cast<__int128>(a) : static_cast<__int128>(as);
          __int128 res;
          if (sh >= 0) res = sh >= static_cast<int>(esz) ? 0 : (v * (static_cast<__int128>(1) << sh));
          else {
            int rs = -sh;
            if (rs > 100) rs = 100;
            const __int128 rnd = opcode == 0x0A ? (static_cast<__int128>(1) << (rs - 1)) : 0;
            res = (v + rnd) >> rs;
          }
          r = static_cast<uint64_t>(res);
          break;
        }
        case 0x0C: r = U ? (a > b ? a : b) : (as > bs ? a : b); break;
        case 0x0D: r = U ? (a < b ? a : b) : (as < bs ? a : b); break;
        case 0x0E: r = U ? (a > b ? a - b : b - a) : static_cast<uint64_t>(as > bs ? static_cast<__int128>(as) - bs : static_cast<__int128>(bs) - as); break;
        case 0x0F: r = d + (U ? (a > b ? a - b : b - a) : static_cast<uint64_t>(as > bs ? static_cast<__int128>(as) - bs : static_cast<__int128>(bs) - as)); break;
        case 0x10: r = U ? a - b : a + b; break;
        case 0x11: r = U ? (a == b ? mk(esz) : 0) : ((a & b) ? mk(esz) : 0); break;
        case 0x12: r = U ? d - a * b : d + a * b; break;
        case 0x13:
          if (!U) r = a * b;
          else { for (int k = 0; k < 8; k++) if ((b >> k) & 1) r ^= a << k; }   // PMUL (8-bit, polinom)
          break;
      }
      setLane(R, esz, i, r & mk(esz));
    }
    s_.vr[rd] = R;
    return StopReason::None;
  }

  // ---------- İki register diğer ----------
  if ((insn & 0x9F3E0C00u) == 0x0E200800u) {
    const unsigned opcode = (insn >> 12) & 0x1F;
    const unsigned esz = 8u << size, n = scalarOp_ ? 1u : (Q ? 128u : 64u) / esz;
    const unsigned dbits = Q ? 128 : 64;
    (void)dbits;
    if (!U && (opcode == 0x00 || opcode == 0x01 || opcode == 0x04 || opcode == 0x05)) {
      if (opcode == 0x00 && size == 3) return unsupported();                                  // REV64
      if (opcode == 0x01 && size != 0) return unsupported();                                  // REV16
      if (opcode == 0x04 && size == 3) return unsupported();                                  // CLS
      if (opcode == 0x05 && size != 0) return unsupported();                                  // CNT
      for (unsigned i = 0; i < n; i++) {
        uint64_t r;
        if (opcode == 0x00) r = getLane(A, esz, i ^ (64 / esz - 1));
        else if (opcode == 0x01) r = getLane(A, esz, i ^ 1);
        else if (opcode == 0x04) r = clsE(getLane(A, esz, i), esz);
        else r = static_cast<uint64_t>(__builtin_popcountll(getLane(A, 8, i)));
        setLane(R, esz, i, r);
      }
      s_.vr[rd] = R;
      return StopReason::None;
    }
    if (U && opcode == 0x05) {                // NOT (size 0) / RBIT (size 1): bayt başına
      if (size > 1) return unsupported();
      const unsigned nb = Q ? 16 : 8;
      for (unsigned i = 0; i < nb; i++) setLane(R, 8, i, size == 0 ? ~getLane(A, 8, i) : rbitByte(getLane(A, 8, i)));
      s_.vr[rd] = R;
      return StopReason::None;
    }
    if (U && (opcode == 0x00 || opcode == 0x04)) {
      if (opcode == 0x00 && size > 1) return unsupported();                                   // REV32
      if (opcode == 0x04 && size == 3) return unsupported();                                  // CLZ
      for (unsigned i = 0; i < n; i++) {
        const uint64_t r = opcode == 0x00 ? getLane(A, esz, i ^ (32 / esz - 1)) : clzE(getLane(A, esz, i), esz);
        setLane(R, esz, i, r);
      }
      s_.vr[rd] = R;
      return StopReason::None;
    }
    if ((!U && (opcode == 0x08 || opcode == 0x09 || opcode == 0x0A || opcode == 0x0B)) ||
        (U && (opcode == 0x08 || opcode == 0x09 || opcode == 0x0B))) {   // CMxx #0, ABS, NEG
      if (size == 3 && !Q) return unsupported();
      for (unsigned i = 0; i < n; i++) {
        const int64_t as = sx(getLane(A, esz, i), esz);
        uint64_t r;
        if (opcode == 0x0B) r = static_cast<uint64_t>(U ? -static_cast<__int128>(as) : (as < 0 ? -static_cast<__int128>(as) : static_cast<__int128>(as)));
        else {
          bool t;
          if (!U) t = opcode == 0x08 ? as > 0 : opcode == 0x09 ? as == 0 : as < 0;
          else t = opcode == 0x08 ? as >= 0 : as <= 0;       // CMGE / CMLE
          r = t ? mk(esz) : 0;
        }
        setLane(R, esz, i, r & mk(esz));
      }
      s_.vr[rd] = R;
      return StopReason::None;
    }
    if (opcode == 0x02 || opcode == 0x06) {   // SADDLP/UADDLP, SADALP/UADALP
      if (size == 3) return unsupported();
      const unsigned wesz = esz * 2, wn = n / 2;
      for (unsigned i = 0; i < wn; i++) {
        const __int128 a0 = U ? static_cast<__int128>(getLane(A, esz, 2 * i)) : static_cast<__int128>(sx(getLane(A, esz, 2 * i), esz));
        const __int128 a1 = U ? static_cast<__int128>(getLane(A, esz, 2 * i + 1)) : static_cast<__int128>(sx(getLane(A, esz, 2 * i + 1), esz));
        uint64_t r = static_cast<uint64_t>(a0 + a1);
        if (opcode == 0x06) r += getLane(D, wesz, i);
        setLane(R, wesz, i, r & mk(wesz));
      }
      s_.vr[rd] = R;
      return StopReason::None;
    }
    if (!U && opcode == 0x12) {               // XTN / XTN2
      if (size == 3) return unsupported();
      const unsigned half = 64 / esz;
      R = Q ? D : VReg();
      for (unsigned i = 0; i < half; i++) setLane(R, esz, i + (Q ? half : 0), getLane(A, esz * 2, i));
      s_.vr[rd] = R;
      return StopReason::None;
    }
    if (U && opcode == 0x13) {                // SHLL / SHLL2
      if (size == 3) return unsupported();
      const unsigned half = 64 / esz;
      for (unsigned i = 0; i < half; i++) setLane(R, esz * 2, i, (getLane(A, esz, i + (Q ? half : 0)) << esz) & mk(esz * 2));
      s_.vr[rd] = R;
      return StopReason::None;
    }
    return unsupported();
  }

  // ---------- Şeritler arası (across lanes) ----------
  if ((insn & 0x9F3E0C00u) == 0x0E300800u) {
    const unsigned opcode = (insn >> 12) & 0x1F, esz = 8u << size, n = (Q ? 128u : 64u) / esz;
    if (size == 3 || (size == 2 && !Q)) return unsupported();
    uint64_t res;
    if (opcode == 0x03) {                     // SADDLV / UADDLV
      __int128 sum = 0;
      for (unsigned i = 0; i < n; i++) sum += U ? static_cast<__int128>(getLane(A, esz, i)) : static_cast<__int128>(sx(getLane(A, esz, i), esz));
      res = static_cast<uint64_t>(sum) & mk(esz * 2);
    } else if (opcode == 0x0A || opcode == 0x1A) {   // SMAXV/UMAXV, SMINV/UMINV
      uint64_t best = getLane(A, esz, 0);
      for (unsigned i = 1; i < n; i++) {
        const uint64_t v = getLane(A, esz, i);
        const bool better = opcode == 0x0A ? (U ? v > best : sx(v, esz) > sx(best, esz)) : (U ? v < best : sx(v, esz) < sx(best, esz));
        if (better) best = v;
      }
      res = best;
    } else if (opcode == 0x1B && !U) {        // ADDV
      uint64_t sum = 0;
      for (unsigned i = 0; i < n; i++) sum += getLane(A, esz, i);
      res = sum & mk(esz);
    } else {
      return unsupported();                   // FP across-lanes: 2c-3
    }
    R.lo = res;
    s_.vr[rd] = R;
    return StopReason::None;
  }

  // ---------- Kopya: DUP / INS / UMOV / SMOV ----------
  if ((insn & 0x9FE08400u) == 0x0E000400u) {
    const unsigned imm5 = (insn >> 16) & 0x1F, imm4 = (insn >> 11) & 0xF;
    if (imm5 == 0) return unsupported();
    const unsigned tz = static_cast<unsigned>(__builtin_ctz(imm5));
    if (tz > 3) return unsupported();
    const unsigned esz = 8u << tz, idx = imm5 >> (tz + 1);
    const bool op = ((insn >> 29) & 1) != 0;
    if (op) {                                 // INS (element)
      if (!Q) return unsupported();
      const unsigned src = imm4 >> tz;
      R = D;
      setLane(R, esz, idx, getLane(A, esz, src));
      s_.vr[rd] = R;
      return StopReason::None;
    }
    if (imm4 == 0x0 || imm4 == 0x1) {         // DUP (element / general)
      if (esz == 64 && !Q) return unsupported();
      const unsigned n = (Q ? 128u : 64u) / esz;
      const uint64_t v = imm4 == 0 ? getLane(A, esz, idx) : (getX(rn) & mk(esz));
      for (unsigned i = 0; i < n; i++) setLane(R, esz, i, v);
      s_.vr[rd] = R;
      return StopReason::None;
    }
    if (imm4 == 0x3) {                        // INS (general)
      if (!Q) return unsupported();
      R = D;
      setLane(R, esz, idx, getX(rn) & mk(esz));
      s_.vr[rd] = R;
      return StopReason::None;
    }
    if (imm4 == 0x5 || imm4 == 0x7) {         // SMOV / UMOV
      const uint64_t v = getLane(A, esz, idx);
      if (imm4 == 0x7) {
        if ((esz == 64) != Q) return unsupported();
        setX(rd, v);
      } else {
        if (esz == 64 || (!Q && esz == 32)) return unsupported();
        const uint64_t sv = static_cast<uint64_t>(sx(v, esz));
        setX(rd, Q ? sv : (sv & 0xFFFFFFFFull));
      }
      return StopReason::None;
    }
    return unsupported();
  }

  // ---------- Değiştirilmiş immediate: MOVI / MVNI / ORR / BIC ----------
  if ((insn & 0x9FF80400u) == 0x0F000400u) {
    const unsigned cmode = (insn >> 12) & 0xF;
    const uint64_t imm8 = (((insn >> 16) & 7) << 5) | ((insn >> 5) & 0x1F);
    const bool op = U;                        // bit 29
    uint64_t imm64;
    bool isOrrBic = false;
    if (cmode < 8) {                          // 32-bit, kaydırmalı
      const uint64_t v = (imm8 << (8 * (cmode >> 1))) & 0xFFFFFFFFull;
      imm64 = v | (v << 32);
      isOrrBic = (cmode & 1) != 0;
    } else if (cmode < 12) {                  // 16-bit, kaydırmalı
      const uint64_t v = (imm8 << (8 * ((cmode >> 1) & 1))) & 0xFFFFull;
      imm64 = v | (v << 16) | (v << 32) | (v << 48);
      isOrrBic = (cmode & 1) != 0;
    } else if (cmode < 14) {                  // 32-bit MSL
      const uint64_t v = cmode == 12 ? ((imm8 << 8) | 0xFF) : ((imm8 << 16) | 0xFFFF);
      imm64 = v | (v << 32);
    } else if (cmode == 14) {
      if (!op) { imm64 = 0; for (int k = 0; k < 8; k++) imm64 |= imm8 << (8 * k); }
      else { imm64 = 0; for (int k = 0; k < 8; k++) if ((imm8 >> k) & 1) imm64 |= 0xFFull << (8 * k); }
    } else {
      return unsupported();                   // FMOV (vektör, immediate): 2c-3
    }
    if (isOrrBic) {
      R.lo = op ? (D.lo & ~imm64) : (D.lo | imm64);
      R.hi = Q ? (op ? (D.hi & ~imm64) : (D.hi | imm64)) : 0;
    } else {
      const uint64_t v = (op && cmode != 14) ? ~imm64 : imm64;
      R.lo = v;
      R.hi = Q ? v : 0;
    }
    s_.vr[rd] = R;
    return StopReason::None;
  }

  // ---------- Permütasyon: UZP/TRN/ZIP ----------
  if ((insn & 0xBF208C00u) == 0x0E000800u) {
    const unsigned opcode = (insn >> 12) & 7, esz = 8u << size, n = (Q ? 128u : 64u) / esz;
    if ((size == 3 && !Q) || opcode == 0 || opcode == 4) return unsupported();
    const bool part2 = (opcode & 4) != 0;
    for (unsigned i = 0; i < n; i++) {
      uint64_t v;
      switch (opcode & 3) {
        case 1: {  // UZP
          const unsigned j = 2 * (i % (n / 2)) + (part2 ? 1 : 0);
          v = getLane(i < n / 2 ? A : B, esz, j);
          break;
        }
        case 2: {  // TRN
          const unsigned j = (i & ~1u) + (part2 ? 1 : 0);
          v = getLane((i & 1) ? B : A, esz, j);
          break;
        }
        default: { // ZIP
          const unsigned j = (i >> 1) + (part2 ? n / 2 : 0);
          v = getLane((i & 1) ? B : A, esz, j);
          break;
        }
      }
      setLane(R, esz, i, v);
    }
    s_.vr[rd] = R;
    return StopReason::None;
  }

  // ---------- EXT ----------
  if ((insn & 0xBFE08400u) == 0x2E000000u) {
    const unsigned idx = (insn >> 11) & 0xF, nb = Q ? 16 : 8;
    if (!Q && idx >= 8) return unsupported();
    uint8_t cat[32];
    for (unsigned i = 0; i < 16; i++) { cat[i] = static_cast<uint8_t>(getLane(A, 8, i)); cat[16 + i] = static_cast<uint8_t>(getLane(B, 8, i)); }
    if (!Q) { for (unsigned i = 0; i < 8; i++) cat[8 + i] = static_cast<uint8_t>(getLane(B, 8, i)); }
    for (unsigned i = 0; i < nb; i++) setLane(R, 8, i, cat[idx + i]);
    s_.vr[rd] = R;
    return StopReason::None;
  }

  // ---------- TBL / TBX ----------
  if ((insn & 0xBFE08C00u) == 0x0E000000u) {
    const unsigned len = ((insn >> 13) & 3) + 1, nb = Q ? 16 : 8;
    const bool tbx = ((insn >> 12) & 1) != 0;
    uint8_t tab[64];
    for (unsigned r = 0; r < len; r++)
      for (unsigned i = 0; i < 16; i++) tab[r * 16 + i] = static_cast<uint8_t>(getLane(s_.vr[(rn + r) & 31], 8, i));
    R = tbx ? D : VReg();
    if (!Q) R.hi = 0;
    for (unsigned i = 0; i < nb; i++) {
      const uint64_t ix = getLane(B, 8, i);
      if (ix < len * 16) setLane(R, 8, i, tab[ix]);
      else if (!tbx) setLane(R, 8, i, 0);
    }
    s_.vr[rd] = R;
    return StopReason::None;
  }

  // ---------- Immediate ile kaydırma ----------
  if ((insn & 0x9F800400u) == 0x0F000400u && ((insn >> 19) & 0xF) != 0) {
    const unsigned immh = (insn >> 19) & 0xF, immhb = (insn >> 16) & 0x7F, opcode = (insn >> 11) & 0x1F;
    const unsigned hb = highestBit(immh);
    if (opcode == 0x10 || opcode == 0x11 || opcode == 0x14) {       // SHRN/RSHRN, SSHLL/USHLL
      if (immh & 8) return unsupported();
      const unsigned esz = 8u << hb, half = 64 / esz;               // esz: hedef (dar) / kaynak (geniş) elemanı
      if (opcode == 0x14) {                                         // SSHLL/USHLL (+2)
        const unsigned sh = immhb - esz;
        for (unsigned i = 0; i < half; i++) {
          const uint64_t v = getLane(A, esz, i + (Q ? half : 0));
          const __int128 w = U ? static_cast<__int128>(v) : static_cast<__int128>(sx(v, esz));
          setLane(R, esz * 2, i, static_cast<uint64_t>(w * (static_cast<__int128>(1) << sh)) & mk(esz * 2));
        }
      } else {
        if (U) return unsupported();                                // SQSHRUN/SQRSHRUN
        const unsigned sh = esz * 2 - immhb;
        R = Q ? D : VReg();
        for (unsigned i = 0; i < half; i++) {
          unsigned __int128 v = getLane(A, esz * 2, i);
          if (opcode == 0x11) v += static_cast<unsigned __int128>(1) << (sh - 1);
          setLane(R, esz, i + (Q ? half : 0), static_cast<uint64_t>(v >> sh) & mk(esz));
        }
      }
      s_.vr[rd] = R;
      return StopReason::None;
    }
    if (opcode == 0x00 || opcode == 0x02 || opcode == 0x04 || opcode == 0x06 || opcode == 0x08 || opcode == 0x0A) {
      const unsigned esz = 8u << hb, n = scalarOp_ ? 1u : (Q ? 128u : 64u) / esz;
      if ((immh & 8) && !Q) return unsupported();
      if (opcode == 0x08 && !U) return unsupported();
      const bool left = opcode == 0x0A;
      const unsigned sh = left ? immhb - esz : esz * 2 - immhb;
      for (unsigned i = 0; i < n; i++) {
        const uint64_t a = getLane(A, esz, i), d = getLane(D, esz, i);
        const __int128 v = U ? static_cast<__int128>(a) : static_cast<__int128>(sx(a, esz));
        uint64_t r;
        switch (opcode) {
          case 0x00: r = static_cast<uint64_t>(v >> sh); break;                                       // SSHR/USHR
          case 0x02: r = d + static_cast<uint64_t>(v >> sh); break;                                   // SSRA/USRA
          case 0x04: r = static_cast<uint64_t>((v + (static_cast<__int128>(1) << (sh - 1))) >> sh); break;   // SRSHR/URSHR
          case 0x06: r = d + static_cast<uint64_t>((v + (static_cast<__int128>(1) << (sh - 1))) >> sh); break;  // SRSRA/URSRA
          case 0x08: {                                                                                  // SRI
            const uint64_t m = sh >= 64 ? 0 : (mk(esz) >> sh);
            r = (d & ~m) | ((sh >= 64 ? 0 : (a >> sh)) & m);
            break;
          }
          default:
            if (!U) r = a << sh;                                                                      // SHL
            else r = (a << sh) | (d & ((1ull << sh) - 1));                                            // SLI
            break;
        }
        setLane(R, esz, i, r & mk(esz));
      }
      s_.vr[rd] = R;
      return StopReason::None;
    }
    return unsupported();
  }

  // ---------- Skaler: DUP (element) = MOV Bd/Hd/Sd/Dd, Vn.T[i];  ADDP Dd, Vn.2D ----------
  if ((insn & 0xDFE08400u) == 0x5E000400u && ((insn >> 11) & 0xF) == 0) {
    const unsigned imm5 = (insn >> 16) & 0x1F;
    if (imm5 == 0) return unsupported();
    const unsigned tz = static_cast<unsigned>(__builtin_ctz(imm5));
    if (tz > 3) return unsupported();
    R.lo = getLane(A, 8u << tz, imm5 >> (tz + 1));
    s_.vr[rd] = R;
    return StopReason::None;
  }
  if ((insn & 0xFFFFFC00u) == 0x5EF1B800u) {  // ADDP Dd, Vn.2D
    R.lo = A.lo + A.hi;
    s_.vr[rd] = R;
    return StopReason::None;
  }

  // ---------- Yapı yükleme/saklama: LD1..LD4 / ST1..ST4 ----------
  const bool multiNo = (insn & 0xBFBF0000u) == 0x0C000000u, multiPost = (insn & 0xBFA00000u) == 0x0C800000u;
  const bool singleNo = (insn & 0xBF9F0000u) == 0x0D000000u, singlePost = (insn & 0xBF800000u) == 0x0D800000u;
  if (multiNo || multiPost || singleNo || singlePost) {
    const unsigned size = (insn >> 10) & 3;   // yapı komutlarında 'size' alanı bit 11:10'dadır
    const bool load = ((insn >> 22) & 1) != 0, post = multiPost || singlePost;
    const unsigned opcode = (insn >> 12) & 0xF;
    uint64_t addr = getXSp(rn);
    const uint64_t base = addr;
    uint64_t total = 0;
    auto rd8 = [&](unsigned bytes, uint64_t a, uint64_t* v) -> StopReason {
      return loadMemory(a, bytes, v, pc, insn);
    };
    auto wr8 = [&](unsigned bytes, uint64_t a, uint64_t v) -> StopReason {
      return storeMemory(a, bytes, v, pc, insn);
    };
    if (multiNo || multiPost) {
      unsigned rpt, selem;
      switch (opcode) {
        case 0x0: rpt = 1; selem = 4; break;
        case 0x2: rpt = 4; selem = 1; break;
        case 0x4: rpt = 1; selem = 3; break;
        case 0x6: rpt = 3; selem = 1; break;
        case 0x7: rpt = 1; selem = 1; break;
        case 0x8: rpt = 1; selem = 2; break;
        case 0xA: rpt = 2; selem = 1; break;
        default: return unsupported();
      }
      const unsigned esz = 8u << size, ebytes = esz / 8, n = (Q ? 128u : 64u) / esz;
      if (size == 3 && !Q && selem != 1) return unsupported();
      VReg tmp[4];
      for (unsigned r = 0; r < rpt * selem; r++) tmp[r] = load ? VReg() : s_.vr[(rd + r) & 31];
      uint64_t off = 0;
      for (unsigned r = 0; r < rpt; r++)
        for (unsigned e = 0; e < n; e++)
          for (unsigned s = 0; s < selem; s++) {
            VReg& t = tmp[r * selem + s];
            if (load) { uint64_t v; const StopReason sr = rd8(ebytes, addr + off, &v); if (sr != StopReason::None) return sr; setLane(t, esz, e, v); }
            else { const StopReason sr = wr8(ebytes, addr + off, getLane(t, esz, e)); if (sr != StopReason::None) return sr; }
            off += ebytes;
          }
      if (load) for (unsigned r = 0; r < rpt * selem; r++) s_.vr[(rd + r) & 31] = tmp[r];
      total = off;
    } else {
      const unsigned op3 = (insn >> 13) & 7;                       // opcode = bit 15:13, S = bit 12
      const unsigned sc = op3 >> 1, selem = (((op3 & 1) << 1) | ((insn >> 21) & 1)) + 1;
      unsigned scale = sc, index = 0;
      bool repl = false;
      const unsigned sS = (insn >> 12) & 1;
      if (sc == 3) {
        if (!load || sS) return unsupported();
        scale = size; repl = true;
      } else if (sc == 0) index = (Q << 3) | (sS << 2) | size;
      else if (sc == 1) { if (size & 1) return unsupported(); index = (Q << 2) | (sS << 1) | (size >> 1); }
      else {
        if (size & 2) return unsupported();
        if (!(size & 1)) index = (Q << 1) | sS;
        else { if (sS) return unsupported(); index = Q; scale = 3; }
      }
      const unsigned esz = 8u << scale, ebytes = esz / 8;
      uint64_t off = 0;
      for (unsigned s = 0; s < selem; s++) {
        const unsigned t = (rd + s) & 31;
        if (repl) {
          uint64_t v; const StopReason sr = rd8(ebytes, addr + off, &v); if (sr != StopReason::None) return sr;
          VReg x;
          const unsigned n = (Q ? 128u : 64u) / esz;
          for (unsigned i = 0; i < n; i++) setLane(x, esz, i, v);
          s_.vr[t] = x;
        } else if (load) {
          uint64_t v; const StopReason sr = rd8(ebytes, addr + off, &v); if (sr != StopReason::None) return sr;
          setLane(s_.vr[t], esz, index, v);
        } else {
          const StopReason sr = wr8(ebytes, addr + off, getLane(s_.vr[t], esz, index)); if (sr != StopReason::None) return sr;
        }
        off += ebytes;
      }
      total = off;
    }
    if (post) {
      const unsigned rmr = (insn >> 16) & 31;
      setXSp(rn, base + (rmr == 31 ? total : getX(rmr)));
    }
    return StopReason::None;
  }

  return unsupported();
}

}  // namespace ncv
