#include "ncv_softfloat.h"

namespace ncv {
namespace sf {
namespace {

typedef unsigned __int128 u128;

struct Info { int eb, mb, bias, emin; };
inline Info info(Fmt f) {
  switch (f) {
    case H: return Info{5, 10, 15, -14};
    case S: return Info{8, 23, 127, -126};
    default: return Info{11, 52, 1023, -1022};
  }
}
inline uint64_t signBit(const Info& i) { return 1ull << (i.eb + i.mb); }
inline uint64_t fracMask(const Info& i) { return (1ull << i.mb) - 1; }
inline uint64_t expMask(const Info& i) { return ((1ull << i.eb) - 1) << i.mb; }
inline uint64_t mkZero(const Info& i, bool s) { return s ? signBit(i) : 0; }
inline uint64_t mkInf(const Info& i, bool s) { return expMask(i) | (s ? signBit(i) : 0); }
inline uint64_t mkDefNaN(const Info& i) { return expMask(i) | (1ull << (i.mb - 1)); }
inline uint64_t mkMax(const Info& i, bool s) { return (expMask(i) - (1ull << i.mb)) | fracMask(i) | (s ? signBit(i) : 0); }
inline int clz128(u128 x) {
  const uint64_t hi = static_cast<uint64_t>(x >> 64);
  return hi ? __builtin_clzll(hi) : 64 + __builtin_clzll(static_cast<uint64_t>(x));
}
u128 isqrt128(u128 n) {
  u128 res = 0, bit = static_cast<u128>(1) << 126;
  while (bit > n) bit >>= 2;
  while (bit) {
    if (n >= res + bit) { n -= res + bit; res = (res >> 1) + bit; }
    else res >>= 1;
    bit >>= 2;
  }
  return res;
}

enum K { KZero, KNorm, KInf, KQNaN, KSNaN };
// KNorm: değer = (sig / 2^63) * 2^exp, sig'in 63. biti 1 (normalize)
struct U { bool sign; K k; int exp; uint64_t sig; uint64_t raw; };

inline bool isNaN(const U& u) { return u.k == KQNaN || u.k == KSNaN; }

U unpack(Fmt f, uint64_t bits, Env& env) {
  const Info i = info(f);
  U u;
  u.raw = bits;
  u.sign = ((bits >> (i.eb + i.mb)) & 1) != 0;
  u.k = KZero; u.exp = 0; u.sig = 0;
  const uint64_t frac = bits & fracMask(i);
  const uint64_t ef = (bits >> i.mb) & ((1ull << i.eb) - 1);
  if (ef == (1ull << i.eb) - 1) {
    if (frac == 0) u.k = KInf;
    else u.k = ((frac >> (i.mb - 1)) & 1) ? KQNaN : KSNaN;
    return u;
  }
  if (ef == 0) {
    if (frac == 0) return u;
    if (env.fz && f != H) { env.flags |= IDC; return u; }  // girdi denormal -> sıfıra yıkanır
    const int p = 63 - __builtin_clzll(frac);
    u.k = KNorm; u.exp = i.emin - i.mb + p; u.sig = frac << (63 - p);
    return u;
  }
  u.k = KNorm; u.exp = static_cast<int>(ef) - i.bias; u.sig = ((1ull << i.mb) | frac) << (63 - i.mb);
  return u;
}

// ARM FPProcessNaNs (2 operand)
bool procNaN2(const Info& i, const U& a, const U& b, Env& env, uint64_t* out) {
  const U* n = nullptr;
  if (a.k == KSNaN) n = &a; else if (b.k == KSNaN) n = &b;
  else if (a.k == KQNaN) n = &a; else if (b.k == KQNaN) n = &b;
  if (!n) return false;
  if (n->k == KSNaN) env.flags |= IOC;
  *out = env.dn ? mkDefNaN(i) : (n->raw | (1ull << (i.mb - 1)));
  return true;
}
// ARM FPProcessNaNs3 (sıra: c, a, b = addend, op1, op2)
bool procNaN3(const Info& i, const U& c, const U& a, const U& b, Env& env, uint64_t* out) {
  const U* n = nullptr;
  if (c.k == KSNaN) n = &c; else if (a.k == KSNaN) n = &a; else if (b.k == KSNaN) n = &b;
  else if (c.k == KQNaN) n = &c; else if (a.k == KQNaN) n = &a; else if (b.k == KQNaN) n = &b;
  if (!n) return false;
  if (n->k == KSNaN) env.flags |= IOC;
  *out = env.dn ? mkDefNaN(i) : (n->raw | (1ull << (i.mb - 1)));
  return true;
}

// value = (m / 2^L) * 2^e,  m'nin baş biti L (L <= 126), altında sticky biti olabilir. ARM FPRound.
uint64_t roundPack(Fmt f, bool sign, int e, u128 m, int L, bool sticky, Env& env) {
  const Info i = info(f);
  if (env.fz && f != H && e < i.emin) { env.flags |= UFC; return mkZero(i, sign); }
  int biased = e - i.emin + 1;
  if (biased < 0) biased = 0;
  const int sh = L - i.mb + (biased == 0 ? (i.emin - e) : 0);
  enum { EXACT, BELOW, TIE, ABOVE } cmp;
  u128 intm;
  bool inexact;
  if (sh <= 0) {
    intm = m << (-sh); inexact = sticky; cmp = sticky ? BELOW : EXACT;
  } else if (sh < 128) {
    intm = m >> sh;
    const u128 rem = m & ((static_cast<u128>(1) << sh) - 1);
    const u128 half = static_cast<u128>(1) << (sh - 1);
    inexact = (rem != 0) || sticky;
    if (rem > half) cmp = ABOVE;
    else if (rem == half) cmp = sticky ? ABOVE : TIE;
    else cmp = inexact ? BELOW : EXACT;
  } else {
    intm = 0; inexact = true; cmp = BELOW;
  }
  if (biased == 0 && inexact) env.flags |= UFC;  // tininess: yuvarlamadan ÖNCE
  bool up = false;
  switch (env.rmode) {
    case RN: up = (cmp == ABOVE) || (cmp == TIE && (intm & 1)); break;
    case RP: up = inexact && !sign; break;
    case RM: up = inexact && sign; break;
    default: break;
  }
  uint64_t im = static_cast<uint64_t>(intm);
  if (up) {
    im++;
    if (im == (1ull << i.mb)) biased = 1;
    if (im == (1ull << (i.mb + 1))) { biased++; im >>= 1; }
  }
  if (biased >= (1 << i.eb) - 1) {
    env.flags |= OFC | IXC;
    const bool toInf = env.rmode == RN || (env.rmode == RP && !sign) || (env.rmode == RM && sign);
    return toInf ? mkInf(i, sign) : mkMax(i, sign);
  }
  if (inexact) env.flags |= IXC;
  return (sign ? signBit(i) : 0) | (static_cast<uint64_t>(biased) << i.mb) | (im & fracMask(i));
}

// X = değer * 2^125 / 2^e biçimli (baş bit 125). Her iki operand sonlu ve sıfırdan farklı.
uint64_t addAligned(Fmt f, bool sa, int ea, u128 xa, bool sb, int eb, u128 xb, Env& env, int escale = 0) {
  if (ea < eb || (ea == eb && xa < xb)) {
    const bool ts = sa; sa = sb; sb = ts;
    const int te = ea; ea = eb; eb = te;
    const u128 tx = xa; xa = xb; xb = tx;
  }
  const int d = ea - eb;
  u128 y;
  bool sticky = false;
  if (d == 0) y = xb;
  else if (d >= 127) { y = 0; sticky = true; }
  else { y = xb >> d; sticky = (xb & ((static_cast<u128>(1) << d) - 1)) != 0; }
  u128 s;
  if (sa == sb) {
    s = xa + y;
  } else {
    s = xa - y;
    if (sticky) s -= 1;  // gerçek fark: xa - (y + eps)
    if (s == 0 && !sticky) return mkZero(info(f), env.rmode == RM);  // tam sıfır
  }
  const int L = 127 - clz128(s);
  return roundPack(f, sa, ea + (L - 125) + escale, s, L, sticky, env);
}

int cmpVal(const U& a, const U& b) {  // NaN olmayan operandlar: -1, 0, 1
  const int ca = a.k == KZero ? 0 : (a.sign ? -1 : 1);
  const int cb = b.k == KZero ? 0 : (b.sign ? -1 : 1);
  if (ca != cb) return ca < cb ? -1 : 1;
  if (ca == 0) return 0;
  int m;
  if (a.k != b.k) m = (a.k == KInf) ? 1 : -1;
  else if (a.k == KInf) m = 0;
  else if (a.exp != b.exp) m = a.exp > b.exp ? 1 : -1;
  else m = a.sig == b.sig ? 0 : (a.sig > b.sig ? 1 : -1);
  return ca > 0 ? m : -m;
}

uint64_t addSub(Fmt f, uint64_t a, uint64_t b, Env& env, bool subtract) {
  const Info i = info(f);
  U ua = unpack(f, a, env), ub = unpack(f, b, env);
  uint64_t out;
  if (procNaN2(i, ua, ub, env, &out)) return out;
  if (subtract) ub.sign = !ub.sign;
  const bool ia = ua.k == KInf, ib = ub.k == KInf, za = ua.k == KZero, zb = ub.k == KZero;
  if (ia && ib) {
    if (ua.sign == ub.sign) return mkInf(i, ua.sign);
    env.flags |= IOC;
    return mkDefNaN(i);
  }
  if (ia) return mkInf(i, ua.sign);
  if (ib) return mkInf(i, ub.sign);
  if (za && zb) return ua.sign == ub.sign ? mkZero(i, ua.sign) : mkZero(i, env.rmode == RM);
  if (za) return (ub.raw & ~signBit(i)) | (ub.sign ? signBit(i) : 0);
  if (zb) return ua.raw;
  return addAligned(f, ua.sign, ua.exp, static_cast<u128>(ua.sig) << 62, ub.sign, ub.exp, static_cast<u128>(ub.sig) << 62, env);
}

}  // namespace

uint64_t add(Fmt f, uint64_t a, uint64_t b, Env& e) { return addSub(f, a, b, e, false); }
uint64_t sub(Fmt f, uint64_t a, uint64_t b, Env& e) { return addSub(f, a, b, e, true); }

uint64_t mul(Fmt f, uint64_t a, uint64_t b, Env& env) {
  const Info i = info(f);
  U ua = unpack(f, a, env), ub = unpack(f, b, env);
  uint64_t out;
  if (procNaN2(i, ua, ub, env, &out)) return out;
  const bool sign = ua.sign != ub.sign;
  const bool ia = ua.k == KInf, ib = ub.k == KInf, za = ua.k == KZero, zb = ub.k == KZero;
  if ((ia && zb) || (ib && za)) { env.flags |= IOC; return mkDefNaN(i); }
  if (ia || ib) return mkInf(i, sign);
  if (za || zb) return mkZero(i, sign);
  u128 p = static_cast<u128>(ua.sig) * ub.sig;
  int L = (p >> 127) ? 127 : 126;
  const int e = ua.exp + ub.exp + (L - 126);
  bool sticky = false;
  if (L == 127) { sticky = (p & 1) != 0; p >>= 1; L = 126; }
  return roundPack(f, sign, e, p, L, sticky, env);
}

uint64_t div(Fmt f, uint64_t a, uint64_t b, Env& env) {
  const Info i = info(f);
  U ua = unpack(f, a, env), ub = unpack(f, b, env);
  uint64_t out;
  if (procNaN2(i, ua, ub, env, &out)) return out;
  const bool sign = ua.sign != ub.sign;
  const bool ia = ua.k == KInf, ib = ub.k == KInf, za = ua.k == KZero, zb = ub.k == KZero;
  if ((ia && ib) || (za && zb)) { env.flags |= IOC; return mkDefNaN(i); }
  if (ia) return mkInf(i, sign);
  if (zb) { env.flags |= DZC; return mkInf(i, sign); }
  if (ib || za) return mkZero(i, sign);
  const u128 n = static_cast<u128>(ua.sig) << 64;
  const u128 q = n / ub.sig;
  const bool sticky = (n % ub.sig) != 0;
  const int L = 127 - clz128(q);
  return roundPack(f, sign, ua.exp - ub.exp + (L - 64), q, L, sticky, env);
}

uint64_t sqrt(Fmt f, uint64_t a, Env& env) {
  const Info i = info(f);
  U u = unpack(f, a, env);
  if (isNaN(u)) {
    if (u.k == KSNaN) env.flags |= IOC;
    return env.dn ? mkDefNaN(i) : (u.raw | (1ull << (i.mb - 1)));
  }
  if (u.k == KZero) return mkZero(i, u.sign);
  if (u.sign) { env.flags |= IOC; return mkDefNaN(i); }
  if (u.k == KInf) return mkInf(i, false);
  const bool odd = (u.exp & 1) != 0;
  const int k = (u.exp - (odd ? 1 : 0)) / 2;
  const u128 n = static_cast<u128>(u.sig) << (odd ? 64 : 63);
  const u128 r = isqrt128(n);
  const bool sticky = (n - r * r) != 0;
  return roundPack(f, false, k, r, 63, sticky, env);
}

namespace {
uint64_t mulAddImpl(Fmt f, uint64_t a, uint64_t b, uint64_t c, Env& env, int escale);
}

uint64_t mulAdd(Fmt f, uint64_t a, uint64_t b, uint64_t c, Env& env) { return mulAddImpl(f, a, b, c, env, 0); }

namespace {
uint64_t mulAddImpl(Fmt f, uint64_t a, uint64_t b, uint64_t c, Env& env, int escale) {
  const Info i = info(f);
  U ua = unpack(f, a, env), ub = unpack(f, b, env), uc = unpack(f, c, env);
  const bool ia = ua.k == KInf, ib = ub.k == KInf, ic = uc.k == KInf;
  const bool za = ua.k == KZero, zb = ub.k == KZero, zc = uc.k == KZero;
  if (uc.k == KQNaN && ((ia && zb) || (za && ib))) { env.flags |= IOC; return mkDefNaN(i); }
  uint64_t out;
  if (procNaN3(i, uc, ua, ub, env, &out)) return out;
  const bool signP = ua.sign != ub.sign, infP = ia || ib, zeroP = za || zb;
  if ((ia && zb) || (za && ib) || (ic && infP && uc.sign != signP)) { env.flags |= IOC; return mkDefNaN(i); }
  if ((ic && !uc.sign) || (infP && !signP)) return mkInf(i, false);
  if ((ic && uc.sign) || (infP && signP)) return mkInf(i, true);
  if (zc && zeroP && uc.sign == signP) return mkZero(i, uc.sign);
  if (zeroP) return zc ? mkZero(i, env.rmode == RM) : uc.raw;
  u128 p = static_cast<u128>(ua.sig) * ub.sig;
  const int Lp = (p >> 127) ? 127 : 126;
  const int ep = ua.exp + ub.exp + (Lp - 126);
  const u128 xp = p >> (Lp - 125);  // sig'ler <= 53 anlamlı bit: alt >= 22 bit sıfır, kayıpsız
  if (zc) return roundPack(f, signP, ep + escale, xp, 125, false, env);
  return addAligned(f, signP, ep, xp, uc.sign, uc.exp, static_cast<u128>(uc.sig) << 62, env, escale);
}
}  // namespace

uint64_t minMax(Fmt f, uint64_t a, uint64_t b, Env& env, bool isMin, bool num) {
  const Info i = info(f);
  U ua = unpack(f, a, env), ub = unpack(f, b, env);
  if (num) {  // FMAXNM/FMINNM: sessiz NaN, sayıya karşı "etkisiz" sonsuzla değiştirilir
    if (ua.k == KQNaN && ub.k != KQNaN) { ua.k = KInf; ua.sign = !isMin; }
    else if (ub.k == KQNaN && ua.k != KQNaN) { ub.k = KInf; ub.sign = !isMin; }
  }
  uint64_t out;
  if (procNaN2(i, ua, ub, env, &out)) return out;
  if (ua.k == KZero && ub.k == KZero)
    return mkZero(i, isMin ? (ua.sign || ub.sign) : (ua.sign && ub.sign));
  const int c = cmpVal(ua, ub);
  const U& pick = isMin ? (c <= 0 ? ua : ub) : (c >= 0 ? ua : ub);
  if (pick.k == KInf) return mkInf(i, pick.sign);
  if (pick.k == KZero) return mkZero(i, pick.sign);
  return pick.raw;
}

unsigned compare(Fmt f, uint64_t a, uint64_t b, Env& env, bool signaling) {
  U ua = unpack(f, a, env), ub = unpack(f, b, env);
  if (isNaN(ua) || isNaN(ub)) {
    if (signaling || ua.k == KSNaN || ub.k == KSNaN) env.flags |= IOC;
    return 3;  // 0011 sırasız
  }
  const int c = cmpVal(ua, ub);
  return c == 0 ? 6 : (c < 0 ? 8 : 2);
}

uint64_t convert(Fmt from, Fmt to, uint64_t a, Env& env) {
  const Info is = info(from), id = info(to);
  U u = unpack(from, a, env);
  if (isNaN(u)) {
    if (u.k == KSNaN) env.flags |= IOC;
    if (env.dn) return mkDefNaN(id);
    const uint64_t fs = a & fracMask(is);
    uint64_t fd = id.mb >= is.mb ? (fs << (id.mb - is.mb)) : (fs >> (is.mb - id.mb));
    fd |= 1ull << (id.mb - 1);
    return (u.sign ? signBit(id) : 0) | expMask(id) | fd;
  }
  if (u.k == KInf) return mkInf(id, u.sign);
  if (u.k == KZero) return mkZero(id, u.sign);
  return roundPack(to, u.sign, u.exp, u.sig, 63, false, env);
}

uint64_t roundInt(Fmt f, uint64_t a, Rnd rm, bool exact, Env& env) {
  const Info i = info(f);
  U u = unpack(f, a, env);
  if (isNaN(u)) {
    if (u.k == KSNaN) env.flags |= IOC;
    return env.dn ? mkDefNaN(i) : (u.raw | (1ull << (i.mb - 1)));
  }
  if (u.k == KInf) return mkInf(i, u.sign);
  if (u.k == KZero) return mkZero(i, u.sign);
  if (u.exp >= i.mb) return u.raw;  // |x| >= 2^mb: zaten tamsayı
  const int s = 63 - u.exp;
  u128 ip;
  bool inexact = false, above = false, tie = false;
  if (s >= 128) { ip = 0; inexact = true; }
  else {
    ip = static_cast<u128>(u.sig) >> s;
    const u128 rem = static_cast<u128>(u.sig) & ((static_cast<u128>(1) << s) - 1);
    const u128 half = static_cast<u128>(1) << (s - 1);
    if (rem) { inexact = true; above = rem > half; tie = rem == half; }
  }
  bool bump = false;
  switch (rm) {
    case RN: bump = above || (tie && (ip & 1)); break;
    case RA: bump = above || tie; break;
    case RP: bump = inexact && !u.sign; break;
    case RM: bump = inexact && u.sign; break;
    default: break;
  }
  const uint64_t m = static_cast<uint64_t>(ip) + (bump ? 1 : 0);
  if (exact && inexact) env.flags |= IXC;
  if (m == 0) return mkZero(i, u.sign);
  const int p = 63 - __builtin_clzll(m);
  Env tmp;  // tam gösterilebilir: bayrak üretmez
  return roundPack(f, u.sign, p, m, p, false, tmp);
}

uint64_t toInt(Fmt f, uint64_t a, bool isSigned, unsigned width, unsigned fbits, Rnd rm, Env& env) {
  U u = unpack(f, a, env);
  const uint64_t wmask = width == 64 ? ~0ull : ((1ull << width) - 1);
  if (isNaN(u)) { env.flags |= IOC; return 0; }
  const bool sign = u.sign;
  u128 M = 0;
  bool inexact = false, huge = false;
  if (u.k == KInf) {
    huge = true;
  } else if (u.k == KNorm) {
    const int e = u.exp + static_cast<int>(fbits);
    if (e >= 72) {
      huge = true;
    } else {
      const int s = 63 - e;
      u128 ip;
      bool above = false, tie = false;
      if (s <= 0) ip = static_cast<u128>(u.sig) << (-s);
      else if (s >= 128) { ip = 0; inexact = true; }
      else {
        ip = static_cast<u128>(u.sig) >> s;
        const u128 rem = static_cast<u128>(u.sig) & ((static_cast<u128>(1) << s) - 1);
        const u128 half = static_cast<u128>(1) << (s - 1);
        if (rem) { inexact = true; above = rem > half; tie = rem == half; }
      }
      bool bump = false;
      switch (rm) {
        case RN: bump = above || (tie && (ip & 1)); break;
        case RA: bump = above || tie; break;
        case RP: bump = inexact && !sign; break;
        case RM: bump = inexact && sign; break;
        default: break;
      }
      M = ip + (bump ? 1 : 0);
    }
  }
  bool ovf = false;
  uint64_t res = 0;
  const u128 maxS = (static_cast<u128>(1) << (width - 1)) - 1, maxU = (static_cast<u128>(1) << width) - 1;
  if (huge) { ovf = true; M = static_cast<u128>(1) << 100; }
  if (isSigned) {
    if (!sign) { if (M > maxS) { ovf = true; res = static_cast<uint64_t>(maxS); } else res = static_cast<uint64_t>(M); }
    else { if (M > maxS + 1) { ovf = true; res = static_cast<uint64_t>(static_cast<u128>(1) << (width - 1)); }
           else res = 0 - static_cast<uint64_t>(M); }
  } else {
    if (!sign) { if (M > maxU) { ovf = true; res = static_cast<uint64_t>(maxU); } else res = static_cast<uint64_t>(M); }
    else { if (M != 0) ovf = true; res = 0; }
  }
  if (ovf) env.flags |= IOC; else if (inexact) env.flags |= IXC;
  return res & wmask;
}

uint64_t fromInt(Fmt f, uint64_t v, bool isSigned, unsigned srcWidth, unsigned fbits, Env& env) {
  bool sign = false;
  uint64_t mag;
  if (isSigned) {
    const int64_t sv = srcWidth == 64 ? static_cast<int64_t>(v)
                                      : static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(v)));
    sign = sv < 0;
    mag = sign ? 0 - static_cast<uint64_t>(sv) : static_cast<uint64_t>(sv);
  } else {
    mag = srcWidth == 64 ? v : (v & 0xFFFFFFFFull);
  }
  if (mag == 0) return 0;
  const int p = 63 - __builtin_clzll(mag);
  return roundPack(f, sign, p - static_cast<int>(fbits), mag, p, false, env);
}


namespace {
inline uint64_t constTwo(Fmt f) { return f == S ? 0x40000000ull : 0x4000000000000000ull; }
inline uint64_t constThree(Fmt f) { return f == S ? 0x40400000ull : 0x4008000000000000ull; }
inline uint64_t constOnePointFive(Fmt f) { return f == S ? 0x3FC00000ull : 0x3FF8000000000000ull; }
}  // namespace

uint64_t mulX(Fmt f, uint64_t a, uint64_t b, Env& env) {
  const Info i = info(f);
  Env tmp;
  tmp.fz = env.fz;
  U ua = unpack(f, a, tmp), ub = unpack(f, b, tmp);
  if ((ua.k == KInf && ub.k == KZero) || (ua.k == KZero && ub.k == KInf)) {
    env.flags |= tmp.flags;
    return constTwo(f) | ((ua.sign != ub.sign) ? signBit(i) : 0);
  }
  return mul(f, a, b, env);
}

uint64_t recipStep(Fmt f, uint64_t a, uint64_t b, Env& env) {
  const Info i = info(f);
  a ^= signBit(i);
  U ua = unpack(f, a, env), ub = unpack(f, b, env);
  uint64_t out;
  if (procNaN2(i, ua, ub, env, &out)) return out;
  const bool ia = ua.k == KInf, ib = ub.k == KInf, za = ua.k == KZero, zb = ub.k == KZero;
  if ((ia && zb) || (za && ib)) return constTwo(f);
  if (ia || ib) return mkInf(i, ua.sign != ub.sign);
  if (za || zb) return constTwo(f);
  const uint64_t r = mulAddImpl(f, a, b, constTwo(f), env, 0);
  return (r & ~signBit(i)) == 0 ? 0 : r;
}

uint64_t rsqrtStep(Fmt f, uint64_t a, uint64_t b, Env& env) {
  const Info i = info(f);
  a ^= signBit(i);
  U ua = unpack(f, a, env), ub = unpack(f, b, env);
  uint64_t out;
  if (procNaN2(i, ua, ub, env, &out)) return out;
  const bool ia = ua.k == KInf, ib = ub.k == KInf, za = ua.k == KZero, zb = ub.k == KZero;
  if ((ia && zb) || (za && ib)) return constOnePointFive(f);
  if (ia || ib) return mkInf(i, ua.sign != ub.sign);
  if (za || zb) return constOnePointFive(f);
  const uint64_t r = mulAddImpl(f, a, b, constThree(f), env, -1);
  return (r & ~signBit(i)) == 0 ? 0 : r;
}

namespace {
unsigned recipEstimateInt(unsigned a) {  // 256..511 -> 256..511
  a = a * 2 + 1;
  const unsigned b = (1u << 19) / a;
  return (b + 1) / 2;
}
unsigned rsqrtEstimateInt(unsigned a) {  // 128..511 -> 256..511
  if (a < 256) a = a * 2 + 1;
  else { a = (a >> 1) << 1; a = (a + 1) * 2; }
  unsigned b = 512;
  while (static_cast<uint64_t>(a) * (b + 1) * (b + 1) < (1ull << 28)) b++;
  return (b + 1) / 2;
}
}  // namespace

uint64_t recipEstimate(Fmt f, uint64_t a, Env& env) {
  const Info i = info(f);
  U u = unpack(f, a, env);
  if (isNaN(u)) {
    if (u.k == KSNaN) env.flags |= IOC;
    return env.dn ? mkDefNaN(i) : (u.raw | (1ull << (i.mb - 1)));
  }
  if (u.k == KInf) return mkZero(i, u.sign);
  if (u.k == KZero) { env.flags |= DZC; return mkInf(i, u.sign); }
  const int tiny = f == S ? -128 : -1024;
  if (u.exp < tiny) {
    env.flags |= OFC | IXC;
    const bool toInf = env.rmode == RN || (env.rmode == RP && !u.sign) || (env.rmode == RM && u.sign);
    return toInf ? mkInf(i, u.sign) : mkMax(i, u.sign);
  }
  if (env.fz && ((f == S && u.exp >= 126) || (f == D && u.exp >= 1022))) { env.flags |= UFC; return mkZero(i, u.sign); }
  uint64_t frac52;
  int exp;
  if (f == S) { frac52 = (a & 0x7FFFFFull) << 29; exp = static_cast<int>((a >> 23) & 0xFF); }
  else { frac52 = a & 0xFFFFFFFFFFFFFull; exp = static_cast<int>((a >> 52) & 0x7FF); }
  if (exp == 0) {
    if (((frac52 >> 51) & 1) == 0) { exp = -1; frac52 = (frac52 & ((1ull << 50) - 1)) << 2; }
    else frac52 = (frac52 & ((1ull << 51) - 1)) << 1;
  }
  const unsigned scaled = 256u | static_cast<unsigned>((frac52 >> 44) & 0xFF);
  int resultExp = (f == S ? 253 : 2045) - exp;
  const unsigned est = recipEstimateInt(scaled);
  uint64_t fraction = static_cast<uint64_t>(est & 0xFF) << 44;
  if (resultExp == 0) fraction = (1ull << 51) | (fraction >> 1);
  else if (resultExp == -1) { fraction = (1ull << 50) | (fraction >> 2); resultExp = 0; }
  return (u.sign ? signBit(i) : 0) | (static_cast<uint64_t>(resultExp) << i.mb) | (fraction >> (52 - i.mb));
}

uint64_t rsqrtEstimate(Fmt f, uint64_t a, Env& env) {
  const Info i = info(f);
  U u = unpack(f, a, env);
  if (isNaN(u)) {
    if (u.k == KSNaN) env.flags |= IOC;
    return env.dn ? mkDefNaN(i) : (u.raw | (1ull << (i.mb - 1)));
  }
  if (u.k == KZero) { env.flags |= DZC; return mkInf(i, u.sign); }
  if (u.sign) { env.flags |= IOC; return mkDefNaN(i); }
  if (u.k == KInf) return mkZero(i, false);
  uint64_t frac52;
  int exp;
  if (f == S) { frac52 = (a & 0x7FFFFFull) << 29; exp = static_cast<int>((a >> 23) & 0xFF); }
  else { frac52 = a & 0xFFFFFFFFFFFFFull; exp = static_cast<int>((a >> 52) & 0x7FF); }
  if (exp == 0) {
    while (((frac52 >> 51) & 1) == 0) { frac52 = (frac52 & ((1ull << 51) - 1)) << 1; exp--; }
    frac52 = (frac52 & ((1ull << 51) - 1)) << 1;
  }
  const unsigned scaled = (exp & 1) == 0 ? (256u | static_cast<unsigned>((frac52 >> 44) & 0xFF)) : (128u | static_cast<unsigned>((frac52 >> 45) & 0x7F));
  const int resultExp = ((f == S ? 380 : 3068) - exp) / 2;
  const unsigned est = rsqrtEstimateInt(scaled);
  const uint64_t fraction = static_cast<uint64_t>(est & 0xFF) << 44;
  return (static_cast<uint64_t>(resultExp) << i.mb) | (fraction >> (52 - i.mb));
}

uint64_t convertOdd(Fmt from, Fmt to, uint64_t a, Env& env) {
  Env e;
  e.rmode = RZ; e.fz = env.fz; e.dn = env.dn;
  uint64_t r = convert(from, to, a, e);
  env.flags |= e.flags;
  const Info id = info(to);
  const bool finite = (r & expMask(id)) != expMask(id);
  if ((e.flags & IXC) && finite) r |= 1;
  return r;
}

}  // namespace sf
}  // namespace ncv
