// NCV - SIMD&FP yükleme/saklama ve SKALER FP (S/D; half yalnızca FCVT ve yükleme/saklama). Vektör (ASIMD) komutları 2c-2/2c-3.
#include "ncv_cpu.h"
#include "ncv_softfloat.h"

namespace ncv {
namespace {

inline uint64_t signExtend(uint64_t v, unsigned bits) {
  const uint64_t m = 1ull << (bits - 1);
  return (v ^ m) - m;
}

sf::Env makeEnv(uint32_t fpcr) {
  sf::Env e;
  e.rmode = static_cast<sf::Rnd>((fpcr >> 22) & 3);
  e.fz = ((fpcr >> 24) & 1) != 0;
  e.dn = ((fpcr >> 25) & 1) != 0;
  return e;
}

bool condHolds(unsigned cond, const CpuState& s) {
  bool r;
  switch (cond >> 1) {
    case 0: r = s.z; break;
    case 1: r = s.c; break;
    case 2: r = s.n; break;
    case 3: r = s.v; break;
    case 4: r = s.c && !s.z; break;
    case 5: r = (s.n == s.v); break;
    case 6: r = (s.n == s.v) && !s.z; break;
    default: r = true; break;
  }
  if ((cond & 1) && cond != 0xF) r = !r;
  return r;
}

inline bool scalarFmt(unsigned type, sf::Fmt* f) {
  if (type == 0) { *f = sf::S; return true; }
  if (type == 1) { *f = sf::D; return true; }
  return false;
}
inline uint64_t fmtMask(sf::Fmt f) { return f == sf::S ? 0xFFFFFFFFull : ~0ull; }
inline uint64_t fmtSign(sf::Fmt f) { return f == sf::S ? 0x80000000ull : 0x8000000000000000ull; }

}  // namespace

StopReason Cpu::execFp(uint32_t insn, uint64_t pc) {
  auto unsupported = [&]() { return stopWith(StopReason::UnsupportedInstruction, pc, insn); };
  const unsigned rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
  const unsigned type = (insn >> 22) & 3;
  sf::Env env = makeEnv(s_.fpcr);
  auto commit = [&]() { s_.fpsr |= env.flags; };
  auto setScalar = [&](unsigned r, uint64_t bits) { s_.vr[r].lo = bits; s_.vr[r].hi = 0; };
  sf::Fmt f = sf::S;

  // ---------- SIMD&FP yükleme/saklama ----------
  auto vAccess = [&](bool load, unsigned bytes, unsigned rt, uint64_t addr) -> StopReason {
    if (load) {
      uint64_t lo = 0, hi = 0;
      StopReason st = loadMemory(addr, bytes == 16 ? 8 : bytes, &lo, pc, insn);
      if (st != StopReason::None) return st;
      if (bytes == 16) {
        st = loadMemory(addr + 8, 8, &hi, pc, insn);
        if (st != StopReason::None) return st;
      }
      s_.vr[rt].lo = lo;
      s_.vr[rt].hi = hi;
    } else {
      StopReason st = storeMemory(addr, bytes == 16 ? 8 : bytes, s_.vr[rt].lo, pc, insn);
      if (st != StopReason::None) return st;
      if (bytes == 16) {
        st = storeMemory(addr + 8, 8, s_.vr[rt].hi, pc, insn);
        if (st != StopReason::None) return st;
      }
    }
    return StopReason::None;
  };
  // (size, opc) -> bayt sayısı; opc[1]=1 yalnızca size==0 için Q (128-bit)
  auto sizeOf = [&](unsigned size, unsigned opc, unsigned* bytes, unsigned* scale) -> bool {
    if (opc >> 1) { if (size != 0) return false; *bytes = 16; *scale = 4; return true; }
    *bytes = 1u << size; *scale = size; return true;
  };

  if ((insn & 0x3F000000u) == 0x3D000000u) {  // LDR/STR (unsigned offset)
    const unsigned size = (insn >> 30) & 3, opc = (insn >> 22) & 3;
    unsigned bytes, scale;
    if (!sizeOf(size, opc, &bytes, &scale)) return unsupported();
    return vAccess(opc & 1, bytes, rd, getXSp(rn) + (static_cast<uint64_t>((insn >> 10) & 0xFFF) << scale));
  }
  if ((insn & 0x3F200000u) == 0x3C000000u) {  // LDUR/STUR, pre/post-index
    const unsigned size = (insn >> 30) & 3, opc = (insn >> 22) & 3, idx = (insn >> 10) & 3;
    unsigned bytes, scale;
    if (idx == 2 || !sizeOf(size, opc, &bytes, &scale)) return unsupported();
    const uint64_t imm9 = signExtend((insn >> 12) & 0x1FF, 9), base = getXSp(rn);
    const StopReason r = vAccess(opc & 1, bytes, rd, idx == 1 ? base : base + imm9);
    if (r != StopReason::None) return r;
    if (idx == 1 || idx == 3) setXSp(rn, base + imm9);
    return StopReason::None;
  }
  if ((insn & 0x3F200C00u) == 0x3C200800u) {  // LDR/STR (register offset)
    const unsigned size = (insn >> 30) & 3, opc = (insn >> 22) & 3, option = (insn >> 13) & 7;
    unsigned bytes, scale;
    if (!sizeOf(size, opc, &bytes, &scale) || (option != 2 && option != 3 && option != 6 && option != 7)) return unsupported();
    const uint64_t rmv = getX(rm);
    uint64_t off = (option == 2) ? (rmv & 0xFFFFFFFFull) : (option == 6) ? signExtend(rmv & 0xFFFFFFFFull, 32) : rmv;
    if ((insn >> 12) & 1) off <<= scale;
    return vAccess(opc & 1, bytes, rd, getXSp(rn) + off);
  }
  if ((insn & 0x3F000000u) == 0x1C000000u) {  // LDR (literal)
    const unsigned opc = (insn >> 30) & 3;
    if (opc == 3) return unsupported();
    return vAccess(true, 4u << opc, rd, pc + (signExtend((insn >> 5) & 0x7FFFFu, 19) << 2));
  }
  if ((insn & 0x3E000000u) == 0x2C000000u) {  // LDP/STP/LDNP/STNP (SIMD&FP)
    const unsigned opc = (insn >> 30) & 3, idx = (insn >> 23) & 3, rt2 = (insn >> 10) & 31;
    const bool load = ((insn >> 22) & 1) != 0;
    if (opc == 3 || (load && rd == rt2)) return unsupported();
    const unsigned scale = 2 + opc, bytes = 1u << scale;
    const uint64_t off = signExtend((insn >> 15) & 0x7F, 7) << scale, base = getXSp(rn);
    const uint64_t addr = (idx == 1) ? base : base + off;
    if (load) {
      VReg a, b;
      uint64_t lo = 0, hi = 0;
      for (int k = 0; k < 2; k++) {
        const uint64_t ad = addr + static_cast<uint64_t>(k) * bytes;
        StopReason st = loadMemory(ad, bytes == 16 ? 8 : bytes, &lo, pc, insn);
        if (st != StopReason::None) return st;
        hi = 0;
        if (bytes == 16) {
          st = loadMemory(ad + 8, 8, &hi, pc, insn);
          if (st != StopReason::None) return st;
        }
        (k == 0 ? a : b).lo = lo;
        (k == 0 ? a : b).hi = hi;
      }
      s_.vr[rd] = a;
      s_.vr[rt2] = b;
    } else {
      StopReason r = vAccess(false, bytes, rd, addr);
      if (r != StopReason::None) return r;
      r = vAccess(false, bytes, rt2, addr + bytes);
      if (r != StopReason::None) return r;
    }
    if (idx == 1 || idx == 3) setXSp(rn, base + off);
    return StopReason::None;
  }

  // ---------- Skaler FP ----------
  if ((insn & 0xFF207C00u) == 0x1E204000u) {  // FP veri işleme (1 kaynak)
    const unsigned opcode = (insn >> 15) & 0x3F;
    if (opcode >= 4 && opcode <= 7) {          // FCVT (S/D/H arası)
      static const sf::Fmt kDst[8] = {sf::S, sf::S, sf::S, sf::S, sf::S, sf::D, sf::S, sf::H};
      if (opcode == 6) return unsupported();   // BFCVT (BF16)
      sf::Fmt src;
      if (type == 0) src = sf::S; else if (type == 1) src = sf::D; else if (type == 3) src = sf::H; else return unsupported();
      const sf::Fmt dst = kDst[opcode];
      if (dst == src) return unsupported();
      const uint64_t in = s_.vr[rn].lo & (src == sf::H ? 0xFFFFull : fmtMask(src));
      setScalar(rd, sf::convert(src, dst, in, env));
      commit();
      return StopReason::None;
    }
    if (!scalarFmt(type, &f)) return unsupported();  // half aritmetiği FEAT_FP16 ister
    const uint64_t a = s_.vr[rn].lo & fmtMask(f);
    uint64_t res;
    switch (opcode) {
      case 0: res = a; break;                                   // FMOV
      case 1: res = a & ~fmtSign(f); break;                     // FABS (bit işlemi)
      case 2: res = a ^ fmtSign(f); break;                      // FNEG (bit işlemi)
      case 3: res = sf::sqrt(f, a, env); break;
      case 8: res = sf::roundInt(f, a, sf::RN, false, env); break;   // FRINTN
      case 9: res = sf::roundInt(f, a, sf::RP, false, env); break;   // FRINTP
      case 10: res = sf::roundInt(f, a, sf::RM, false, env); break;  // FRINTM
      case 11: res = sf::roundInt(f, a, sf::RZ, false, env); break;  // FRINTZ
      case 12: res = sf::roundInt(f, a, sf::RA, false, env); break;  // FRINTA
      case 14: res = sf::roundInt(f, a, env.rmode, true, env); break;   // FRINTX
      case 15: res = sf::roundInt(f, a, env.rmode, false, env); break;  // FRINTI
      default: return unsupported();
    }
    setScalar(rd, res);
    commit();
    return StopReason::None;
  }
  if ((insn & 0xFF20FC07u) == 0x1E202000u) {  // FCMP/FCMPE
    if (!scalarFmt(type, &f)) return unsupported();
    const bool signaling = ((insn >> 4) & 1) != 0, withZero = ((insn >> 3) & 1) != 0;
    if (withZero && rm != 0) return unsupported();
    const uint64_t a = s_.vr[rn].lo & fmtMask(f), b = withZero ? 0 : (s_.vr[rm].lo & fmtMask(f));
    const unsigned nzcv = sf::compare(f, a, b, env, signaling);
    s_.n = (nzcv >> 3) & 1; s_.z = (nzcv >> 2) & 1; s_.c = (nzcv >> 1) & 1; s_.v = nzcv & 1;
    commit();
    return StopReason::None;
  }
  if ((insn & 0xFF201FE0u) == 0x1E201000u) {  // FMOV (immediate)
    if (!scalarFmt(type, &f)) return unsupported();
    const uint64_t imm8 = (insn >> 13) & 0xFF, b6 = (imm8 >> 6) & 1, sign = (imm8 >> 7) & 1;
    uint64_t bits;
    if (f == sf::S) bits = (sign << 31) | (((b6 ^ 1) << 7 | (b6 ? 0x1Fu : 0u) << 2 | ((imm8 >> 4) & 3)) << 23) | ((imm8 & 0xF) << 19);
    else bits = (sign << 63) | (((b6 ^ 1) << 10 | (b6 ? 0xFFu : 0u) << 2 | ((imm8 >> 4) & 3)) << 52) | ((imm8 & 0xF) << 48);
    setScalar(rd, bits);
    return StopReason::None;
  }
  if ((insn & 0xFF200C00u) == 0x1E200400u) {  // FCCMP/FCCMPE
    if (!scalarFmt(type, &f)) return unsupported();
    if (condHolds((insn >> 12) & 0xF, s_)) {
      const unsigned nzcv = sf::compare(f, s_.vr[rn].lo & fmtMask(f), s_.vr[rm].lo & fmtMask(f), env, ((insn >> 4) & 1) != 0);
      s_.n = (nzcv >> 3) & 1; s_.z = (nzcv >> 2) & 1; s_.c = (nzcv >> 1) & 1; s_.v = nzcv & 1;
    } else {
      s_.n = (insn >> 3) & 1; s_.z = (insn >> 2) & 1; s_.c = (insn >> 1) & 1; s_.v = insn & 1;
    }
    commit();
    return StopReason::None;
  }
  if ((insn & 0xFF200C00u) == 0x1E200800u) {  // FP veri işleme (2 kaynak)
    if (!scalarFmt(type, &f)) return unsupported();
    const uint64_t a = s_.vr[rn].lo & fmtMask(f), b = s_.vr[rm].lo & fmtMask(f);
    uint64_t res;
    switch ((insn >> 12) & 0xF) {
      case 0: res = sf::mul(f, a, b, env); break;
      case 1: res = sf::div(f, a, b, env); break;
      case 2: res = sf::add(f, a, b, env); break;
      case 3: res = sf::sub(f, a, b, env); break;
      case 4: res = sf::minMax(f, a, b, env, false, false); break;   // FMAX
      case 5: res = sf::minMax(f, a, b, env, true, false); break;    // FMIN
      case 6: res = sf::minMax(f, a, b, env, false, true); break;    // FMAXNM
      case 7: res = sf::minMax(f, a, b, env, true, true); break;     // FMINNM
      case 8: res = sf::mul(f, a, b, env) ^ fmtSign(f); break;       // FNMUL (NaN dahil işaret çevrilir)
      default: return unsupported();
    }
    setScalar(rd, res);
    commit();
    return StopReason::None;
  }
  if ((insn & 0xFF200C00u) == 0x1E200C00u) {  // FCSEL
    if (!scalarFmt(type, &f)) return unsupported();
    setScalar(rd, (condHolds((insn >> 12) & 0xF, s_) ? s_.vr[rn].lo : s_.vr[rm].lo) & fmtMask(f));
    return StopReason::None;
  }
  if ((insn & 0xFF000000u) == 0x1F000000u) {  // FMADD/FMSUB/FNMADD/FNMSUB
    if (!scalarFmt(type, &f)) return unsupported();
    const unsigned ra = (insn >> 10) & 31;
    const bool o1 = ((insn >> 21) & 1) != 0, o0 = ((insn >> 15) & 1) != 0;
    uint64_t n = s_.vr[rn].lo & fmtMask(f), m = s_.vr[rm].lo & fmtMask(f), a = s_.vr[ra].lo & fmtMask(f);
    if (o1) a ^= fmtSign(f);          // FNMADD, FNMSUB: addend negatif
    if (o1 != o0) n ^= fmtSign(f);    // FMSUB, FNMADD: çarpılan negatif
    setScalar(rd, sf::mulAdd(f, n, m, a, env));
    commit();
    return StopReason::None;
  }
  if ((insn & 0x7F20FC00u) == 0x1E200000u) {  // FP <-> tamsayı dönüşümleri, FMOV (genel yazmaç)
    const bool x64 = (insn >> 31) != 0;
    const unsigned rmode = (insn >> 19) & 3, opcode = (insn >> 16) & 7;
    // FMOV, X <-> V.D[1]
    if (x64 && type == 2 && rmode == 1 && (opcode == 6 || opcode == 7)) {
      if (opcode == 6) setX(rd, s_.vr[rn].hi); else s_.vr[rd].hi = getX(rn);
      return StopReason::None;
    }
    if (!scalarFmt(type, &f)) return unsupported();
    if (rmode == 0 && opcode == 6) {            // FMOV Wd/Xd, Sn/Dn
      if (x64 != (f == sf::D)) return unsupported();
      setX(rd, s_.vr[rn].lo & fmtMask(f));
      return StopReason::None;
    }
    if (rmode == 0 && opcode == 7) {            // FMOV Sd/Dd, Wn/Xn
      if (x64 != (f == sf::D)) return unsupported();
      setScalar(rd, getX(rn) & fmtMask(f));
      return StopReason::None;
    }
    if ((rmode == 0 && (opcode == 2 || opcode == 3))) {   // SCVTF / UCVTF (tamsayı)
      setScalar(rd, sf::fromInt(f, getX(rn), opcode == 2, x64 ? 64 : 32, 0, env));
      commit();
      return StopReason::None;
    }
    sf::Rnd rm_;
    bool isSigned;
    if (opcode == 0 || opcode == 1) {           // FCVT{N,P,M,Z}{S,U}
      static const sf::Rnd kMode[4] = {sf::RN, sf::RP, sf::RM, sf::RZ};
      rm_ = kMode[rmode]; isSigned = opcode == 0;
    } else if (rmode == 0 && (opcode == 4 || opcode == 5)) {   // FCVTA{S,U}
      rm_ = sf::RA; isSigned = opcode == 4;
    } else {
      return unsupported();                     // FJCVTZS vb.
    }
    const uint64_t r = sf::toInt(f, s_.vr[rn].lo & fmtMask(f), isSigned, x64 ? 64 : 32, 0, rm_, env);
    setX(rd, r);
    commit();
    return StopReason::None;
  }
  if ((insn & 0x7F200000u) == 0x1E000000u) {  // FP <-> sabit-nokta
    const bool x64 = (insn >> 31) != 0;
    const unsigned rmode = (insn >> 19) & 3, opcode = (insn >> 16) & 7, scale = (insn >> 10) & 63;
    if (!scalarFmt(type, &f) || (!x64 && scale < 32)) return unsupported();
    const unsigned fbits = 64 - scale;
    if (rmode == 0 && (opcode == 2 || opcode == 3)) {         // SCVTF/UCVTF (sabit-nokta)
      setScalar(rd, sf::fromInt(f, getX(rn), opcode == 2, x64 ? 64 : 32, fbits, env));
      commit();
      return StopReason::None;
    }
    if (rmode == 3 && (opcode == 0 || opcode == 1)) {         // FCVTZS/FCVTZU (sabit-nokta)
      setX(rd, sf::toInt(f, s_.vr[rn].lo & fmtMask(f), opcode == 0, x64 ? 64 : 32, fbits, sf::RZ, env));
      commit();
      return StopReason::None;
    }
    return unsupported();
  }
  return execAsimd(insn, pc);   // vektör (ASIMD) komutları; tanınmazsa UnsupportedInstruction
}

}  // namespace ncv
